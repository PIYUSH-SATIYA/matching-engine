#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "affinity.hpp"
#include "benchmark.hpp"
#include "event_sinks.hpp"
#include "matching_engine.hpp"
#include "network_layer.hpp"
#include "order.hpp"
#include "order_book_fast.hpp"
#include "ring_buffer.hpp"
#include "spsc_ring_buffer.hpp"

static std::atomic<bool> g_stop{false};

static void sigint_handler(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

template <class Queue, class Book>
void runApp(uint16_t port, int cpu_net, int cpu_eng, bool rt,
            const char* log_path) {
    Queue queue;
    Book  book;

    // Push adapter: network thread calls this
    auto push_fn = [](void* ctx, const Order& o) {
        static_cast<Queue*>(ctx)->push(o);
    };

    NetworkServer server(port, push_fn, &queue);
    if (!server.start()) {
        std::fprintf(stderr, "Failed to start network server on port %u\n", port);
        return;
    }

    // Sink: CountingSink (always) + optional TradeLogSink
    CountingSink  counting;
    TradeLogSink  tlog(1 << 20);  // 1M entries

    LatencyRecorder latency(4000000);

    // Engine thread
    std::thread eng_thread([&] {
        pinThisThread(cpu_eng);
        if (rt) setRealtimeFifo(50);

        if (log_path) {
            TeeSink<CountingSink, TradeLogSink> tee(counting, tlog);
            MatchingEngine<Book, decltype(tee)> engine(book, tee);
            while (true) {
                Order o = queue.pop();
                if (o.id == 0) break;
                auto t0 = o.submit_time;
                engine.process(o);
                auto t1 = std::chrono::steady_clock::now();
                uint64_t ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
                latency.record(ns);
            }
        } else {
            MatchingEngine<Book, CountingSink> engine(book, counting);
            while (true) {
                Order o = queue.pop();
                if (o.id == 0) break;
                auto t0 = o.submit_time;
                engine.process(o);
                auto t1 = std::chrono::steady_clock::now();
                uint64_t ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
                latency.record(ns);
            }
        }
    });

    // Network thread
    std::thread net_thread([&] {
        pinThisThread(cpu_net);
        server.run(g_stop);
        // Push sentinel so engine exits
        Order sentinel{};
        sentinel.id = 0;
        queue.push(sentinel);
    });

    net_thread.join();
    eng_thread.join();

    // Print summary
    std::printf("\n=== Shutdown ===\n");
    std::printf("trades=%llu accepted=%llu rejected=%llu\n",
                (unsigned long long)counting.trades,
                (unsigned long long)counting.accepted,
                (unsigned long long)counting.rejected);
    latency.summary();

    if (log_path) {
        tlog.flushToFile(log_path);
        std::printf("Log written to %s\n", log_path);
    }
}

int main(int argc, char** argv) {
    std::signal(SIGINT, sigint_handler);

    uint16_t    port       = 9000;
    std::string queue_type = "spsc";
    std::string book_type  = "v3";
    int         cpu_net    = -1;
    int         cpu_eng    = -1;
    bool        rt         = false;
    const char* log_path   = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--queue") == 0 && i + 1 < argc)
            queue_type = argv[++i];
        else if (std::strcmp(argv[i], "--book") == 0 && i + 1 < argc)
            book_type = argv[++i];
        else if (std::strcmp(argv[i], "--cpu-net") == 0 && i + 1 < argc)
            cpu_net = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--cpu-engine") == 0 && i + 1 < argc)
            cpu_eng = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--rt") == 0)
            rt = true;
        else if (std::strcmp(argv[i], "--log") == 0 && i + 1 < argc)
            log_path = argv[++i];
    }

    using FastBook = OrderBookFast<>;

    if (queue_type == "spsc") {
        runApp<SpscRingBuffer<65536>, FastBook>(port, cpu_net, cpu_eng, rt, log_path);
    } else {
        runApp<RingBuffer, FastBook>(port, cpu_net, cpu_eng, rt, log_path);
    }

    return 0;
}
