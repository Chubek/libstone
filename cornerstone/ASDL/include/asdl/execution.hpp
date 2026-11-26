#ifndef ASDL_EXECUTION_HPP
#define ASDL_EXECUTION_HPP

struct lua_State;

namespace asdl {

struct Schema;

// Installs execution on the session's global `asdl` table, after the native
// node API and signed-library loader are installed. Pushes the private Lua
// controller as its only stack result. Call once per session. Throws on
// bootstrap or schema-hook errors; discard the session if installation fails.
void push_execution(lua_State* lua, const Schema& schema);

} // namespace asdl

#endif
