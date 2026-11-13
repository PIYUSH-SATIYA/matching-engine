#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>
#include <sys/time.h>

#ifdef __linux__
#include <sched.h>
#endif

#include "affinity.hpp"
#include "benchmark.hpp"
#include "event_sinks.hpp"
#include "matching_engine.hpp"
#include "network_layer.hpp"
#include "order.hpp"
#include "order_book.hpp"
#include "order_book_fast.hpp"
#include "ring_buffer.hpp"
#include "spsc_ring_buffer.hpp"

static std::atomic<bool> g_stop{false};

static void sigint_handler(int) { g_stop.store(true, std::memory_order_relaxed); }

struct Options {
    std::uint16_t port = 9000;
    std::string queue = "spsc";
    std::string book = "fast-slab";
    std::string net = "epoll";
    std::string sched = "default";
    std::string config = "default";
    std::string kind = "latency";
    int cpu_net = -1;
    int cpu_engine = -1;
    int run = 0;
    std::size_t warmup = 0;
    const char* samples_path = nullptr;
    const char* stats_path = nullptr;
    const char* log_path = nullptr;
    bool count_migrations = false;
    bool exit_on_disconnect = false;
};

struct EngineStats {
    std::uint64_t nvcsw = 0;
    std::uint64_t nivcsw = 0;
    std::uint64_t minflt = 0;
    std::uint64_t majflt = 0;
    std::uint64_t migrations = 0;
    // Cumulative time this thread spent runnable but waiting on a run queue.
    // /proc/thread-self/schedstat reports it directly, which turns "how much
    // of the tail is the scheduler?" into a number instead of a guess.
    std::uint64_t runq_ns = 0;
    double user_s = 0;
    double sys_s = 0;
};

static double timevalSeconds(const timeval& tv) {
    return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) / 1e6;
}

// Second field of /proc/thread-self/schedstat: ns spent waiting for a CPU.
static std::uint64_t threadRunqueueNs() {
#ifdef __linux__
    std::FILE* f = std::fopen("/proc/thread-self/schedstat", "r");
    if (!f) return 0;
    unsigned long long on_cpu = 0, runq = 0, slices = 0;
    const int n = std::fscanf(f, "%llu %llu %llu", &on_cpu, &runq, &slices);
    std::fclose(f);
    return (n == 3) ? static_cast<std::uint64_t>(runq) : 0;
#else
    return 0;
#endif
}

