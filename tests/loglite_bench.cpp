// Throughput / stress benchmark for loglite.
// Not registered in ctest (it is a manual, long-running benchmark):
//   cmake --build build --config Release --target loglite_bench
//   .\build\Release\loglite_bench.exe [MiB-per-repeat]     (default: 4)
//
// Two sink modes are measured separately:
//   none : no sink at all -> pure frontend cost (vsnprintf + format + enqueue)
//   file : rolling file sink with a flush per line -> true end-to-end throughput
// "submit" times only the producer loop; "e2e" also includes flush() waiting
// for the single writer thread to drain the queue.
#define LOGLITE_COMPILE_LEVEL 2
#include <loglite/loglite.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kRepeats = 3;
using Clock = std::chrono::steady_clock;

enum class SinkMode { none, file };

struct Config {
    std::size_t   payload_bytes;
    std::uint64_t messages;      // per repeat, per whole case
    int           threads;
    SinkMode      mode;
};

struct Timings {
    double          submit_s = 0.0;
    double          total_s = 0.0;
    std::uint64_t   messages = 0;
};

std::string make_payload(std::size_t n) {
    std::string s(n, 'x');
    for (std::size_t i = 0; i < n; i += 97) {
        s[i] = static_cast<char>('A' + (i % 26));
    }
    return s;
}

void fresh() {
    ::loglite::flush();
    ::loglite::initialize();
    ::loglite::reset_sinks();
    ::loglite::set_callback(nullptr, nullptr);
    ::loglite::set_level(loglite::Level::trace);
    ::loglite::set_format(loglite::Format::text);
    ::loglite::set_queue_capacity(0);   // unbounded: no drops during measurement
}

std::filesystem::path bench_dir() {
    static const std::filesystem::path dir = [] {
        auto d = std::filesystem::temp_directory_path() / "loglite_bench";
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        return d;
    }();
    return dir;
}

std::uint64_t messages_for(std::size_t size, std::uint64_t target_bytes) {
    std::uint64_t n = target_bytes / static_cast<std::uint64_t>(size);
    n = std::max<std::uint64_t>(n, 200);
    n = std::min<std::uint64_t>(n, 120000);
    return n;
}

Timings run_case(const Config& cfg) {
    fresh();

    if (cfg.mode == SinkMode::file) {
        auto path = bench_dir() /
                    ("bench_" + std::to_string(cfg.payload_bytes) + ".log");
        std::error_code ec;
        std::filesystem::remove(path, ec);
        // Threshold far above the measured volume: no rotation during the run.
        loglite::add_file_sink_utf8(path.u8string(), 1ull << 40, 1);
    }

    const std::string payload = make_payload(cfg.payload_bytes);
    const std::uint64_t warm = std::min<std::uint64_t>(
        cfg.messages,
        std::max<std::uint64_t>(32, 262144ull / cfg.payload_bytes));
    for (std::uint64_t i = 0; i < warm; ++i) {
        LOGLITE_LOG_INFO("%s", payload.c_str());
    }
    loglite::flush();

    const std::uint64_t per_thread = cfg.messages / cfg.threads;
    const std::uint64_t total = per_thread * cfg.threads;
    Timings acc;

    for (int r = 0; r < kRepeats; ++r) {
        std::atomic<bool> go{ false };
        std::atomic<int>  ready{ 0 };
        std::vector<std::thread> workers;
        workers.reserve(cfg.threads);

        auto body = [&] {
            ready.fetch_add(1, std::memory_order_relaxed);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (std::uint64_t i = 0; i < per_thread; ++i) {
                LOGLITE_LOG_INFO("%s", payload.c_str());
            }
        };
        for (int t = 0; t < cfg.threads; ++t) {
            workers.emplace_back(body);
        }
        while (ready.load(std::memory_order_relaxed) < cfg.threads) {
            std::this_thread::yield();
        }

        const auto t0 = Clock::now();
        go.store(true, std::memory_order_release);
        for (auto& w : workers) w.join();
        const auto t1 = Clock::now();
        loglite::flush();
        const auto t2 = Clock::now();

        acc.submit_s += std::chrono::duration<double>(t1 - t0).count();
        acc.total_s += std::chrono::duration<double>(t2 - t0).count();
        acc.messages += total;
    }
    return acc;
}

struct Rates {
    double msgps;
    double mibps;
    double us_per_msg;
};

Rates rates(const Timings& t, std::size_t size, bool end_to_end) {
    const double seconds = end_to_end ? t.total_s : t.submit_s;
    Rates r{ 0.0, 0.0, 0.0 };
    if (seconds <= 0.0 || t.messages == 0) return r;
    r.msgps = static_cast<double>(t.messages) / seconds;
    r.mibps = static_cast<double>(t.messages) *
              static_cast<double>(size) / seconds / (1024.0 * 1024.0);
    r.us_per_msg = seconds * 1e6 / static_cast<double>(t.messages);
    return r;
}

