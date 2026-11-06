// Self-contained test suite for loglite. No external test framework.
// Build: cmake -S . -B build && cmake --build build && ctest --test-dir build
#define LOGLITE_COMPILE_LEVEL 0
#define LOGLITE_ENABLE_SHORT_MACROS
#include <loglite/loglite.h>
#include <loglite/prefix.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// ---- Project-side prefixing (added later, never inside the library) ----
#define PROJ_LOG(...)      LOGLITE_EXPAND(LOGLITE_LOG(__VA_ARGS__))
#define PROJ_LOG_INFO(...) LOGLITE_LOG_INFO(__VA_ARGS__)
namespace LOGLITE_PREFIXED(PROJ, log) = loglite;

static int g_checks = 0;
static int g_failures = 0;

// Every assertion is a synchronisation point: wait for the async writer so the
// assertion observes a fully drained queue.
#define CHECK(expr)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        ::loglite::flush();                                                \
        const bool loglite_check_ok_ = (expr);                             \
        if (!loglite_check_ok_) {                                          \
            ++g_failures;                                                  \
            std::cerr << "[FAIL] " #expr " @" << __FILE__ << ":"          \
                      << __LINE__ << "\n";                                 \
        }                                                                  \
    } while (0)

struct CountingSink final : loglite::Sink {
    std::atomic<long> lines{ 0 };
    std::atomic<long> bytes{ 0 };
    void write(const loglite::Record&, std::string_view line) override {
        lines.fetch_add(1, std::memory_order_relaxed);
        bytes.fetch_add(static_cast<long>(line.size()), std::memory_order_relaxed);
    }
};

static std::filesystem::path test_dir() {
    static const std::filesystem::path dir = [] {
        auto d = std::filesystem::temp_directory_path() / "loglite_tests";
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        return d;
    }();
    return dir;
}

