# loglite

[English](README.md) | **简体中文**

一个零第三方依赖、C++17、前后端分离的异步日志库。

- **前端**：`LOGLITE_LOG*` 宏在调用线程完成消息格式化与整行格式化，只做一次极短的入队。
- **后端**：单一 writer 线程按 FIFO 顺序把记录分发给所有 `Sink`；文件、控制台、回调、自定义后端可热插拔。
- **时序**：全局序号 `seq` 在入队临界区内分配，单消费者严格 FIFO —— 同一线程内的日志严格按调用顺序写出，全局也严格有序（`seq` 为内部字段，不打印）。
- **批量刷新**：文件 sink 按 64 KiB 累积刷新，writer 每 200 ms 定时刷新，`flush()` / `shutdown()` 强制完整刷新。
- **时间戳**：微秒（`epoch_us`，文本 `HH:MM:SS.ffffff`，JSON `ts` 为微秒 epoch）。
- 库本体不含任何项目前缀，项目侧可通过宏自行加前缀。

---

## 目录结构

```
3rdparty/loglite/
├── CMakeLists.txt              # 唯一的项目定义
├── readme.md                   # English
├── readme_zh.md                # 本文档
├── include/loglite/
│   ├── loglite.h               # 公共 API 与宏
│   └── prefix.h                # 前缀辅助宏
├── src/loglite.cpp             # 实现
├── sample/loglite_sample.cpp   # 完整使用示例
├── tests/
│   ├── loglite_tests.cpp       # 25 个用例 / 154 checks
│   └── loglite_bench.cpp       # 吞吐量基准
└── build/                      # 所有解决方案、工程文件、二进制都在这里生成
```

源码根目录只保留 `CMakeLists.txt` 与源码目录；`.sln`、`.vcxproj`、exe/lib 全部由 CMake 在 `build/` 内生成。

---

## 构建

需要 CMake ≥ 3.15、C++17 编译器（Windows 下为 VS2022 / v143）。

```powershell
cd 3rdparty/loglite
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
cmake --build build --config Release
```

生成的工程与产物：

| 内容 | 路径 |
|---|---|
| 解决方案 / 工程 | `build/loglite.sln`、`build/*.vcxproj` |
| 静态库 | `build/lib/<Config>/loglite.lib` |
| 可执行文件 | `build/bin/<Config>/loglite_tests.exe`、`loglite_sample.exe`、`loglite_bench.exe` |

CMake 选项：

| 选项 | 默认 | 说明 |
|---|---|---|
| `LOGLITE_BUILD_SHARED` | `OFF` | `ON` 时构建 DLL（定义 `LOGLITE_BUILD_SHARED` / `LOGLITE_USE_SHARED`） |
| `LOGLITE_BUILD_TESTS` | `ON` | 构建测试并注册到 ctest |
| `LOGLITE_BUILD_SAMPLE` | `ON` | 构建示例并注册为 ctest 冒烟测试 |
| `LOGLITE_BUILD_BENCH` | `ON` | 构建吞吐量基准（手动运行，不进 ctest） |

运行：

```powershell
ctest --test-dir build -C Release --output-on-failure
.\build\bin\Release\loglite_sample.exe
.\build\bin\Release\loglite_bench.exe 4        # 参数 = 每轮 payload MiB
```

集成到其它工程：把 `include/` 加入包含目录，链接 `loglite.lib`（或直接用 CMake `add_subdirectory` 后 `target_link_libraries(app PRIVATE loglite)`）。

---

## 快速上手

```cpp
#include <loglite/loglite.h>
#include <cstdio>

int main() {
    loglite::initialize();                       // 懒启动 writer，也可省略（首次 submit 会启动）
    loglite::reset_sinks();                      // 去掉默认控制台 sink
    loglite::set_level(loglite::Level::trace);
    loglite::add_file_sink_utf8("app.log");      // 默认 4 MiB 滚动、3 个备份

    LOGLITE_LOG_INFO("start, image %dx%d", 512, 512);
    LOGLITE_LOG_WARN("score=%.2f", 12.34);
    LOGLITE_LOG_ERROR("failed: %s", "timeout");

    loglite::flush();        // 等待队列排空 + 所有 sink 刷新
    loglite::shutdown();     // 退出前 drain + flush + join（必须）
    return 0;
}
```

