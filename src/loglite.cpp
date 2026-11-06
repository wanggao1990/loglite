#include <loglite/loglite.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <pthread.h>
#endif

namespace loglite {
namespace {

std::atomic<int>          g_level{ static_cast<int>(Level::info) };
std::atomic<int>          g_format{ static_cast<int>(Format::text) };
std::atomic<bool>         g_stopped{ false };
// Batched flush policy: file sinks flush after this many bytes, and the writer
// flushes all sinks on this fixed interval (0 disables the timer).
std::atomic<std::size_t>  g_flush_bytes{ 64 * 1024 };
std::atomic<unsigned>     g_flush_interval_ms{ 200 };
thread_local bool         t_in_callback = false;

class ConsoleSink final : public Sink {
public:
    void write(const Record&, std::string_view line) override {
        std::string tmp(line);
        std::fwrite(tmp.data(), 1, tmp.size(), stderr);
        std::fflush(stderr);
    }
};

class RollingFileSink final : public Sink {
public:
    RollingFileSink(std::filesystem::path path, std::uint64_t max_bytes, int max_files)
        : path_(std::move(path)),
          max_bytes_(max_bytes ? max_bytes : 1),
          max_files_(std::max(1, max_files)) {
        std::error_code ec;
        if (path_.has_parent_path()) {
            std::filesystem::create_directories(path_.parent_path(), ec);
        }
        ofs_.open(path_, std::ios::app | std::ios::binary);
    }

    void write(const Record&, std::string_view line) override {
        if (!ofs_.is_open()) return;
        const std::streampos pos = ofs_.tellp();
        if (pos != std::streampos(-1) &&
            static_cast<std::uint64_t>(pos) + line.size() > max_bytes_) {
            rotate();
        }
        ofs_.write(line.data(), static_cast<std::streamsize>(line.size()));
        bytes_since_flush_ += line.size();
        const std::size_t threshold = g_flush_bytes.load(std::memory_order_relaxed);
        if (threshold == 0 || bytes_since_flush_ >= threshold) {
            ofs_.flush();
            bytes_since_flush_ = 0;
        }
    }

    void flush() override {
        if (ofs_.is_open()) {
            ofs_.flush();
            bytes_since_flush_ = 0;
        }
    }

private:
    std::filesystem::path numbered(int i) const {
        std::filesystem::path p = path_;
        p += ".";
        p += std::to_string(i);
        return p;
    }

    void rotate() {
        ofs_.close();   // close() flushes the stream buffer
        bytes_since_flush_ = 0;
        std::error_code ec;
        if (max_files_ > 1) {
            std::filesystem::remove(numbered(max_files_ - 1), ec);
            for (int i = max_files_ - 2; i >= 1; --i) {
                std::filesystem::rename(numbered(i), numbered(i + 1), ec);
            }
            std::filesystem::rename(path_, numbered(1), ec);
        }
        ofs_.open(path_, std::ios::trunc | std::ios::binary);
    }

    std::filesystem::path path_;
    std::uint64_t         max_bytes_;
    int                   max_files_;
    std::ofstream         ofs_;
    std::size_t           bytes_since_flush_ = 0;
};

// Immutable configuration snapshot. The writer thread keeps a shared_ptr to a
// snapshot while doing I/O, so reconfiguration never races with a write and
// sinks stay alive until the in-flight batch is finished.
struct Config {
    std::vector<std::shared_ptr<Sink>> sinks;
    LogCallback cb = nullptr;
    void*       cb_ctx = nullptr;
};

struct Pending {
    std::uint64_t seq = 0;
    Level         level = Level::info;
    std::uint64_t epoch_us = 0;
    std::uint32_t tid = 0;
    std::string   file;              // deep copy of __FILE__
    int           line_no = 0;
    std::string   message;           // deep copy of the formatted message
    std::string   formatted;         // complete line, built outside the queue lock
    bool          suppress_callback = false;
};

struct State {
    std::mutex                    cfg_mtx;
    std::shared_ptr<const Config> config;