static std::string read_file(const std::filesystem::path& p) {
    ::loglite::flush();
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

static std::vector<std::string> lines_of(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < s.size()) {
        const auto pos = s.find('\n', start);
        if (pos == std::string::npos) {
            out.emplace_back(s.substr(start));
            break;
        }
        out.emplace_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

static void clean(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
    for (int i = 1; i <= 5; ++i) {
        std::filesystem::path q = p;
        q += ".";
        q += std::to_string(i);
        std::filesystem::remove(q, ec);
    }
}

static void fresh() {
    ::loglite::flush();
    ::loglite::initialize();
    ::loglite::reset_sinks();
    ::loglite::set_callback(nullptr, nullptr);
    ::loglite::set_level(loglite::Level::trace);
    ::loglite::set_format(loglite::Format::text);
    ::loglite::set_queue_capacity(0);
    ::loglite::set_flush_policy(64 * 1024, 200);   // restore defaults
    CHECK(loglite::sink_count() == 0);
}

// ------------------------------------------------------------ basic tests ----
static void test_text_format_and_levels() {
    fresh();
    CountingSink c;
    loglite::add_sink(&c, false);
    CHECK(loglite::sink_count() == 1);

    LOGLITE_LOG_TRACE("trace %d", 1);
    LOGLITE_LOG_DEBUG("debug %d", 2);
    LOGLITE_LOG_INFO("info %d", 3);
    LOGLITE_LOG_WARN("warn %d", 4);
    LOGLITE_LOG_ERROR("error %d", 5);
    LOG_INFO("short macro %d", 6);   // LOGLITE_ENABLE_SHORT_MACROS

    CHECK(c.lines.load() == 6);
    CHECK(loglite::get_level() == loglite::Level::trace);
    CHECK(loglite::get_format() == loglite::Format::text);
    fresh();
}

static void test_runtime_level_filter() {
    fresh();
    CountingSink c;
    loglite::add_sink(&c, false);
    loglite::set_level(loglite::Level::warn);
    CHECK(!loglite::enabled(loglite::Level::info));
    CHECK(loglite::enabled(loglite::Level::warn));

    LOGLITE_LOG_TRACE("dropped");
    LOGLITE_LOG_INFO("dropped");
    LOGLITE_LOG_WARN("kept warn");
    LOGLITE_LOG_ERROR("kept error");
    CHECK(c.lines.load() == 2);

    loglite::set_level(loglite::Level::off);
    LOGLITE_LOG_ERROR("dropped too");
    CHECK(c.lines.load() == 2);
    fresh();
}

static void test_compile_time_filter() {
    fresh();
    CountingSink c;
    loglite::add_sink(&c, false);
    loglite::set_level(loglite::Level::trace);

    int side_effect = 0;
    auto bump = [&side_effect]() -> int { ++side_effect; return 7; };

#undef LOGLITE_COMPILE_LEVEL
#define LOGLITE_COMPILE_LEVEL 4   // error only
    LOGLITE_LOG_INFO("dropped at compile time %d", bump());
    CHECK(side_effect == 0);
    CHECK(c.lines.load() == 0);
    LOGLITE_LOG_ERROR("kept %d", bump());
    CHECK(side_effect == 1);
    CHECK(c.lines.load() == 1);
#undef LOGLITE_COMPILE_LEVEL
#define LOGLITE_COMPILE_LEVEL 0
    fresh();
}

static void test_custom_sink_and_reset() {
    fresh();
    CountingSink a, b;
    loglite::add_sink(&a, false);
    loglite::add_sink(&b, false);
    CHECK(loglite::sink_count() == 2);
    LOGLITE_LOG_INFO("both");
    CHECK(a.lines.load() == 1);
    CHECK(b.lines.load() == 1);

    loglite::reset_sinks();
    CHECK(loglite::sink_count() == 0);
    LOGLITE_LOG_INFO("none");
    CHECK(a.lines.load() == 1);

    loglite::add_sink(nullptr, true);
    CHECK(loglite::sink_count() == 0);
    fresh();
}

static void test_file_sink_unicode_path_and_message() {
    fresh();
    // UTF-8 bytes for "zhong wen" written as escape sequences so the source
    // file encoding never matters.
    const char* cn = "\xE4\xB8\xAD\xE6\x96\x87";
    auto dir = test_dir() / std::filesystem::u8path(std::string("dir_") + cn);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto path = dir / std::filesystem::u8path(std::string("log_") + cn + ".log");
    clean(path);

    loglite::add_file_sink_utf8(path.u8string(), 1ull << 20, 1);
    LOGLITE_LOG_INFO("utf8 message: %s", cn);

    const std::string content = read_file(path);
    CHECK(!content.empty());
    CHECK(content.find(cn) != std::string::npos);
    CHECK(content.find("[INFO ]") != std::string::npos);
    CHECK(content.find("loglite_tests.cpp:") != std::string::npos);
    fresh();
}

static void test_rolling() {
    fresh();
    auto path = test_dir() / "rolling.log";
    clean(path);

    loglite::add_file_sink_utf8(path.u8string(), 256, 3);
    for (int i = 0; i < 100; ++i) {
        LOGLITE_LOG_INFO("line %03d padding padding padding padding padding", i);
    }
    loglite::flush();

    CHECK(std::filesystem::exists(path));
    CHECK(std::filesystem::exists(test_dir() / "rolling.log.1"));
    CHECK(std::filesystem::exists(test_dir() / "rolling.log.2"));
    CHECK(!std::filesystem::exists(test_dir() / "rolling.log.3"));

    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    CHECK(!ec);
    CHECK(size <= 512);   // one line may slightly exceed the threshold
    fresh();
}

static void test_json_format() {
    fresh();
    auto path = test_dir() / "json.log";
    clean(path);
    loglite::add_file_sink_utf8(path.u8string(), 1ull << 20, 1);
    loglite::set_format(loglite::Format::json);
    CHECK(loglite::get_format() == loglite::Format::json);

    LOGLITE_LOG_WARN("quote \" slash \\ newline \n utf8 \xE4\xB8\xAD\xE6\x96\x87");
    const std::string content = read_file(path);
    CHECK(content.rfind("{\"ts\":", 0) == 0);
    CHECK(content.find("\"seq\":") == std::string::npos);   // seq is not printed
    const auto colon = content.find(':');
    const unsigned long long ts_us = (colon == std::string::npos) ? 0 :
        std::strtoull(content.c_str() + colon + 1, nullptr, 10);
    CHECK(ts_us > 1000000000000000ull);   // microseconds since epoch
    CHECK(content.find("\"level\":\"WARN\"") != std::string::npos);
    CHECK(content.find("\"file\":\"loglite_tests.cpp\"") != std::string::npos);
    CHECK(content.find("\"line\":") != std::string::npos);
    CHECK(content.find("\\\"") != std::string::npos);
    CHECK(content.find("\\\\") != std::string::npos);
    CHECK(content.find("\\n") != std::string::npos);
    CHECK(lines_of(content).size() == 1);   // embedded newline was escaped
    fresh();
}

static std::atomic<int> g_callback_calls{ 0 };
static void reentrant_callback(int, const char*, void*) {
    g_callback_calls.fetch_add(1, std::memory_order_relaxed);
    LOGLITE_LOG_INFO("nested from callback");   // must not deadlock or recurse
}

static void test_callback_and_reentrancy() {
    fresh();
    CountingSink c;
    loglite::add_sink(&c, false);
    g_callback_calls.store(0);
    loglite::set_callback(&reentrant_callback, nullptr);

    auto fut = std::async(std::launch::async, [] {
        LOGLITE_LOG_WARN("trigger");
    });
    const bool finished =
        fut.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
    CHECK(finished);
    if (finished) fut.get();

    CHECK(g_callback_calls.load() == 1);        // no infinite recursion
    CHECK(c.lines.load() == 2);                 // trigger + nested
    loglite::set_callback(nullptr, nullptr);
    fresh();
}

static void test_long_message_and_edge_cases() {
    fresh();
    auto path = test_dir() / "long.log";
    clean(path);
    loglite::add_file_sink_utf8(path.u8string(), 1ull << 30, 1);

    const std::string big(3000, 'x');
    LOGLITE_LOG_INFO("%s", big.c_str());
    LOGLITE_LOG_INFO("");
    const char* null_fmt = nullptr;
    LOGLITE_LOG_INFO(null_fmt);

    const std::string content = read_file(path);
    CHECK(content.size() > 3000);                // heap formatting path
    CHECK(content.find(std::string(2000, 'x')) != std::string::npos);
    CHECK(lines_of(content).size() == 3);
    fresh();
}

static void test_c_api() {
    fresh();
    auto path = test_dir() / "c_api.log";
    clean(path);
    const std::string u8 = path.u8string();

    loglite_set_level(static_cast<int>(loglite::Level::trace));
    loglite_set_format(0);
    loglite_reset_sinks();
    loglite_set_queue_capacity(0);
    loglite_add_file_utf8(u8.c_str(), 1ull << 20, 1);
    LOGLITE_LOG_INFO("c api line");
    loglite_flush();
    CHECK(read_file(path).find("c api line") != std::string::npos);
    CHECK(loglite_queue_size() == 0);
    CHECK(loglite_dropped_count() >= 0);

    loglite_shutdown();
    CHECK(loglite::is_shutdown());
    CHECK(loglite::sink_count() == 0);
    LOGLITE_LOG_INFO("silenced");
    CHECK(read_file(path).find("silenced") == std::string::npos);

    loglite_initialize();
    CHECK(!loglite::is_shutdown());
    loglite_add_console();
    CHECK(loglite::sink_count() == 1);
    fresh();
}

static void test_shutdown_and_initialize() {
    fresh();
    CountingSink c;
    loglite::add_sink(&c, false);
    LOGLITE_LOG_INFO("before");
    CHECK(c.lines.load() == 1);

    loglite::shutdown();
    CHECK(loglite::is_shutdown());
    LOGLITE_LOG_INFO("silenced");
    CHECK(c.lines.load() == 1);

    loglite::initialize();
    CHECK(!loglite::is_shutdown());
    loglite::add_sink(&c, false);
    LOGLITE_LOG_INFO("after");
    CHECK(c.lines.load() == 2);
    fresh();
}

// ------------------------------------------------- async + ordering tests ----
class SlowSink final : public loglite::Sink {
public:
    std::atomic<long> lines{ 0 };
    std::vector<std::string> order;   // written by the single writer thread
    void write(const loglite::Record& r, std::string_view) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        lines.fetch_add(1, std::memory_order_relaxed);
        order.emplace_back(std::to_string(r.seq));
    }
};

static void test_async_nonblocking() {
    fresh();
    SlowSink slow;
    loglite::add_sink(&slow, false);

    constexpr int kLines = 60;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kLines; ++i) {
        LOGLITE_LOG_INFO("async %d", i);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const auto submit_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    const bool had_backlog = loglite::queue_size() > 0;

    // Synchronous I/O would cost 60 * 10ms = 600ms on the caller thread.
    CHECK(submit_ms < 300);
    CHECK(had_backlog);
    loglite::flush();
    CHECK(slow.lines.load() == kLines);

    bool ordered = slow.order.size() == static_cast<std::size_t>(kLines);
    const unsigned long long base =
        ordered ? std::strtoull(slow.order[0].c_str(), nullptr, 10) : 0;
    for (int i = 0; ordered && i < kLines; ++i) {
        ordered = (std::strtoull(slow.order[i].c_str(), nullptr, 10) == base + i);
    }
    CHECK(ordered);
    fresh();
}

class SeqSink final : public loglite::Sink {
public:
    std::vector<unsigned long long> seqs;   // written by the single writer thread
    void write(const loglite::Record& r, std::string_view) override {
        seqs.push_back(r.seq);
    }
};

static void test_microsecond_timestamp() {
    fresh();
    auto path = test_dir() / "us.log";
    clean(path);
    loglite::add_file_sink_utf8(path.u8string(), 1ull << 20, 1);
    LOGLITE_LOG_INFO("timestamp check");

    const auto lines = lines_of(read_file(path));
    CHECK(lines.size() == 1);
    bool six_digits = (lines.size() == 1);
    if (six_digits) {
        const auto dot = lines[0].find('.');
        six_digits = (dot != std::string::npos && dot + 7 < lines[0].size());
        for (int i = 0; six_digits && i < 6; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(lines[0][dot + 1 + i]))) {
                six_digits = false;
            }
        }
        six_digits = six_digits && lines[0][dot + 7] == ' ';
    }
    CHECK(six_digits);   // HH:MM:SS.ffffff
    fresh();
}