static void writeStats(const Options& opt, std::uint64_t seen,
                       const CountingSink& counting, const NetStats& net,
                       std::uint64_t measured, double duration_s,
                       double throughput, const std::vector<std::uint64_t>& sorted,
                       const std::vector<std::uint64_t>& arr_sorted,
                       const std::vector<std::uint64_t>& svc_sorted,
                       const EngineStats& es, const EngineStats& net_es) {
    if (!opt.stats_path) return;
    FILE* f = std::fopen(opt.stats_path, "a");
    if (!f) {
        std::fprintf(stderr, "cannot open stats file %s\n", opt.stats_path);
        return;
    }
    if (std::ftell(f) == 0) {
        std::fprintf(f,
                     "config,run,kind,orders,warmup,measured,accepted,trades,"
                     "rejected,invalid,duration_s,throughput,p50,p90,p99,p999,"
                     "max,minflt,majflt,nvcsw,nivcsw,user_s,sys_s,migrations,"
                     "kernel_ops,traded_qty,traded_notional,net_nvcsw,net_nivcsw,"
                     "net_user_s,net_sys_s,spsc_yields,net_ingest_slow,"
                     "net_ingest_max_ns,engine_runq_ns,net_runq_ns,"
                     "arr_p50,arr_p99,arr_p999,svc_p50,svc_p99,svc_p999\n");
    }
    const auto p = [&](double q) {
        return static_cast<unsigned long long>(
            LatencyRecorder::percentile(sorted, q));
    };
    const std::uint64_t kernel_ops =
        (opt.net == "uring")
            ? net.submits + net.completions + net.accepts
            : net.epoll_waits + net.recv_calls + net.accepts;
    const std::uint64_t spsc_yields =
        (opt.queue == "spsc")
            ? SpscRingBuffer<16384>::g_yields.load(std::memory_order_relaxed)
            : 0;
    std::fprintf(
        f,
        "%s,%d,%s,%llu,%zu,%llu,%llu,%llu,%llu,%llu,%.6f,%.0f,%llu,%llu,%llu,"
        "%llu,%llu,%llu,%llu,%llu,%llu,%.6f,%.6f,%llu,%llu,%llu,%llu,%llu,%llu,"
        "%.6f,%.6f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
        opt.config.c_str(), opt.run, opt.kind.c_str(),
        static_cast<unsigned long long>(seen), opt.warmup,
        static_cast<unsigned long long>(measured),
        static_cast<unsigned long long>(counting.accepted),
        static_cast<unsigned long long>(counting.trades),
        static_cast<unsigned long long>(counting.rejected),
        static_cast<unsigned long long>(net.invalid), duration_s, throughput,
        p(0.50), p(0.90), p(0.99), p(0.999),
        static_cast<unsigned long long>(LatencyRecorder::percentile(sorted, 1.0)),
        static_cast<unsigned long long>(es.minflt),
        static_cast<unsigned long long>(es.majflt),
        static_cast<unsigned long long>(es.nvcsw),
        static_cast<unsigned long long>(es.nivcsw), es.user_s, es.sys_s,
        static_cast<unsigned long long>(es.migrations),
        static_cast<unsigned long long>(kernel_ops),
        static_cast<unsigned long long>(counting.traded_quantity),
        static_cast<unsigned long long>(counting.traded_notional),
        static_cast<unsigned long long>(net_es.nvcsw),
        static_cast<unsigned long long>(net_es.nivcsw), net_es.user_s,
        net_es.sys_s, static_cast<unsigned long long>(spsc_yields),
        static_cast<unsigned long long>(net.ingest_slow),
        static_cast<unsigned long long>(net.ingest_max_ns),
        static_cast<unsigned long long>(es.runq_ns),
        static_cast<unsigned long long>(net_es.runq_ns),
        static_cast<unsigned long long>(
            LatencyRecorder::percentile(arr_sorted, 0.50)),
        static_cast<unsigned long long>(
            LatencyRecorder::percentile(arr_sorted, 0.99)),
        static_cast<unsigned long long>(
            LatencyRecorder::percentile(arr_sorted, 0.999)),
        static_cast<unsigned long long>(
            LatencyRecorder::percentile(svc_sorted, 0.50)),
        static_cast<unsigned long long>(
            LatencyRecorder::percentile(svc_sorted, 0.99)),
        static_cast<unsigned long long>(
            LatencyRecorder::percentile(svc_sorted, 0.999)));
    std::fclose(f);
}