    std::mutex              q_mtx;
    std::condition_variable q_cv;
    std::condition_variable drained_cv;
    std::deque<Pending>     queue;
    std::uint64_t           next_seq = 0;
    std::uint64_t           processed = 0;
    std::uint64_t           dropped = 0;
    std::uint64_t           flush_request = 0;
    std::uint64_t           flush_done = 0;
    std::size_t             capacity = 0;   // 0 = unbounded
    bool                    stop = false;
    bool                    worker_running = false;
    std::thread             worker;
};

// Deliberately leaked: a DLL may be unloaded while other static objects are
// still logging. `shutdown()` joins the writer and clears resources explicitly.
State& state() {
    static State* s = []() -> State* {
        auto* p = new State();
        auto cfg = std::make_shared<Config>();
        cfg->sinks.push_back(std::make_shared<ConsoleSink>());
        p->config = std::move(cfg);
        return p;
    }();
    return *s;
}

std::shared_ptr<const Config> snapshot_config() {
    std::lock_guard<std::mutex> lk(state().cfg_mtx);
    return state().config;
}

std::string_view basename(std::string_view path) noexcept {
    const auto pos = path.find_last_of("\\/");
    return pos == std::string_view::npos ? path : path.substr(pos + 1);
}

const char* level_name(Level level) noexcept {
    switch (level) {
        case Level::trace: return "TRACE";
        case Level::debug: return "DEBUG";
        case Level::info:  return "INFO";
        case Level::warn:  return "WARN";
        case Level::error: return "ERROR";
        default:           return "OFF";
    }
}

std::uint64_t now_us() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::uint32_t current_thread_id() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint32_t>(::GetCurrentThreadId());
#else
    return static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(::pthread_self()));
#endif
}

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    out += b;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

void format_text(const Record& r, std::string& out) {
    std::tm tm{};
    const std::time_t sec = static_cast<std::time_t>(r.epoch_us / 1000000);
#if defined(_WIN32)
    ::localtime_s(&tm, &sec);
#else
    ::localtime_r(&sec, &tm);
#endif
    char head[160];
    std::snprintf(head, sizeof(head),
                  "%04d-%02d-%02d %02d:%02d:%02d.%06llu [%-5s] [tid %u] [",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<unsigned long long>(r.epoch_us % 1000000),
                  level_name(r.level),
                  r.thread_id);
    out.assign(head);
    out.append(basename(r.file));
    out.push_back(':');
    out.append(std::to_string(r.line));
    out.append("] ");
    out.append(r.message);
    out.push_back('\n');
}

void format_json(const Record& r, std::string& out) {
    out = "{\"ts\":" + std::to_string(r.epoch_us)
        + ",\"level\":\"" + level_name(r.level)
        + "\",\"tid\":" + std::to_string(r.thread_id)
        + ",\"file\":\"" + json_escape(basename(r.file))
        + "\",\"line\":" + std::to_string(r.line)
        + ",\"msg\":\"" + json_escape(r.message) + "\"}\n";
}

struct CallbackScope {
    bool& flag;
    explicit CallbackScope(bool& f) : flag(f) { flag = true; }
    ~CallbackScope() { flag = false; }
};

void flush_sinks(const std::shared_ptr<const Config>& cfg) noexcept {
    for (const auto& sink : cfg->sinks) {
        if (!sink) continue;
        try {
            sink->flush();
        } catch (...) {
        }
    }
}

void worker_loop();

// Called with q_mtx held.
void ensure_worker_locked(State& s) {
    if (s.worker_running) return;
    s.stop = false;
    s.worker_running = true;
    s.worker = std::thread(worker_loop);
}

