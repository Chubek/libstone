#pragma once
#include "Runtime.hpp"
#include <map>
#include <stdexcept>

namespace sandstone::archml {

struct Location {
    std::size_t offset = 0, line = 1, column = 1;
};
class Error : public std::runtime_error {
public:
    Error(std::string source, Location location, std::string message);
    const Location location;
};
struct Node {
    enum class Kind { Symbol, String, List };
    Kind kind = Kind::Symbol;
    std::string text;
    std::vector<Node> children;
    Location location;
    std::string_view head() const;
    const Node *property(std::string_view name) const;
};

class Document {
public:
    static Document parse(std::string_view text, std::string source = "<memory>");
    static Document load(const std::string &path);
    const std::string &name() const { return name_; }
    const Node &root() const { return root_; }
    const Node *find(std::string_view kind, std::string_view name) const;
    const std::vector<runtime::Instruction> &instructions() const { return instructions_; }
    const runtime::Instruction *instruction(std::string_view name) const;
    std::vector<std::uint8_t> encode(std::string_view name, std::span<const std::uint64_t> values) const;
    bool decode(std::span<const std::uint8_t> bytes, runtime::Decoded &result) const;
private:
    std::string name_, source_;
    Node root_;
    std::map<std::pair<std::string, std::string>, std::size_t> records_;
    std::vector<runtime::Instruction> instructions_;
    void validate();
    [[noreturn]] void fail(const Node &, const std::string &) const;
};

// Canonical representations preserve unknown metadata, ordering, and atom/string
// distinctions. JSON symbols are {"symbol":"..."}; lists are arrays.
std::string format(const Node &node);
std::string json(const Node &node);
std::string quote(std::string_view text);
} // namespace sandstone::archml
