#include <ArchML.hpp>

namespace {
namespace archml = sandstone::archml;

constexpr std::size_t max_code_bytes = 64 * 1024;

int nibble(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::vector<std::uint8_t> read_hex(std::string_view text) {
    std::vector<std::uint8_t> bytes;
    int high = -1;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (high == -1 && (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')) continue;
        const int value = nibble(ch);
        if (value < 0)
            throw std::invalid_argument("expected a hex digit at input byte " + std::to_string(i));
        if (high == -1) high = value;
        else {
            if (bytes.size() == max_code_bytes)
                throw std::invalid_argument("machine code exceeds 64 KiB");
            bytes.push_back(static_cast<std::uint8_t>(high * 16 + value));
            high = -1;
        }
    }
    if (high != -1) throw std::invalid_argument("incomplete hex byte at end of input");
    return bytes;
}

std::string json_strings(std::span<const std::string> values) {
    std::string result = "[";
    for (const auto &value : values) {
        if (result.size() > 1) result += ',';
        result += archml::quote(value);
    }
    return result + ']';
}

// Preserve ArchML's symbol/string distinction and distinguish missing metadata
// from an explicitly empty list.
std::string property_items(const archml::Node &form, std::string_view key) {
    const auto *node = form.property(key);
    if (!node) return "null";
    std::string result = "[";
    for (std::size_t i = 1; i < node->children.size(); ++i) {
        if (i != 1) result += ',';
        result += archml::json(node->children[i]);
    }
    return result + ']';
}

std::string analyze(const archml::Document &doc, std::string_view hex) {
    const auto bytes = read_hex(hex);
    std::string result = "{\"profile\":" + archml::quote(doc.name()) +
                         ",\"byteLength\":" + std::to_string(bytes.size()) + ",\"instructions\":[";
    archml::runtime::Decoded decoded;
    for (std::size_t offset = 0; offset < bytes.size(); offset += decoded.size) {
        if (!doc.decode(std::span(bytes).subspan(offset), decoded))
            throw std::invalid_argument("no matching ArchML instruction at code byte offset " +
                                        std::to_string(offset));
        const auto &form = doc.instructions()[decoded.instruction];
        const auto &record = *doc.find("instruction", form.name);
        const auto &operand_nodes = record.property("operands")->children;
        if (offset) result += ',';
        result += "{\"offset\":" + std::to_string(offset) +
                  ",\"size\":" + std::to_string(decoded.size) +
                  ",\"name\":" + archml::quote(form.name) +
                  ",\"assembly\":" + archml::quote(form.assembly) +
                  ",\"effects\":" + property_items(record, "effects") +
                  ",\"implicitUses\":" + property_items(record, "implicit-use") +
                  ",\"implicitDefs\":" + property_items(record, "implicit-def") +
                  ",\"features\":" + json_strings(form.features) +
                  ",\"modes\":" + json_strings(form.modes) + ",\"operands\":[";
        for (std::size_t i = 0; i < decoded.operands.size(); ++i) {
            if (i) result += ',';
            const auto &operand = form.operands[i];
            // ArchML's validated AST and decoder both use declaration order.
            const auto &access = operand_nodes[i + 1].property("access")->children[1].text;
            const auto bits = decoded.operands[i];
            const auto value = operand.is_signed ? std::to_string(std::bit_cast<std::int64_t>(bits))
                                                 : std::to_string(bits);
            result += "{\"name\":" + archml::quote(operand.name) +
                      ",\"kind\":" + archml::quote(operand.kind) +
                      ",\"access\":" + archml::quote(access) +
                      ",\"value\":" + archml::quote(value) + '}';
        }
        result += "]}";
    }
    return result + "]}";
}

// LLSynth's native bridge copies the borrowed String immediately. Only a
// failed query returns an empty string; the diagnostic is read on that thread.
thread_local std::string response;
thread_local const char *fallback_error = nullptr;
} // namespace

extern "C" const char *llsynth_isa_query(std::int64_t operation, const char *path,
                                        const char *first, const char *second) noexcept {
    fallback_error = nullptr;
    try {
        const auto doc = archml::Document::load(path);
        switch (operation) {
        case 0: response = doc.name(); break;
        case 1: response = archml::json(doc.root()); break;
        case 2: {
            const auto *record = doc.find(first, second);
            response = record ? archml::json(*record) : "null";
            break;
        }
        case 3: response = analyze(doc, first); break;
        default: throw std::invalid_argument("unknown std.isa query");
        }
        return response.c_str();
    } catch (const std::exception &error) {
        try {
            response = error.what();
            return "";
        } catch (...) {
            // Use a static message if retaining the diagnostic itself fails.
        }
    } catch (...) {
    }
    fallback_error = "native std.isa query failed";
    return "";
}

extern "C" const char *llsynth_isa_error() noexcept {
    return fallback_error ? fallback_error : response.c_str();
}