void worker_loop() {
    State& s = state();
    for (;;) {
        std::deque<Pending> batch;
        bool want_flush = false;
        bool stopping = false;
        bool exit_now = false;
        {
            std::unique_lock<std::mutex> lk(s.q_mtx);
            const unsigned interval =
                g_flush_interval_ms.load(std::memory_order_relaxed);
            const auto ready = [&s] {
                return s.stop || !s.queue.empty() ||
                       s.flush_done < s.flush_request;
            };
            bool timed_out = false;
            if (interval == 0) {
                s.q_cv.wait(lk, ready);
            } else {
                timed_out = !s.q_cv.wait_for(
                    lk, std::chrono::milliseconds(interval), ready);
            }
            if (!s.queue.empty()) batch.swap(s.queue);   // FIFO order preserved
            stopping = s.stop;
            want_flush = timed_out ||
                         (s.flush_done < s.flush_request) ||
                         stopping;   // flush on every pass while shutting down
        }

        auto cfg = snapshot_config();
        for (auto& item : batch) {
            Record r{ item.seq, item.level, item.epoch_us, item.tid,
                      std::string_view(item.file), item.line_no,
                      std::string_view(item.message) };
            for (const auto& sink : cfg->sinks) {
                if (sink) sink->write(r, std::string_view(item.formatted));
            }
            if (cfg->cb && !item.suppress_callback) {
                CallbackScope scope(t_in_callback);
                cfg->cb(static_cast<int>(item.level),
                        item.formatted.c_str(), cfg->cb_ctx);
            }
        }
        if (want_flush) flush_sinks(cfg);

        {
            std::lock_guard<std::mutex> lk(s.q_mtx);
            s.processed += batch.size();
            if (want_flush) s.flush_done = s.flush_request;
            // Re-check after the batch: records enqueued during shutdown are
            // still drained instead of being abandoned.
            exit_now = s.stop && s.queue.empty();
        }
        s.drained_cv.notify_all();
        if (exit_now) break;
    }
    {
        std::lock_guard<std::mutex> lk(s.q_mtx);
        s.worker_running = false;
        s.flush_done = s.flush_request;
    }
    s.drained_cv.notify_all();
}

} // namespace (anonymous)

void initialize() noexcept {
    g_stopped.store(false, std::memory_order_relaxed);
    try {
        State& s = state();
        std::lock_guard<std::mutex> lk(s.q_mtx);
        ensure_worker_locked(s);
    } catch (...) {
    }
}

void shutdown() noexcept {
    g_stopped.store(true, std::memory_order_relaxed);
    State& s = state();
    std::thread to_join;
    {
        std::lock_guard<std::mutex> lk(s.q_mtx);
        s.stop = true;
        ++s.flush_request;          // request the final flush
        if (s.worker.joinable()) to_join = std::move(s.worker);
    }
    s.q_cv.notify_all();
    // join outside the lock: the worker needs q_mtx to drain and exit.
    if (to_join.joinable()) to_join.join();
    {
        std::lock_guard<std::mutex> lk(s.q_mtx);
        s.worker_running = false;
        s.flush_done = s.flush_request;
    }
    s.drained_cv.notify_all();
    try {
        std::lock_guard<std::mutex> lk(s.cfg_mtx);
        s.config = std::make_shared<Config>();
    } catch (...) {
    }
}

bool is_shutdown() noexcept {
    return g_stopped.load(std::memory_order_relaxed);
}