template <class Queue, class Book>
int runApp(const Options& opt) {
    auto queue = std::make_unique<Queue>();
    auto book = std::make_unique<Book>();

    auto push_fn = [](void* ctx, const Order& o) {
        static_cast<Queue*>(ctx)->push(o);
    };

    std::unique_ptr<INetworkServer> server =
        (opt.net == "uring")
            ? makeUringServer(opt.port, push_fn, queue.get(),
                              opt.exit_on_disconnect)
            : makeEpollServer(opt.port, push_fn, queue.get(),
                              opt.exit_on_disconnect);
    if (!server) {
        std::fprintf(stderr,
                     "network backend '%s' is not available in this build\n",
                     opt.net.c_str());
        return 2;
    }
    if (!server->start()) {
        std::fprintf(stderr, "failed to start %s server on port %u\n",
                     opt.net.c_str(), opt.port);
        return 2;
    }

    CountingSink counting;
    std::unique_ptr<TradeLogSink> tlog;
    if (opt.log_path) tlog = std::make_unique<TradeLogSink>(1u << 20);

    LatencyRecorder latency(4'000'000, opt.warmup);
    // Same samples split into their two components: arrival (client send ->
    // network thread parse) and service (parse -> engine completion).
    LatencyRecorder arrival(4'000'000, opt.warmup);
    LatencyRecorder service(4'000'000, opt.warmup);
    std::atomic<bool> rt_failed{false};
    std::uint64_t seen = 0;
    std::uint64_t measured = 0;
    std::chrono::steady_clock::time_point window_start{};
    std::chrono::steady_clock::time_point window_end{};
    EngineStats es;
    EngineStats net_es;

    std::thread eng_thread([&] {
        if (opt.sched == "pin" || opt.sched == "rt") pinThisThread(opt.cpu_engine);
        if (opt.sched == "rt" && !setRealtimeFifo(50)) {
            std::fprintf(stderr,
                         "SCHED_FIFO denied: need CAP_SYS_NICE (run in the "
                         "SYS_NICE container or setcap the binary)\n");
            rt_failed.store(true, std::memory_order_relaxed);
            g_stop.store(true, std::memory_order_relaxed);
            return;
        }

        struct rusage base {};
        bool have_base = false;
        std::uint64_t runq_base = 0;
        auto loop = [&](auto& engine) {
            int last_cpu = -1;
            bool first = true;
            while (true) {
                Order o = queue->pop();
                if (o.id == 0) break;  // reserved shutdown sentinel
                if (first) {
                    // Baseline after the producer has started: excludes the
                    // engine's startup idle-spin from per-order CPU accounting.
                    getrusage(RUSAGE_THREAD, &base);
                    runq_base = threadRunqueueNs();
                    have_base = true;
                    first = false;
                }
                auto t0 = o.submit_time;
                if (t0 == Timestamp{}) t0 = std::chrono::steady_clock::now();
                engine.process(o);
                const auto t1 = std::chrono::steady_clock::now();
                std::int64_t ns =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                        .count();
                if (ns < 0) ns = 0;
                if (seen >= opt.warmup) {
                    if (seen == opt.warmup) {
                        window_start = std::chrono::steady_clock::now();
                    }
                    window_end = t1;
                    ++measured;
                }
                ++seen;
                latency.record(static_cast<std::uint64_t>(ns));
                if (o.enqueue_time != Timestamp{}) {
                    std::int64_t arr = std::chrono::duration_cast<
                                           std::chrono::nanoseconds>(
                                           o.enqueue_time - t0)
                                           .count();
                    std::int64_t svc = std::chrono::duration_cast<
                                           std::chrono::nanoseconds>(t1 -
                                                                     o.enqueue_time)
                                           .count();
                    if (arr < 0) arr = 0;
                    if (svc < 0) svc = 0;
                    arrival.record(static_cast<std::uint64_t>(arr));
                    service.record(static_cast<std::uint64_t>(svc));
                }

                if (opt.count_migrations && (seen & 63u) == 0) {
#ifdef __linux__
                    const int cpu = sched_getcpu();
                    if (last_cpu >= 0 && cpu != last_cpu) ++es.migrations;
                    last_cpu = cpu;
#endif
                }
            }
        };

        if (tlog) {
            TeeSink<CountingSink, TradeLogSink> tee(counting, *tlog);
            MatchingEngine<Book, decltype(tee)> engine(*book, tee);
            loop(engine);
        } else {
            MatchingEngine<Book, CountingSink> engine(*book, counting);
            loop(engine);
        }

        struct rusage ru {};
        if (have_base && getrusage(RUSAGE_THREAD, &ru) == 0) {
            es.nvcsw = static_cast<std::uint64_t>(ru.ru_nvcsw - base.ru_nvcsw);
            es.nivcsw = static_cast<std::uint64_t>(ru.ru_nivcsw - base.ru_nivcsw);
            es.minflt = static_cast<std::uint64_t>(ru.ru_minflt - base.ru_minflt);
            es.majflt = static_cast<std::uint64_t>(ru.ru_majflt - base.ru_majflt);
            es.user_s = timevalSeconds(ru.ru_utime) - timevalSeconds(base.ru_utime);
            es.sys_s = timevalSeconds(ru.ru_stime) - timevalSeconds(base.ru_stime);
            const std::uint64_t runq = threadRunqueueNs();
            es.runq_ns = (runq > runq_base) ? runq - runq_base : 0;
        }
    });

    std::thread net_thread([&] {
        if (opt.sched != "default" && opt.cpu_net >= 0) pinThisThread(opt.cpu_net);
        struct rusage nbase {};
        getrusage(RUSAGE_THREAD, &nbase);
        const std::uint64_t nrunq_base = threadRunqueueNs();
        server->run(g_stop);
        struct rusage nru {};
        if (getrusage(RUSAGE_THREAD, &nru) == 0) {
            net_es.nvcsw = static_cast<std::uint64_t>(nru.ru_nvcsw - nbase.ru_nvcsw);
            net_es.nivcsw = static_cast<std::uint64_t>(nru.ru_nivcsw - nbase.ru_nivcsw);
            net_es.user_s =
                timevalSeconds(nru.ru_utime) - timevalSeconds(nbase.ru_utime);
            net_es.sys_s =
                timevalSeconds(nru.ru_stime) - timevalSeconds(nbase.ru_stime);
            const std::uint64_t nrunq = threadRunqueueNs();
            net_es.runq_ns = (nrunq > nrunq_base) ? nrunq - nrunq_base : 0;
        }
        // The engine thread exits on rt_failed without consuming, so only
        // enqueue the sentinel if the consumer is still alive.
        if (!rt_failed.load(std::memory_order_relaxed)) {
            Order sentinel{};
            sentinel.id = 0;  // reserved shutdown sentinel
            queue->push(sentinel);
        }
    });

    net_thread.join();
    eng_thread.join();

    if (rt_failed.load(std::memory_order_relaxed)) return 2;

    if (tlog && opt.log_path) {
        tlog->flushToFile(opt.log_path);
        std::printf("log written to %s\n", opt.log_path);
    }

    const NetStats net = server->stats();
    const auto sorted = latency.sorted();
    double duration_s = 0;
    if (measured > 1 && window_end > window_start) {
        duration_s = std::chrono::duration<double>(window_end - window_start).count();
    }
    const double throughput =
        duration_s > 0 ? static_cast<double>(measured) / duration_s : 0;

    if (latency.dropped()) {
        std::fprintf(stderr, "warning: %zu latency samples dropped (capacity)\n",
                     latency.dropped());
    }

    const auto arr_sorted = arrival.sorted();
    const auto svc_sorted = service.sorted();
    writeStats(opt, seen, counting, net, measured, duration_s, throughput, sorted,
               arr_sorted, svc_sorted, es, net_es);

    std::printf(
        "\n=== %s run=%d ===\n"
        "orders=%llu warmup=%zu measured=%llu accepted=%llu trades=%llu "
        "rejected=%llu invalid=%llu\n"
        "throughput=%.0f/s duration=%.6fs\n"
        "p50=%llu p90=%llu p99=%llu p99.9=%llu max=%llu ns\n"
        "minflt=%llu majflt=%llu nvcsw=%llu nivcsw=%llu user=%.3fs sys=%.3fs "
        "migrations=%llu kernel_ops=%llu runq=%.3fms\n"
        "net: nvcsw=%llu nivcsw=%llu user=%.3fs sys=%.3fs runq=%.3fms\n"
        "split: arrival p50=%llu p99=%llu p99.9=%llu | service p50=%llu "
        "p99=%llu p99.9=%llu ns\n",
        opt.config.c_str(), opt.run,
        static_cast<unsigned long long>(seen), opt.warmup,
        static_cast<unsigned long long>(measured),
        static_cast<unsigned long long>(counting.accepted),
        static_cast<unsigned long long>(counting.trades),
        static_cast<unsigned long long>(counting.rejected),
        static_cast<unsigned long long>(net.invalid), throughput, duration_s,
        static_cast<unsigned long long>(
            LatencyRecorder::percentile(sorted, 0.50)),
        static_cast<unsigned long long>(
            LatencyRecorder::percentile(sorted, 0.90)),
        static_cast<unsigned long long>(
            LatencyRecorder::percentile(sorted, 0.99)),
        static_cast<unsigned long long>(
            LatencyRecorder::percentile(sorted, 0.999)),
        static_cast<unsigned long long>(
            LatencyRecorder::percentile(sorted, 1.0)),
        static_cast<unsigned long long>(es.minflt),
        static_cast<unsigned long long>(es.majflt),
        static_cast<unsigned long long>(es.nvcsw),
        static_cast<unsigned long long>(es.nivcsw), es.user_s, es.sys_s,
        static_cast<unsigned long long>(es.migrations),
        static_cast<unsigned long long>(
            (opt.net == "uring")
                ? net.submits + net.completions + net.accepts
                : net.epoll_waits + net.recv_calls + net.accepts),
        static_cast<double>(es.runq_ns) / 1e6,
        static_cast<unsigned long long>(net_es.nvcsw),
        static_cast<unsigned long long>(net_es.nivcsw), net_es.user_s,
        net_es.sys_s, static_cast<double>(net_es.runq_ns) / 1e6,
        static_cast<unsigned long long>(LatencyRecorder::percentile(arr_sorted, 0.50)),
        static_cast<unsigned long long>(LatencyRecorder::percentile(arr_sorted, 0.99)),
        static_cast<unsigned long long>(LatencyRecorder::percentile(arr_sorted, 0.999)),
        static_cast<unsigned long long>(LatencyRecorder::percentile(svc_sorted, 0.50)),
        static_cast<unsigned long long>(LatencyRecorder::percentile(svc_sorted, 0.99)),
        static_cast<unsigned long long>(LatencyRecorder::percentile(svc_sorted, 0.999)));

    if (ingestStatsEnabled()) {
        std::printf("ingest: orders_older_than_1ms=%llu max=%llu ns\n",
                    static_cast<unsigned long long>(net.ingest_slow),
                    static_cast<unsigned long long>(net.ingest_max_ns));
    }

    if (opt.samples_path && !latency.writeSamples(opt.samples_path)) {
        std::fprintf(stderr, "cannot write samples to %s\n", opt.samples_path);
    }
    return 0;
}

