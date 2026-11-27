#include "LibDSL.hpp"

dsl::SExpr from_peer(std::string_view source)
{
  return dsl::parse_sexpr(source);
}

std::string render_from_peer(std::string_view source)
{
  dsl::terminal::StyledText text;
  return text.text(source).render();
}
