#pragma once
// This header deliberately names the vendored MemTKX API and remains optional.
#if __has_include(<memtkx/MemTKX.hpp>)
#include <memtkx/MemTKX.hpp>
#define MOONSTONE_HAS_MEMTKX 1
namespace moonstone::chisel { namespace vendored_memtkx = ::MemTKX; }
#else
#define MOONSTONE_HAS_MEMTKX 0
#endif
