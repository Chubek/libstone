#pragma once
// Independent feature probes for LibStone's vendored third_party checkout.
// Existence is not evidence of ABI compatibility or implemented integration.
#if __has_include(<memtkx/MemTKX.hpp>)
#define MOONSTONE_VENDOR_MEMTKX 1
#else
#define MOONSTONE_VENDOR_MEMTKX 0
#endif
#if __has_include(<Satie.hpp>)
#define MOONSTONE_VENDOR_SATIE 1
#else
#define MOONSTONE_VENDOR_SATIE 0
#endif
#if __has_include(<EquinoxNG.hpp>)
#define MOONSTONE_VENDOR_EQUINOX 1
#else
#define MOONSTONE_VENDOR_EQUINOX 0
#endif
#if __has_include(<ExtensionTk.h>)
#define MOONSTONE_VENDOR_EXOLANGTK 1
#else
#define MOONSTONE_VENDOR_EXOLANGTK 0
#endif
