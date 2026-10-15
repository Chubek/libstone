#pragma once
#include <Jiterati.hpp>
#include <filesystem>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace gateml {
/** Original byte span; lines and byte columns are one-based. */
struct Span { size_t begin = 0, end = 0, line = 1, column = 1; };
/** Located lexical, grammatical, or semantic failure. */
class Error : public std::runtime_error {
public:
    Span span;
    std::string source;
    Error(std::string message, Span location) : std::runtime_error(std::move(message)), span(location) {}
};
/** Ordered lexical rule. Equal-length matches prefer the earlier rule. */
struct LexRule { std::string name, pattern; bool skip = false; Span span; };
/** A labeled BNF production; an empty body denotes epsilon. */
struct Production { std::string head; std::vector<std::string> body; std::string node; Span span; };
/** Validated, backend-independent frontend definition. */
struct Frontend {
    std::string name, start;
    std::vector<LexRule> lexer;
    std::vector<Production> productions;
    std::vector<std::string> nodes;
    std::map<std::string, std::string> lowering, options;
};
/** Owned source tree. Terminal text and all spans refer to original input. */
struct Tree { std::string name, text; Span span; bool terminal = false; std::vector<Tree> children; };
/** Facility hook receiving already-lowered nonterminal children in source order.
 * Return an i64 value owned by the supplied function. Terminal children remain
 * accessible through tree.children. Hooks may emit additional Jiterati IR.
 */
using LoweringHook = std::function<jiterati::Value(const Tree&, jiterati::Function&,
    jiterati::Block&, const std::vector<jiterati::Value>&)>;
/** Facility name to implementation; custom hooks override built-in hooks. */
using LoweringHooks = std::map<std::string, LoweringHook>;
/** Compile GateML text through the generated NovoParse/libglr parser. */
Frontend compile(const std::string& source);
/** Load one .gml file or concatenate the ordered files in a bundle MANIFEST. */
Frontend load(const std::filesystem::path& path);
/** Tokenize with libchomsky3 and parse using NovoParse/libglr; reject ambiguity. */
Tree parse(const Frontend& frontend, const std::string& source);
/** Lower a tree to a zero-argument i64 Jiterati function using declared hooks. */
std::unique_ptr<jiterati::Module> lower(const Frontend& frontend, const Tree& tree,
    const LoweringHooks& facilities = {});
/** Render deterministic NovoParse BNF over the frontend's token tape. */
std::string grammar(const Frontend& frontend);
/** Emit the version-1 JSON frontend descriptor for future facility backends. */
std::string descriptor(const Frontend& frontend);
/** Render an owned tree as JSON, including original source spans. */
std::string json(const Tree& tree);
}