void set_level(Level level) noexcept {
    g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

Level get_level() noexcept {
    return static_cast<Level>(g_level.load(std::memory_order_relaxed));
}

bool enabled(Level level) noexcept {
    const int lv = static_cast<int>(level);
    return !g_stopped.load(std::memory_order_relaxed) &&
           lv >= g_level.load(std::memory_order_relaxed) &&
           lv < static_cast<int>(Level::off);
}

void set_format(Format format) noexcept {
    g_format.store(static_cast<int>(format), std::memory_order_relaxed);
}

Format get_format() noexcept {
    return static_cast<Format>(g_format.load(std::memory_order_relaxed));
}

void reset_sinks() noexcept {
    try {
        std::lock_guard<std::mutex> lk(state().cfg_mtx);
        state().config = std::make_shared<Config>();
    } catch (...) {
    }
}

std::size_t sink_count() noexcept {
    try {
        return snapshot_config()->sinks.size();
    } catch (...) {
        return 0;
    }
}

void add_sink_shared(std::shared_ptr<Sink> sink) noexcept {
    try {
        State& s = state();
        std::lock_guard<std::mutex> lk(s.cfg_mtx);
        auto next = std::make_shared<Config>(*s.config);   // copy-on-write
        next->sinks.push_back(std::move(sink));
        s.config = std::move(next);
    } catch (...) {
    }
}

void add_console_sink() noexcept {
    try {
        add_sink_shared(std::make_shared<ConsoleSink>());
    } catch (...) {
    }
}

void add_file_sink_utf8(std::string_view utf8_path,
                        std::uint64_t max_bytes, int max_files) noexcept {
    if (utf8_path.empty()) return;
    try {
        auto path = std::filesystem::u8path(std::string(utf8_path));
        add_sink_shared(std::make_shared<RollingFileSink>(
            std::move(path), max_bytes, max_files));
    } catch (...) {
    }
}

void add_sink(Sink* sink, bool take_ownership) noexcept {
    if (!sink) return;
    try {
        std::shared_ptr<Sink> sp;
        if (take_ownership) {
            sp.reset(sink);
        } else {
            sp.reset(sink, [](Sink*) {});   // caller keeps ownership
        }
        add_sink_shared(std::move(sp));
    } catch (...) {
        if (take_ownership) delete sink;
    }
}

void set_callback(LogCallback callback, void* context) noexcept {
    try {
        State& s = state();
        std::lock_guard<std::mutex> lk(s.cfg_mtx);
        auto next = std::make_shared<Config>(*s.config);
        next->cb = callback;
        next->cb_ctx = context;
        s.config = std::move(next);
    } catch (...) {
    }
}

void set_flush_policy(std::size_t bytes, unsigned interval_ms) noexcept {
    g_flush_bytes.store(bytes, std::memory_order_relaxed);
    g_flush_interval_ms.store(interval_ms, std::memory_order_relaxed);
    // Wake the writer so a newly shortened interval takes effect immediately.
    state().q_cv.notify_one();
}

std::size_t flush_threshold() noexcept {
    return g_flush_bytes.load(std::memory_order_relaxed);
}

unsigned flush_interval_ms() noexcept {
    return g_flush_interval_ms.load(std::memory_order_relaxed);
}

void flush() noexcept {
    try {
        State& s = state();
        std::unique_lock<std::mutex> lk(s.q_mtx);
        const std::uint64_t target = s.next_seq;
        const std::uint64_t request = ++s.flush_request;
        lk.unlock();
        s.q_cv.notify_one();
        lk.lock();
        s.drained_cv.wait(lk, [&s, target, request] {
            return (s.processed >= target && s.flush_done >= request) ||
                   !s.worker_running;
        });
    } catch (...) {
    }
}

std::size_t queue_size() noexcept {
    try {
        std::lock_guard<std::mutex> lk(state().q_mtx);
        return state().queue.size();
    } catch (...) {
        return 0;
    }
}

std::uint64_t dropped_count() noexcept {
    try {
        std::lock_guard<std::mutex> lk(state().q_mtx);
        return state().dropped;
    } catch (...) {
        return 0;
    }
}

void set_queue_capacity(std::size_t capacity) noexcept {
    try {
        std::lock_guard<std::mutex> lk(state().q_mtx);
        state().capacity = capacity;
    } catch (...) {
    }
}

void submit(Level level, std::string_view file, int line,
            const char* fmt, ...) noexcept {
    if (!enabled(level)) return;
    // Message formatting AND line formatting happen on the caller thread,
    // completely outside the queue lock. The lock only assigns the sequence
    // number and pushes the finished record.
    try {
        const char* safe_fmt = fmt ? fmt : "";
        char stackbuf[1024];
        va_list ap;
        va_start(ap, fmt);
        const int n = std::vsnprintf(stackbuf, sizeof(stackbuf), safe_fmt, ap);
        va_end(ap);

        std::string message;
        if (n < 0) {
            message = "<format error>";
        } else if (static_cast<std::size_t>(n) < sizeof(stackbuf)) {
            message.assign(stackbuf, static_cast<std::size_t>(n));
        } else {
            message.resize(static_cast<std::size_t>(n) + 1);
            va_list ap2;
            va_start(ap2, fmt);       // fresh va_list for the second pass
            std::vsnprintf(message.data(), message.size(), safe_fmt, ap2);
            va_end(ap2);
            message.resize(static_cast<std::size_t>(n));
        }

        Pending item;
        item.level = level;
        item.epoch_us = now_us();
        item.tid = current_thread_id();
        item.file = std::string(file);
        item.line_no = line;
        item.message = std::move(message);
        item.suppress_callback = t_in_callback;

        // seq is no longer printed, so the line can be formatted before the
        // sequence number is known.
        Record preview{ 0, item.level, item.epoch_us, item.tid,
                        std::string_view(item.file), item.line_no,
                        std::string_view(item.message) };
        if (static_cast<Format>(g_format.load(std::memory_order_relaxed)) ==
            Format::json) {
            format_json(preview, item.formatted);
        } else {
            format_text(preview, item.formatted);
        }

        State& s = state();
        {
            std::lock_guard<std::mutex> lk(s.q_mtx);
            // shutdown() sets s.stop under this same lock. A caller that
            // passed enabled() before shutdown but acquires the lock after it
            // must NOT enqueue: the writer is already gone, so the record
            // would sit in the queue forever. Drop it explicitly instead.
            if (s.stop) return;
            if (s.capacity != 0 && s.queue.size() >= s.capacity) {
                ++s.dropped;          // drop-newest: submit never blocks
                return;
            }
            ensure_worker_locked(s);
            item.seq = s.next_seq++;  // allocated in the enqueue critical section
            s.queue.push_back(std::move(item));
        }
        s.q_cv.notify_one();
    } catch (...) {
    }
}

} // namespace loglite

