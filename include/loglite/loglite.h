// loglite - a small dependency-free C++17 asynchronous logging library
// Frontend: LOGLITE_LOG* macros -> formatted line -> queue
// Backend : ONE writer thread drains the queue in submission order and calls
//           every Sink (console / rolling file / callback / custom).
//
// submit() never performs I/O; only message/line formatting and a very short
// enqueue (sequence number + push) happen on the caller thread.
// Global ordering is guaranteed: the sequence number is allocated inside the
// same critical section as the enqueue, and a single consumer writes in FIFO
// order; seq is exposed to sinks for ordering checks but is NOT printed.
//
// Writes are batched: a file sink flushes after `flush_threshold()` bytes, the
// writer flushes all sinks every `flush_interval_ms()` milliseconds, and
// flush()/shutdown() force a synchronous flush. See set_flush_policy().
//
// ---------------- Default startup policy ----------------
//   level              : info (runtime); compile-time gate = 2 with NDEBUG,
//                        0 otherwise (override with LOGLITE_COMPILE_LEVEL)
//   format             : text, microsecond timestamps, seq internal only
//   sinks              : one console sink (OutputDebugStringA / stderr)
//   callback           : none
//   queue              : unbounded (capacity 0, never drops)
//   flush              : 64 KiB per file sink + 200 ms writer timer;
//                        flush() and shutdown() force a complete flush
//   file sink          : not created by default; add_file_sink_utf8 defaults
//                        to 4 MiB rolling size with 3 files
//   writer thread      : started lazily on first submit or initialize();
//                        shutdown() MUST be called before unloading a DLL
//   short LOG_* macros : disabled unless LOGLITE_ENABLE_SHORT_MACROS
//
// This library deliberately uses NO project-specific prefix. To give it a
// project prefix later, wrap the stable LOGLITE_LOG* macros from your own
// header; see <loglite/prefix.h>.
#ifndef LOGLITE_LOGLITE_H
#define LOGLITE_LOGLITE_H

#include <cstddef>
#include <cstdint>
#include <string_view>

#if defined(_WIN32) && defined(LOGLITE_BUILD_SHARED)
#  define LOGLITE_API __declspec(dllexport)
#elif defined(_WIN32) && defined(LOGLITE_USE_SHARED)
#  define LOGLITE_API __declspec(dllimport)
#else
#  define LOGLITE_API
#endif

#if defined(_MSVC_LANG)
#  if _MSVC_LANG < 201703L
#    error "loglite requires C++17 (/std:c++17)"
#  endif
#elif __cplusplus < 201703L
#  error "loglite requires C++17"
#endif

// Compile-time level gate. Levels below this are completely removed by
// `if constexpr` - their arguments are never evaluated.
//   trace=0 debug=1 info=2 warn=3 error=4 off=5
#ifndef LOGLITE_COMPILE_LEVEL
#  ifdef NDEBUG
#    define LOGLITE_COMPILE_LEVEL 2
#  else
#    define LOGLITE_COMPILE_LEVEL 0
#  endif
#endif

namespace loglite {

enum class Level : int {
    trace = 0,
    debug = 1,
    info = 2,
    warn = 3,
    error = 4,
    off = 5
};

enum class Format { text, json };

struct Record {
    std::uint64_t    seq;        // global submission order (internal, not printed)
    Level            level;
    std::uint64_t    epoch_us;   // microseconds since Unix epoch
    std::uint32_t    thread_id;
    std::string_view file;      // valid while this record is dispatched
    int              line;
    std::string_view message;   // UTF-8, valid while this record is dispatched
};

// ---------------- Backend extension point ----------------
class Sink {
public:
    virtual ~Sink() = default;
    // `formatted_line` is the complete, already formatted line (ends with '\n').
    // Called on the single writer thread, in submission order; no internal
    // lock is held. Implementations may call the logging API (it only enqueues).
    virtual void write(const Record& record, std::string_view formatted_line) = 0;
    // Called on the writer thread by the timer, by flush(), and on shutdown.
    // Implementations should push any buffered data towards the OS here.
    virtual void flush() {}
};

using LogCallback = void (*)(int level, const char* line, void* context);

// ---------------- Lifecycle ----------------
// The default state owns one console sink. `shutdown()` drains the queue,
// flushes all sinks, joins the writer thread, drops all sinks and silences
// logging. It MUST be called before a DLL containing loglite is unloaded.
LOGLITE_API void initialize() noexcept;
LOGLITE_API void shutdown() noexcept;
LOGLITE_API bool is_shutdown() noexcept;

// ---------------- Configuration (all thread-safe) ----------------
LOGLITE_API void        set_level(Level level) noexcept;
LOGLITE_API Level       get_level() noexcept;
LOGLITE_API bool        enabled(Level level) noexcept;
LOGLITE_API void        set_format(Format format) noexcept;
LOGLITE_API Format      get_format() noexcept;
LOGLITE_API void        reset_sinks() noexcept;
LOGLITE_API std::size_t sink_count() noexcept;

LOGLITE_API void add_console_sink() noexcept;
LOGLITE_API void add_file_sink_utf8(std::string_view utf8_path,
                                    std::uint64_t max_bytes = 100ull << 20, // 100 MB
                                    int max_files = 3) noexcept;
// `take_ownership=false` keeps the caller responsible for the sink's lifetime.
LOGLITE_API void add_sink(Sink* sink, bool take_ownership = true) noexcept;
LOGLITE_API void set_callback(LogCallback callback, void* context) noexcept;

// ---------------- Batched flush policy ----------------
// bytes       : file sinks flush after accumulating this many bytes
//               (default 64 KiB; 0 restores a flush after every line).
// interval_ms : the writer flushes all sinks on this fixed interval
//               (default 200 ms; 0 disables the timer).
// flush() and shutdown() always force a complete flush regardless of policy.
LOGLITE_API void        set_flush_policy(std::size_t bytes, unsigned interval_ms) noexcept;
LOGLITE_API std::size_t flush_threshold() noexcept;
LOGLITE_API unsigned    flush_interval_ms() noexcept;

// ---------------- Async queue control ----------------
// Wait until every record submitted before this call is written AND flushed.
LOGLITE_API void flush() noexcept;
LOGLITE_API std::size_t queue_size() noexcept;
LOGLITE_API std::uint64_t dropped_count() noexcept;
// 0 = unbounded (default, never drops). With a capacity, a full queue drops
// the NEW record (counted by dropped_count) so submit() never blocks on I/O.
LOGLITE_API void set_queue_capacity(std::size_t capacity) noexcept;

// ---------------- Frontend core (called by the macros) ----------------
LOGLITE_API void submit(Level level, std::string_view file, int line,
                        const char* fmt, ...) noexcept;

} // namespace loglite

