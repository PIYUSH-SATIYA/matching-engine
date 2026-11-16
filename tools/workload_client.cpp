// Profile-driven workload client for realistic/synthetic market-like loads.
//
// This intentionally lives beside load_client.cpp.  The original client and
// its benchmark semantics remain unchanged.

#include <arpa/inet.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <limits>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "wire.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 9000;
    std::uint64_t orders = 1'010'000;
    std::uint64_t seed = 42;
    double rate = 200'000.0;
    std::string profile = "market-like";
    std::string output;
    std::size_t batch = 0;
    double invalid_rate = 0.0;
    double duplicate_rate = 0.0;
    double buy_probability = 0.5;
};

struct Counters {
    std::uint64_t invalid_id = 0;
    std::uint64_t invalid_side = 0;
    std::uint64_t invalid_quantity = 0;
    std::uint64_t duplicate = 0;
    std::uint64_t valid = 0;
    std::uint64_t buys = 0;
    std::uint64_t sells = 0;
    std::int64_t min_price = std::numeric_limits<std::int64_t>::max();
    std::int64_t max_price = std::numeric_limits<std::int64_t>::min();
    std::uint64_t min_quantity = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_quantity = 0;
    long double price_sum = 0;
    long double quantity_sum = 0;
};

[[noreturn]] void usage(const char* name, int rc) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "  --host HOST                 default 127.0.0.1\n"
        "  --port PORT                 default 9000\n"
        "  --orders N                  total wire messages\n"
        "  --rate ORDERS/S             0 = unthrottled\n"
        "  --seed N                    deterministic seed\n"
        "  --profile NAME              uniform|clustered-book|market-like|burst|regime-switch|invalid-mix\n"
        "  --invalid-rate FRACTION    malformed-field fraction\n"
        "  --duplicate-rate FRACTION  duplicate-id fraction\n"
        "  --buy-probability FRACTION override side probability\n"
        "  --batch N                   default 64 when paced, 1024 unthrottled\n"
        "  --output DIR                write manifest.json and client-summary.csv\n",
        name);
    std::exit(rc);
}

double value(const char* name, const char* text) {
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    if (!end || *end != '\0' || !std::isfinite(v)) {
        std::fprintf(stderr, "invalid %s: %s\n", name, text);
        std::exit(2);
    }
    return v;
}

std::uint64_t integer(const char* name, const char* text) {
    char* end = nullptr;
    const auto v = std::strtoull(text, &end, 10);
    if (!end || *end != '\0') {
        std::fprintf(stderr, "invalid %s: %s\n", name, text);
        std::exit(2);
    }
    return v;
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) usage(argv[0], 2);
            return argv[++i];
        };
        if (a == "--help" || a == "-h") usage(argv[0], 0);
        else if (a == "--host") o.host = next();
        else if (a == "--port") o.port = static_cast<std::uint16_t>(integer("port", next()));
        else if (a == "--orders") o.orders = integer("orders", next());
        else if (a == "--rate") o.rate = value("rate", next());
        else if (a == "--seed") o.seed = integer("seed", next());
        else if (a == "--profile") o.profile = next();
        else if (a == "--output") o.output = next();
        else if (a == "--batch") o.batch = static_cast<std::size_t>(integer("batch", next()));
        else if (a == "--invalid-rate") o.invalid_rate = value("invalid-rate", next());
        else if (a == "--duplicate-rate") o.duplicate_rate = value("duplicate-rate", next());
        else if (a == "--buy-probability") o.buy_probability = value("buy-probability", next());
        else usage(argv[0], 2);
    }
    if (o.orders == 0 || o.rate < 0 || o.invalid_rate < 0 ||
        o.invalid_rate > 1 || o.duplicate_rate < 0 || o.duplicate_rate > 1 ||
        o.buy_probability < 0 || o.buy_probability > 1) {
        std::fprintf(stderr, "invalid non-negative/range option\n");
        std::exit(2);
    }
    if (o.profile != "uniform" && o.profile != "clustered-book" &&
        o.profile != "market-like" && o.profile != "burst" && o.profile != "invalid-mix" &&
        o.profile != "regime-switch") {
        std::fprintf(stderr, "unknown profile: %s\n", o.profile.c_str());
        std::exit(2);
    }
    return o;
}

