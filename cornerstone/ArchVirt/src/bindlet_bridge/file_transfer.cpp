#include "file_transfer.hpp"
#include <cstring>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <thread>
#include <algorithm>

namespace domvirt::bindlet_bridge {

namespace fs = std::filesystem;

uint32_t FileTransferEngine::compute_checksum(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

bool FileTransferEngine::validate_path(const char* path) const {
    if (!path || path[0] == '\0') return false;
    size_t len = std::strlen(path);
    if (len >= FILE_TRANSFER_MAX_PATH) return false;
    if (std::strstr(path, "..") != nullptr) return false;
    if (path[0] == '~') return false;
    return true;
}

VmResult FileTransferEngine::initialize(uint64_t gpa, uint8_t* hva, size_t buffer_size) {
    if (initialized_) {
        return {VmErrorCode::SUCCESS, "Already initialized"};
    }
    if (!hva || buffer_size < FILE_TRANSFER_CTRL_SIZE + FILE_TRANSFER_CHUNK_SIZE) {
        return {VmErrorCode::MEMORY_ALLOC_FAILED,
                "Buffer too small: need at least 8KB (4KB ctrl + 4KB data)"};
    }
    gpa_base_ = gpa;
    ctrl_ = reinterpret_cast<FileTransferControlBlock*>(hva);
    data_buffer_ = hva + FILE_TRANSFER_CTRL_SIZE;
    buffer_size_ = buffer_size;
    std::memset(ctrl_, 0, sizeof(FileTransferControlBlock));
    stats_ = {};
    initialized_ = true;
    return {VmErrorCode::SUCCESS, "File transfer engine initialized"};
}

void FileTransferEngine::shutdown() {
    initialized_ = false;
    ctrl_ = nullptr;
    data_buffer_ = nullptr;
}

void FileTransferEngine::set_io_write_callback(std::function<void(uint16_t, uint8_t)> cb) {
    io_write_cb_ = std::move(cb);
}

VmResult FileTransferEngine::send_ack(uint32_t chunk_id, FileTransferStatus status) {
    ctrl_->command = static_cast<uint32_t>(FileTransferCommand::FILE_ACK);
    ctrl_->status = static_cast<uint32_t>(status);
    ctrl_->chunk_id = chunk_id;
    if (io_write_cb_) {
        io_write_cb_(FILE_TRANSFER_DOORBELL_PORT, 1);
    }
    return {VmErrorCode::SUCCESS, ""};
}

VmResult FileTransferEngine::send_error(FileTransferStatus error) {
    ctrl_->command = static_cast<uint32_t>(FileTransferCommand::FILE_ERROR);
    ctrl_->status = static_cast<uint32_t>(error);
    if (io_write_cb_) {
        io_write_cb_(FILE_TRANSFER_DOORBELL_PORT, 1);
    }
    return {VmErrorCode::BRIDGE_ERROR, "File transfer error signaled"};
}

VmResult FileTransferEngine::wait_for_ack(uint32_t expected_chunk_id) {
    for (uint32_t retry = 0; retry < FILE_TRANSFER_RETRY_MAX; ++retry) {
        auto start = std::chrono::steady_clock::now();
        while (true) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                > FILE_TRANSFER_TIMEOUT_MS) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            uint32_t cmd = ctrl_->command;
            if (cmd == static_cast<uint32_t>(FileTransferCommand::FILE_ACK)) {
                if (ctrl_->chunk_id == expected_chunk_id) {
                    auto st = static_cast<FileTransferStatus>(ctrl_->status);
                    if (st == FileTransferStatus::SUCCESS) {
                        return {VmErrorCode::SUCCESS, ""};
                    }
                    return {VmErrorCode::BRIDGE_ERROR, "Guest reported error"};
                }
            } else if (cmd == static_cast<uint32_t>(FileTransferCommand::FILE_ERROR)) {
                return {VmErrorCode::BRIDGE_ERROR, "Guest reported error"};
            }
        }
        stats_.retries++;
    }
    return {VmErrorCode::TIMEOUT, "Timeout waiting for guest acknowledgment"};
}

