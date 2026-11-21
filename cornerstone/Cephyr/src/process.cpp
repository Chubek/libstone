#include <cephyr/diagnostics.hpp>
#include <cephyr/process.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>

extern char **environ;

namespace cephyr {
namespace {

class Pipe {
public:
    int fd[2]{-1, -1};
    ~Pipe() { for (std::size_t i = 0; i < 2; ++i) close_end(i); }
    void close_end(std::size_t end)
    {
        if (fd[end] >= 0) {
            close(fd[end]);
            fd[end] = -1;
        }
    }
    int create()
    {
        if (pipe2(fd, O_CLOEXEC) < 0)
            return errno;
        // File actions use descriptor 1 and 2 as destinations, even if the
        // caller had one of its standard descriptors closed.
        for (std::size_t i = 0; i < 2; ++i) {
            if (fd[i] <= STDERR_FILENO) {
                const int replacement = fcntl(fd[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
                if (replacement < 0)
                    return errno;
                close_end(i);
                fd[i] = replacement;
            }
        }
        return 0;
    }
};

std::string error_message(int err)
{
    return std::generic_category().message(err);
}

} // namespace

bool executable_available(std::string_view command)
{
    if (command.empty())
        return false;
    const auto executable = [](const std::filesystem::path &path) {
        std::error_code ec;
        return std::filesystem::is_regular_file(path, ec) && access(path.c_str(), X_OK) == 0;
    };
    const std::filesystem::path name(command);
    if (name.has_parent_path())
        return executable(name);
    const char *env = std::getenv("PATH");
    std::string_view path = env ? env : "/bin:/usr/bin";
    for (;;) {
        const auto split = path.find(':');
        if (executable(std::filesystem::path(path.substr(0, split)) / name))
            return true;
        if (split == std::string_view::npos)
            return false;
        path.remove_prefix(split + 1);
    }
}

int run_process(const std::vector<std::string> &args, Diagnostics &diag,
                 const std::filesystem::path &cwd, std::span<const int> descriptors, int stdout_fd)
{
    if (args.empty() || args.front().empty()) {
        diag.error("cephyr.tool", "no executable configured");
        return 127;
    }
    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (const auto &arg : args) {
        if (arg.find('\0') != std::string::npos) {
            diag.error("cephyr.tool", "NUL byte in tool argument");
            return 127;
        }
        argv.push_back(const_cast<char *>(arg.c_str()));
    }
    argv.push_back(nullptr);
    Pipe pipe;
    posix_spawn_file_actions_t actions;
    int err = pipe.create();
    if (!err) err = posix_spawn_file_actions_init(&actions);
    if (err) {
        diag.error("cephyr.tool", "cannot prepare tool execution: " + error_message(err));
        return 127;
    }
    if (!cwd.empty())
        err = posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());
    if (!err) err = posix_spawn_file_actions_adddup2(&actions, stdout_fd < 0 ? pipe.fd[1] : stdout_fd,
                                                   STDOUT_FILENO);
    if (!err) err = posix_spawn_file_actions_adddup2(&actions, pipe.fd[1], STDERR_FILENO);
    if (!err) err = posix_spawn_file_actions_addclose(&actions, pipe.fd[0]);
    if (!err) err = posix_spawn_file_actions_addclose(&actions, pipe.fd[1]);
    // POSIX specifies that this special case clears FD_CLOEXEC in the child.
    for (const int fd : descriptors) {
        if (!err)
            err = posix_spawn_file_actions_adddup2(&actions, fd, fd);
    }
    pid_t pid = -1;
    if (!err) err = posix_spawnp(&pid, argv.front(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    pipe.close_end(1);
    if (err) {
        diag.error("cephyr.tool", "cannot execute '" + args.front() + "': " + error_message(err));
        return 127;
    }

    std::string output;
    constexpr std::size_t cap = 32 * 1024;
    char buffer[4096];
    bool truncated = false;
    for (;;) {
        const auto bytes = read(pipe.fd[0], buffer, sizeof(buffer));
        if (!bytes)
            break;
        if (bytes < 0) {
            if (errno == EINTR)
                continue;
            diag.error("cephyr.tool", "cannot read tool diagnostics: " + error_message(errno));
            break;
        }
        const auto n = std::min(static_cast<std::size_t>(bytes), cap - output.size());
        output.append(buffer, n);
        truncated |= n < static_cast<std::size_t>(bytes);
    }
    pipe.close_end(0);
    const auto last = output.find_last_not_of(" \t\n\r");
    output.resize(last == std::string::npos ? 0 : last + 1);
    if (truncated)
        output += " [tool output truncated]";

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        diag.error("cephyr.tool", "cannot wait for '" + args.front() + "': " + error_message(errno));
        return 127;
    }
    if (WIFSIGNALED(status)) {
        const int signal = WTERMSIG(status);
        diag.error("cephyr.tool", "'" + args.front() + "' terminated by signal " +
            std::to_string(signal) + (output.empty() ? "" : ": " + output));
        return 128 + signal;
    }
    if (!WIFEXITED(status)) {
        diag.error("cephyr.tool", "unexpected termination of '" + args.front() + "'");
        return 127;
    }
    const int code = WEXITSTATUS(status);
    if (code)
        diag.error("cephyr.tool", "'" + args.front() + "' exited with status " +
            std::to_string(code) + (output.empty() ? "" : ": " + output));
    else if (!output.empty())
        diag.note("cephyr.tool", args.front() + ": " + output);
    return code;
}

} // namespace cephyr