int connectTo(const Options& o) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port[16];
    std::snprintf(port, sizeof(port), "%u", o.port);
    addrinfo* res = nullptr;
    if (::getaddrinfo(o.host.c_str(), port, &hints, &res) != 0 || !res) {
        std::fprintf(stderr, "getaddrinfo failed for %s\n", o.host.c_str());
        return -1;
    }
    const int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { ::freeaddrinfo(res); return -1; }
    int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    for (int attempt = 0; attempt < 500; ++attempt) {
        if (::connect(fd, res->ai_addr, res->ai_addrlen) == 0) {
            ::freeaddrinfo(res);
            return fd;
        }
        if (errno != ECONNREFUSED && errno != ENOENT) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::perror("connect");
    ::freeaddrinfo(res);
    ::close(fd);
    return -1;
}

double normal(std::mt19937_64& rng, double mean, double sd) {
    return std::normal_distribution<double>(mean, sd)(rng);
}

std::int64_t clampPrice(double p) {
    const auto rounded = static_cast<std::int64_t>(std::llround(p));
    return std::max<std::int64_t>(0, std::min<std::int64_t>(65535, rounded));
}

struct Generator {
    const Options& o;
    std::mt19937_64 rng;
    std::bernoulli_distribution coin{0.5};
    double mid = 32768.0;
    double drift = 0.0;
    std::uint64_t regime = 0;

    explicit Generator(const Options& options) : o(options), rng(options.seed) {}

    wire::Msg next(std::uint64_t sequence, Counters& c) {
        wire::Msg m{};
        const bool bad = std::bernoulli_distribution(o.invalid_rate)(rng);
        const bool duplicate = !bad && std::bernoulli_distribution(o.duplicate_rate)(rng);
        m.id = duplicate ? std::max<std::uint64_t>(1, sequence / 2) : sequence + 1;
        if (duplicate) ++c.duplicate;

        double buy_p = o.buy_probability;
        double spread = 12.0;
        double size_mean = 30.0;
        if (o.profile == "market-like" || o.profile == "regime-switch" ||
            o.profile == "invalid-mix") {
            ++regime;
            const std::uint64_t period = regime / 10'000;
            const int state = static_cast<int>(period % 4);
            buy_p = state == 1 ? 0.68 : state == 3 ? 0.32 : 0.50;
            drift = normal(rng, 0.0, state == 2 ? 0.10 : 0.025);
            mid = std::max(1000.0, std::min(64500.0, mid + drift));
            spread = state == 2 ? 4.0 : 14.0;
            size_mean = state == 2 ? 70.0 : 28.0;
        } else if (o.profile == "clustered-book") {
            spread = 8.0;
        } else if (o.profile == "burst") {
            const std::uint64_t phase = sequence % 20'000;
            const bool burst = phase >= 15'000;
            buy_p = burst ? 0.62 : 0.50;
            spread = burst ? 5.0 : 18.0;
            size_mean = burst ? 80.0 : 20.0;
        }

        const bool buy = std::bernoulli_distribution(buy_p)(rng);
        m.side = buy ? 0 : 1;
        double distance = 0.0;
        if (o.profile == "uniform") {
            distance = normal(rng, 0.0, 20.0);
        } else {
            const double u = std::generate_canonical<double, 53>(rng);
            if (u < 0.70) distance = normal(rng, 0.0, spread);
            else if (u < 0.92) distance = normal(rng, 0.0, spread * 3.0);
            else distance = normal(rng, 0.0, spread * 10.0);
        }
        m.price = clampPrice(mid + (buy ? -spread : spread) + distance);
        const double q = std::exp(normal(rng, std::log(size_mean), 0.75));
        m.quantity = static_cast<std::uint64_t>(std::max(1.0, std::min(1000.0, q)));
        m.client_ts_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now().time_since_epoch()).count());
        std::memset(m.pad, 0, sizeof(m.pad));

        if (bad) {
            const int kind = static_cast<int>(sequence % 3);
            if (kind == 0) { m.id = 0; ++c.invalid_id; }
            else if (kind == 1) { m.side = 2; ++c.invalid_side; }
            else { m.quantity = 0; ++c.invalid_quantity; }
        } else {
            ++c.valid;
            if (buy) ++c.buys; else ++c.sells;
            c.min_price = std::min(c.min_price, m.price);
            c.max_price = std::max(c.max_price, m.price);
            c.min_quantity = std::min(c.min_quantity, m.quantity);
            c.max_quantity = std::max(c.max_quantity, m.quantity);
            c.price_sum += m.price;
            c.quantity_sum += m.quantity;
        }
        return m;
    }
};

