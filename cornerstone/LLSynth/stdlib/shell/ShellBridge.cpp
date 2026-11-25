#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <crt_externs.h>
#else
extern char** environ;
#endif

namespace {

constexpr std::size_t limit = 8 * 1024 * 1024;
constexpr char bridge_failure[] = "-1\n0\nnative shell bridge failed";

struct File {
    int fd = -1;

    File() {
        if (auto* stream = std::tmpfile()) {
            // Reserve only fds >= 3, including if the host closed a stdio fd.
            fd = fcntl(fileno(stream), F_DUPFD_CLOEXEC, 3);
            std::fclose(stream);
        }
    }
    ~File() { if (fd >= 0) close(fd); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
};

struct SpawnActions {
    posix_spawn_file_actions_t value;
    int error = posix_spawn_file_actions_init(&value);
    ~SpawnActions() { if (!error) posix_spawn_file_actions_destroy(&value); }
};

std::string packet(int status, std::string_view out, std::string_view err) {
    // Private, length-delimited UTF-8 (sizes are bytes, not code points).
    std::string result = std::to_string(status) + '\n' + std::to_string(out.size()) + '\n';
    result.append(out);
    result.append(err);
    return result;
}

std::string failure(std::string_view message) { return packet(-1, {}, message); }

bool write_script(int fd, std::string_view script) {
    while (!script.empty()) {
        const auto n = write(fd, script.data(), script.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        script.remove_prefix(static_cast<std::size_t>(n));
    }
    return lseek(fd, 0, SEEK_SET) == 0;
}

bool read_capture(int fd, std::string& result) {
    char buffer[8192];
    while (true) {
        // pread leaves the child's write offset alone, including if a
        // background command still has the file open after libsh exits.
        const auto n = pread(fd, buffer, sizeof(buffer), static_cast<off_t>(result.size()));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return false;
        if (n == 0) return true;
        if (static_cast<std::size_t>(n) > limit - result.size()) return false;
        result.append(buffer, static_cast<std::size_t>(n));
    }
}

bool valid_text(std::string_view text) {
    // The existing String ABI cannot carry embedded NULs or non-UTF-8 bytes.
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first == 0) return false;
        if (first < 0x80) continue;
        const int count = first >= 0xc2 && first <= 0xdf ? 1 :
                          first >= 0xe0 && first <= 0xef ? 2 :
                          first >= 0xf0 && first <= 0xf4 ? 3 : 0;
        if (!count || static_cast<std::size_t>(count) > text.size() - i) return false;
        const auto second = static_cast<unsigned char>(text[i]);
        if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second > 0x9f) ||
            (first == 0xf0 && second < 0x90) || (first == 0xf4 && second > 0x8f)) return false;
        for (int j = 0; j < count; ++j)
            if ((static_cast<unsigned char>(text[i++]) & 0xc0) != 0x80) return false;
    }
    return true;
}

std::string run(std::string_view source) {
    if (source.size() > limit) return failure("script exceeds 8 MiB");
    if (!valid_text(source)) return failure("script is not NUL-free UTF-8");
    File script, out, err;
    if (script.fd < 0 || out.fd < 0 || err.fd < 0)
        return failure("cannot create temporary shell streams");
    if (!write_script(script.fd, source)) return failure("cannot write shell script");

    SpawnActions actions;
    if (actions.error || posix_spawn_file_actions_adddup2(&actions.value, script.fd, STDIN_FILENO) ||
        posix_spawn_file_actions_adddup2(&actions.value, out.fd, STDOUT_FILENO) ||
        posix_spawn_file_actions_adddup2(&actions.value, err.fd, STDERR_FILENO))
        return failure("cannot configure child shell streams");

    // No host shell or interpolation: libsh consumes the exact script on fd 0.
    const char* executable = std::getenv("LLSYNTH_LIBSH");
    if (!executable || !*executable) executable = "libsh";
    char stdin_option[] = "-s";
    char* argv[] = {const_cast<char*>(executable), stdin_option, nullptr};
#if defined(__APPLE__)
    char** environment = *_NSGetEnviron();
#else
    char** environment = environ;
#endif
    pid_t child;
    const int code = posix_spawnp(&child, executable, &actions.value, nullptr, argv, environment);
    if (code) return failure("cannot start libsh (error " + std::to_string(code) + ')');

    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return failure("cannot wait for libsh");
    }
    std::string stdout_text, stderr_text;
    if (!read_capture(out.fd, stdout_text) || !read_capture(err.fd, stderr_text))
        return failure("cannot read shell output, or a stream exceeds 8 MiB");
    if (!valid_text(stdout_text) || !valid_text(stderr_text))
        return failure("shell output is not NUL-free UTF-8");
    const int result = WIFEXITED(status) ? WEXITSTATUS(status) :
                       WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 125;
    return packet(result, stdout_text, stderr_text);
}

struct PacketView {
    int status = -1;
    std::string_view out;
    std::string_view err = "invalid shell result packet";
};

template <class Integer>
bool take_number(std::string_view& input, Integer& value) {
    auto end = input.find('\n');
    if (end == std::string_view::npos) return false;
    const auto parsed = std::from_chars(input.data(), input.data() + end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + end) return false;
    input.remove_prefix(end + 1);
    return true;
}

PacketView unpack(std::string_view input) {
    int status;
    std::size_t size;
    if (!take_number(input, status) || !take_number(input, size) || size > input.size())
        return {};
    return {status, input.substr(0, size), input.substr(size)};
}

// The native interop caller copies each returned string before its next call.
const char* copy_result(std::string_view text) {
    thread_local std::string result;
    if (text.empty()) result.clear();
    else result.assign(text);
    return result.c_str();
}

} // namespace

extern "C" {

__attribute__((visibility("default")))
const char* llsynth_shell_run_v1(const char* source) noexcept {
    try {
        return source ? copy_result(run(source)) : bridge_failure;
    } catch (...) {
        return bridge_failure;
    }
}

__attribute__((visibility("default")))
std::int64_t llsynth_shell_status_v1(const char* input) noexcept {
    return unpack(input ? input : "").status;
}

__attribute__((visibility("default")))
const char* llsynth_shell_stdout_v1(const char* input) noexcept {
    try {
        return copy_result(unpack(input ? input : "").out);
    } catch (...) {
        return nullptr;
    }
}

__attribute__((visibility("default")))
const char* llsynth_shell_stderr_v1(const char* input) noexcept {
    try {
        return copy_result(unpack(input ? input : "").err);
    } catch (...) {
        return nullptr;
    }
}

} // extern "C"