### 零初始化的行为

```cpp
#include <loglite/loglite.h>
int main() {
    LOGLITE_LOG_INFO("hello");
    return 0;                // 不保证可见
}
```

默认级别是 `info`，这条日志会入队；默认 sink 在 Windows 下会同时输出调试器（`OutputDebugStringA`）与 `stdout`。但写入是**异步**的：writer 线程可能还没被调度，进程就退出了；默认定时刷新是 200 ms，来不及触发。库刻意不注册 `atexit`（DLL 卸载阶段处于 loader lock，join 线程会死锁）。

**要保证可见与不丢日志，必须显式调用 `flush()` 与 `shutdown()`：**

```cpp
LOGLITE_LOG_INFO("hello");
loglite::flush();
loglite::shutdown();
```

---

## 默认策略

| 项目 | 默认值 | 调整方式 |
|---|---|---|
| 运行时级别 | `info` (2)，`trace/debug` 被过滤 | `set_level()` / `loglite_set_level()`；`off` 关闭全部 |
| 编译期级别门限 | Release(`NDEBUG`)=2，Debug=0 | `LOGLITE_COMPILE_LEVEL`，`if constexpr` 编译期消除 |
| 输出格式 | `text` | `set_format(Format::json)` |
| 时间戳 | 微秒 | 固定 |
| `seq` | 内部字段，**不打印** | 供自定义 sink 校验顺序 |
| 默认 sink | 1 个 `ConsoleSink`：Windows = `OutputDebugStringA` + `stdout` + `fflush`，其它 = `stderr` + `fflush` | `reset_sinks()` 清空后自行添加 |
| 回调 | 无 | `set_callback()` |
| 文件 sink | **默认不创建** | `add_file_sink_utf8()`，默认 4 MiB × 3 滚动 |
| 队列容量 | `0` = 无界，不丢日志 | `set_queue_capacity(n)`；满时丢最新并计 `dropped_count()` |
| 文件刷新阈值 | 64 KiB | `set_flush_policy(bytes, ms)`；`bytes=0` 恢复每行刷新 |
| 定时刷新 | 200 ms | `interval_ms=0` 禁用定时器 |
| 强制刷新 | `flush()` / `shutdown()` 无条件完整刷新 | — |
| writer 线程 | 懒启动（首次 `submit()` 或 `initialize()`），**单线程** | `shutdown()` 排空 + 刷新 + join |
| 短宏 `LOG_*` | 关闭 | 定义 `LOGLITE_ENABLE_SHORT_MACROS` |

---

## API 参考

### 生命周期

| 函数 | 说明 |
|---|---|
| `void initialize() noexcept` | 恢复可记录状态并确保 writer 启动 |
| `void shutdown() noexcept` | 置停、排空队列、刷新所有 sink、join writer、清空 sink（幂等） |
| `bool is_shutdown() noexcept` | 是否已停止 |

`shutdown()` 之后 `submit()` 会被 `enabled()` 丢弃，需要 `initialize()` 才能继续。

### 配置（均线程安全）

```cpp
void   set_level(Level) noexcept;   Level   get_level() noexcept;
bool   enabled(Level) noexcept;
void   set_format(Format) noexcept; Format  get_format() noexcept;
void   reset_sinks() noexcept;      size_t  sink_count() noexcept;
void   add_console_sink() noexcept;
void   add_file_sink_utf8(std::string_view utf8_path,
                          std::uint64_t max_bytes = 4ull << 20,
                          int max_files = 3) noexcept;
void   add_sink(Sink*, bool take_ownership = true) noexcept;
void   set_callback(LogCallback, void* ctx) noexcept;
```

