#pragma once

namespace moonstone::mahak {
inline namespace v1 {

// Process entry point for the `mahak` tool. Returns one of the documented exit
// codes: 0 success, 1 internal error, 2 configuration invalid, 3 execution
// failure, 4 confirmed regression (compare --fail-on-regression), 5 not found.
int cli_main(int argc, char **argv);

} // namespace v1
} // namespace moonstone::mahak
