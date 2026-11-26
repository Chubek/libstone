#include "bootstrap.hpp"

#include <cstdint>
#include <memory>
#include <variant>

namespace ast = bootstrap_ast;

static_assert(ast::reflection::module == "Bootstrap");
static_assert(ast::reflection::types.size() == 8);
static_assert(ast::reflection::types[2].constructors[3].name == "NonemptySequence");
static_assert(ast::reflection::types[7].fields[1].qualifier ==
              ast::reflection::cardinality::nonempty_sequence);

int main()
{
    ast::type_ref type;
    type.value = ast::Named{"position"};
    type.value = ast::Applied{"list", {std::make_shared<ast::type_ref>(type)}};
    const auto& inner = std::get<ast::Applied>(type.value).arguments.at(0);
    return std::get<ast::Named>(inner->value).name == "position" ? 0 : 1;
}
