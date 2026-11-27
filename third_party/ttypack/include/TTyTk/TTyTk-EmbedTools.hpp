#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-PsuedoTerm.hpp>

#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>
#include <unistd.h>

namespace ttytk::embed {

/** Forward one input chunk, returning zero at EOF. Call only when ready for I/O. */
[[nodiscard]] inline auto passthrough(const int input_fd, const int output_fd) -> Result<std::size_t> {
    char buffer[4096];
    ssize_t read_count;
    do {
        read_count = ::read(input_fd, buffer, sizeof(buffer));
    } while (read_count < 0 && errno == EINTR);
    if (read_count < 0) return Error{errno, std::strerror(errno)};
    std::size_t written = 0;
    while (written < static_cast<std::size_t>(read_count)) {
        const auto count = ::write(output_fd, buffer + written,
                                   static_cast<std::size_t>(read_count) - written);
        if (count < 0) {
            if (errno == EINTR) continue;
            return Error{errno, std::strerror(errno)};
        }
        if (count == 0) return Error{EIO, "output made no progress"};
        written += static_cast<std::size_t>(count);
    }
    return written;
}

/** Wrap trusted shell syntax in an exec command, replacing the current shell. */
[[nodiscard]] inline auto wrap_shell(const std::string_view command) -> std::string {
    return "exec " + std::string{command};
}

/** Quote an environment value for POSIX shell syntax; return empty for an invalid name or NUL value. */
[[nodiscard]] inline auto inject_env(const std::string_view name, const std::string_view value)
    -> std::string {
    constexpr std::string_view letters{"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_"};
    if (name.empty() || letters.find(name.front()) == std::string_view::npos ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789") !=
            std::string_view::npos ||
        value.find('\0') != std::string_view::npos) return {};
    auto command = "export " + std::string{name} + "='";
    for (const auto byte : value) {
        if (byte == '\'') command += "'\\''";
        else command += byte;
    }
    command += '\'';
    return command;
}

}  // namespace ttytk::embed
