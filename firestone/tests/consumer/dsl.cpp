#include <LibDSL.hpp>

int main()
{
  auto program = dsl::cli::OptParser::from_text("(program installed (flag verbose (short v)))");
  auto parsed = program.parse({"-v"});
  return parsed && parsed.has("verbose") ? 0 : 1;
}
