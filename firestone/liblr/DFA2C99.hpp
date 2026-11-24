#pragma once
#include "LexerGenerator.hpp"
#include <algorithm>
#include <cstddef>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>

namespace firestone::lr {

// Emit a standalone C99 translation unit (8-bit bytes) with this scanner ABI:
// int name(const unsigned char *input, size_t size, size_t *cursor,
//          size_t *token, size_t *begin, size_t *length);
// Status: 1 token, 0 EOF, -1 unknown byte. Null outputs, null input with
// nonzero size, and an out-of-range cursor return -2 without writing.
// EOF/error set token to zero. Skipped matches are consumed internally.
// Valid calls provide an input buffer of size bytes (NULL allowed iff size is
// zero) and four distinct writable size_t objects disjoint from input. *cursor
// must be <= size. Supply an unused, non-reserved symbol name for the consumer.
// The generated unit fails to compile if its target's size_t cannot hold IDs.
inline std::string emitC99(const DFA& dfa, std::string_view name = "firestone_lr_scan") {
    detail::require_scanner_name(name);
    detail::require_nonnullable(dfa);
    const auto& states = dfa.states();
    const auto alphabet = dfa.byte_classes();
    // Use the class map only when it pays for its 256 bytes even on a
    // C99 target with a two-byte size_t.
    const auto saved_columns = byte_count - alphabet.representatives.size();
    const auto compressed = saved_columns != 0 && states.size() > byte_count / (2 * saved_columns);
    const auto width = compressed ? alphabet.representatives.size() : byte_count;
    auto largest = states.size();
    for (const auto& state : states)
        if (state.acceptance) largest = std::max(largest, state.acceptance->token);
    detail::require_codegen_id(largest);

    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "#include <limits.h>\n#include <stddef.h>\n\n"
           "#if CHAR_BIT != 8\n#error \"Firestone lexer requires 8-bit bytes\"\n#endif\n\n"
           "int " << name << "(const unsigned char *input, size_t size, size_t *cursor,\n"
           "    size_t *token, size_t *begin, size_t *length) {\n"
           "    typedef char fs_lr_target_size_check[((size_t)" << largest << "ULL == " << largest << "ULL) ? 1 : -1];\n"
           "    (void)sizeof(fs_lr_target_size_check);\n";
    if (compressed) {
        out << "    static const unsigned char byte_class[256] = {";
        for (std::size_t byte = 0; byte < byte_count; ++byte) {
            if (byte % 16 == 0) out << "\n       ";
            out << ' ' << static_cast<unsigned int>(alphabet.index[byte]) << ',';
        }
        out << "\n    };\n";
    }
    out << "    static const size_t transitions[][" << width << "] = {\n";
    for (const auto& state : states) {
        out << "        {";
        for (std::size_t cls = 0; cls < width; ++cls) {
            if (cls && cls % 16 == 0) out << "\n         ";
            auto target = state.transitions[compressed ? alphabet.representatives[cls] : cls];
            out << ' ' << (target == no_state ? 0 : target + 1) << ',';
        }
        out << " },\n";
    }
    out << "    };\n"
           "    static const struct { size_t token; unsigned char kind; } accept[] = {\n";
    for (const auto& state : states) {
        if (state.acceptance)
            out << "        { " << state.acceptance->token << "ULL, " << (state.acceptance->skip ? 2 : 1) << " },\n";
        else out << "        { 0, 0 },\n";
    }
    out << "    };\n"
           "    size_t start, i, state, end, winner;\n"
           "    if (!cursor || !token || !begin || !length || (!input && size != 0) || *cursor > size)\n"
           "        return -2;\n"
           "    start = *cursor;\n"
           "    while (start < size) {\n"
           "        state = " << dfa.start() << ";\n"
           "        end = start;\n"
           "        winner = 0;\n"
           "        for (i = start; i < size; ++i) {\n"
           "            size_t next = transitions[state][" << (compressed ? "byte_class[input[i]]" : "input[i]") << "];\n"
           "            if (next == 0) break;\n"
           "            state = next - 1;\n"
           "            if (accept[state].kind) { end = i + 1; winner = next; }\n"
           "        }\n"
           "        if (winner == 0) {\n"
           "            *cursor = start + 1; *token = 0; *begin = start; *length = 1;\n"
           "            return -1;\n"
           "        }\n"
           "        state = winner - 1;\n"
           "        if (accept[state].kind == 2) { start = end; continue; }\n"
           "        *cursor = end; *token = accept[state].token; *begin = start; *length = end - start;\n"
           "        return 1;\n"
           "    }\n"
           "    *cursor = start; *token = 0; *begin = start; *length = 0;\n"
           "    return 0;\n"
           "}\n";
    return out.str();
}

inline std::string emitC99(const Lexer& lexer, std::string_view name = "firestone_lr_scan") {
    return emitC99(lexer.dfa(), name);
}

} // namespace firestone::lr
