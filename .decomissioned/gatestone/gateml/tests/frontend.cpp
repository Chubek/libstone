#include <gateml/GateML.hpp>
#include <iostream>
#include <functional>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
gateml::Error rejected(const std::function<void()>& operation) {
    try { operation(); }
    catch (const gateml::Error& error) { return error; }
    throw std::runtime_error("expected a located failure");
}
const gateml::Tree* find(const gateml::Tree& tree, const std::string& name) {
    if (tree.name == name) return &tree;
    for (const auto& child : tree.children) if (auto* result = find(child, name)) return result;
    return nullptr;
}
std::vector<jiterati::Opcode> entry_opcodes(const jiterati::Function& function) {
    std::vector<jiterati::Opcode> opcodes;
    for (auto* instruction : function.entry_block()->instructions()) opcodes.push_back(instruction->opcode());
    return opcodes;
}
gateml::LoweringHooks demo_facilities() {
    gateml::LoweringHooks hooks;
    hooks["variable"] = [](const gateml::Tree&, jiterati::Function& function, jiterati::Block&,
        const std::vector<jiterati::Value>&) { return function.const_i64(7); };
    hooks["assign"] = [](const gateml::Tree& node, jiterati::Function&, jiterati::Block&,
        const std::vector<jiterati::Value>& values) {
        if (values.size() != 1) throw gateml::Error("demo assign expects one value", node.span);
        return values[0];
    };
    hooks["call"] = [](const gateml::Tree& node, jiterati::Function&, jiterati::Block&,
        const std::vector<jiterati::Value>& values) {
        if (values.size() != 2) throw gateml::Error("demo call expects receiver and argument", node.span);
        return values[1];
    };
    hooks["callempty"] = [](const gateml::Tree&, jiterati::Function& function, jiterati::Block&,
        const std::vector<jiterati::Value>&) { return function.const_i64(0); };
    return hooks;
}
const char* minimal = R"(frontend Tiny;
lexer { token NUMBER = "[0-9]+"; }
parser { start expression; rule expression = NUMBER => Number; }
ast { node Number; }
adapter { lower Number = integer; }
)";
}