`LogCallback = void (*)(int level, const char* line, void* ctx)`，由 writer 线程串行调用；回调内再打日志是安全的（该条会被标记 `suppress_callback`，不会递归触发回调）。

### 异步队列与刷新策略

```cpp
void          flush() noexcept;                 // 等待所有已提交记录写出并刷新
std::size_t   queue_size() noexcept;
std::uint64_t dropped_count() noexcept;
void          set_queue_capacity(std::size_t) noexcept;
void          set_flush_policy(std::size_t bytes, unsigned interval_ms) noexcept;
std::size_t   flush_threshold() noexcept;
unsigned      flush_interval_ms() noexcept;
```

### `Record` 与 `Sink`

```cpp
struct Record {
    std::uint64_t    seq;        // 全局提交序号（内部，不打印）
    Level            level;
    std::uint64_t    epoch_us;   // 微秒 epoch
    std::uint32_t    thread_id;
    std::string_view file;
    int              line;
    std::string_view message;    // UTF-8
};

class Sink {
public:
    virtual ~Sink() = default;
    // formatted_line 为完整一行（含 '\n'），由单一 writer 线程按序调用
    virtual void write(const Record&, std::string_view formatted_line) = 0;
    // 定时 tick / flush() / shutdown() 时调用，用于把缓冲刷向 OS
    virtual void flush() {}
};
```

自定义后端示例：

```cpp
class ErrorCounter final : public loglite::Sink {
public:
    std::atomic<int> errors{0};
    void write(const loglite::Record& r, std::string_view) override {
        if (r.level == loglite::Level::error) errors.fetch_add(1);
    }
};

ErrorCounter counter;
loglite::add_sink(&counter, false);   // false = 调用方持有对象所有权
```

### 宏

```cpp
LOGLITE_LOG(level, ...)               // 通用入口
LOGLITE_LOG_TRACE/DEBUG/INFO/WARN/ERROR(...)
LOG_TRACE/DEBUG/INFO/WARN/ERROR(...)  // 需 LOGLITE_ENABLE_SHORT_MACROS，且每个用 #ifndef 保护
```

被 `LOGLITE_COMPILE_LEVEL` 丢弃的级别，其参数**不会被求值**（`if constexpr`）。

### C API

`loglite_initialize`、`loglite_shutdown`、`loglite_set_level`、`loglite_set_format`、`loglite_reset_sinks`、`loglite_add_console`、`loglite_add_file_utf8`、`loglite_flush`、`loglite_dropped_count`、`loglite_set_queue_capacity`、`loglite_queue_size`、`loglite_set_flush_policy`。均为 `extern "C"`，便于 FFI / 跨模块调用。

### 项目侧前缀

`prefix.h` 提供 `LOGLITE_EXPAND`、`LOGLITE_PREFIXED(prefix, name)`、`LOGLITE_LEVEL_LIST(X)`：

```cpp
#include <loglite/loglite.h>
#include <loglite/prefix.h>

#define APP_LOG(...)       LOGLITE_EXPAND(LOGLITE_LOG(__VA_ARGS__))
#define APP_LOG_INFO(...)  LOGLITE_LOG_INFO(__VA_ARGS__)
#define APP_LOG_WARN(...)  LOGLITE_LOG_WARN(__VA_ARGS__)
namespace LOGLITE_PREFIXED(APP, log) = loglite;   // -> APP_log
```

`LOGLITE_EXPAND` 是 MSVC 传统预处理器下变参转发必需的展开层；工程同时启用 `/Zc:preprocessor`。

---

## 时序一致性

- 同一线程的程序顺序 → 依次在同一把队列锁内入队；其它线程可以插在两条之间，但不改变这两条的相对顺序。
- `seq` 的分配与 `deque::push_back` 在同一临界区，因此 `seq` 恒等于队列位置；单 writer 用 `batch.swap` 按 FIFO 取整批并顺序遍历所有 sink。
- 所有 sink 看到相同的顺序；`flush()` 返回后，之前提交的所有记录都已写出并刷新。
- 已修复的竞态：`shutdown()` 与「已通过 `enabled()` 但尚未取得队列锁」的提交 —— 这类迟到记录在锁内被 `s.stop` 明确丢弃，不会滞留队列。

