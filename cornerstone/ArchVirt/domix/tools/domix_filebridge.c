#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>

#define FILE_TRANSFER_CHUNK_SIZE 4096
#define FILE_TRANSFER_MAX_PATH 512
#define FILE_TRANSFER_CTRL_SIZE 4096
#define FILE_TRANSFER_CTRL_HEADER_SIZE 36
#define FILE_TRANSFER_DOORBELL_PORT 0x3F9
#define FILE_TRANSFER_GPA 0x10000000

typedef enum {
    CMD_NONE = 0,
    CMD_FILE_WRITE_BEGIN = 0x1001,
    CMD_FILE_WRITE_CHUNK = 0x1002,
    CMD_FILE_WRITE_END = 0x1003,
    CMD_FILE_READ_REQUEST = 0x1004,
    CMD_FILE_READ_RESPONSE = 0x1005,
    CMD_FILE_ACK = 0x1006,
    CMD_FILE_ERROR = 0x1007,
    CMD_FILE_PING = 0x1008,
    CMD_FILE_PONG = 0x1009,
} file_cmd_t;

typedef enum {
    STATUS_SUCCESS = 0,
    STATUS_ERROR_FILE_NOT_FOUND = 1,
    STATUS_ERROR_PERMISSION_DENIED = 2,
    STATUS_ERROR_DISK_FULL = 3,
    STATUS_ERROR_CHECKSUM_MISMATCH = 4,
    STATUS_ERROR_TIMEOUT = 5,
    STATUS_ERROR_INVALID_PATH = 6,
    STATUS_ERROR_INTERNAL = 7,
    STATUS_ERROR_BUSY = 8,
} file_status_t;

typedef struct {
    volatile uint32_t command;
    volatile uint32_t status;
    volatile uint64_t file_size;
    volatile uint32_t path_length;
    volatile uint32_t chunk_id;
    volatile uint32_t chunk_size;
    volatile uint32_t checksum;
    volatile uint32_t reserved_pad;
    char path[FILE_TRANSFER_MAX_PATH];
    uint8_t padding[FILE_TRANSFER_CTRL_SIZE - FILE_TRANSFER_CTRL_HEADER_SIZE - FILE_TRANSFER_MAX_PATH];
} file_ctrl_t;

static uint8_t *g_data_buffer = NULL;

