#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-Winsize.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(TTYTK_PLATFORM_MACOS)
#include <crt_externs.h>
#endif

namespace ttytk {

/** Own a PTY master descriptor and track its spawned child. */
class PTY {
public:
    PTY() = default;
    PTY(const PTY&) = delete;
    auto operator=(const PTY&) -> PTY& = delete;

    PTY(PTY&& other) noexcept : master_(std::exchange(other.master_, -1)),
                                child_(std::exchange(other.child_, -1)) {}

    auto operator=(PTY&& other) noexcept -> PTY& {
        if (this != &other) {
            close_master();
            master_ = std::exchange(other.master_, -1);
            child_ = std::exchange(other.child_, -1);
        }
        return *this;
    }

    inline ~PTY() {
        close_master();
    }

    /**
     * Spawn the program in its own session with the slave as controlling tty.
     * Reports terminal setup/exec errors before returning the master.
     * The caller is responsible for reaping the child with wait() or waitpid().
     */
    [[nodiscard]] static inline auto fork(const std::string& program,
                                          const std::vector<std::string>& arguments)
        -> Result<PTY> {
        if (program.empty() || program.find('\0') != std::string::npos) {
            return Error{EINVAL, "invalid PTY program name"};
        }
        std::vector<char*> argv{};
        argv.reserve(arguments.size() + 2);
        argv.push_back(const_cast<char*>(program.c_str()));
        for (const auto& argument : arguments) {
            if (argument.find('\0') != std::string::npos) {
                return Error{EINVAL, "PTY arguments cannot contain NUL"};
            }
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);

        // Prepare PATH candidates before fork so the child need not allocate or
        // run the libc PATH search after inheriting a multithreaded process.
        std::vector<std::string> paths{};
        if (program.find('/') != std::string::npos) {
            paths.push_back(program);
        } else {
            const char* const path = std::getenv("PATH");
            std::string_view remaining{path ? path : "/usr/bin:/bin"};
            for (;;) {
                const auto delimiter = remaining.find(':');
                const auto dir = remaining.substr(0, delimiter);
                paths.push_back(dir.empty() ? program : std::string{dir} + '/' + program);
                if (delimiter == std::string_view::npos) break;
                remaining.remove_prefix(delimiter + 1);
            }
        }
        std::vector<char*> shell_argv{};
        shell_argv.reserve(argv.size() + 1);
        char shell[] = "/bin/sh";
        shell_argv.push_back(shell);
        shell_argv.insert(shell_argv.end(), argv.begin(), argv.end());
#if defined(TTYTK_PLATFORM_MACOS)
        auto* const environment = *_NSGetEnviron();
#else
        auto* const environment = ::environ;
#endif

        const int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (master < 0) return Error{errno, std::strerror(errno)};
        if (::grantpt(master) != 0 || ::unlockpt(master) != 0) {
            const int error_number = errno;
            static_cast<void>(::close(master));
            return Error{error_number, std::strerror(error_number)};
        }

        char slave_name[256]{};
#if defined(TTYTK_PLATFORM_LINUX)
        const int name_error = ::ptsname_r(master, slave_name, sizeof(slave_name));
        if (name_error != 0) {
            static_cast<void>(::close(master));
            return Error{name_error, std::strerror(name_error)};
        }
#elif defined(TTYTK_PLATFORM_MACOS)
        {
            // ptsname() exposes static storage on this platform.
            static std::mutex name_mutex{};
            std::lock_guard lock{name_mutex};
            const char* const name = ::ptsname(master);
            if (name == nullptr || std::strlen(name) >= sizeof(slave_name)) {
                const int error_number = name == nullptr ? errno : ENAMETOOLONG;
                static_cast<void>(::close(master));
                return Error{error_number, std::strerror(error_number)};
            }
            std::strcpy(slave_name, name);
        }
#endif

        int errors[2]{};
        const int pipe_error = make_error_pipe(errors);
        if (pipe_error != 0) {
            ::close(master);
            return Error{pipe_error, std::strerror(pipe_error)};
        }
        const pid_t child = ::fork();
        if (child < 0) {
            const int error_number = errno;
            ::close(master);
            ::close(errors[0]);
            ::close(errors[1]);
            return Error{error_number, std::strerror(error_number)};
        }
        if (child == 0) {
            ::close(errors[0]);
            ::close(master);
            if (::setsid() < 0) fail_child(errors[1], errno);
            const int slave = ::open(slave_name, O_RDWR | O_NOCTTY);
            if (slave < 0) fail_child(errors[1], errno);
            if (::ioctl(slave, TIOCSCTTY, 0) < 0) fail_child(errors[1], errno);
            for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; ++fd) {
                if (::dup2(slave, fd) < 0) fail_child(errors[1], errno);
            }
            if (slave > STDERR_FILENO) ::close(slave);
            int exec_error = ENOENT;
            for (auto& path : paths) {
                ::execve(path.c_str(), argv.data(), environment);
                if (errno == ENOEXEC) {
                    shell_argv[1] = path.data();
                    ::execve(shell, shell_argv.data(), environment);
                    fail_child(errors[1], errno);
                }
                if (errno == EACCES) exec_error = EACCES;
                else if (errno != ENOENT && errno != ENOTDIR) {
                    fail_child(errors[1], errno);
                }
            }
            fail_child(errors[1], exec_error);
        }
        ::close(errors[1]);
        int child_error{};
        ssize_t count;
        do { count = ::read(errors[0], &child_error, sizeof(child_error)); }
        while (count < 0 && errno == EINTR);
        const int read_error = errno;
        ::close(errors[0]);
        if (count != 0) {
            ::close(master);
            if (count < 0) child_error = read_error;
            else if (count != static_cast<ssize_t>(sizeof(child_error))) child_error = EIO;
            // On a child setup error the child exits immediately.
            if (count > 0) {
                while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
            }
            return Error{child_error, std::strerror(child_error)};
        }
        return PTY{master, child};
    }

