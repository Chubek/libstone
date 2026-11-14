#include <charconv>
#include <cmath>
#include <exoml/ExoML.hpp>
#include <iomanip>
#include <limits>

namespace moonstone::exoml {
namespace {
bool alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool operation_valid(std::string_view op) {
    return op == "~get!" || op == "~setvar!" || op == "~setconst!" || op == "~hatch!";
}
void trim(std::string_view &text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
}
std::string read_string(std::string_view &text) {
    if (text.empty() || text.front() != '"')
        throw std::invalid_argument("expected string literal");
    text.remove_prefix(1);
    std::string result;
    while (!text.empty()) {
        char c = text.front();
        text.remove_prefix(1);
        if (c == '"')
            return result;
        if (static_cast<unsigned char>(c) < 32)
            throw std::invalid_argument("unescaped control character");
        if (c == '\\') {
            if (text.empty())
                throw std::invalid_argument("unterminated escape");
            c = text.front();
            text.remove_prefix(1);
            switch (c) {
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            case '0':
                c = '\0';
                break;
            case '"':
            case '\\':
                break;
            default:
                throw std::invalid_argument("unsupported string escape");
            }
        }
        result += c;
    }
    throw std::invalid_argument("unterminated string");
}
Diagnostic error(std::string id, std::string message, const SourceSpan &source) {
    Diagnostic result;
    result.id = std::move(id);
    result.message = std::move(message);
    result.primary = source;
    return result;
}
} // namespace

bool valid_identifier(std::string_view name) noexcept {
    return !name.empty() && alpha(name.front()) &&
           std::all_of(name.begin() + 1, name.end(),
                       [](char c) { return alpha(c) || (c >= '0' && c <= '9'); });
}
bool valid_service_name(std::string_view name) noexcept {
    for (;;) {
        auto split = name.find(':');
        if (!valid_identifier(name.substr(0, split)))
            return false;
        if (split == std::string_view::npos)
            return true;
        name.remove_prefix(split + 1);
    }
}
std::string_view value_type(const Value &value) noexcept {
    constexpr std::string_view names[] = {"null",    "integer",  "boolean", "string",
                                          "strings", "unsigned", "real"};
    return value.valueless_by_exception() ? "invalid" : names[value.index()];
}
std::string stringify(const Value &value) {
    if (auto p = std::get_if<std::string>(&value))
        return *p;
    if (auto p = std::get_if<std::int64_t>(&value))
        return std::to_string(*p);
    if (auto p = std::get_if<std::uint64_t>(&value))
        return std::to_string(*p);
    if (auto p = std::get_if<bool>(&value))
        return *p ? "true" : "false";
    if (auto p = std::get_if<double>(&value)) {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::setprecision(std::numeric_limits<double>::max_digits10) << *p;
        return out.str();
    }
    if (auto p = std::get_if<std::vector<std::string>>(&value)) {
        std::string out = "[";
        for (const auto &item : *p) {
            if (out.size() > 1)
                out += ',';
            out += '"' + json_escape(item) + '"';
        }
        return out + "]";
    }
    return "null";
}
Value parse_literal(std::string_view text) {
    trim(text);
    if (text == "null")
        return std::monostate{};
    if (text == "true")
        return true;
    if (text == "false")
        return false;
    if (!text.empty() && text.front() == '"') {
        auto value = read_string(text);
        trim(text);
        if (!text.empty())
            throw std::invalid_argument("trailing literal input");
        return value;
    }
    if (!text.empty() && text.front() == '[') {
        text.remove_prefix(1);
        trim(text);
        std::vector<std::string> values;
        if (!text.empty() && text.front() != ']') {
            for (;;) {
                values.push_back(read_string(text));
                trim(text);
                if (text.empty() || text.front() != ',')
                    break;
                text.remove_prefix(1);
                trim(text);
            }
        }
        if (text.empty() || text.front() != ']')
            throw std::invalid_argument("expected ']' in string list");
        text.remove_prefix(1);
        trim(text);
        if (!text.empty())
            throw std::invalid_argument("trailing list input");
        return values;
    }
    if (text.empty())
        throw std::invalid_argument("empty literal");
    if (text.back() == 'u') {
        text.remove_suffix(1);
        std::uint64_t number{};
        auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), number);
        if (ec == std::errc{} && end == text.data() + text.size())
            return number;
    } else if (text.find_first_of(".eE") != std::string_view::npos) {
        double number{};
        auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), number);
        if (ec == std::errc{} && end == text.data() + text.size() && std::isfinite(number))
            return number;
    } else {
        std::int64_t number{};
        auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), number);
        if (ec == std::errc{} && end == text.data() + text.size())
            return number;
    }
    throw std::invalid_argument("invalid or out-of-range literal: " + std::string(text));
}

