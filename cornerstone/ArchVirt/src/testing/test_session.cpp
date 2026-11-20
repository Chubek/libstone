#include "test_session.hpp"
#include <algorithm>
#include <chrono>
#include <thread>

namespace domvirt::testing {
namespace {
using Json = nlohmann::json;
constexpr std::size_t input_limit = 65536, output_limit = 1024 * 1024;
std::uint64_t number(const Json& args, const char* key, std::uint64_t fallback = 0) {
    if (!args.contains(key)) return fallback;
    const auto& value = args.at(key);
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0) return value.get<std::uint64_t>();
    throw std::invalid_argument(std::string(key) + " must be a non-negative integer");
}
std::string hex(std::span<const std::uint8_t> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text; text.reserve(bytes.size() * 2);
    for (auto byte : bytes) { text += digits[byte >> 4]; text += digits[byte & 15]; }
    return text;
}
std::vector<std::uint8_t> unhex(const std::string& text) {
    if (text.size() % 2 || text.size() > 2 * input_limit) throw std::invalid_argument("Hex data must contain at most 65536 complete bytes");
    auto digit = [](char ch) -> unsigned {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        throw std::invalid_argument("Invalid hexadecimal byte");
    };
    std::vector<std::uint8_t> bytes; bytes.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) bytes.push_back((digit(text[i]) << 4) | digit(text[i + 1]));
    return bytes;
}
void fields(const Json& args, std::initializer_list<const char*> allowed) {
    for (auto it = args.begin(); it != args.end(); ++it)
        if (std::none_of(allowed.begin(), allowed.end(), [&](const char* name) { return it.key() == name; }))
            throw std::invalid_argument("Unknown argument: " + it.key());
}
struct VmFailure : std::runtime_error {
    VmErrorCode code;
    explicit VmFailure(const VmResult& result) : std::runtime_error(result.message), code(result.code) {}
};
void check(const VmResult& result) { if (!result) throw VmFailure(result); }
}

TestSession::TestSession(machine::BootOptions defaults, bool managed,
                         machine::Runtime::Serial serial, machine::Runtime::Console console)
    : defaults_(std::move(defaults)), managed_(managed), external_serial_(std::move(serial)), external_console_(std::move(console)) {}
