#pragma once

#if defined(_WIN32) && defined(MOONSTONE_SHARED)
#if defined(MOONSTONE_BUILDING_CORE)
#define MOONSTONE_API __declspec(dllexport)
#else
#define MOONSTONE_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(MOONSTONE_SHARED)
#define MOONSTONE_API __attribute__((visibility("default")))
#else
#define MOONSTONE_API
#endif

#define MOONSTONE_API_VERSION 1

#if defined(_WIN32)
#define MOONSTONE_C_CALL __cdecl
#else
#define MOONSTONE_C_CALL
#endif
