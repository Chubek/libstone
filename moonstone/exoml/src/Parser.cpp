#include "ParserBridge.h"
#include <exception>
#include <exoml/ExoML.hpp>
#include <mutex>

namespace moonstone::exoml {
namespace {
struct Parsed {
    Module module;
    std::vector<std::pair<std::size_t, std::size_t>> offsets;
    std::exception_ptr error;
};
void collect(void *opaque, int kind, const char *first, const char *second, const char *third,
             std::size_t begin, std::size_t end) noexcept {
    auto &result = *static_cast<Parsed *>(opaque);
    if (result.error)
        return;
    try {
        if (kind == 0) {
            Command command;
            command.binding = first;
            command.operation = second;
            command.function = third;
            result.module.commands.push_back(std::move(command));
            result.offsets.emplace_back(begin, end);
        } else
            result.module.commands.back().arguments.emplace_back(first);
    } catch (...) {
        result.error = std::current_exception();
    }
}
} // namespace
Module Parser::parse() const {
    if (input_.size() > limits_.input_bytes) {
        SourceSpan source;
        source.file = file_;
        throw ParseError("input byte budget exceeded", std::move(source));
    }
    std::vector<std::size_t> lines{0};
    for (std::size_t i = 0; i < input_.size(); ++i)
        if (input_[i] == '\n')
            lines.push_back(i + 1);
    auto span = [&](std::size_t begin, std::size_t end) {
        SourceSpan result;
        result.file = file_;
        auto first = std::upper_bound(lines.begin(), lines.end(), begin) - lines.begin() - 1;
        auto last = std::upper_bound(lines.begin(), lines.end(), end) - lines.begin() - 1;
        result.line = static_cast<std::uint32_t>(first + 1);
        result.column = static_cast<std::uint32_t>(begin - lines[first] + 1);
        result.end_line = static_cast<std::uint32_t>(last + 1);
        result.end_column = static_cast<std::uint32_t>(end - lines[last] + 1);
        return result;
    };
    if (input_.find('\0') != std::string_view::npos)
        throw ParseError("NUL byte in source", span(0, 0));
    Parsed parsed;
    std::string text(input_);
    std::size_t offset = 0;
    int status;
    {
        static std::mutex parser_mutex;
        std::lock_guard lock(parser_mutex);
        status = exoml_parse_commands(text.c_str(), limits_.commands, limits_.arguments, collect,
                                      &parsed, &offset);
    }
    if (parsed.error)
        std::rethrow_exception(parsed.error);
    if (status)
        throw ParseError(status == 2   ? "command or argument budget exceeded"
                         : status == 3 ? "parser allocation failed"
                                       : "invalid ExoML syntax",
                         span(offset, offset));
    for (std::size_t i = 0; i < parsed.module.commands.size(); ++i) {
        auto &command = parsed.module.commands[i];
        command.source = span(parsed.offsets[i].first, parsed.offsets[i].second);
        std::reverse(command.arguments.begin(), command.arguments.end());
        for (const auto &arg : command.arguments) {
            if (!arg.empty() && arg.front() == '$')
                continue;
            try {
                (void)parse_literal(arg);
            } catch (const std::exception &e) {
                throw ParseError(e.what(), command.source);
            }
        }
    }
    std::reverse(parsed.module.commands.begin(), parsed.module.commands.end());
    return std::move(parsed.module);
}
} // namespace moonstone::exoml
