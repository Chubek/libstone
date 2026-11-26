#include "calc.hpp"

#include <cassert>

using namespace calc_ast;

static_assert(reflection::module == "Calculator");
static_assert(reflection::types.size() == 3);
static_assert(reflection::types[0].fields[0].qualifier == reflection::cardinality::nonempty_sequence);
static_assert(reflection::types[1].attributes[0].qualifier == reflection::cardinality::optional);
static_assert(reflection::types[1].constructors[0].cpp_name == "calc_ast::Number");
static_assert(reflection::types[2].constructors[0].tag == static_cast<std::size_t>(op::Plus));

int main()
{
    auto answer = std::make_shared<expr>(expr{Number{42}, 10});
    document tree{{answer}, "demo"};
    assert(tree.forms[0] == answer);
    assert(std::get<Number>(tree.forms[0]->value).value == 42);
    assert(tree.forms[0]->offset == 10);
}