template <class Queue>
int runWithBook(const Options& opt) {
    if (opt.book == "v1") return runApp<Queue, OrderBook>(opt);
    if (opt.book == "fast-heap")
        return runApp<Queue, OrderBookFast<(1u << 20), (1u << 16), true>>(opt);
    return runApp<Queue, OrderBookFast<>>(opt);
}

static void usage(const char* argv0) {
    std::printf(
        "usage: %s [options]\n"
        "  --port N           listen port (default 9000)\n"
        "  --queue mutex|spsc (default spsc)\n"
        "  --book v1|fast-slab|fast-heap (default fast-slab)\n"
        "  --net epoll|uring  (default epoll)\n"
        "  --sched default|pin|rt (default default)\n"
        "  --cpu-engine N     engine core for pin/rt\n"
        "  --cpu-net N        network core (pinned when >= 0)\n"
        "  --warmup N         discard first N latency samples\n"
        "  --samples PATH     write raw latency samples (binary)\n"
        "  --stats PATH       append one CSV row per run\n"
        "  --config NAME      config label for the CSV\n"
        "  --run N            run number for the CSV\n"
        "  --kind NAME        latency|throughput label\n"
        "  --count-migrations sample sched_getcpu (adds overhead)\n"
        "  --exit-on-disconnect stop after the last client disconnects\n"
        "  --log PATH         write a trade log\n",
        argv0);
}

