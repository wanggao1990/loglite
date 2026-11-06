// Optional prefix helpers for loglite.
//
// The library itself defines only LOGLITE_LOG* macros. A project adds its own
// prefix with one thin wrapper header, e.g. "proj_log.h":
//
//   #pragma once
//   #include <loglite/loglite.h>
//   #define PROJ_LOG(...)       LOGLITE_EXPAND(LOGLITE_LOG(__VA_ARGS__))
//   #define PROJ_LOG_INFO(...)  LOGLITE_LOG_INFO(__VA_ARGS__)
//   #define PROJ_LOG_WARN(...)  LOGLITE_LOG_WARN(__VA_ARGS__)
//   #define PROJ_LOG_ERROR(...) LOGLITE_LOG_ERROR(__VA_ARGS__)
//
// The C preprocessor cannot generate a #define directive from a macro argument,
// so the wrapper header is the portable way to prefix macro names. For C++
// identifiers (namespace aliases, functions, enums) the token-pasting helpers
// below do generate prefixed names automatically:
//
//   namespace LOGLITE_PREFIXED(PROJ, log) = loglite;   // -> PROJ_log
//   #define TABLE_ROW(suffix, lvl) { ::loglite::Level::lvl, #suffix },
//   static const std::pair<Level, const char*> table[] = {
//       LOGLITE_LEVEL_LIST(TABLE_ROW)
//   };
//
// Both forms are exercised by loglite_tests.cpp (`test_prefix_helpers`).
#ifndef LOGLITE_PREFIX_H
#define LOGLITE_PREFIX_H

#include <loglite/loglite.h>

#define LOGLITE_CAT_IMPL(a, b) a##b
#define LOGLITE_CAT(a, b) LOGLITE_CAT_IMPL(a, b)

// Extra expansion layer: MSVC's legacy preprocessor does not split __VA_ARGS__
// when a variadic wrapper forwards every argument to LOGLITE_LOG.
#define LOGLITE_EXPAND(...) __VA_ARGS__

// LOGLITE_PREFIXED(PROJ, LOG_INFO) -> PROJ_LOG_INFO
#define LOGLITE_PREFIXED(prefix, name) LOGLITE_CAT(LOGLITE_CAT(prefix, _), name)

// X(SUFFIX, level_enum) for every loggable level.
#define LOGLITE_LEVEL_LIST(X)   \
    X(TRACE, trace)             \
    X(DEBUG, debug)             \
    X(INFO, info)               \
    X(WARN, warn)               \
    X(ERROR, error)

#endif // LOGLITE_PREFIX_H
