#pragma once
#include "Core.hpp"
#include "SExpr.hpp"
#if __has_include("../chisel/include/chisel/Chisel.hpp")
#include "../chisel/include/chisel/Chisel.hpp"
#else
#include <chisel/Chisel.hpp>
#endif
#if __has_include("../chisel/include/chisel/GC.hpp")
#include "../chisel/include/chisel/GC.hpp"
#else
#include <chisel/GC.hpp>
#endif
#if __has_include("../supersonic/include/supersonic/Supersonic.hpp")
#include "../supersonic/include/supersonic/Supersonic.hpp"
#else
#include <supersonic/Supersonic.hpp>
#endif
#if __has_include("../syren/include/syren/Syren.hpp")
#include "../syren/include/syren/Syren.hpp"
#else
#include <syren/Syren.hpp>
#endif
#if __has_include("../syren/include/syren/RuntimeDiag.hpp")
#include "../syren/include/syren/RuntimeDiag.hpp"
#else
#include <syren/RuntimeDiag.hpp>
#endif
#if __has_include("../exoml/include/exoml/ExoML.hpp")
#include "../exoml/include/exoml/ExoML.hpp"
#else
#include <exoml/ExoML.hpp>
#endif

// Plaid is header-only apart from its separately linked optional C ABI runtime.
#if __has_include("../plaid/Plaid.hpp")
#include "../plaid/Plaid.hpp"
#endif