VmResult FileTransferEngine::push_file(const char* host_path, const char* guest_path,
                                        FileTransferProgressFn progress) {
    if (!initialized_) {
        return {VmErrorCode::BRIDGE_ERROR, "Engine not initialized"};
    }
    if (!validate_path(host_path) || !validate_path(guest_path)) {
        return {VmErrorCode::BRIDGE_ERROR, "Invalid path (contains .. or empty)"};
    }

    std::error_code ec;
    if (!fs::exists(host_path, ec) || !fs::is_regular_file(host_path, ec)) {
        return {VmErrorCode::BOOT_LOAD_FAILED, "Host file not found: " + std::string(host_path)};
    }

    auto file_size = fs::file_size(host_path, ec);
    if (ec) {
        return {VmErrorCode::BOOT_LOAD_FAILED, "Cannot read host file size"};
    }

    std::ifstream infile(host_path, std::ios::binary);
    if (!infile) {
        return {VmErrorCode::BOOT_LOAD_FAILED, "Cannot open host file for reading"};
    }

    size_t guest_path_len = std::strlen(guest_path);
    ctrl_->command = static_cast<uint32_t>(FileTransferCommand::FILE_WRITE_BEGIN);
    ctrl_->file_size = file_size;
    ctrl_->path_length = static_cast<uint32_t>(guest_path_len);
    std::memcpy(ctrl_->path, guest_path, guest_path_len);
    ctrl_->path[guest_path_len] = '\0';
    if (io_write_cb_) {
        io_write_cb_(FILE_TRANSFER_DOORBELL_PORT, 1);
    }

    auto ack_result = wait_for_ack(0);
    if (!ack_result) {
        stats_.success = false;
        stats_.last_error = FileTransferStatus::ERROR_TIMEOUT;
        return ack_result;
    }

    std::vector<uint8_t> file_data(file_size);
    infile.read(reinterpret_cast<char*>(file_data.data()), file_size);
    infile.close();

    uint32_t full_checksum = compute_checksum(file_data.data(), file_size);
    uint32_t total_chunks = (file_size + FILE_TRANSFER_CHUNK_SIZE - 1) / FILE_TRANSFER_CHUNK_SIZE;

    stats_.bytes_transferred = 0;
    stats_.chunks_sent = 0;

    for (uint32_t chunk_id = 0; chunk_id < total_chunks; ++chunk_id) {
        size_t offset = static_cast<size_t>(chunk_id) * FILE_TRANSFER_CHUNK_SIZE;
        size_t chunk_size = std::min(FILE_TRANSFER_CHUNK_SIZE, file_size - offset);
        uint32_t chunk_checksum = compute_checksum(file_data.data() + offset, chunk_size);

        std::memcpy(data_buffer_, file_data.data() + offset, chunk_size);

        ctrl_->command = static_cast<uint32_t>(FileTransferCommand::FILE_WRITE_CHUNK);
        ctrl_->chunk_id = chunk_id;
        ctrl_->chunk_size = static_cast<uint32_t>(chunk_size);
        ctrl_->checksum = chunk_checksum;
        if (io_write_cb_) {
            io_write_cb_(FILE_TRANSFER_DOORBELL_PORT, 1);
        }

        ack_result = wait_for_ack(chunk_id);
        if (!ack_result) {
            stats_.success = false;
            stats_.last_error = FileTransferStatus::ERROR_TIMEOUT;
            return ack_result;
        }

        stats_.bytes_transferred += chunk_size;
        stats_.chunks_sent++;
        if (progress) {
            progress(stats_.bytes_transferred, file_size);
        }
    }

    ctrl_->command = static_cast<uint32_t>(FileTransferCommand::FILE_WRITE_END);
    ctrl_->file_size = file_size;
    ctrl_->checksum = full_checksum;
    if (io_write_cb_) {
        io_write_cb_(FILE_TRANSFER_DOORBELL_PORT, 1);
    }

    ack_result = wait_for_ack(total_chunks);
    if (!ack_result) {
        stats_.success = false;
        stats_.last_error = FileTransferStatus::ERROR_TIMEOUT;
        return ack_result;
    }

    stats_.success = true;
    stats_.last_error = FileTransferStatus::SUCCESS;
    return {VmErrorCode::SUCCESS, "File pushed successfully"};
}

