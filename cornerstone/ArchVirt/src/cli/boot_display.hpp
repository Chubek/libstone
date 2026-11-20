#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace domvirt {

class BootDisplay {
public:
    explicit BootDisplay(bool headless = false);
    ~BootDisplay();

    BootDisplay(const BootDisplay&) = delete;
    BootDisplay& operator=(const BootDisplay&) = delete;

    void set_status(std::string status, float progress);
    void append_message(std::string message);
    void set_guest_framebuffer(const void* base,
                               unsigned width,
                               unsigned height,
                               unsigned pitch_bytes,
                               std::uint32_t format);
    void set_guest_text_buffer(const void* base,
                               unsigned columns,
                               unsigned rows,
                               unsigned pitch_bytes);
    bool try_read_input(std::uint8_t& value);
    void stop();

    [[nodiscard]] bool available() const noexcept { return available_.load(); }
    [[nodiscard]] bool closed() const noexcept { return closed_.load(); }

private:
    void run();
    void enqueue_input(std::uint8_t value);

    static constexpr unsigned WIDTH = 960;
    static constexpr unsigned HEIGHT = 540;

    std::thread thread_;
    std::mutex state_mutex_;
    std::string status_{"Booting ArchVirt"};
    float progress_{0.0f};
    std::vector<std::string> messages_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> closed_{false};
    std::atomic<bool> available_{false};
    std::mutex framebuffer_mutex_;
    const std::uint8_t* guest_framebuffer_{nullptr};
    std::vector<std::uint8_t> framebuffer_copy_;
    unsigned guest_width_{0};
    unsigned guest_height_{0};
    unsigned guest_pitch_{0};
    std::uint32_t guest_format_{0};
    const std::uint8_t* guest_text_buffer_{nullptr};
    std::vector<std::uint8_t> text_copy_;
    unsigned guest_text_columns_{0};
    unsigned guest_text_rows_{0};
    unsigned guest_text_pitch_{0};
    std::mutex input_mutex_;
    std::deque<std::uint8_t> input_queue_;
};

} // namespace domvirt
