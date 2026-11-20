#ifndef CEPHYR_DRIVER_HPP
#define CEPHYR_DRIVER_HPP

#include <iosfwd>

namespace cephyr {

// The CLI alone connects the driver to process-standard streams.
extern "C" int cephyr_cli_main(int argc, const char *const *argv, std::istream &input,
                              std::ostream &output, std::ostream &diagnostics);

} // namespace cephyr

#endif
