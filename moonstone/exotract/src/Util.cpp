#include <exotract/Exotract.hpp>
#include <atomic>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <cstring>
#include <csignal>
#include <sstream>
#include <set>
#include <array>
#include <system_error>
#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif
namespace moonstone::exotract {
namespace {
void require_clean(std::string_view text, const char *label) {
    if (text.empty() || text.find('\0') != std::string_view::npos ||
        text.find('\n') != std::string_view::npos || text.find('\r') != std::string_view::npos)
        throw std::invalid_argument(std::string("invalid ") + label);
}
}
std::string json_quote(std::string_view s) {
    std::string out = "\"";
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned char ch : s) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 32) {
                    out += "\\u00";
                    out += digits[ch >> 4]; out += digits[ch & 15];
                } else out += static_cast<char>(ch);
        }
    }
    return out + '"';
}
std::string diagnostic_json(const Diagnostic &d) {
    const char *level = d.severity == Diagnostic::Severity::Error ? "error" :
                        d.severity == Diagnostic::Severity::Warning ? "warning" : "note";
    return "{\"code\":" + json_quote(d.code) + ",\"severity\":" + json_quote(level) +
           ",\"message\":" + json_quote(d.message) + "}";
}
std::string read_file(const fs::path &path, std::size_t max_bytes) {
    std::error_code ec;
    if (fs::file_size(path, ec) > max_bytes || ec)
        throw std::runtime_error("missing or oversized input: " + path.string());
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read: " + path.string());
    std::string body;
    std::array<char, 8192> buffer;
    while (f) {
        f.read(buffer.data(), buffer.size());
        const auto n = static_cast<std::size_t>(f.gcount());
        if (n > max_bytes - body.size()) throw std::length_error("input exceeds byte budget");
        body.append(buffer.data(), n);
    }
    if (!f.eof()) throw std::runtime_error("input read failed: " + path.string());
    return body;
}
void write_file_atomic(const fs::path &path, std::string_view body) {
#if defined(__unix__) || defined(__APPLE__)
    fs::create_directories(path.parent_path().empty() ? fs::path(".") : path.parent_path());
    std::string pattern = path.string() + ".tmp-XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back(0);
    int fd = mkstemp(name.data());
    if (fd < 0) throw std::runtime_error("cannot create private output: " + path.string());
    try {
        std::size_t offset = 0;
        while (offset < body.size()) {
            const auto n = write(fd, body.data() + offset, body.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("cannot write output: " + path.string());
            offset += static_cast<std::size_t>(n);
        }
        const int closed = close(fd);
        fd = -1;
        if (closed != 0) throw std::runtime_error("cannot close output: " + path.string());
        fs::rename(name.data(), path);
    } catch (...) {
        if (fd >= 0) close(fd);
        unlink(name.data());
        throw;
    }
#else
    (void)path; (void)body;
    throw std::runtime_error("atomic publication unsupported on this platform");
#endif
}

void validate_config(const Config &cfg) {
    require_clean(cfg.name, "library name");
    require_clean(cfg.path.string(), "header path");
    auto identifier = [](std::string_view n) {
        const auto alpha=[](char c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; };
        return !n.empty() && alpha(n[0]) && std::all_of(n.begin(),n.end(),[&](char c){return alpha(c)||(c>='0'&&c<='9');});
    };
    std::set<std::string> implementation;
    for (const auto &macro : cfg.header_only.implementation_defines) {
        if (!identifier(macro) || !implementation.insert(macro).second)
            throw std::invalid_argument("invalid or duplicate implementation macro: " + macro);
    }
    for (const auto &path : cfg.include_directories) require_clean(path.string(), "include path");
    for (const auto &path : cfg.header_only.dependencies) {
        require_clean(path, "dependency path");
        if (path.find_first_of("\"\\") != std::string::npos) throw std::invalid_argument("unsafe dependency path");
    }
    if (cfg.cstd != "c11" && cfg.cstd != "C11")
        throw std::invalid_argument("Exotract supports C11 only");
    if (cfg.header_only.style != "implementation-macro" && cfg.header_only.style != "single-header")
        throw std::invalid_argument("unknown header-only style");
    if (cfg.header_only.implementation_translation_units != 1)
        throw std::invalid_argument("header-only implementations require exactly one TU");
    for (const auto &macro : cfg.header_only.implementation_defines) require_clean(macro, "implementation macro");
    for (const auto &macro : cfg.defines) {
        require_clean(macro, "macro");
        auto name=macro.substr(0,macro.find('='));
        if (!identifier(name)) throw std::invalid_argument("invalid define name: " + name);
        if(name.starts_with("MULIB_IMPL_") ||
            std::find(cfg.header_only.implementation_defines.begin(),
                      cfg.header_only.implementation_defines.end(),name)!=
                cfg.header_only.implementation_defines.end())
            throw std::invalid_argument("implementation macro passed to declaration-only discovery: "+name);
    }
    for (const auto &symbol: cfg.required_symbols) require_clean(symbol,"required symbol");
    for (const auto &macro : cfg.undefines) if (!identifier(macro)) throw std::invalid_argument("invalid undefine");
    for (const auto &symbol : cfg.required_symbols) if (!identifier(symbol)) throw std::invalid_argument("invalid required symbol");
    if (cfg.max_output_bytes == 0 || cfg.max_output_bytes > 256 * 1024 * 1024 ||
        cfg.command_timeout_ms == 0 || cfg.command_timeout_ms > 120000)
        throw std::invalid_argument("invalid command budgets");
    require_clean(cfg.compiler, "compiler");
    if(!cfg.target.empty())require_clean(cfg.target,"target");
    const std::set<std::string> allowed={"-m32","-m64","-fsigned-char","-funsigned-char","-fshort-enums","-fno-short-enums","-fshort-wchar","-fno-short-wchar"};
    for(const auto &arg:cfg.compiler_arguments)if(!allowed.contains(arg))throw std::invalid_argument("unsupported compiler argument: "+arg);
    for(const auto &path:cfg.provenance_files)require_clean(path.string(),"provenance path");
}
CommandResult run_command(const Command &cmd, const Config &cfg,
                          const std::map<std::string, std::string> &subs) {
    CommandResult result;
    if (cmd.argv.empty()) throw std::invalid_argument("empty subprocess argv");
    std::vector<std::string> args = cmd.argv;
    if (args[0].empty()) throw std::invalid_argument("empty executable");
    if (!cfg.command_timeout_ms || !cfg.max_output_bytes) throw std::invalid_argument("zero command budget");
    for (const auto &[key, value] : subs) {
        (void)value;
        if (key.empty()) throw std::invalid_argument("empty substitution key");
    }
    for (auto &arg : args) {
        for (const auto &[key, value] : subs) {
            std::size_t pos = 0;
            while ((pos = arg.find(key, pos)) != std::string::npos) {
                arg.replace(pos, key.size(), value); pos += value.size();
            }
        }
        if (arg.find('\0') != std::string::npos) throw std::invalid_argument("NUL argv");
    }
#if defined(__unix__) || defined(__APPLE__)
    int out[2]{-1,-1}, err[2]{-1,-1};
    auto make_pipe = [](int *fds) {
#ifdef __linux__
        if (pipe2(fds, O_CLOEXEC)) throw std::runtime_error("pipe failed");
#else
        if (pipe(fds)) throw std::runtime_error("pipe failed");
        for (int i=0;i<2;++i) fcntl(fds[i], F_SETFD, FD_CLOEXEC);
#endif
        // Spawn actions must not alias stdin/stdout/stderr even in a host that
        // has closed its standard descriptors.
        for (int i=0;i<2;++i) if (fds[i] <= STDERR_FILENO) {
            int moved = fcntl(fds[i], F_DUPFD_CLOEXEC, 3);
            if (moved < 0) { close(fds[0]); close(fds[1]); throw std::runtime_error("dup pipe failed"); }
            close(fds[i]); fds[i] = moved;
        }
    };
    make_pipe(out);
    try { make_pipe(err); } catch (...) { close(out[0]); close(out[1]); throw; }
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    int status = posix_spawn_file_actions_init(&actions);
    if (status) { for(int f:{out[0],out[1],err[0],err[1]})close(f); throw std::runtime_error("spawn actions failed"); }
    status = posix_spawnattr_init(&attr);
    if (status) { posix_spawn_file_actions_destroy(&actions); for(int f:{out[0],out[1],err[0],err[1]})close(f); throw std::runtime_error("spawn attributes failed"); }
    auto setup = [&](int rc) { if (!status) status=rc; };
    setup(posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0));
    setup(posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO));
    setup(posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO));
    for (int fd : {out[0],out[1],err[0],err[1]}) setup(posix_spawn_file_actions_addclose(&actions, fd));
    setup(posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP));
    setup(posix_spawnattr_setpgroup(&attr, 0));
    std::vector<char *> argv;
    for (auto &arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid = -1;
    if (!status) status = posix_spawnp(&pid, argv[0], &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(out[1]); close(err[1]);
    if (status) { close(out[0]);close(err[0]); result.err = "spawn failed: " + std::string(strerror(status)); return result; }
    bool io_error = false;
    for (int fd : {out[0], err[0]}) if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) io_error = true;
    const auto start = std::chrono::steady_clock::now();
    bool out_open = true, err_open = true, child_done = false;
    int child_status = 0;
    while (!child_done) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
        if (elapsed >= cfg.command_timeout_ms || result.overflow || io_error) {
            result.timed_out = !result.overflow && !io_error;
            // The unreaped group leader reserves its PID while descendants
            // hold pipes. Kill the whole group, then reap exactly once.
            kill(-pid, SIGKILL);
            while (waitpid(pid, &child_status, 0) < 0 && errno == EINTR) {}
            break;
        }
        struct pollfd fds[] = {{out_open ? out[0] : -1, POLLIN | POLLHUP, 0},
                               {err_open ? err[0] : -1, POLLIN | POLLHUP, 0}};
        if (poll(fds, 2, 10) < 0 && errno != EINTR) { io_error=true; continue; }
        for (int i = 0; i != 2; ++i) {
            bool &active = i == 0 ? out_open : err_open;
            if (!active) continue;
            auto &dst = i == 0 ? result.out : result.err;
            char buf[8192];
            // Fair bounded draining: a noisy stream cannot starve the timeout
            // or the other stream.
            for (int reads=0; reads<16; ++reads) {
                const ssize_t n = read(fds[i].fd, buf, sizeof buf);
                if (n > 0) {
                    if (static_cast<std::size_t>(n) > cfg.max_output_bytes -
                        std::min(cfg.max_output_bytes, result.out.size() + result.err.size())) {
                        result.overflow = true; break;
                    }
                    dst.append(buf, static_cast<std::size_t>(n));
                } else if (n == 0) { active = false; break; }
                else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                else if (errno == EINTR) continue;
                else { io_error=true; break; }
            }
        }
        if (!out_open && !err_open) {
            const pid_t w = waitpid(pid, &child_status, WNOHANG);
            if (w == pid) child_done=true;
            else if (w < 0 && errno != EINTR) { io_error=true; break; }
        }
    }
    close(out[0]); close(err[0]);
    if (!result.timed_out && !result.overflow && !io_error) {
        if (WIFEXITED(child_status)) result.exit_code = WEXITSTATUS(child_status);
        else if (WIFSIGNALED(child_status)) result.exit_code = 128 + WTERMSIG(child_status);
    }

#else
    (void)cfg; (void)subs;
    result.err = "process backend unsupported on this platform";
#endif
    return result;
}
} // namespace moonstone::exotract