void writeMetadata(const Options& o, const Counters& c, double elapsed) {
    if (o.output.empty()) return;
    std::filesystem::create_directories(o.output);
    std::ofstream json(o.output + "/manifest.json");
    json << "{\n"
         << "  \"profile\": \"" << o.profile << "\",\n"
         << "  \"seed\": " << o.seed << ",\n"
         << "  \"orders\": " << o.orders << ",\n"
         << "  \"rate\": " << o.rate << ",\n"
         << "  \"batch\": " << o.batch << ",\n"
         << "  \"invalid_rate\": " << o.invalid_rate << ",\n"
         << "  \"duplicate_rate\": " << o.duplicate_rate << ",\n"
         << "  \"generated\": " << o.orders << ",\n"
         << "  \"valid_generated\": " << c.valid << ",\n"
         << "  \"invalid_id\": " << c.invalid_id << ",\n"
         << "  \"invalid_side\": " << c.invalid_side << ",\n"
         << "  \"invalid_quantity\": " << c.invalid_quantity << ",\n"
         << "  \"duplicate_generated\": " << c.duplicate << ",\n"
         << "  \"buys\": " << c.buys << ",\n"
         << "  \"sells\": " << c.sells << ",\n"
         << "  \"price_min\": " << (c.valid ? c.min_price : 0) << ",\n"
         << "  \"price_max\": " << (c.valid ? c.max_price : 0) << ",\n"
         << "  \"price_mean\": " << (c.valid ? static_cast<double>(c.price_sum / c.valid) : 0) << ",\n"
         << "  \"quantity_mean\": " << (c.valid ? static_cast<double>(c.quantity_sum / c.valid) : 0) << ",\n"
         << "  \"elapsed_s\": " << elapsed << "\n}\n";
    std::ofstream csv(o.output + "/client-summary.csv");
    csv << "generated,valid,invalid_id,invalid_side,invalid_quantity,duplicate,buys,sells,"
           "price_min,price_max,price_mean,quantity_mean,elapsed_s\n";
    csv << o.orders << ',' << c.valid << ',' << c.invalid_id << ',' << c.invalid_side << ','
        << c.invalid_quantity << ',' << c.duplicate << ',' << c.buys << ',' << c.sells << ','
        << (c.valid ? c.min_price : 0) << ',' << (c.valid ? c.max_price : 0) << ','
        << (c.valid ? static_cast<double>(c.price_sum / c.valid) : 0) << ','
        << (c.valid ? static_cast<double>(c.quantity_sum / c.valid) : 0) << ',' << elapsed << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    const Options o = parse(argc, argv);
    const int fd = connectTo(o);
    if (fd < 0) return 1;

    Generator gen(o);
    Counters counters;
    const std::size_t batch_size = o.batch ? o.batch : (o.rate > 0 ? 64 : 1024);
    std::vector<wire::Msg> batch(batch_size);
    const auto start = Clock::now();
    std::uint64_t sent = 0;
    std::uint64_t batch_no = 0;
    while (sent < o.orders) {
        const std::size_t n = static_cast<std::size_t>(
            std::min<std::uint64_t>(o.orders - sent, batch_size));
        if (o.rate > 0) {
            const double due_s = static_cast<double>(batch_no * batch_size) / o.rate;
            const auto due = start + std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(due_s));
            const auto now = Clock::now();
            if (due > now) {
                const auto spin_at = due - std::chrono::microseconds(200);
                if (spin_at > now) std::this_thread::sleep_until(spin_at);
                while (Clock::now() < due) {}
            }
        }
        for (std::size_t i = 0; i < n; ++i) batch[i] = gen.next(sent + i, counters);
        const char* p = reinterpret_cast<const char*>(batch.data());
        std::size_t bytes = n * sizeof(wire::Msg);
        while (bytes) {
            const ssize_t written = ::send(fd, p, bytes, 0);
            if (written <= 0) { std::perror("send"); ::close(fd); return 1; }
            p += written;
            bytes -= static_cast<std::size_t>(written);
        }
        sent += n;
        ++batch_no;
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    ::shutdown(fd, SHUT_WR);
    ::close(fd);
    writeMetadata(o, counters, elapsed);
    std::printf("profile=%s generated=%llu valid=%llu invalid=%llu duplicates=%llu "
                "buys=%llu sells=%llu elapsed=%.3fs rate=%.0f/s\n",
                o.profile.c_str(), static_cast<unsigned long long>(o.orders),
                static_cast<unsigned long long>(counters.valid),
                static_cast<unsigned long long>(counters.invalid_id + counters.invalid_side + counters.invalid_quantity),
                static_cast<unsigned long long>(counters.duplicate),
                static_cast<unsigned long long>(counters.buys),
                static_cast<unsigned long long>(counters.sells), elapsed,
                static_cast<double>(o.orders) / elapsed);
    return 0;
}