会「缺条」但**不会乱序**的场景：

| 场景 | 后果 |
|---|---|
| `set_queue_capacity(n)` 且队列满 | 丢最新，剩余仍有序；默认无界不丢 |
| 并发 `set_level()` / `shutdown()` | 部分记录被 `enabled()` 过滤 |
| 系统时钟回拨 | 时间戳可能非单调，写出顺序不变 |
| 回调内再打日志 | 进入下一批，与触发它的记录之间会夹入其它记录 |
| 未 `flush()` 就崩溃 | 尾部缓冲丢失 |

---

## 生命周期与 DLL（重要）

| 场景 | 未调用 `shutdown()` 的后果 |
|---|---|
| EXE 正常退出 | 一般不致命：`State` 是故意泄漏的堆对象，其 `std::thread` 不会析构，避免 joinable thread 触发 `std::terminate`；但**尾部日志丢失**、可能写出半行 |
| DLL `FreeLibrary` | **致命**：OS 不会终止 DLL 创建的线程，卸载后 writer 继续执行已卸载代码 / 访问已卸载 vtable → `0xC0000005` 崩溃（延迟触发，最多一个 200 ms tick） |

- 必须在 `FreeLibrary` 之前由宿主显式调用导出的关闭函数（内部转调 `loglite_shutdown()`）；EXE 在 `main()` 末尾调用。
- **不要**在 `DllMain(DLL_PROCESS_DETACH)` 里调用 `shutdown()`：loader lock 下 join 线程 / 做 I/O 会死锁。
- `set_flush_policy(0, 0)`（每行刷新）只能减少日志丢失，**不能**解决 DLL 卸载崩溃。

---

## 性能与已知限制

- 所有 sink 由**单一 writer**串行调用：任一慢 sink（Windows 控制台 / 重定向管道 / 附加调试器时的 `OutputDebugStringA`）会拖住全部 sink，包括文件 sink（head-of-line blocking）。文件内容仍有序、不缺，只是落盘变晚。
- Windows 默认 `ConsoleSink` 每条都 `fflush(stdout)`；GUI / 无控制台宿主建议 `reset_sinks()` 后只挂文件 sink。
- 默认无界队列在慢 sink 持续跟不上时会增长内存；可用 `set_queue_capacity(n)` 限流，代价是丢最新日志（可由 `dropped_count()` 观测）。
- 生产端仍需一次 `q_mtx` 临界区（分配 seq + push），这是全局严格有序的代价；当前没有 thread-local 缓冲。
- 单进程内使用，不做跨进程同步，`flush()` 不保证物理 fsync。

---

## Benchmark

环境：Windows、VS2022 v143（MSVC 19.38）、x64 **Release**、CMake 生成、`build/bin/Release/loglite_bench.exe 1`（每轮 1 MiB payload，3 轮累计取平均）。

测量口径：

- `submit`：`sink=none`，只计时生产者循环（`vsnprintf` + 整行格式化 + 入队），不含 writer 等待。
- `e2e`：`sink=file`，计时生产者 + `flush()`（等待 writer 排空并落盘）。
- 两者是不同配置，不能直接相减。
- `1023 / 1024 / 1025 B` 用于跨过实现中 1 KiB 的栈格式化缓冲区边界。

### 日志大小扫描（1 个生产者线程）

