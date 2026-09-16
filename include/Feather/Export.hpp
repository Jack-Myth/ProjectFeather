#pragma once

#if defined(_WIN32) && defined(FEATHER_CORE_SHARED)
#if defined(FEATHER_CORE_BUILD)
#define FEATHER_API __declspec(dllexport)
#else
#define FEATHER_API __declspec(dllimport)
#endif
#else
#define FEATHER_API
#endif

#if defined(_WIN32) && defined(FEATHER_DEBUG_SHARED)
#if defined(FEATHER_DEBUG_BUILD)
#define FEATHER_DEBUG_API __declspec(dllexport)
#else
#define FEATHER_DEBUG_API __declspec(dllimport)
#endif
#else
#define FEATHER_DEBUG_API
#endif

#if defined(_WIN32)
#define FEATHER_NATIVE_EXPORT __declspec(dllexport)
#else
#define FEATHER_NATIVE_EXPORT __attribute__((visibility("default")))
#endif
