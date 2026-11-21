#include "moonstone_tools.hpp"

#include <cephyr/diagnostics.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <exception>
#include <filesystem>
#include <future>
#include <stdexcept>
#include <system_error>
#include <utility>

#if CEPHYR_HAVE_EXOML || CEPHYR_HAVE_QMR
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if CEPHYR_HAVE_PROLIFIC
#include <prolific/Prolific.hpp>
#endif
#if CEPHYR_HAVE_EXOML
#include <exoml/StaticFFI.hpp>
#endif
#if CEPHYR_HAVE_EXOTRACT
#include <exotract/Exotract.hpp>
#endif
#if CEPHYR_HAVE_QMR
#include <qmr/Qmr.hpp>
#if CEPHYR_HAVE_PLAID
#include <plaid/loom/Scheduler.hpp>
#endif
#endif

namespace cephyr {

#if CEPHYR_HAVE_PROLIFIC
struct MoonstoneProfile::Impl {
    moonstone::prolific::Recorder recorder;

    void record_loss() noexcept
    {
        try {
            recorder.record_loss();
        } catch (...) {
            // Observation failure cannot replace a compilation failure.
        }
    }
};

MoonstoneProfile::MoonstoneProfile() : impl_(std::make_unique<Impl>()) {}
#else
struct MoonstoneProfile::Impl {};
MoonstoneProfile::MoonstoneProfile()
{
    throw std::logic_error("Cephyr was built without Prolific profiling");
}
#endif
MoonstoneProfile::~MoonstoneProfile() = default;

MoonstoneProfile::Span::~Span() noexcept
{
#if CEPHYR_HAVE_PROLIFIC
    if (id_) {
        try {
            impl_->recorder.end(id_);
        } catch (...) {
            // Recorder accounts for a failed end in its snapshot.
        }
    }
#endif
}

MoonstoneProfile::Span MoonstoneProfile::measure(std::string_view stage,
                                                std::string_view input) noexcept
{
#if CEPHYR_HAVE_PROLIFIC
    using namespace moonstone::prolific;
    moonstone::prolific::Location location;
    try {
        if (input.size() > Limits{}.text) {
            impl_->record_loss();
            return {};
        }
        location.file = input;
    } catch (...) {
        impl_->record_loss();
        return {};
    }
    try {
        const auto begin = impl_->recorder.begin(Domain::Compiler, stage, std::move(location));
        return Span(impl_.get(), begin.span);
    } catch (...) {
        // Recorder accounts for a failed begin in its snapshot.
    }
#else
    (void)stage;
    (void)input;
#endif
    return {};
}

std::string MoonstoneProfile::report(bool succeeded)
{
#if CEPHYR_HAVE_PROLIFIC
    using namespace moonstone::prolific;
    try {
        impl_->recorder.emit(Domain::Compiler, succeeded ? Kind::Instant : Kind::Error,
                             "cephyr.result", succeeded ? 0 : 1,
                             succeeded ? "success" : "failure");
    } catch (...) {
        // Take a snapshot even if recording the outcome failed.
    }
    Report result;
    result.data = impl_->recorder.snapshot();
    return result.json();
#else
    (void)succeeded;
    throw std::logic_error("Cephyr was built without Prolific profiling");
#endif
}

namespace moonstone_tools {
namespace {

constexpr std::string_view exoml_command = "--moonstone-exoml";
constexpr std::string_view exotract_command = "--moonstone-exotract";
constexpr std::string_view qmr_compile_command = "--moonstone-qmr-compile";
constexpr std::string_view qmr_check_command = "--moonstone-qmr-check";

[[maybe_unused]] std::optional<std::string> unavailable(Diagnostics &diag, std::string_view component)
{
    diag.error("cephyr.moonstone.unavailable",
               "Cephyr was built without Moonstone " + std::string(component) + " support");
    return std::nullopt;
}

#if CEPHYR_HAVE_EXOTRACT
void require_file(const char *path)
{
    if (!*path || !std::filesystem::is_regular_file(path))
        throw std::invalid_argument("expected an existing regular file");
}
#endif

#if CEPHYR_HAVE_EXOML || CEPHYR_HAVE_QMR
std::string read_source(const char *path, std::size_t limit)
{
    // Validate the opened descriptor, not a separate path lookup. O_NONBLOCK
    // prevents a FIFO from hanging the caller before that validation.
    struct Input {
        int fd;
        ~Input() { if (fd >= 0) ::close(fd); }
    } input{::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY)};
    if (input.fd < 0)
        throw std::system_error(errno, std::generic_category(), "cannot open input");
    struct stat info{};
    if (::fstat(input.fd, &info) != 0)
        throw std::system_error(errno, std::generic_category(), "cannot inspect input");
    if (!S_ISREG(info.st_mode))
        throw std::invalid_argument("expected a regular input file");
    std::array<char, 8192> chunk{};
    std::string source;
    for (;;) {
        const auto size = ::read(input.fd, chunk.data(), chunk.size());
        if (size == 0)
            return source;
        if (size < 0) {
            if (errno == EINTR)
                continue;
            throw std::system_error(errno, std::generic_category(), "cannot read input");
        }
        const auto bytes = static_cast<std::size_t>(size);
        if (bytes > limit - source.size())
            throw std::length_error("input exceeds byte limit");
        source.append(chunk.data(), bytes);
    }
}
#endif

#if CEPHYR_HAVE_EXOML
std::optional<std::string> exoml(const char *file, std::string_view symbol, Diagnostics &diag)
{
    using namespace moonstone::exoml;
    if (!valid_identifier(symbol))
        throw std::invalid_argument("expected an ExoML output symbol");
    try {
        const auto text = read_source(file, ParseLimits{}.input_bytes);
        auto module = Parser(text, file).parse();
        for (const auto &item : module.commands) {
            if (item.operation == "~hatch!") {
                diag.error("cephyr.moonstone.exoml", "hatch commands are not permitted",
                           {item.source.file, item.source.line, item.source.column});
                return std::nullopt;
            }
        }
        Interpreter interpreter;
        install_static_ffi(interpreter);
        const auto errors = interpreter.execute(module, {.stop_on_error = true});
        for (const auto &error : errors)
            diag.error(error.id, error.message,
                       {error.primary.file, error.primary.line, error.primary.column});
        if (!errors.empty())
            return std::nullopt;
        const auto found = interpreter.symbols().find(std::string(symbol));
        const auto *output = found == interpreter.symbols().end() ? nullptr :
            std::get_if<std::string>(&found->second);
        if (!output)
            throw std::invalid_argument("ExoML output must name a string symbol: " + std::string(symbol));
        return *output;
    } catch (const ParseError &error) {
        diag.error("cephyr.moonstone.exoml", error.what(),
                   {error.source.file, error.source.line, error.source.column});
    } catch (const std::exception &error) {
        diag.error("cephyr.moonstone.exoml", error.what(), {file});
    }
    return std::nullopt;
}
#endif

#if CEPHYR_HAVE_QMR
std::string qmr_compile(const char *file)
{
    const auto text = read_source(file, moonstone::qmr::Limits{}.max_template_bytes);
    return moonstone::qmr::compile(text, file).lua_source;
}

template <class F>
bool qmr_diagnostics(const char *file, Diagnostics &diag, F &&operation)
{
    try {
        operation();
        return true;
    } catch (const moonstone::qmr::Error &error) {
        // QMR's template byte limit keeps line and column in unsigned range.
        const auto prefix = error.file() + ":" + std::to_string(error.line()) + ":" +
            std::to_string(error.column()) + ": " + error.code() + ": ";
        std::string_view message = error.what();
        if (message.starts_with(prefix))
            message.remove_prefix(prefix.size());
        diag.error(error.code(), message,
                   {error.file(), static_cast<unsigned>(error.line()),
                    static_cast<unsigned>(error.column())});
    } catch (const std::exception &error) {
        diag.error("cephyr.moonstone.qmr", error.what(), {file});
    }
    return false;
}

constexpr unsigned max_qmr_workers = 8;
constexpr std::size_t max_qmr_files = 128;

std::optional<std::string> qmr_check(std::span<const char *const> files, Diagnostics &diag)
{
    namespace qmr = moonstone::qmr;
    unsigned jobs = 1;
    if (!files.empty() && std::string_view(files.front()).starts_with("--jobs=")) {
        const auto count = std::string_view(files.front()).substr(7);
        const auto [end, ec] = std::from_chars(count.data(), count.data() + count.size(), jobs);
        if (ec != std::errc{} || end != count.data() + count.size() ||
            jobs == 0 || jobs > max_qmr_workers)
            throw std::invalid_argument("--jobs expects an integer from 1 to 8");
        files = files.subspan(1);
    }
    if (files.empty() || files.size() > max_qmr_files)
        throw std::invalid_argument("QMR check requires 1 to 128 template files");
    bool success = true;
    if (jobs == 1) {
        for (const auto *file : files)
            if (!qmr_diagnostics(file, diag, [&] { (void)qmr_compile(file); }))
                success = false;
    } else {
#if CEPHYR_HAVE_PLAID
        const auto worker_count = std::min<std::size_t>(jobs, files.size());
        moonstone::plaid::loom::WorkStealingPool workers(worker_count, worker_count);
        // Bounded waves; workers only receive owned in-memory text and names.
        // File read errors are saved beside their peers so diagnostics remain
        // ordered by input even when a later job finishes first.
        struct Job {
            std::future<void> done;
            std::exception_ptr read_error;
        };
        for (std::size_t first = 0; first < files.size(); first += worker_count) {
            const auto batch = files.subspan(first, std::min(worker_count, files.size() - first));
            std::array<Job, max_qmr_workers> pending;
            for (std::size_t i = 0; i < batch.size(); ++i) {
                std::string text;
                try {
                    text = read_source(batch[i], qmr::Limits{}.max_template_bytes);
                } catch (...) {
                    pending[i].read_error = std::current_exception();
                    continue;
                }
                pending[i].done = workers.submit([text = std::move(text),
                                                  name = std::string(batch[i])]() mutable {
                    (void)qmr::compile(text, std::move(name));
                });
            }
            for (std::size_t i = 0; i < batch.size(); ++i)
                if (!qmr_diagnostics(batch[i], diag, [&] {
                        if (pending[i].read_error)
                            std::rethrow_exception(pending[i].read_error);
                        pending[i].done.get();
                    }))
                    success = false;
        }
#else
        return unavailable(diag, "Plaid");
#endif
    }
    if (!success)
        return std::nullopt;
    return std::to_string(files.size()) + " QMR template(s) translated\n";
}
#endif

} // namespace

bool profile_available() noexcept
{
#if CEPHYR_HAVE_PROLIFIC
    return true;
#else
    return false;
#endif
}

bool is_tool(std::string_view command) noexcept
{
    return command == exoml_command || command == exotract_command ||
        command == qmr_compile_command || command == qmr_check_command;
}

std::optional<std::string> run_tool(std::string_view command, std::span<const char *const> args,
                                    Diagnostics &diag)
{
    (void)args; // All tools may be compiled out.
    try {
        if (command == exoml_command) {
#if CEPHYR_HAVE_EXOML
            if (args.size() != 2)
                throw std::invalid_argument("expected FILE SYMBOL");
            return exoml(args[0], args[1], diag);
#else
            return unavailable(diag, "ExoML");
#endif
        }
        if (command == exotract_command) {
#if CEPHYR_HAVE_EXOTRACT
            if (args.size() < 2 || args.size() > 129)
                throw std::invalid_argument("expected HEADER and 1 to 128 implementation macro names");
            try {
                require_file(args[0]);
                moonstone::exotract::Config config;
                config.name = "Cephyr";
                config.path = args[0];
                config.header_only.implementation_defines.assign(args.begin() + 1, args.end());
                return moonstone::exotract::Engine(std::move(config)).implementation_source();
            } catch (const std::exception &error) {
                diag.error("cephyr.moonstone.exotract", error.what(), {args[0]});
                return std::nullopt;
            }
#else
            return unavailable(diag, "Exotract");
#endif
        }
        if (command == qmr_compile_command || command == qmr_check_command) {
#if CEPHYR_HAVE_QMR
            if (command == qmr_check_command)
                return qmr_check(args, diag);
            if (args.size() != 1)
                throw std::invalid_argument("expected a template file");
            std::optional<std::string> result;
            qmr_diagnostics(args[0], diag, [&] { result = qmr_compile(args[0]); });
            return result;
#else
            return unavailable(diag, "QMR");
#endif
        }
        throw std::invalid_argument("unknown Moonstone tool command");
    } catch (const std::exception &error) {
        diag.error("cephyr.moonstone.tool", std::string(command) + ": " + error.what());
        return std::nullopt;
    }
}

} // namespace moonstone_tools
} // namespace cephyr