static void test_global_sequence_consistency() {
    fresh();
    auto path = test_dir() / "seq.log";
    clean(path);
    SeqSink seqs;
    loglite::add_sink(&seqs, false);
    loglite::add_file_sink_utf8(path.u8string(), 1ull << 40, 1);

    constexpr int kThreads = 8;
    constexpr int kLines = 200;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([t] {
            for (int i = 0; i < kLines; ++i) {
                LOGLITE_LOG_INFO("t=%d i=%d", t, i);
            }
        });
    }
    for (auto& w : workers) w.join();
    loglite::flush();

    const auto lines = lines_of(read_file(path));
    const std::size_t expected = static_cast<std::size_t>(kThreads * kLines);
    CHECK(lines.size() == expected);

    // seq stays internal: strictly increasing, no loss, no duplication.
    bool seq_ok = (seqs.seqs.size() == expected);
    const unsigned long long base = seq_ok ? seqs.seqs[0] : 0;
    for (std::size_t i = 0; seq_ok && i < seqs.seqs.size(); ++i) {
        if (seqs.seqs[i] != base + i) { seq_ok = false; break; }
    }
    CHECK(seq_ok);

    bool printed_seq = false;
    for (const auto& line : lines) {
        if (line.find("[seq ") != std::string::npos) printed_seq = true;
    }
    CHECK(!printed_seq);   // seq must not appear in the formatted output
    fresh();
}

