#include "parser.hpp"
#include "ld.hpp"
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>

static void check(bool ok, const std::string& message) {
  if (!ok) throw std::runtime_error(message);
}
static void expressions() {
  for (const auto& [source, expected] : std::vector<std::pair<std::string, std::string>>{
    {"1+2*3;", "(1 + (2 * 3))"},
    {"(1+2)*3;", "((1 + 2) * 3)"},
    {"1 || 0 && 2 | 3 ^ 4 & 5;", "(1 || (0 && (2 | (3 ^ (4 & 5)))))"},
    {"3 <= 4 == 1;", "((3 <= 4) == 1)"},
    {"1 ? 2 : 3 ? 4 : 5;", "(1 ? 2 : (3 ? 4 : 5))"},
    {"ALIGN(/* comment */ 16) + SIZEOF(.text);", "(ALIGN(16) + SIZEOF(.text))"},
    {"-1 + ~2 + !0;", "(((-1) + (~2)) + (!0))"},
    {"1K + 0xff + 077;", "((1024 + 255) + 63)"},
    {"0xffffffffffffffff;", "18446744073709551615"},
    {"name-other;", "(name - other)"},
    {"ADDR(\".text-with-dash\");", "ADDR(.text-with-dash)"},
  }) {
    size_t used = 0;
    auto expr = qld::script::parseExpression(source.c_str(), used);
    check(expr && expr->dump() == expected, source + ": " + (expr ? expr->dump() : "<null>"));
    check(used == source.size() - 1, "incorrect expression boundary: " + source);
  }
}
int main() try {
  expressions();
  // The generated C parser has globals; the bridge serializes parsing but
  // translates separate arena-owned results independently.
  std::vector<std::future<void>> jobs;
  for (int i = 0; i < 8; ++i)
    jobs.push_back(std::async(std::launch::async, [] { for (int j = 0; j < 20; ++j) expressions(); }));
  for (auto& job : jobs) job.get();
  for (const char *bad : {"x = 1+;", "x = MAX(1,,2);", "x = 18446744073709551616;",
                         "x = 0xGG;", "x = 1 ? 2;", "x = (1;", "x = ALIGN(1,);"})
    check(!qld::checkScript(bad), std::string("accepted malformed expression: ") + bad);
  check(!qld::checkScript("x = " + std::string(600, '(') + "1" + std::string(600, ')') + ";"),
        "unbounded nesting");
  std::cout << "generated linker expression frontend regressions passed\n";
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
