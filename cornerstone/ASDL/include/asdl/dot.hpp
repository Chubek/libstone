#ifndef ASDL_DOT_HPP
#define ASDL_DOT_HPP

#include <string>

namespace asdl {

struct Schema;

enum class DotView { schema, dependencies, imports };

// Render a validated schema. Uses declaration order and stable numeric IDs.
std::string emit_dot(const Schema &schema, DotView view = DotView::schema);

} // namespace asdl

#endif