void Interpreter::register_service(std::string name, HostFunction fn, bool privileged) {
    Registered service;
    service.callback = std::move(fn);
    service.privileged = privileged;
    service.info.ownership.effects.unknown = true;
    Services batch;
    batch.emplace(std::move(name), std::move(service));
    register_services(std::move(batch));
}
void Interpreter::register_services(Services services) {
    for (const auto &[name, service] : services) {
        if (!valid_service_name(name) || !service.callback)
            throw std::invalid_argument("invalid service: " + name);
        if (services_.count(name))
            throw std::invalid_argument("service already registered: " + name);
    }
    auto next = services_;
    next.merge(services);
    services_.swap(next);
}
void Interpreter::allow_service(std::string name) {
    if (!valid_service_name(name))
        throw std::invalid_argument("invalid service name");
    permitted_.insert(std::move(name));
}
void Interpreter::define(std::string name, Value value, bool constant) {
    if (!valid_identifier(name))
        throw std::invalid_argument("invalid binding: " + name);
    if (constants_.count(name))
        throw std::invalid_argument("cannot reassign constant: " + name);
    symbols_.insert_or_assign(name, std::move(value));
    if (constant)
        constants_.insert(std::move(name));
}
Value Interpreter::invoke(const std::string &name, const std::vector<Value> &arguments,
                          std::string_view operation) const {
    if (!operation_valid(operation))
        throw std::invalid_argument("unsupported ExoML operator: " + std::string(operation));
    auto found = services_.find(name);
    if (found == services_.end())
        throw std::invalid_argument("unknown service: " + name);
    const auto &service = found->second;
    if (operation == "~hatch!" && !allow_hatch_)
        throw std::runtime_error("hatching disabled");
    if (service.privileged && (!allow_hatch_ || !permitted_.count(name) || operation != "~hatch!"))
        throw std::runtime_error("service requires explicit hatch capability: " + name);
    if (service.info.thread_affine && service.registration_thread != std::this_thread::get_id())
        throw std::runtime_error("service requires its registration thread: " + name);
    if (service.info.arity && arguments.size() != *service.info.arity)
        throw std::invalid_argument("wrong argument count for " + name + ": expected " +
                                    std::to_string(*service.info.arity));
    return service.callback(arguments);
}
Diagnostics Interpreter::execute(const Module &module, ExecutionOptions options) {
    Diagnostics errors;
    if (module.commands.size() > options.max_commands) {
        errors.push_back(error("EXOML-LIMIT", "command budget exceeded", {}));
        return errors;
    }
    for (const auto &command : module.commands) {
        try {
            if (!operation_valid(command.operation))
                throw std::invalid_argument("unsupported ExoML operator: " + command.operation);
            if (!valid_identifier(command.binding))
                throw std::invalid_argument("invalid binding: " + command.binding);
            if (constants_.count(command.binding))
                throw std::runtime_error("cannot reassign constant: " + command.binding);
            std::vector<Value> args;
            for (const auto &arg : command.arguments) {
                if (arg == "$@") {
                    if (!last_)
                        throw std::invalid_argument("no previous ExoML result");
                    args.push_back(*last_);
                } else if (!arg.empty() && arg.front() == '$') {
                    auto found = symbols_.find(arg.substr(1));
                    if (found == symbols_.end())
                        throw std::invalid_argument("unbound ExoML value: " + arg);
                    args.push_back(found->second);
                } else
                    args.push_back(parse_literal(arg));
            }
            auto value = invoke(command.function, args, command.operation);
            define(command.binding, value, command.operation == "~setconst!");
            last_ = std::move(value);
        } catch (const std::exception &e) {
            errors.push_back(error("EXOML-EVAL", e.what(), command.source));
            if (options.stop_on_error)
                break;
        } catch (...) {
            errors.push_back(error("EXOML-NATIVE", "native service threw a non-standard exception",
                                   command.source));
            if (options.stop_on_error)
                break;
        }
    }
    return errors;
}
} // namespace moonstone::exoml
