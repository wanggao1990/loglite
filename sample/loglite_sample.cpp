// Usage sample for loglite.
// Build with the standalone solution loglite.sln, or with CMake target
// loglite_sample. This file is intentionally self-contained: it shows the
// whole lifecycle from configuration to shutdown.
#define LOGLITE_COMPILE_LEVEL 0        // keep TRACE/DEBUG in this sample
#include <loglite/loglite.h>
#include <loglite/prefix.h>

// ---- Project-side prefixing (the library itself has no project prefix) ----
// Replace APP_ with your own prefix when integrating into another project.
#define APP_LOG(...)       LOGLITE_EXPAND(LOGLITE_LOG(__VA_ARGS__))
#define APP_LOG_INFO(...)  LOGLITE_LOG_INFO(__VA_ARGS__)
#define APP_LOG_WARN(...)  LOGLITE_LOG_WARN(__VA_ARGS__)
#define APP_LOG_ERROR(...) LOGLITE_LOG_ERROR(__VA_ARGS__)
namespace LOGLITE_PREFIXED(APP, log) = loglite;   // -> APP_log

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

// A custom backend: the writer thread calls write() in submission order.
class ErrorCounter final : public loglite::Sink {
public:
    std::atomic<int> errors{ 0 };
    void write(const loglite::Record& r, std::string_view) override {
        if (r.level == loglite::Level::error) {
            errors.fetch_add(1, std::memory_order_relaxed);
        }
    }
};

int main() {
    {

        LOGLITE_LOG_INFO("-----------------------------------%s", "===");
        return 0;
    }

    //return 0;


    // 1) Configure. initialize() starts the single writer thread lazily.
    APP_log::initialize();
    APP_log::reset_sinks();                       // drop the default console sink
    APP_log::set_level(loglite::Level::trace);
    APP_log::set_format(loglite::Format::text);
    APP_log::set_flush_policy(64 * 1024, 200);    // 64 KiB batch + 200 ms timer
    APP_log::set_queue_capacity(0);               // unbounded: never drop

    // 2) Attach backends: a rolling UTF-8 file sink and a custom sink.
    const auto log_path =
        std::filesystem::temp_directory_path() / "loglite_sample.log";
    std::error_code ec;
    std::filesystem::remove(log_path, ec);
    APP_log::add_file_sink_utf8(log_path.u8string(), 4ull << 20, 3);

    ErrorCounter counter;
    APP_log::add_sink(&counter, false);           // caller keeps ownership

    // 3) Log at every level, with project-prefixed and library-native macros.
    APP_LOG_INFO("sample start: %s", log_path.u8string().c_str());
    LOGLITE_LOG_TRACE("trace %d", 1);
    LOGLITE_LOG_DEBUG("debug %.3f", 1.5);
    APP_LOG_WARN("flush policy: %zu bytes / %u ms",
                 APP_log::flush_threshold(), APP_log::flush_interval_ms());
    APP_LOG_INFO("image %dx%d score=%.2f", 512, 512, 12.34);

    // 4) Concurrent producers: each thread's own records stay in order.
    constexpr int kThreads = 4;
    constexpr int kLines = 100;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([t] {
            for (int i = 0; i < kLines; ++i) {
                APP_LOG_INFO("worker=%d frame=%d", t, i);
            }
        });
    }
    for (auto& w : workers) w.join();

    APP_LOG_ERROR("simulated failure: %s", "camera timeout");
    APP_LOG(loglite::Level::info, "generic prefixed entry %d", 42);

    // 5) flush() is the synchronous point: after it, the file is complete.
    APP_log::flush();
    std::ifstream in(log_path, std::ios::binary);
    // Extra parentheses avoid the "most vexing parse" (function declaration).
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    const auto line_count =
        std::count(content.begin(), content.end(), '\n');
    const int errors = counter.errors.load();
    const auto dropped = APP_log::dropped_count();

    constexpr long kExpectedLines = 5 + kThreads * kLines + 2;
    std::cout << "loglite sample\n"
              << "log file : " << log_path.u8string() << "\n"
              << "lines    : " << line_count << " (expected "
              << kExpectedLines << ")\n"
              << "errors   : " << errors << "\n"
              << "dropped  : " << dropped << "\n"
              << "queued   : " << APP_log::queue_size() << "\n";

    // 6) Always shut down before process/DLL exit: drains, flushes, joins.
    APP_log::shutdown();

    const bool ok = (line_count == kExpectedLines) && errors == 1 && dropped == 0;
    std::cout << "status   : " << (ok ? "OK" : "FAILED") << "\n";
    return ok ? 0 : 1;
}