extern "C" {

void loglite_initialize(void) { loglite::initialize(); }
void loglite_shutdown(void) { loglite::shutdown(); }
void loglite_set_level(int level) {
    loglite::set_level(static_cast<loglite::Level>(level));
}
void loglite_set_format(int json) {
    loglite::set_format(json ? loglite::Format::json : loglite::Format::text);
}
void loglite_reset_sinks(void) { loglite::reset_sinks(); }
void loglite_add_console(void) { loglite::add_console_sink(); }
void loglite_add_file_utf8(const char* utf8_path,
                           unsigned long long max_bytes, int max_files) {
    if (!utf8_path) return;
    loglite::add_file_sink_utf8(std::string_view(utf8_path),
                                static_cast<std::uint64_t>(max_bytes), max_files);
}
void loglite_flush(void) { loglite::flush(); }
unsigned long long loglite_dropped_count(void) {
    return static_cast<unsigned long long>(loglite::dropped_count());
}
void loglite_set_queue_capacity(unsigned long long capacity) {
    loglite::set_queue_capacity(static_cast<std::size_t>(capacity));
}
unsigned long long loglite_queue_size(void) {
    return static_cast<unsigned long long>(loglite::queue_size());
}
void loglite_set_flush_policy(unsigned long long bytes, unsigned interval_ms) {
    loglite::set_flush_policy(static_cast<std::size_t>(bytes), interval_ms);
}

} // extern "C"
