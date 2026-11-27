#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-Automaton.hpp>

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <locale>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttytk {

class Recorder {
public:
    /** Starts a new recording with the specified initial terminal dimensions. */
    inline void start(const unsigned columns = 80, const unsigned rows = 24) {
        events_.clear();
        started_ = std::chrono::steady_clock::now();
        columns_ = columns;
        rows_ = rows;
        recording_ = true;
    }

    inline void stop() noexcept { recording_ = false; }

    inline void record_output(const std::string_view bytes) { record('o', bytes); }

    /** Records input explicitly; callers should omit sensitive input. */
    inline void record_input(const std::string_view bytes) { record('i', bytes); }

    [[nodiscard]] inline auto save_asciicast(const std::string& path) const -> Result<void> {
        std::ofstream file{path};
        if (!file) return Error{EIO, "unable to open asciicast output"};
        file.imbue(std::locale::classic());
        file << "{\"version\":2,\"width\":" << columns_ << ",\"height\":" << rows_ << "}\n";
        for (const auto& [time, type, bytes] : events_) {
            file << '[' << time / 1000000 << '.' << std::setw(6) << std::setfill('0')
                 << time % 1000000 << ",\"" << type << "\",";
            write_json(file, bytes);
            file << "]\n";
        }
        file.close();
        if (!file) return Error{EIO, "unable to write asciicast output"};
        return {};
    }

    /** Saves raw events. TTYPACK1 is followed by little-endian u64 columns and rows,
     *  then records of one type byte, u64 elapsed microseconds, u64 length and raw bytes.
     */
    [[nodiscard]] inline auto save_binary(const std::string& path) const -> Result<void> {
        std::ofstream file{path, std::ios::binary};
        if (!file) return Error{EIO, "unable to open recording output"};
        file << "TTYPACK1";
        write_u64(file, columns_);
        write_u64(file, rows_);
        for (const auto& [time, type, bytes] : events_) {
            file.put(type);
            write_u64(file, time);
            write_u64(file, bytes.size());
            file << bytes;
        }
        file.close();
        if (!file) return Error{EIO, "unable to write recording output"};
        return {};
    }

private:
    struct Recorded {
        std::uint64_t time;
        char type;
        std::string bytes;
    };

    inline void record(const char type, const std::string_view bytes) {
        if (!recording_ || bytes.empty()) return;
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started_).count();
        events_.push_back({static_cast<std::uint64_t>(elapsed), type, std::string{bytes}});
    }

    static inline void write_u64(std::ostream& file, std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i, value >>= 8) file.put(static_cast<char>(value & 255U));
    }

    static inline void write_json(std::ofstream& file, const std::string_view value) {
        constexpr std::string_view hex{"0123456789abcdef"};
        file.put('"');
        for (std::size_t i = 0; i < value.size();) {
            const auto c = static_cast<unsigned char>(value[i++]);
            if (c == '"' || c == '\\') file.put('\\').put(static_cast<char>(c));
            else if (c < 0x20) file << "\\u00" << hex[c >> 4] << hex[c & 15];
            else if (c < 0x80) file.put(static_cast<char>(c));
            else {
                const std::size_t extra = c >= 0xC2 && c <= 0xDF ? 1 :
                    c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : 0;
                bool valid = extra != 0 && extra <= value.size() - i;
                for (std::size_t j = 0; valid && j < extra; ++j) {
                    const auto next = static_cast<unsigned char>(value[i + j]);
                    valid = next >= 0x80 && next <= 0xBF &&
                            (j != 0 || !((c == 0xE0 && next < 0xA0) ||
                              (c == 0xED && next >= 0xA0) || (c == 0xF0 && next < 0x90) ||
                              (c == 0xF4 && next >= 0x90)));
                }
                if (valid) {
                    file << value.substr(i - 1, extra + 1);
                    i += extra;
                } else {
                    file << "\\ufffd";
                }
            }
        }
        file.put('"');
    }

    bool recording_{};
    unsigned columns_{80}, rows_{24};
    std::chrono::steady_clock::time_point started_{};
    std::vector<Recorded> events_{};
};

}  // namespace ttytk
