#pragma once

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace am8 {

// A temporary sibling keeps the rename on the output filesystem. Close the
// writer before renaming; on any failure leave the old output in place.
inline void publish(const std::filesystem::path& output, std::string_view source) {
    const auto pattern = output.string() + ".XXXXXX";
    std::vector<char> temporary(pattern.begin(), pattern.end());
    temporary.push_back('\0');
    auto error = [&](int code, const char* operation) {
        return std::system_error(code, std::generic_category(), output.string() + ": " + operation);
    };
    const auto fd = ::mkstemp(temporary.data());
    if (fd < 0) throw error(errno, "create temporary output");
    struct Cleanup {
        const char* path;
        ~Cleanup() { if (path) ::unlink(path); }
    } cleanup{temporary.data()};
    auto* stream = ::fdopen(fd, "wb");
    if (!stream) {
        const auto code = errno;
        ::close(fd);
        throw error(code, "open temporary output");
    }
    const auto written = std::fwrite(source.data(), 1, source.size(), stream);
    const auto write_error = errno;
    const auto closed = std::fclose(stream);
    if (written != source.size()) throw error(write_error ? write_error : EIO, "write output");
    if (closed != 0) throw error(errno, "close output");
    if (std::rename(temporary.data(), output.c_str()) != 0) throw error(errno, "publish output");
    cleanup.path = nullptr;
}

} // namespace am8