TestSession::~TestSession() { stop(); }
void TestSession::attach(bindlet_bridge::ControlSocket& socket) {
    for (const char* command : {"query-capabilities", "query-status", "status", "boot", "reset", "stop", "quit",
                               "pause", "resume", "step", "console-write", "console-read", "query-screen",
                               "read-memory", "write-memory", "query-registers"})
        socket.register_handler(command, [this, name = std::string(command)](const std::string& request) { return handle(name, request); });
}
VmResult TestSession::boot_locked(const machine::BootOptions& options, bool paused) {
    runtime_.stop(); runtime_.join();
    {
        std::lock_guard lock(io_mutex_);
        input_.clear(); output_.clear(); screen_.clear(); output_begin_ = output_end_ = 0;
        ++generation_;
    }
    boot_result_ = runtime_.initialize(options, [this](bool write, std::uint8_t value) { return serial(write, value); },
        [this](std::span<const std::uint8_t> cells) { console(cells); });
    if (!boot_result_) return boot_result_;
    boot_result_ = runtime_.start(paused);
    if (boot_result_) { configured_ = options; configured_valid_ = true; }
    return boot_result_;
}
VmResult TestSession::boot(const machine::BootOptions& options, bool paused) {
    std::lock_guard lock(mutex_); return boot_locked(options, paused);
}
void TestSession::stop() { std::lock_guard lock(mutex_); runtime_.stop(); runtime_.join(); }
bool TestSession::running() const { std::lock_guard lock(mutex_); return runtime_.running(); }
VmResult TestSession::result() const {
    std::lock_guard lock(mutex_); return boot_result_ ? runtime_.result() : boot_result_;
}
std::uint8_t TestSession::serial(bool write, std::uint8_t value) {
    {
        std::lock_guard lock(io_mutex_);
        if (write) {
            output_.push_back(value); ++output_end_;
            if (output_.size() > output_limit) { output_.pop_front(); ++output_begin_; }
        } else if (!input_.empty()) {
            value = input_.front(); input_.pop_front(); return value;
        }
    }
    if (external_serial_) return external_serial_(write, value);
    if (!write) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return 0;
}
void TestSession::console(std::span<const std::uint8_t> cells) {
    { std::lock_guard lock(io_mutex_); screen_.assign(cells.begin(), cells.end()); }
    if (external_console_) external_console_(cells);
}
Json TestSession::status_locked() const {
    const auto state = runtime_.state();
    const auto result = !boot_result_ ? boot_result_ : state == "empty" ? VmResult{} : runtime_.result();
    return {{"protocol_version", 1}, {"generation", generation_}, {"state", state},
        {"running", state == "running"}, {"active", runtime_.running()}, {"machine", runtime_.machine_name()},
        {"backend", runtime_.backend()}, {"instructions", runtime_.instruction_count()},
        {"instruction_count_supported", runtime_.backend() == "software"},
        {"result", {{"code", static_cast<int>(result.code)}, {"message", result.message},
                    {"complete", state == "halted" || state == "stopped" || state == "failed" || !boot_result_}}}};
}
std::string TestSession::handle(const std::string& command, const std::string& request) {
    std::lock_guard lock(mutex_);
    try {
        const auto req = Json::parse(request);
        const auto args = req.value("arguments", Json::object());
        if (!args.is_object()) throw std::invalid_argument("arguments must be an object");
        Json value = Json::object();
        if (command == "query-capabilities") {
            fields(args, {});
            value = {{"protocol_version", 1}, {"machines", {"x86_64", "aarch64", "riscv64"}}, {"managed", managed_},
                {"console_capacity", output_limit}, {"input_capacity", input_limit}, {"max_memory_bytes", 65536},
                {"max_step_count", 10000}, {"single_step_backend", "software"},
                {"commands", {"query-capabilities", "query-status", "boot", "reset", "pause", "resume", "step",
                    "stop", "quit", "console-read", "console-write", "query-screen", "read-memory", "write-memory", "query-registers"}}};
        } else if (command == "query-status" || command == "status") {
            fields(args, {}); value = status_locked();
        } else if (command == "boot" || command == "reset") {
            if (!managed_) throw std::invalid_argument("boot/reset require --test-server");
            auto options = command == "reset" ? configured_ : defaults_;
            if (command == "reset") {
                fields(args, {"paused"});
                if (!configured_valid_) throw std::invalid_argument("No successful boot to reset");
            } else {
                fields(args, {"machine", "backend", "archml", "kernel", "image", "firmware", "cmdline",
                              "memory_mb", "max_instructions", "domix", "domios", "paused", "boot_mode", "disk"});
                options.boot_mode = args.value("boot_mode", options.boot_mode);
                options.disk_path = args.value("disk", options.disk_path);
                options.machine = args.value("machine", options.machine);
                options.backend = args.value("backend", options.backend);
                options.archml_path = args.value("archml", options.archml_path);
                options.kernel_path = args.value("kernel", options.kernel_path);
                options.image_path = args.value("image", options.image_path);
                options.firmware_path = args.value("firmware", options.firmware_path);
                options.cmdline = args.value("cmdline", options.cmdline);
                options.memory_mb = number(args, "memory_mb", options.memory_mb);
                options.instruction_limit = number(args, "max_instructions", options.instruction_limit);
                options.bundled_domix = args.value("domix", options.kernel_path.empty() && options.bundled_domix);
                options.domios = args.value("domios", options.domios);
            }
            check(boot_locked(options, args.value("paused", false)));
            value = status_locked();
        } else if (command == "stop" || command == "quit") {
            fields(args, {}); runtime_.stop(); runtime_.join();
            value = status_locked();
            if (command == "quit") quitting_.store(true);
        } else if (command == "pause" || command == "resume" || command == "step") {
            if (command == "step") {
                fields(args, {"count"}); check(runtime_.step(number(args, "count", 1)));
            } else {
                fields(args, {}); check(command == "pause" ? runtime_.pause() : runtime_.resume());
            }
            value = status_locked();
        } else if (command == "console-write") {
            fields(args, {"text", "data_hex"});
            if (args.contains("text") == args.contains("data_hex")) throw std::invalid_argument("Supply text or data_hex");
            if (!runtime_.running()) throw std::invalid_argument("VM is not active");
            std::vector<std::uint8_t> bytes;
            if (args.contains("text")) { const auto text = args.at("text").get<std::string>(); bytes.assign(text.begin(), text.end()); }
            else bytes = unhex(args.at("data_hex").get<std::string>());
            std::lock_guard io_lock(io_mutex_);
            if (bytes.size() > input_limit - input_.size()) throw std::invalid_argument("Console input queue is full");
            input_.insert(input_.end(), bytes.begin(), bytes.end());
            value = {{"accepted", bytes.size()}};
        } else if (command == "console-read") {
            fields(args, {"offset", "limit", "generation"});
            if (number(args, "generation", generation_) != generation_) throw std::invalid_argument("Console generation changed; reset your cursor");
            const auto offset = number(args, "offset"), limit = number(args, "limit", 65536);
            if (!limit || limit > 65536) throw std::invalid_argument("Console limit must be 1..65536");
            std::lock_guard io_lock(io_mutex_);
            if (offset > output_end_) throw std::invalid_argument("Console offset is beyond the output stream");
            const auto start = std::max(offset, output_begin_), size = std::min(limit, output_end_ - start);
            auto begin = output_.begin() + static_cast<std::ptrdiff_t>(start - output_begin_);
            std::vector<std::uint8_t> bytes(begin, begin + static_cast<std::ptrdiff_t>(size));
            value = {{"generation", generation_}, {"begin", output_begin_}, {"end", output_end_},
                     {"next", start + size}, {"truncated", offset < output_begin_}, {"data_hex", hex(bytes)}};
        } else if (command == "query-screen") {
            fields(args, {}); std::lock_guard io_lock(io_mutex_);
            value = {{"generation", generation_}, {"columns", 80}, {"rows", 25}, {"format", "vga-text"}, {"data_hex", hex(screen_)}};
        } else if (command == "query-registers") {
            fields(args, {}); std::map<std::string, std::uint64_t> registers;
            check(runtime_.registers(registers)); value = {{"machine", runtime_.machine_name()}, {"registers", registers}};
        } else if (command == "read-memory") {
            fields(args, {"address", "size"});
            if (!args.contains("address") || !args.contains("size")) throw std::invalid_argument("address and size are required");
            std::vector<std::uint8_t> bytes;
            check(runtime_.read_memory(number(args, "address"), number(args, "size"), bytes));
            value = {{"address", number(args, "address")}, {"data_hex", hex(bytes)}};
        } else if (command == "write-memory") {
            fields(args, {"address", "data_hex"});
            if (!args.contains("address") || !args.contains("data_hex")) throw std::invalid_argument("address and data_hex are required");
            const auto bytes = unhex(args.at("data_hex").get<std::string>());
            check(runtime_.write_memory(number(args, "address"), bytes)); value = {{"written", bytes.size()}};
        }
        return Json{{"return", value}}.dump();
    } catch (const VmFailure& error) {
        return Json{{"error", {{"code", "vm-error"}, {"vm_code", static_cast<int>(error.code)}, {"message", error.what()}}}}.dump();
    } catch (const std::exception& error) {
        return Json{{"error", {{"code", "invalid-argument"}, {"message", error.what()}}}}.dump();
    }
}
} // namespace domvirt::testing
