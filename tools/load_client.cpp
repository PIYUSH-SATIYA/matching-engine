// load_client HOST PORT N [seed] [RATE]
//
// Sends N orders in the shared 40-byte wire format.  RATE > 0 throttles to
// RATE orders/sec in batches of 64 using an absolute schedule (no drift).
// Each message carries a CLOCK_MONOTONIC timestamp taken when the batch is
// built; server and client must run on the same host.

#include <arpa/inet.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <random>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "wire.hpp"

static std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "Usage: load_client HOST PORT N [seed] [RATE]\n");
        return 1;
    }

    const char* host = argv[1];
    const std::uint16_t port = static_cast<std::uint16_t>(std::atoi(argv[2]));
    const std::uint64_t N = static_cast<std::uint64_t>(std::atoll(argv[3]));
    const unsigned seed = (argc >= 5) ? static_cast<unsigned>(std::atoi(argv[4])) : 42u;
    const double rate = (argc >= 6) ? std::atof(argv[5]) : 0.0;

    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    std::snprintf(port_str, sizeof(port_str), "%u", port);
    if (::getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        std::fprintf(stderr, "getaddrinfo failed\n");
        return 1;
    }

    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        std::perror("socket");
        return 1;
    }

    int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // Retry briefly so the harness can start the server first without having
    // to probe the port with a throwaway connection.
    bool connected = false;
    for (int attempt = 0; attempt < 500; ++attempt) {
        if (::connect(fd, res->ai_addr, res->ai_addrlen) == 0) {
            connected = true;
            break;
        }
        if (errno != ECONNREFUSED && errno != ENOENT) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::freeaddrinfo(res);
    if (!connected) {
        std::perror("connect");
        ::close(fd);
        return 1;
    }

    std::mt19937_64 rng(seed);
    std::normal_distribution<double> price_noise(0.0, 20.0);
    std::uniform_int_distribution<std::uint64_t> qty_dist(1, 100);
    std::uniform_int_distribution<int> side_dist(0, 1);

    static constexpr std::size_t BATCH_THROTTLE = 64;
    static constexpr std::size_t BATCH_FREE = 1024;
    const std::size_t BATCH = (rate > 0.0) ? BATCH_THROTTLE : BATCH_FREE;
    wire::Msg batch[BATCH_FREE];

    using Clock = std::chrono::steady_clock;
    using Dur = std::chrono::duration<double>;
    const auto t_start = Clock::now();

    std::uint64_t sent = 0;
    std::uint64_t batch_k = 0;
    while (sent < N) {
        const std::size_t chunk =
            (N - sent < BATCH) ? static_cast<std::size_t>(N - sent) : BATCH;

        if (rate > 0.0) {
            const double due_sec =
                static_cast<double>(batch_k) * static_cast<double>(BATCH) / rate;
            const auto due =
                t_start + std::chrono::duration_cast<Clock::duration>(Dur(due_sec));
            const auto now = Clock::now();
            if (due > now) {
                const auto sleep_until = due - std::chrono::microseconds(200);
                if (sleep_until > now) std::this_thread::sleep_until(sleep_until);
                while (Clock::now() < due) {
                }
            }
        }

        const std::uint64_t ts = nowNs();
        for (std::size_t i = 0; i < chunk; ++i) {
            wire::Msg& m = batch[i];
            m.id = sent + i + 1;
            m.side = static_cast<std::uint8_t>(side_dist(rng));
            std::memset(m.pad, 0, sizeof(m.pad));
            const double p = 10000.0 + price_noise(rng);
            m.price = static_cast<std::int64_t>(std::round(p));
            m.quantity = qty_dist(rng);
            m.client_ts_ns = ts;
        }

        const char* ptr = reinterpret_cast<const char*>(batch);
        std::size_t total = chunk * sizeof(wire::Msg);
        while (total > 0) {
            const ssize_t r = ::send(fd, ptr, total, 0);
            if (r <= 0) {
                std::perror("send");
                ::close(fd);
                return 1;
            }
            ptr += r;
            total -= static_cast<std::size_t>(r);
        }
        sent += chunk;
        ++batch_k;
    }

    const auto t_end = Clock::now();
    const double elapsed = std::chrono::duration<double>(t_end - t_start).count();
    std::printf("Sent %llu orders in %.3f s = %.0f orders/sec\n",
                static_cast<unsigned long long>(N), elapsed,
                static_cast<double>(N) / elapsed);

    ::close(fd);
    return 0;
}