class GateSink final : public loglite::Sink {
public:
    std::atomic<bool> entered{ false };
    std::atomic<bool> go{ false };
    std::atomic<long> lines{ 0 };
    std::vector<unsigned long long> seqs;   // written by the writer thread
    void write(const loglite::Record& r, std::string_view) override {
        if (!entered.exchange(true)) {
            while (!go.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        lines.fetch_add(1, std::memory_order_relaxed);
        seqs.push_back(r.seq);
    }
};

static void test_bounded_queue_drop_preserves_order() {
    fresh();
    GateSink gate;
    loglite::add_sink(&gate, false);
    const auto dropped_before = loglite::dropped_count();
    loglite::set_queue_capacity(4);

    LOGLITE_LOG_INFO("gate");
    for (int i = 0; i < 500 && !gate.entered.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool entered = gate.entered.load();
    for (int i = 0; i < 50; ++i) {
        LOGLITE_LOG_INFO("burst %d", i);
    }
    gate.go.store(true);
    loglite::flush();

    CHECK(entered);
    CHECK(loglite::dropped_count() - dropped_before > 0);
    CHECK(gate.lines.load() <= 5);       // 1 in flight + 4 queued
    bool ordered = !gate.seqs.empty();
    const unsigned long long base = ordered ? gate.seqs[0] : 0;
    for (std::size_t i = 0; ordered && i < gate.seqs.size(); ++i) {
        if (gate.seqs[i] != base + i) ordered = false;
    }
    CHECK(ordered);                       // drops do not corrupt ordering
    loglite::set_queue_capacity(0);
    fresh();
}

static void test_multithread_file_integrity() {
    fresh();
    auto path = test_dir() / "mt.log";
    clean(path);
    loglite::add_file_sink_utf8(path.u8string(), 1ull << 40, 1);

    constexpr int kThreads = 8;
    constexpr int kLines = 400;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([t] {
            for (int i = 0; i < kLines; ++i) {
                LOGLITE_LOG_INFO("thread=%d seq=%d payload=%s",
                                 t, i, "0123456789abcdef");
            }
        });
    }
    for (auto& w : workers) w.join();

    const auto lines = lines_of(read_file(path));
    CHECK(lines.size() == static_cast<std::size_t>(kThreads * kLines));
    int malformed = 0;
    for (const auto& line : lines) {
        if (line.size() < 40 || line[4] != '-' || line[7] != '-' ||
            line.find("[INFO ]") == std::string::npos ||
            line.find("[tid ") == std::string::npos ||
            line.find("payload=0123456789abcdef") == std::string::npos) {
            ++malformed;
        }
    }
    CHECK(malformed == 0);   // no interleaved / torn lines
    fresh();
}

static void test_multithread_concurrent_config() {
    fresh();
    CountingSink counter;
    loglite::add_sink(&counter, false);

    std::thread configurator([] {
        for (int i = 0; i < 3000; ++i) {
            loglite::set_level(i % 2 ? loglite::Level::trace
                                     : loglite::Level::warn);
            loglite::set_format(i % 2 ? loglite::Format::json
                                      : loglite::Format::text);
        }
    });

    constexpr int kThreads = 6;
    constexpr int kLines = 300;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([] {
            for (int i = 0; i < kLines; ++i) {
                LOGLITE_LOG_ERROR("stress %d", i);
            }
        });
    }
    configurator.join();
    for (auto& w : workers) w.join();

    CHECK(counter.lines.load() == kThreads * kLines);
    CHECK(counter.bytes.load() > 0);
    fresh();   // drop the non-owned sink before it goes out of scope
}

static void test_flush_and_shutdown_drain() {
    fresh();
    CountingSink c;
    loglite::add_sink(&c, false);
    for (int i = 0; i < 20; ++i) LOGLITE_LOG_INFO("drain %d", i);
    loglite::flush();
    CHECK(c.lines.load() == 20);
    CHECK(loglite::queue_size() == 0);

    for (int i = 0; i < 10; ++i) LOGLITE_LOG_INFO("before shutdown %d", i);
    loglite::shutdown();                  // must drain before returning
    CHECK(c.lines.load() == 30);
    CHECK(loglite::queue_size() == 0);

    loglite::initialize();
    fresh();
}

static std::mutex          g_cb_mtx;
static std::thread::id     g_cb_thread_id;
static std::atomic<int>    g_cb_count{ 0 };
static void worker_thread_callback(int, const char*, void*) {
    {
        std::lock_guard<std::mutex> lk(g_cb_mtx);
        g_cb_thread_id = std::this_thread::get_id();
    }
    g_cb_count.fetch_add(1, std::memory_order_relaxed);
    LOGLITE_LOG_INFO("nested from callback");   // enqueued, callback suppressed
}

static void test_callback_runs_on_writer_thread() {
    fresh();
    CountingSink c;
    loglite::add_sink(&c, false);
    g_cb_count.store(0);
    loglite::set_callback(&worker_thread_callback, nullptr);

    const auto main_thread = std::this_thread::get_id();
    LOGLITE_LOG_WARN("trigger");
    loglite::flush();
    loglite::flush();                  // second pass drains the nested record

    std::thread::id cb_thread;
    {
        std::lock_guard<std::mutex> lk(g_cb_mtx);
        cb_thread = g_cb_thread_id;
    }
    CHECK(cb_thread != main_thread);
    CHECK(g_cb_count.load() == 1);    // no callback recursion
    CHECK(c.lines.load() == 2);       // trigger + nested
    loglite::set_callback(nullptr, nullptr);
    fresh();
}

#define COLLECT_LEVEL(suffix, lvl) { ::loglite::Level::lvl, #suffix },
static const std::pair<loglite::Level, const char*> kLevelTable[] = {
    LOGLITE_LEVEL_LIST(COLLECT_LEVEL)
};
#undef COLLECT_LEVEL

static std::string read_file_raw(const std::filesystem::path& p) {
    // Deliberately does NOT call loglite::flush(): used to verify that the
    // timer / byte threshold flushed the file on its own.
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

class FlushCountingSink final : public loglite::Sink {
public:
    std::atomic<long> writes{ 0 };
    std::atomic<long> flushes{ 0 };
    void write(const loglite::Record&, std::string_view) override {
        writes.fetch_add(1, std::memory_order_relaxed);
    }
    void flush() override {
        flushes.fetch_add(1, std::memory_order_relaxed);
    }
};

static void test_flush_is_batched() {
    fresh();
    FlushCountingSink counter;
    loglite::add_sink(&counter, false);
    loglite::set_flush_policy(1u << 30, 0);   // no timer, huge byte threshold

    for (int i = 0; i < 100; ++i) {
        LOGLITE_LOG_INFO("batched %d", i);
    }
    const long flushes_before = counter.flushes.load();   // sample before CHECK
    CHECK(flushes_before <= 1);          // not one flush per line
    loglite::flush();
    CHECK(counter.flushes.load() >= 1);  // explicit flush is honoured
    CHECK(counter.writes.load() == 100);
    fresh();
}

static void test_flush_interval_triggers() {
    fresh();
    auto path = test_dir() / "interval.log";
    clean(path);
    FlushCountingSink counter;
    loglite::add_sink(&counter, false);
    loglite::add_file_sink_utf8(path.u8string(), 1ull << 40, 1);
    loglite::set_flush_policy(1u << 30, 20);   // 20 ms timer, huge threshold

    LOGLITE_LOG_INFO("timer flush marker");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    const long flushes_before = counter.flushes.load();
    const std::string raw = read_file_raw(path);   // no explicit flush
    CHECK(flushes_before >= 1);       // timer fired on its own
    CHECK(raw.find("timer flush marker") != std::string::npos);
    CHECK(counter.writes.load() == 1);
    fresh();
}

static void test_flush_policy_api() {
    fresh();
    loglite_set_flush_policy(4096, 50);
    CHECK(loglite::flush_threshold() == 4096);
    CHECK(loglite::flush_interval_ms() == 50);

    loglite::set_flush_policy(0, 0);   // legacy behaviour: flush every line
    CHECK(loglite::flush_threshold() == 0);
    CHECK(loglite::flush_interval_ms() == 0);
    LOGLITE_LOG_INFO("legacy per-line flush");
    CHECK(loglite::queue_size() == 0);
    fresh();
}

class OrderSink final : public loglite::Sink {
public:
    std::vector<std::string> messages;   // written by the single writer thread
    void write(const loglite::Record& r, std::string_view) override {
        messages.emplace_back(r.message);
    }
};

static void test_per_thread_order() {
    fresh();
    OrderSink order;
    loglite::add_sink(&order, false);

    constexpr int kThreads = 8;
    constexpr int kLines = 200;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([t] {
            for (int i = 0; i < kLines; ++i) {
                LOGLITE_LOG_INFO("owner=%d step=%d", t, i);
            }
        });
    }
    for (auto& w : workers) w.join();
    loglite::flush();

    CHECK(order.messages.size() ==
          static_cast<std::size_t>(kThreads * kLines));
    // In the global stream, each thread's own steps must read 0,1,2,... in
    // order - other threads may interleave, but never reorder its own records.
    std::vector<int> next(kThreads, 0);
    bool ordered = (order.messages.size() ==
                    static_cast<std::size_t>(kThreads * kLines));
    for (const auto& msg : order.messages) {
        const auto owner_pos = msg.find("owner=");
        const auto step_pos = msg.find(" step=");
        int owner = -1, step = -1;
        try {
            if (owner_pos == std::string::npos ||
                step_pos == std::string::npos) {
                ordered = false;
                break;
            }
            owner = std::stoi(msg.substr(owner_pos + 6));
            step = std::stoi(msg.substr(step_pos + 6));
        } catch (...) {
            ordered = false;
            break;
        }
        if (owner < 0 || owner >= kThreads) {
            ordered = false;
            break;
        }
        if (step != next[owner]) {
            ordered = false;
            break;
        }
        ++next[owner];
    }
    CHECK(ordered);

    bool complete = true;
    for (int t = 0; t < kThreads; ++t) {
        if (next[t] != kLines) complete = false;
    }
    CHECK(complete);
    fresh();
}

static void test_shutdown_drains_concurrent_submit() {
    fresh();
    CountingSink counter;
    loglite::add_sink(&counter, false);

    constexpr int kThreads = 8;
    constexpr int kLines = 500;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([] {
            for (int i = 0; i < kLines; ++i) {
                LOGLITE_LOG_INFO("shutdown drain %d", i);
            }
        });
    }
    for (auto& w : workers) w.join();