void print_size_table(std::uint64_t target_bytes,
                      std::vector<std::string>& csv) {
    const std::size_t sizes[] = {
        32, 128, 512, 1023, 1024, 1025, 4096, 16384, 65536
    };

    std::cout << "-- payload size sweep (1 producer thread) --\n"
              << std::right << std::fixed
              << std::setw(9)  << "size(B)" << " |"
              << std::setw(15) << "submit msg/s" << " |"
              << std::setw(12) << "submit MiB/s" << " |"
              << std::setw(13) << "submit us/msg" << " |"
              << std::setw(15) << "e2e msg/s" << " |"
              << std::setw(12) << "e2e MiB/s" << " |"
              << std::setw(13) << "e2e us/msg" << "\n";
    std::cout << std::string(105, '-') << "\n";

    for (const std::size_t size : sizes) {
        const auto n = messages_for(size, target_bytes);
        const auto front = run_case(Config{ size, n, 1, SinkMode::none });
        const auto file = run_case(Config{ size, n, 1, SinkMode::file });
        const Rates f = rates(front, size, false);
        const Rates e = rates(file, size, true);

        std::cout << std::setprecision(0)
                  << std::setw(9)  << size << " |"
                  << std::setw(15) << f.msgps << " |"
                  << std::setprecision(2)
                  << std::setw(12) << f.mibps << " |"
                  << std::setw(13) << f.us_per_msg << " |"
                  << std::setprecision(0)
                  << std::setw(15) << e.msgps << " |"
                  << std::setprecision(2)
                  << std::setw(12) << e.mibps << " |"
                  << std::setw(13) << e.us_per_msg << "\n";

        std::ostringstream row;
        row << std::fixed << std::setprecision(3)
            << size << ',' << f.msgps << ',' << f.mibps << ',' << f.us_per_msg
            << ',' << e.msgps << ',' << e.mibps << ',' << e.us_per_msg;
        csv.push_back(row.str());
    }
}

void print_scaling_table(std::uint64_t target_bytes,
                         std::vector<std::string>& csv) {
    const std::size_t sizes[] = { 128, 4096 };
    const int thread_counts[] = { 1, 2, 4, 8 };
    const std::uint64_t small_target =
        std::max<std::uint64_t>(target_bytes / 4, 1ull << 20);

    std::cout << "\n-- producer scaling (file sink, end-to-end) --\n"
              << std::right << std::fixed
              << std::setw(9)  << "size(B)" << " |"
              << std::setw(8)  << "threads" << " |"
              << std::setw(15) << "e2e msg/s" << " |"
              << std::setw(12) << "e2e MiB/s" << " |"
              << std::setw(13) << "e2e us/msg" << "\n";
    std::cout << std::string(70, '-') << "\n";

    for (const std::size_t size : sizes) {
        for (const int threads : thread_counts) {
            const auto n = messages_for(size, small_target);
            const auto t = run_case(Config{ size, n, threads, SinkMode::file });
            const Rates r = rates(t, size, true);

            std::cout << std::setprecision(0)
                      << std::setw(9)  << size << " |"
                      << std::setw(8)  << threads << " |"
                      << std::setw(15) << r.msgps << " |"
                      << std::setprecision(2)
                      << std::setw(12) << r.mibps << " |"
                      << std::setw(13) << r.us_per_msg << "\n";

            std::ostringstream row;
            row << std::fixed << std::setprecision(3)
                << size << ',' << threads << ',' << r.msgps << ','
                << r.mibps << ',' << r.us_per_msg;
            csv.push_back(row.str());
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
    const std::uint64_t target_mib =
        (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 4;
    const std::uint64_t target_bytes =
        std::max<std::uint64_t>(1, target_mib) * 1024ull * 1024ull;

    std::cout << "== loglite throughput benchmark ==\n"
              << "payload per repeat: " << target_mib << " MiB, repeats: "
              << kRepeats << ", sink=none / sink=file\n"
              << "submit = producer loop only; e2e = producer + flush (writer drained)\n"
              << "note: 1023/1024/1025 B straddle the 1 KiB stack formatting buffer\n\n";

    std::vector<std::string> size_csv;
    std::vector<std::string> scaling_csv;
    print_size_table(target_bytes, size_csv);
    print_scaling_table(target_bytes, scaling_csv);

    std::cout << "\n#csv size_bytes,submit_msgps,submit_mibps,submit_us_per_msg,"
                 "e2e_msgps,e2e_mibps,e2e_us_per_msg\n";
    for (const auto& row : size_csv) std::cout << row << "\n";
    std::cout << "#csv size_bytes,threads,e2e_msgps,e2e_mibps,e2e_us_per_msg\n";
    for (const auto& row : scaling_csv) std::cout << row << "\n";

    loglite::shutdown();
    return 0;
}