static uint32_t compute_checksum(const uint8_t *data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        int j;
        for (j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

static int validate_path(const char *path) {
    if (!path || path[0] == '\0') return 0;
    if (strlen(path) >= FILE_TRANSFER_MAX_PATH) return 0;
    if (strstr(path, "..") != NULL) return 0;
    if (path[0] == '~') return 0;
    return 1;
}

static void signal_host(void) {
    outb(1, FILE_TRANSFER_DOORBELL_PORT);
}

static void sleep_ms(unsigned int milliseconds) {
    struct timespec request;
    struct timespec remaining;

    request.tv_sec = milliseconds / 1000u;
    request.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;

    while (nanosleep(&request, &remaining) != 0 && errno == EINTR)
        request = remaining;
}

static void send_ack(file_ctrl_t *ctrl, uint32_t chunk_id, file_status_t status) {
    ctrl->command = CMD_FILE_ACK;
    ctrl->status = (uint32_t)status;
    ctrl->chunk_id = chunk_id;
    signal_host();
}

static void send_error(file_ctrl_t *ctrl, file_status_t error) {
    ctrl->command = CMD_FILE_ERROR;
    ctrl->status = (uint32_t)error;
    signal_host();
}

static void handle_write_begin(file_ctrl_t *ctrl) {
    const char *guest_path = ctrl->path;
    if (!validate_path(guest_path)) {
        send_error(ctrl, STATUS_ERROR_INVALID_PATH);
        return;
    }

    uint64_t file_size = ctrl->file_size;
    FILE *out = fopen(guest_path, "wb");
    if (!out) {
        send_error(ctrl, STATUS_ERROR_PERMISSION_DENIED);
        return;
    }

    if (file_size == 0) {
        fclose(out);
        send_ack(ctrl, 0, STATUS_SUCCESS);
        return;
    }

    fclose(out);
    send_ack(ctrl, 0, STATUS_SUCCESS);
}

static void handle_write_chunk(file_ctrl_t *ctrl) {
    uint32_t chunk_id = ctrl->chunk_id;
    size_t chunk_size = ctrl->chunk_size;
    uint32_t expected_checksum = ctrl->checksum;
    const char *guest_path = ctrl->path;

    if (!validate_path(guest_path)) {
        send_error(ctrl, STATUS_ERROR_INVALID_PATH);
        return;
    }

    if (chunk_size > FILE_TRANSFER_CHUNK_SIZE) {
        send_error(ctrl, STATUS_ERROR_INTERNAL);
        return;
    }

    uint32_t actual_checksum = compute_checksum(g_data_buffer, chunk_size);
    if (actual_checksum != expected_checksum) {
        send_error(ctrl, STATUS_ERROR_CHECKSUM_MISMATCH);
        return;
    }

    FILE *out = fopen(guest_path, "ab");
    if (!out) {
        send_error(ctrl, STATUS_ERROR_PERMISSION_DENIED);
        return;
    }

    size_t written = fwrite(g_data_buffer, 1, chunk_size, out);
    fclose(out);

    if (written != chunk_size) {
        send_error(ctrl, STATUS_ERROR_DISK_FULL);
        return;
    }

    send_ack(ctrl, chunk_id, STATUS_SUCCESS);
}

static void handle_write_end(file_ctrl_t *ctrl) {
    const char *guest_path = ctrl->path;
    if (!validate_path(guest_path)) {
        send_error(ctrl, STATUS_ERROR_INVALID_PATH);
        return;
    }

    struct stat st;
    if (stat(guest_path, &st) != 0) {
        send_error(ctrl, STATUS_ERROR_FILE_NOT_FOUND);
        return;
    }

    if ((uint64_t)st.st_size != ctrl->file_size) {
        send_error(ctrl, STATUS_ERROR_CHECKSUM_MISMATCH);
        return;
    }

    FILE *in = fopen(guest_path, "rb");
    if (!in) {
        send_error(ctrl, STATUS_ERROR_INTERNAL);
        return;
    }

    uint8_t *full_data = (uint8_t *)malloc(st.st_size);
    if (!full_data) {
        fclose(in);
        send_error(ctrl, STATUS_ERROR_INTERNAL);
        return;
    }

    size_t total_read = 0;
    while (total_read < (size_t)st.st_size) {
        size_t n = fread(full_data + total_read, 1, (size_t)st.st_size - total_read, in);
        if (n == 0) break;
        total_read += n;
    }
    fclose(in);

    uint32_t full_checksum = compute_checksum(full_data, total_read);
    free(full_data);

    if (full_checksum != ctrl->checksum) {
        unlink(guest_path);
        send_error(ctrl, STATUS_ERROR_CHECKSUM_MISMATCH);
        return;
    }

    send_ack(ctrl, 0xFFFFFFFFu, STATUS_SUCCESS);
}

static void handle_read_request(file_ctrl_t *ctrl) {
    const char *guest_path = ctrl->path;
    if (!validate_path(guest_path)) {
        send_error(ctrl, STATUS_ERROR_INVALID_PATH);
        return;
    }

    struct stat st;
    if (stat(guest_path, &st) != 0) {
        send_error(ctrl, STATUS_ERROR_FILE_NOT_FOUND);
        return;
    }

    FILE *in = fopen(guest_path, "rb");
    if (!in) {
        send_error(ctrl, STATUS_ERROR_PERMISSION_DENIED);
        return;
    }

    uint8_t *full_data = (uint8_t *)malloc(st.st_size);
    if (!full_data) {
        fclose(in);
        send_error(ctrl, STATUS_ERROR_INTERNAL);
        return;
    }

    size_t total_read = 0;
    while (total_read < (size_t)st.st_size) {
        size_t n = fread(full_data + total_read, 1, (size_t)st.st_size - total_read, in);
        if (n == 0) break;
        total_read += n;
    }
    fclose(in);

    uint32_t full_checksum = compute_checksum(full_data, total_read);
    uint32_t total_chunks = (total_read + FILE_TRANSFER_CHUNK_SIZE - 1) / FILE_TRANSFER_CHUNK_SIZE;

    ctrl->command = CMD_FILE_READ_RESPONSE;
    ctrl->status = STATUS_SUCCESS;
    ctrl->file_size = (uint64_t)total_read;
    ctrl->checksum = full_checksum;
    signal_host();

    uint32_t chunk_id;
    for (chunk_id = 0; chunk_id < total_chunks; ++chunk_id) {
        size_t offset = (size_t)chunk_id * FILE_TRANSFER_CHUNK_SIZE;
        size_t chunk_size = (offset + FILE_TRANSFER_CHUNK_SIZE > total_read)
                            ? (total_read - offset) : FILE_TRANSFER_CHUNK_SIZE;
        uint32_t chunk_checksum = compute_checksum(full_data + offset, chunk_size);

        memcpy(g_data_buffer, full_data + offset, chunk_size);

        ctrl->command = CMD_FILE_READ_RESPONSE;
        ctrl->chunk_id = chunk_id;
        ctrl->chunk_size = (uint32_t)chunk_size;
        ctrl->checksum = chunk_checksum;
        signal_host();

        while (ctrl->command != CMD_FILE_ACK) {
            sleep_ms(10);
        }

        if (ctrl->status != STATUS_SUCCESS) {
            free(full_data);
            return;
        }
    }

    free(full_data);
}

static void handle_ping(file_ctrl_t *ctrl) {
    ctrl->command = CMD_FILE_PONG;
    ctrl->status = STATUS_SUCCESS;
    signal_host();
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    file_ctrl_t *ctrl = (file_ctrl_t *)FILE_TRANSFER_GPA;
    g_data_buffer = (uint8_t *)(FILE_TRANSFER_GPA + FILE_TRANSFER_CTRL_SIZE);

    fprintf(stderr, "domix_filebridge: daemon started, polling at GPA 0x%x\n",
            FILE_TRANSFER_GPA);

    while (1) {
        uint32_t cmd = ctrl->command;

        switch (cmd) {
        case CMD_FILE_WRITE_BEGIN:
            handle_write_begin(ctrl);
            ctrl->command = CMD_NONE;
            break;
        case CMD_FILE_WRITE_CHUNK:
            handle_write_chunk(ctrl);
            ctrl->command = CMD_NONE;
            break;
        case CMD_FILE_WRITE_END:
            handle_write_end(ctrl);
            ctrl->command = CMD_NONE;
            break;
        case CMD_FILE_READ_REQUEST:
            handle_read_request(ctrl);
            ctrl->command = CMD_NONE;
            break;
        case CMD_FILE_PING:
            handle_ping(ctrl);
            ctrl->command = CMD_NONE;
            break;
        case CMD_NONE:
        case CMD_FILE_ACK:
        case CMD_FILE_ERROR:
        case CMD_FILE_PONG:
        default:
            sleep_ms(50);
            break;
        }
    }

    return 0;
}