    loglite::shutdown();     // no explicit flush: shutdown must drain it all
    CHECK(counter.lines.load() == kThreads * kLines);
    CHECK(loglite::queue_size() == 0);
    CHECK(loglite::is_shutdown());

    LOGLITE_LOG_INFO("late after shutdown");
    CHECK(loglite::queue_size() == 0);   // no record stranded in the queue
    CHECK(counter.lines.load() == kThreads * kLines);

    loglite::initialize();
    fresh();
}

static void test_prefix_helpers() {
    fresh();
    CountingSink c;
    loglite::add_sink(&c, false);

    CHECK(sizeof(kLevelTable) / sizeof(kLevelTable[0]) == 5);
    CHECK(PROJ_log::sink_count() == 1);            // prefixed namespace alias
    PROJ_LOG_INFO("prefixed %d", 42);
    PROJ_LOG(::loglite::Level::warn, "prefixed generic %d", 7);
    CHECK(c.lines.load() == 2);
    fresh();
}

#define RUN(fn) do { std::cout << "[RUN ] " #fn "\n"; fn(); } while (0)

int main() {
    std::cout << "== loglite test suite ==\n";
    RUN(test_text_format_and_levels);
    RUN(test_runtime_level_filter);
    RUN(test_compile_time_filter);
    RUN(test_custom_sink_and_reset);
    RUN(test_file_sink_unicode_path_and_message);
    RUN(test_rolling);
    RUN(test_json_format);
    RUN(test_callback_and_reentrancy);
    RUN(test_long_message_and_edge_cases);
    RUN(test_c_api);
    RUN(test_shutdown_and_initialize);
    RUN(test_async_nonblocking);
    RUN(test_microsecond_timestamp);
    RUN(test_global_sequence_consistency);
    RUN(test_bounded_queue_drop_preserves_order);
    RUN(test_multithread_file_integrity);
    RUN(test_multithread_concurrent_config);
    RUN(test_flush_and_shutdown_drain);
    RUN(test_callback_runs_on_writer_thread);
    RUN(test_flush_is_batched);
    RUN(test_flush_interval_triggers);
    RUN(test_flush_policy_api);
    RUN(test_per_thread_order);
    RUN(test_shutdown_drains_concurrent_submit);
    RUN(test_prefix_helpers);

    std::cout << (g_failures == 0 ? "[PASSED] " : "[FAILED] ")
              << (g_checks - g_failures) << "/" << g_checks << " checks\n";
    loglite::shutdown();   // join the writer before process exit
    return g_failures == 0 ? 0 : 1;
}