int main(int argc, char** argv) {
    std::signal(SIGINT, sigint_handler);

    Options opt;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (std::strcmp(a, "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (std::strcmp(a, "--port") == 0) {
            if (const char* v = next()) opt.port = static_cast<std::uint16_t>(std::atoi(v));
        } else if (std::strcmp(a, "--queue") == 0) {
            if (const char* v = next()) opt.queue = v;
        } else if (std::strcmp(a, "--book") == 0) {
            if (const char* v = next()) opt.book = v;
        } else if (std::strcmp(a, "--net") == 0) {
            if (const char* v = next()) opt.net = v;
        } else if (std::strcmp(a, "--sched") == 0) {
            if (const char* v = next()) opt.sched = v;
        } else if (std::strcmp(a, "--cpu-engine") == 0) {
            if (const char* v = next()) opt.cpu_engine = std::atoi(v);
        } else if (std::strcmp(a, "--cpu-net") == 0) {
            if (const char* v = next()) opt.cpu_net = std::atoi(v);
        } else if (std::strcmp(a, "--warmup") == 0) {
            if (const char* v = next()) opt.warmup = std::strtoull(v, nullptr, 10);
        } else if (std::strcmp(a, "--samples") == 0) {
            opt.samples_path = next();
        } else if (std::strcmp(a, "--stats") == 0) {
            opt.stats_path = next();
        } else if (std::strcmp(a, "--config") == 0) {
            if (const char* v = next()) opt.config = v;
        } else if (std::strcmp(a, "--run") == 0) {
            if (const char* v = next()) opt.run = std::atoi(v);
        } else if (std::strcmp(a, "--kind") == 0) {
            if (const char* v = next()) opt.kind = v;
        } else if (std::strcmp(a, "--log") == 0) {
            opt.log_path = next();
        } else if (std::strcmp(a, "--count-migrations") == 0) {
            opt.count_migrations = true;
        } else if (std::strcmp(a, "--exit-on-disconnect") == 0) {
            opt.exit_on_disconnect = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a);
            usage(argv[0]);
            return 1;
        }
    }

    if (opt.queue != "mutex" && opt.queue != "spsc") {
        std::fprintf(stderr, "bad --queue\n");
        return 1;
    }
    if (opt.book != "v1" && opt.book != "fast-slab" && opt.book != "fast-heap") {
        std::fprintf(stderr, "bad --book\n");
        return 1;
    }
    if (opt.net != "epoll" && opt.net != "uring") {
        std::fprintf(stderr, "bad --net\n");
        return 1;
    }
    if (opt.sched != "default" && opt.sched != "pin" && opt.sched != "rt") {
        std::fprintf(stderr, "bad --sched\n");
        return 1;
    }
    if (opt.sched != "default" && opt.cpu_engine < 0) {
        std::fprintf(stderr, "--sched %s needs --cpu-engine\n", opt.sched.c_str());
        return 1;
    }

    if (opt.queue == "spsc") {
        return runWithBook<SpscRingBuffer<16384>>(opt);
    }
    return runWithBook<RingBuffer>(opt);
}