    [[nodiscard]] inline auto master_fd() const noexcept -> int {
        return master_;
    }

    /** Return the tracked child PID, or -1 when none remains to wait for. */
    [[nodiscard]] inline auto child_pid() const noexcept -> pid_t {
        return child_;
    }

    /** Reap the child, returning its native wait status (empty if still running). */
    [[nodiscard]] inline auto wait(const bool nonblocking = false)
        -> Result<std::optional<int>> {
        if (child_ < 0) return Error{ECHILD, "PTY has no tracked child"};
        int status{};
        pid_t result;
        do { result = ::waitpid(child_, &status, nonblocking ? WNOHANG : 0); }
        while (result < 0 && errno == EINTR);
        if (result < 0) {
            const int error = errno;
            if (error == ECHILD) child_ = -1;
            return Error{error, std::strerror(error)};
        }
        if (result == 0) return std::optional<int>{};
        child_ = -1;
        return std::optional<int>{status};
    }

    [[nodiscard]] inline auto resize(const winsize::Size size) -> Result<void> {
        if (master_ < 0) return Error{EBADF, "PTY is not open"};
        return winsize::set(master_, size);
    }

    [[nodiscard]] inline auto write(const std::string_view bytes)
        -> Result<std::size_t> {
        if (master_ < 0) return Error{EBADF, "PTY is not open"};
        ssize_t count;
        do { count = ::write(master_, bytes.data(), bytes.size()); }
        while (count < 0 && errno == EINTR);
        if (count < 0) return Error{errno, std::strerror(errno)};
        return static_cast<std::size_t>(count);
    }

    [[nodiscard]] inline auto read(char* buffer, const std::size_t size)
        -> Result<std::size_t> {
        if (master_ < 0) return Error{EBADF, "PTY is not open"};
        ssize_t count;
        do { count = ::read(master_, buffer, size); }
        while (count < 0 && errno == EINTR);
#if defined(TTYTK_PLATFORM_LINUX)
        // Linux reports a closed slave as EIO instead of EOF at the master.
        if (count < 0 && errno == EIO) return std::size_t{0};
#endif
        if (count < 0) return Error{errno, std::strerror(errno)};
        return static_cast<std::size_t>(count);
    }

private:
    explicit PTY(const int master, const pid_t child) : master_(master), child_(child) {}

    static inline auto make_error_pipe(int (&fds)[2]) -> int {
#if defined(TTYTK_PLATFORM_LINUX)
        if (::pipe2(fds, O_CLOEXEC) < 0) return errno;
#else
        if (::pipe(fds) < 0) return errno;
#endif
        for (auto& fd : fds) {
            if (fd <= STDERR_FILENO) {
                const int replacement = ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
                if (replacement >= 0) {
                    ::close(fd);
                    fd = replacement;
                    continue;
                }
            } else if (::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0) {
                continue;
            }
            const int error = errno;
            ::close(fds[0]);
            ::close(fds[1]);
            return error;
        }
        return 0;
    }

    [[noreturn]] static inline void fail_child(const int fd, const int error) noexcept {
        while (::write(fd, &error, sizeof(error)) < 0 && errno == EINTR) {}
        ::_exit(127);
    }

    inline void close_master() noexcept {
        if (master_ >= 0) static_cast<void>(::close(master_));
        master_ = -1;
    }

    int master_{-1};
    pid_t child_{-1};
};

// NOTE(agent): Several TTyUtils manifests use the corrected spelling
// PseudoTerm, while the manifest-frozen header is intentionally PsuedoTerm.
using PseudoTerm = PTY;

}  // namespace ttytk