VmResult FileTransferEngine::pull_file(const char* guest_path, const char* host_path,
                                        FileTransferProgressFn progress) {
    if (!initialized_) {
        return {VmErrorCode::BRIDGE_ERROR, "Engine not initialized"};
    }
    if (!validate_path(host_path) || !validate_path(guest_path)) {
        return {VmErrorCode::BRIDGE_ERROR, "Invalid path (contains .. or empty)"};
    }

    size_t guest_path_len = std::strlen(guest_path);
    ctrl_->command = static_cast<uint32_t>(FileTransferCommand::FILE_READ_REQUEST);
    ctrl_->path_length = static_cast<uint32_t>(guest_path_len);
    std::memcpy(ctrl_->path, guest_path, guest_path_len);
    ctrl_->path[guest_path_len] = '\0';
    if (io_write_cb_) {
        io_write_cb_(FILE_TRANSFER_DOORBELL_PORT, 1);
    }

    auto ack_result = wait_for_ack(0);
    if (!ack_result) {
        stats_.success = false;
        stats_.last_error = FileTransferStatus::ERROR_TIMEOUT;
        return ack_result;
    }

    if (ctrl_->command != static_cast<uint32_t>(FileTransferCommand::FILE_READ_RESPONSE)) {
        stats_.success = false;
        stats_.last_error = FileTransferStatus::ERROR_INTERNAL;
        return {VmErrorCode::BRIDGE_ERROR, "Guest did not respond with FILE_READ_RESPONSE"};
    }

    auto guest_status = static_cast<FileTransferStatus>(ctrl_->status);
    if (guest_status != FileTransferStatus::SUCCESS) {
        stats_.success = false;
        stats_.last_error = guest_status;
        return {VmErrorCode::BRIDGE_ERROR, "Guest reported error reading file"};
    }

    uint64_t file_size = ctrl_->file_size;
    uint32_t full_checksum = ctrl_->checksum;
    uint32_t total_chunks = (file_size + FILE_TRANSFER_CHUNK_SIZE - 1) / FILE_TRANSFER_CHUNK_SIZE;

    std::error_code ec;
    auto parent = fs::path(host_path).parent_path();
    if (!parent.empty() && !fs::exists(parent, ec)) {
        fs::create_directories(parent, ec);
    }

    std::ofstream outfile(host_path, std::ios::binary | std::ios::trunc);
    if (!outfile) {
        send_ack(0, FileTransferStatus::ERROR_PERMISSION_DENIED);
        return {VmErrorCode::BRIDGE_ERROR, "Cannot open host file for writing"};
    }

    std::vector<uint8_t> file_data;
    file_data.reserve(file_size);
    stats_.bytes_transferred = 0;
    stats_.chunks_sent = 0;

    for (uint32_t chunk_id = 0; chunk_id < total_chunks; ++chunk_id) {
        send_ack(chunk_id, FileTransferStatus::SUCCESS);

        ack_result = wait_for_ack(chunk_id + 0x80000000u);
        if (!ack_result) {
            stats_.success = false;
            stats_.last_error = FileTransferStatus::ERROR_TIMEOUT;
            return ack_result;
        }

        if (ctrl_->command != static_cast<uint32_t>(FileTransferCommand::FILE_READ_RESPONSE)) {
            break;
        }

        size_t chunk_size = ctrl_->chunk_size;
        uint32_t chunk_checksum = compute_checksum(data_buffer_, chunk_size);
        if (chunk_checksum != ctrl_->checksum) {
            send_ack(chunk_id, FileTransferStatus::ERROR_CHECKSUM_MISMATCH);
            stats_.success = false;
            stats_.last_error = FileTransferStatus::ERROR_CHECKSUM_MISMATCH;
            return {VmErrorCode::BRIDGE_ERROR, "Chunk checksum mismatch"};
        }

        file_data.insert(file_data.end(), data_buffer_, data_buffer_ + chunk_size);
        stats_.bytes_transferred += chunk_size;
        stats_.chunks_sent++;
        if (progress) {
            progress(stats_.bytes_transferred, file_size);
        }
    }

    uint32_t computed_full = compute_checksum(file_data.data(), file_data.size());
    if (computed_full != full_checksum) {
        stats_.success = false;
        stats_.last_error = FileTransferStatus::ERROR_CHECKSUM_MISMATCH;
        return {VmErrorCode::BRIDGE_ERROR, "Full file checksum mismatch"};
    }

    outfile.write(reinterpret_cast<const char*>(file_data.data()), file_data.size());
    outfile.close();

    send_ack(total_chunks, FileTransferStatus::SUCCESS);

    stats_.success = true;
    stats_.last_error = FileTransferStatus::SUCCESS;
    return {VmErrorCode::SUCCESS, "File pulled successfully"};
}

VmResult FileTransferEngine::handle_guest_signal() {
    if (!initialized_) return {VmErrorCode::SUCCESS, ""};
    return {VmErrorCode::SUCCESS, ""};
}

} // namespace domvirt::bindlet_bridge