int main() {
    try {
        auto demo = demo_facilities();

        auto frontend = gateml::load(std::string(GATEBINDLE_DIR) + "/C");
        require(frontend.name == "C" && frontend.productions.size() == 12, "bundle declarations lost");
        auto tree = gateml::parse(frontend, " 12 + 3 * (4 - 2) // comment\n");
        require(tree.name == "Add", "expression precedence/root");
        auto* multiply = find(tree, "Multiply");
        require(multiply && find(*multiply, "Subtract"), "multiplication/group precedence");
        auto* integer = find(tree, "INTEGER");
        require(integer && integer->text == "12" && integer->span.begin == 1 && integer->span.end == 3,
            "terminal lexeme/source span");
        auto module = gateml::lower(frontend, tree);
        require(module->functions().size() == 1 && module->functions()[0]->const_count() == 4, "Jiterati constants");
        require(entry_opcodes(*module->functions()[0]) == std::vector<jiterati::Opcode>{jiterati::Opcode::Sub,
            jiterati::Opcode::Mul, jiterati::Opcode::Add, jiterati::Opcode::Ret}, "Jiterati lowering order");
        auto subtraction = gateml::parse(frontend, "10-3-2");
        require(subtraction.name == "Subtract" && subtraction.children[0].name == "Subtract", "left associativity");
        auto comments = gateml::parse(frontend, "/* gap */\n-123");
        require(comments.name == "Pass" && find(comments, "Negate"), "comments/unary parsing");
        require(find(comments, "INTEGER")->span.line == 2, "line mapping");
        auto error = rejected([&] { gateml::parse(frontend, "1+\n@"); });
        require(error.span.line == 2 && error.span.column == 1, "lexical diagnostics");
        error = rejected([&] { gateml::parse(frontend, "1+\n"); });
        require(error.span.begin == 3 && error.span.line == 2, "EOF diagnostic mapping");
        rejected([&] { gateml::lower(frontend, gateml::parse(frontend, "9223372036854775808")); });

        auto cpp = gateml::load(std::string(GATEBINDLE_DIR) + "/C++");
        require(cpp.name == "Cpp" && cpp.productions.size() == 28, "C++ bundle declarations");
        auto assigned = gateml::parse(cpp, "x = 1 + 2 * 3");
        require(assigned.name == "Assign" && find(assigned, "Add") && find(assigned, "Multiply"), "C++ assignment");
        rejected([&] { gateml::lower(cpp, assigned); });
        auto assigned_module = gateml::lower(cpp, assigned, demo);
        require(entry_opcodes(*assigned_module->functions()[0]) == std::vector<jiterati::Opcode>{
            jiterati::Opcode::Mul, jiterati::Opcode::Add, jiterati::Opcode::Ret}, "C++ assignment lowering");
        auto shifted = gateml::parse(cpp, "3 << 1 | 2");
        require(shifted.name == "Pass" && find(shifted, "BitOr") && find(shifted, "Shl"), "C++ shift/bitwise precedence");
        auto shifted_module = gateml::lower(cpp, shifted);
        require(entry_opcodes(*shifted_module->functions()[0]) == std::vector<jiterati::Opcode>{
            jiterati::Opcode::Shl, jiterati::Opcode::Or, jiterati::Opcode::Ret}, "C++ shift/bitwise lowering");
        auto inverted = gateml::lower(cpp, gateml::parse(cpp, "~5"));
        require(find(gateml::parse(cpp, "~5"), "BitNot"), "C++ bitwise negation parsing");
        require(entry_opcodes(*inverted->functions()[0]) == std::vector<jiterati::Opcode>{
            jiterati::Opcode::Not, jiterati::Opcode::Ret}, "C++ bitwise negation");
        auto called = gateml::parse(cpp, "f(4) + 1");
        require(find(called, "Add") && find(called, "Call"), "C++ call parsing");
        auto called_module = gateml::lower(cpp, called, demo);
        require(called_module->functions()[0]->const_count() == 3, "C++ call constants");
        require(entry_opcodes(*called_module->functions()[0]) == std::vector<jiterati::Opcode>{
            jiterati::Opcode::Add, jiterati::Opcode::Ret}, "C++ call lowering");
        auto empty_call = gateml::parse(cpp, "f()");
        require(find(empty_call, "CallEmpty"), "C++ empty call");
        auto empty_module = gateml::lower(cpp, empty_call, demo);
        require(entry_opcodes(*empty_module->functions()[0]) == std::vector<jiterati::Opcode>{
            jiterati::Opcode::Ret}, "C++ empty call lowering");
        require(find(gateml::parse(cpp, "/* c */ 1 // trailing"), "INTEGER"), "C++ comments");

        auto java = gateml::load(std::string(GATEBINDLE_DIR) + "/Java");
        require(java.name == "Java" && java.productions.size() == 29, "Java bundle declarations");
        auto unsigned_shift = gateml::parse(java, "8 >>> 1");
        require(find(unsigned_shift, "Urshr"), "Java unsigned shift");
        auto unsigned_module = gateml::lower(java, unsigned_shift);
        require(entry_opcodes(*unsigned_module->functions()[0]) == std::vector<jiterati::Opcode>{
            jiterati::Opcode::LShr, jiterati::Opcode::Ret}, "Java unsigned shift lowering");
        auto signed_shift = gateml::lower(java, gateml::parse(java, "8 >> 1"));
        require(entry_opcodes(*signed_shift->functions()[0]) == std::vector<jiterati::Opcode>{
            jiterati::Opcode::AShr, jiterati::Opcode::Ret}, "Java signed shift lowering");
        auto java_assigned = gateml::parse(java, "x = 1");
        require(java_assigned.name == "Assign", "Java assignment");
        gateml::lower(java, java_assigned, demo);

        auto rust = gateml::load(std::string(GATEBINDLE_DIR) + "/Rust");
        require(rust.name == "Rust" && rust.productions.size() == 28, "Rust bundle declarations");
        auto banged = gateml::parse(rust, "!5");
        require(banged.name == "Pass" && find(banged, "BitNot"), "Rust negation operator");
        auto banged_module = gateml::lower(rust, banged);
        require(entry_opcodes(*banged_module->functions()[0]) == std::vector<jiterati::Opcode>{
            jiterati::Opcode::Not, jiterati::Opcode::Ret}, "Rust negation lowering");
        auto rust_assigned = gateml::parse(rust, "x = 2 & 3");
        require(rust_assigned.name == "Assign" && find(rust_assigned, "BitAnd"), "Rust assignment");
        auto rust_module = gateml::lower(rust, rust_assigned, demo);
        require(entry_opcodes(*rust_module->functions()[0]) == std::vector<jiterati::Opcode>{
            jiterati::Opcode::And, jiterati::Opcode::Ret}, "Rust bitwise lowering");

        auto python = gateml::load(std::string(GATEBINDLE_DIR) + "/Python");
        require(python.name == "Python" && python.productions.size() == 31, "Python bundle declarations");
        auto power = gateml::parse(python, "2 ** 3 ** 2");
        auto* power_node = find(power, "Power");
        require(power_node && power_node->children.size() == 3 &&
            !find(power_node->children[0], "Power") && find(power_node->children[2], "Power"),
            "Python right-associative power");
        auto negated_power = gateml::parse(python, "-2 ** 2");
        require(find(negated_power, "Negate") && find(negated_power, "Power"), "Python power precedence");
        rejected([&] { gateml::lower(python, power); });
        auto floored = gateml::lower(python, gateml::parse(python, "7 // 2"));
        require(entry_opcodes(*floored->functions()[0]) == std::vector<jiterati::Opcode>{
            jiterati::Opcode::SDiv, jiterati::Opcode::Ret}, "Python floor division lowering");
        auto python_assigned = gateml::parse(python, "x = 1 # comment");
        require(python_assigned.name == "Assign", "Python assignment/comment");
        gateml::lower(python, python_assigned, demo);

        auto ruby = gateml::load(std::string(GATEBINDLE_DIR) + "/Ruby");
        require(ruby.name == "Ruby" && ruby.productions.size() == 30, "Ruby bundle declarations");
        auto ruby_power = gateml::parse(ruby, "2 ** 3");
        require(find(ruby_power, "Power"), "Ruby power");
        rejected([&] { gateml::lower(ruby, ruby_power); });
        auto ruby_modulo = gateml::lower(ruby, gateml::parse(ruby, "7 % 2 # comment"));
        require(entry_opcodes(*ruby_modulo->functions()[0]) == std::vector<jiterati::Opcode>{
            jiterati::Opcode::SRem, jiterati::Opcode::Ret}, "Ruby remainder lowering");

        auto delphi = gateml::load(std::string(GATEBINDLE_DIR) + "/Delphi");
        require(delphi.name == "Delphi" && delphi.productions.size() == 29, "Delphi bundle declarations");
        auto intdiv = gateml::parse(delphi, "10 div 3");
        require(find(intdiv, "IntDiv"), "Delphi integer division");
        require(find(gateml::parse(delphi, "10 DIV 3"), "IntDiv"), "Delphi case-insensitive keywords");
        require(find(gateml::parse(delphi, "1 SHL 2"), "Shl"), "Delphi case-insensitive shift");
        auto modulo = gateml::lower(delphi, gateml::parse(delphi, "10 mod 3"));
        require(entry_opcodes(*modulo->functions()[0]) == std::vector<jiterati::Opcode>{
            jiterati::Opcode::SRem, jiterati::Opcode::Ret}, "Delphi modulo lowering");
        auto walrus = gateml::parse(delphi, "x := 1 + 2");
        require(walrus.name == "Assign" && find(walrus, "Add"), "Delphi walrus assignment");
        auto walrus_module = gateml::lower(delphi, walrus, demo);
        require(entry_opcodes(*walrus_module->functions()[0]) == std::vector<jiterati::Opcode>{
            jiterati::Opcode::Add, jiterati::Opcode::Ret}, "Delphi assignment lowering");
        auto logical_not = gateml::lower(delphi, gateml::parse(delphi, "not 1"));
        require(find(gateml::parse(delphi, "not 1"), "BitNot"), "Delphi negation parsing");
        require(entry_opcodes(*logical_not->functions()[0]) == std::vector<jiterati::Opcode>{
            jiterati::Opcode::Not, jiterati::Opcode::Ret}, "Delphi negation lowering");
        require(find(gateml::parse(delphi, "{c} 1 (* d *) // e"), "INTEGER"), "Delphi comments");
        auto word = gateml::parse(delphi, "divide");
        require(find(word, "Variable") && find(word, "IDENT")->text == "divide", "Delphi keyword prefix lexing");

        auto tiny = gateml::compile(minimal);
        auto owned = gateml::parse(tiny, "42");
        gateml::parse(tiny, "7");
        require(owned.children[0].text == "42", "tree lifetime");
        require(gateml::grammar(tiny).find("expression: \"A\" => Number;") != std::string::npos, "token tape grammar");
        tiny.lowering["Number"] = "external_integer";
        rejected([&] { gateml::lower(tiny, owned); });
        gateml::LoweringHooks facilities;
        facilities["external_integer"] = [](const gateml::Tree& node, jiterati::Function& function,
            jiterati::Block&, const std::vector<jiterati::Value>&) {
            return function.const_i64(std::stoll(node.children[0].text) + 1);
        };
        auto extended = gateml::lower(tiny, owned, facilities);
        require(std::get<int64_t>(extended->functions()[0]->const_value(0)) == 43, "facility callback");
        rejected([] { gateml::compile("frontend Bad; lexer { token X = \"a*\"; } parser { start x; rule x = X => N; } ast { node N; }"); });
        rejected([] { gateml::compile("frontend Bad; lexer { token X = \"[\"; } parser { start x; rule x = X => N; } ast { node N; }"); });
        rejected([] { gateml::compile("frontend Bad; lexer { token X = \"a\"; } parser { start x; rule x = missing => N; } ast { node N; }"); });
        rejected([] { gateml::compile("frontend Bad; lexer { token X = \"a\"; } parser { start x; rule x = x => N; } ast { node N; }"); });
        rejected([] { gateml::compile("frontend Bad; lexer { token X = \"a\"; } parser { start x; rule x = X => Missing; } ast { node N; }"); });
        rejected([] { gateml::compile("frontend Bad; lexer { token X = \"a\"; } parser { start x; rule x = X => N; } ast { node N; } adapter { lower N = add; }"); });
        rejected([] { gateml::compile("frontend Tiny;\nlexer { token X = \"a\" }"); });
        rejected([] { gateml::compile(std::string("frontend Tiny;\0", 15)); });
        auto ambiguous = gateml::compile(R"(frontend Ambiguous;
lexer { token X = "a"; }
parser { start expression; rule expression = expression expression => Pair | X => Atom; }
ast { node Pair; node Atom; })");
        rejected([&] { gateml::parse(ambiguous, "aaa"); });
        auto epsilon = gateml::compile(R"(frontend Empty;
lexer { token X = "a"; }
parser { start empty; rule empty = => Empty; }
ast { node Empty; })");
        require(gateml::parse(epsilon, "").name == "Empty", "epsilon production");
        auto overlap = gateml::compile(R"(frontend Overlap;
lexer { token WORD = "a|ab"; token SHORT = "a"; }
parser { start word; rule word = WORD => Word; }
ast { node Word; })");
        require(gateml::parse(overlap, "ab").children[0].text == "ab", "maximal regex alternation");
        require(gateml::parse(overlap, "a").children[0].name == "WORD", "lexical tie priority");
        std::cout << "GateML frontend tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
