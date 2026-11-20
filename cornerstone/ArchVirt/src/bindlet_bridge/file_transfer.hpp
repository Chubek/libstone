#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <functional>
#include "domvirt_types.hpp"

namespace domvirt::bindlet_bridge {

static constexpr size_t FILE_TRANSFER_CHUNK_SIZE = 4096;
static constexpr size_t FILE_TRANSFER_MAX_PATH = 512;
static constexpr size_t FILE_TRANSFER_CTRL_SIZE = 4096;
static constexpr size_t FILE_TRANSFER_CTRL_HEADER_SIZE = 36;
static constexpr size_t FILE_TRANSFER_RETRY_MAX = 3;
static constexpr uint32_t FILE_TRANSFER_TIMEOUT_MS = 5000;
static constexpr uint16_t FILE_TRANSFER_DOORBELL_PORT = 0x3F9;

enum class FileTransferCommand : uint32_t {
    NONE = 0,
    FILE_WRITE_BEGIN = 0x1001,
    FILE_WRITE_CHUNK = 0x1002,
    FILE_WRITE_END = 0x1003,
    FILE_READ_REQUEST = 0x1004,
    FILE_READ_RESPONSE = 0x1005,
    FILE_ACK = 0x1006,
    FILE_ERROR = 0x1007,
    FILE_PING = 0x1008,
    FILE_PONG = 0x1009,
};

enum class FileTransferStatus : uint32_t {
    SUCCESS = 0,
    ERROR_FILE_NOT_FOUND = 1,
    ERROR_PERMISSION_DENIED = 2,
    ERROR_DISK_FULL = 3,
    ERROR_CHECKSUM_MISMATCH = 4,
    ERROR_TIMEOUT = 5,
    ERROR_INVALID_PATH = 6,
    ERROR_INTERNAL = 7,
    ERROR_BUSY = 8,
};

struct FileTransferControlBlock {
    uint32_t command;
    uint32_t status;
    uint64_t file_size;
    uint32_t path_length;
    uint32_t chunk_id;
    uint32_t chunk_size;
    uint32_t checksum;
    uint32_t reserved_pad;
    char path[FILE_TRANSFER_MAX_PATH];
    uint8_t padding[FILE_TRANSFER_CTRL_SIZE - FILE_TRANSFER_CTRL_HEADER_SIZE - FILE_TRANSFER_MAX_PATH];
};

static_assert(sizeof(FileTransferControlBlock) == FILE_TRANSFER_CTRL_SIZE,
              "Control block must be exactly 4KB");

struct FileTransferStats {
    uint64_t bytes_transferred;
    uint32_t chunks_sent;
    uint32_t retries;
    bool success;
    FileTransferStatus last_error;
};

using FileTransferProgressFn = std::function<void(uint64_t transferred, uint64_t total)>;

class FileTransferEngine {
public:
    FileTransferEngine() = default;
    ~FileTransferEngine() = default;

    FileTransferEngine(const FileTransferEngine&) = delete;
    FileTransferEngine& operator=(const FileTransferEngine&) = delete;

    VmResult initialize(uint64_t gpa, uint8_t* hva, size_t buffer_size);
    void shutdown();

    VmResult push_file(const char* host_path, const char* guest_path,
                       FileTransferProgressFn progress = nullptr);
    VmResult pull_file(const char* guest_path, const char* host_path,
                       FileTransferProgressFn progress = nullptr);

    VmResult handle_guest_signal();
    const FileTransferStats& last_stats() const { return stats_; }

    void set_io_write_callback(std::function<void(uint16_t port, uint8_t value)> cb);

    static uint32_t compute_checksum(const uint8_t* data, size_t size);
    bool validate_path(const char* path) const;

private:
    VmResult wait_for_ack(uint32_t expected_chunk_id);
    VmResult send_ack(uint32_t chunk_id, FileTransferStatus status);
    VmResult send_error(FileTransferStatus error);

    FileTransferControlBlock* ctrl_{nullptr};
    uint8_t* data_buffer_{nullptr};
    uint64_t gpa_base_{0};
    size_t buffer_size_{0};
    FileTransferStats stats_{};
    bool initialized_{false};
    std::function<void(uint16_t port, uint8_t value)> io_write_cb_;
};

} // namespace domvirt::bindlet_bridge