// ---------------- C API (cross-module / FFI friendly) ----------------
extern "C" {
LOGLITE_API void loglite_initialize(void);
LOGLITE_API void loglite_shutdown(void);
LOGLITE_API void loglite_set_level(int level);
LOGLITE_API void loglite_set_format(int json);
LOGLITE_API void loglite_reset_sinks(void);
LOGLITE_API void loglite_add_console(void);
LOGLITE_API void loglite_add_file_utf8(const char* utf8_path,
                                       unsigned long long max_bytes, int max_files);
LOGLITE_API void loglite_flush(void);
LOGLITE_API unsigned long long loglite_dropped_count(void);
LOGLITE_API void loglite_set_queue_capacity(unsigned long long capacity);
LOGLITE_API unsigned long long loglite_queue_size(void);
LOGLITE_API void loglite_set_flush_policy(unsigned long long bytes,
                                          unsigned interval_ms);
}

// ---------------- Stable macros ----------------
// These names carry only the library name, never a project prefix.
#define LOGLITE_LOG(level, ...)                                          \
    do {                                                                  \
        if constexpr (static_cast<int>(level) >= LOGLITE_COMPILE_LEVEL) { \
            if (::loglite::enabled(level)) {                              \
                ::loglite::submit(level, __FILE__, __LINE__, __VA_ARGS__); \
            }                                                             \
        }                                                                 \
    } while (0)

#define LOGLITE_LOG_TRACE(...) LOGLITE_LOG(::loglite::Level::trace, __VA_ARGS__)
#define LOGLITE_LOG_DEBUG(...) LOGLITE_LOG(::loglite::Level::debug, __VA_ARGS__)
#define LOGLITE_LOG_INFO(...)  LOGLITE_LOG(::loglite::Level::info,  __VA_ARGS__)
#define LOGLITE_LOG_WARN(...)  LOGLITE_LOG(::loglite::Level::warn,  __VA_ARGS__)
#define LOGLITE_LOG_ERROR(...) LOGLITE_LOG(::loglite::Level::error, __VA_ARGS__)

// Short macros are OFF by default: they collide with spdlog / easylogging++ /
// glog. Opt in per project, each guarded so an existing definition wins.
#ifdef LOGLITE_ENABLE_SHORT_MACROS
#  ifndef LOG_TRACE
#    define LOG_TRACE(...) LOGLITE_LOG_TRACE(__VA_ARGS__)
#  endif
#  ifndef LOG_DEBUG
#    define LOG_DEBUG(...) LOGLITE_LOG_DEBUG(__VA_ARGS__)
#  endif
#  ifndef LOG_INFO
#    define LOG_INFO(...) LOGLITE_LOG_INFO(__VA_ARGS__)
#  endif
#  ifndef LOG_WARN
#    define LOG_WARN(...) LOGLITE_LOG_WARN(__VA_ARGS__)
#  endif
#  ifndef LOG_ERROR
#    define LOG_ERROR(...) LOGLITE_LOG_ERROR(__VA_ARGS__)
#  endif
#endif

#endif // LOGLITE_LOGLITE_H