| size(B) | submit msg/s | submit MiB/s | submit us/msg | e2e msg/s | e2e MiB/s | e2e us/msg |
|---:|---:|---:|---:|---:|---:|---:|
| 32 | 812,825 | 24.81 | 1.23 | 1,170,491 | 35.72 | 0.85 |
| 128 | 817,312 | 99.77 | 1.22 | 970,827 | 118.51 | 1.03 |
| 512 | 699,231 | 341.42 | 1.43 | 511,242 | 249.63 | 1.96 |
| 1023 | 768,673 | 749.92 | 1.30 | 357,587 | 348.87 | 2.80 |
| 1024 | 740,544 | 723.19 | 1.35 | 371,073 | 362.38 | 2.69 |
| 1025 | 754,443 | 737.48 | 1.33 | 338,985 | 331.36 | 2.95 |
| 4096 | 562,802 | 2198.45 | 1.78 | 121,920 | 476.25 | 8.20 |
| 16384 | 289,533 | 4523.96 | 3.45 | 54,334 | 848.97 | 18.40 |
| 65536 | 50,447 | 3152.93 | 19.82 | 28,571 | 1785.70 | 35.00 |

### 生产者扩展性（file sink，端到端）

| size(B) | threads | e2e msg/s | e2e MiB/s | e2e us/msg |
|---:|---:|---:|---:|---:|
| 128 | 1 | 985,140 | 120.26 | 1.02 |
| 128 | 2 | 877,874 | 107.16 | 1.14 |
| 128 | 4 | 887,086 | 108.29 | 1.13 |
| 128 | 8 | 802,961 | 98.02 | 1.25 |
| 4096 | 1 | 132,562 | 517.82 | 7.54 |
| 4096 | 2 | 146,459 | 572.10 | 6.83 |
| 4096 | 4 | 127,549 | 498.24 | 7.84 |
| 4096 | 8 | 119,394 | 466.38 | 8.38 |

### 与「每行 flush」旧实现的历史对照

同机同参数（1 MiB/轮）下，`e2e` 的提升：

| size(B) | 旧 msg/s | 新 msg/s | 倍数 |
|---:|---:|---:|---:|
| 32 | 196,416 | 1,170,491 | ≈5.96× |
| 128 | 192,840 | 970,827 | ≈5.03× |
| 4096 | 84,157 | 121,920 | ≈1.45× |
| 65536 | 24,290 | 28,571 | ≈1.18× |

优化点：整行格式化移出入队锁、文件按 64 KiB 批量刷新、writer 每 200 ms 定时刷新；`flush()` / `shutdown()` 仍强制完整刷新。

### 解读

- 小消息受「每条固定开销」限制，条/秒高但 MiB/s 低；大消息受内存拷贝与每行 I/O 限制，条/秒下降但 MiB/s 上升。
- 生产者线程 1→8 时吞吐基本持平甚至略降：瓶颈在单 writer + 每行 `fwrite` / flush，以及入队时的 `q_mtx` 串行点，增加生产者线程不会带来线性扩展。
- 若要进一步提升：需要 thread-local 缓冲（减少入队锁竞争）或按 sink 拆分独立通道（消除 head-of-line）；两者都会改变当前的全局严格顺序语义，需权衡。

复现：

```powershell
cmake --build build --config Release --target loglite_bench
.\build\bin\Release\loglite_bench.exe 4
```

程序同时打印人读表格与两组 CSV（`#csv` 开头），便于脚本采集。

---

## 测试

`tests/loglite_tests.cpp` 为自包含断言框架，共 **25 个用例 / 154 checks**；`ctest` 注册 2 个测试：`loglite_tests` 与 `loglite_sample`（冒烟）。

覆盖范围：文本/JSON 格式、微秒时间戳、运行时与编译期级别过滤、UTF-8 中文路径与消息、文件滚动、长消息（>1 KiB 堆格式化路径）、空/null 格式串、回调重入、C API、shutdown/initialize、异步非阻塞提交、全局 `seq` 连续、同线程顺序、有界队列丢弃保序、8 线程文件行完整性、并发配置压力、批量/定时 flush 策略、批量 flush 计数、并发提交后 shutdown 排空、前缀机制。

```powershell
ctest --test-dir build -C Debug   --output-on-failure
ctest --test-dir build -C Release --output-on-failure
```
