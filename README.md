# hyperliquid-cpp

**A production-grade C++20 connector for [Hyperliquid](https://hyperliquid.xyz): market data, order books,
order management and EIP-712 signing — in one dependency-light static library.**

```
 public WS ──► MarketDataClient ──► OrderBook (l2Book + bbo overlay) ──► your strategy
                                                                              │
 private WS / HTTP ◄── ExchangeClient ◄── Signer (msgpack·keccak·EIP-712·secp256k1)
        │                   ▲
        └── orderUpdates ───┘ order table · fills · positions · reconciliation
```

| | |
|---|---|
| **Market data** | `l2Book`, `bbo`, `trades`, `activeAssetCtx`, `allMids`, raw channels · auto-reconnect with subscription replay · heartbeat + stale detection |
| **Order book** | allocation-free, 64 levels/side · snapshot + best-bid/offer overlay · mid, microprice, spread, depth, VWAP |
| **Order management** | place / batch / cancel / cancel-all / modify / scheduleCancel / updateLeverage · WebSocket `post` **or** HTTP · unified order state from acks + `orderUpdates` + `userFills` · positions · automatic reconciliation |
| **Signing** | byte-identical to the official Python SDK (golden-vector tested) · optional precomputed-nonce ECDSA: 0.17 µs per signature · agent (API) wallets · vaults / sub-accounts · `expiresAfter` |
| **Venue rules** | asset ids resolved from `meta`/`spotMeta` · exact price (5 significant figures) and size rounding |
| **Engineering** | exact fixed-point decimals (no floating point on the wire path) · single-threaded epoll reactor · 135 tests incl. end-to-end against a mock venue · ASan/UBSan/TSan clean · GCC 15 / Clang 21 · `-Werror` |

## Performance

Measured on a 2012 Intel i7-3820, single core, Clang 21 `-O3` (current server cores are ~2× faster) — full table and methodology in
[docs/BENCHMARKS.md](docs/BENCHMARKS.md).

| Operation | Latency |
|---|---|
| Parse `bbo` frame → `BboMsg` | **336 ns** |
| Parse `l2Book` (20×20 levels, 1.6 KB) | 4.0 µs |
| Apply snapshot to `OrderBook` | 52 ns |
| Replay of a real mainnet session | **514 MB/s · 1.64 M msg/s** |
| Exact decimal parse (vs `strtod` 101 ns) | **19 ns** |
| Order → signed WebSocket frame (msgpack, Keccak, EIP-712, ECDSA) | 42 µs → **3.3 µs** with precomputed nonces |

Verified: 135 tests on Clang 21 / GCC 11 / GCC 15, ASan+UBSan and ThreadSanitizer clean; signatures (deterministic and
precomputed-nonce) accepted by the live testnet over WebSocket and HTTP — see
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#verification-matrix-v110).

Hyperliquid's own latency floor is block time (~0.2 s), so the connector never is the bottleneck —
it leaves the whole budget to your strategy.

## Quick start

Requirements: Linux x86-64/aarch64, CMake ≥ 3.21, a C++20 compiler (GCC ≥ 11, Clang ≥ 14),
OpenSSL ≥ 3.0 development headers, Ninja, network access at configure time (libsecp256k1 and
simdjson are fetched and built statically).

```bash
scripts/build.sh release           # or: cmake --preset release && cmake --build --preset release
scripts/test.sh release            # 135 tests, ~3 s
build/release/examples/hl_book_printer BTC ETH SOL
build/release/examples/hl_testnet_quoter --dry-run --coin ETH
```

### Docker

```bash
docker build -t hyperliquid-cpp .                  # compiles, runs all 135 tests, produces a ~133 MB runtime image
docker run --rm hyperliquid-cpp hl_book_printer BTC ETH
docker run --rm hyperliquid-cpp hl_testnet_quoter --dry-run --coin ETH
docker run --rm -e HL_PRIVATE_KEY -e HL_ACCOUNT_ADDRESS hyperliquid-cpp hl_testnet_quoter --coin ETH --duration 600
docker run --rm hyperliquid-cpp hl_tests           # or hl_benchmarks
docker build --target dev -t hyperliquid-cpp:dev . # toolchain + built tree + installed library in /opt/hyperliquid-cpp
```

The runtime image (Ubuntu 24.04) runs as an unprivileged user and contains only the example binaries, the test
and benchmark executables and their fixtures. Keys are passed as environment variables, never baked into the image.

### Market data in 20 lines

```cpp
#include <hl/hyperliquid.h>

struct Printer final : hl::MarketDataListener {
    void onBookUpdate(const hl::OrderBook& book, BookUpdate) override {
        std::printf("%s  %s / %s  (%.2f bps)\n", book.coin().c_str(),
                    book.bestBid()->px.toString().c_str(), book.bestAsk()->px.toString().c_str(),
                    book.spreadBps());
    }
};

int main() {
    hl::EventLoop loop;
    Printer printer;
    hl::MarketDataClient md(loop, printer, {.network = hl::Network::Mainnet});
    md.subscribeBook("BTC");   // l2Book + bbo → maintained OrderBook
    md.start();
    loop.run();
}
```

### Orders

```cpp
struct Strategy final : hl::ExchangeListener {
    hl::ExchangeClient* ex = nullptr;

    void onReady() override {
        const hl::AssetInfo* eth = ex->assets().find("ETH");
        hl::OrderRequest req{.coin = "ETH", .side = hl::Side::Buy,
                             .px = eth->roundPx(hl::Decimal::parseOrZero("2390.456"), hl::RoundingMode::Down),
                             .sz = eth->roundSz(hl::Decimal::parseOrZero("0.01")),
                             .tif = hl::Tif::Alo};
        if (auto cloid = ex->placeOrder(req); !cloid) {
            std::printf("rejected locally: %s\n", cloid.error().message.c_str());
        }
    }
    void onOrderUpdate(const hl::Order& o) override {
        std::printf("%s %s oid=%llu filled=%s %s\n", o.cloid.toString().c_str(),
                    std::string(hl::toString(o.state)).c_str(), (unsigned long long)o.oid,
                    o.filledSz.toString().c_str(), o.lastError.c_str());
    }
    void onFill(const hl::Fill& f) override { /* position, PnL … */ }
};

hl::ExchangeConfig cfg;
cfg.network = hl::Network::Testnet;
cfg.privateKey = std::getenv("HL_PRIVATE_KEY");         // API (agent) wallet key
cfg.accountAddress = std::getenv("HL_ACCOUNT_ADDRESS"); // master account
Strategy strategy;
hl::ExchangeClient ex(loop, strategy, cfg);
strategy.ex = &ex;
ex.start();
loop.run();
```

## The testnet demo

`hl_testnet_quoter` is a complete, readable market maker built only on the public API: it maintains
the book from `l2Book` + `bbo`, quotes both sides post-only around the microprice with inventory skew
and a hard position limit, amends quotes in place, backs off on rejections, optionally keeps a
dead-man's switch, and cancels everything on Ctrl-C with a PnL summary.

```bash
export HL_PRIVATE_KEY=0x…        HL_ACCOUNT_ADDRESS=0x…
build/release/examples/hl_testnet_quoter --coin ETH --notional 20 --half-spread-bps 8 --duration 600
```

Setting up a testnet account and API wallet: [docs/TESTNET.md](docs/TESTNET.md).

## Documentation

| Document | Contents |
|---|---|
| [docs/API.md](docs/API.md) | Complete API reference: every class, method, field, callback, state transition and error |
| [docs/RUNNING.md](docs/RUNNING.md) | Building, running natively and in Docker, credentials, low-latency deployment, systemd, monitoring, production checklist |
| [docs/ORDER_MANAGEMENT.md](docs/ORDER_MANAGEMENT.md) | The order-management algorithm step by step: submission, correlation, merging acks/updates/fills, modify, reconciliation, reconnects, latency |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Layers, threading, data flow, order state machine, reliability design |
| [docs/SIGNING.md](docs/SIGNING.md) | Exact Hyperliquid L1-action signing specification with worked vectors; precomputed-nonce ECDSA |
| [docs/TESTNET.md](docs/TESTNET.md) | Testnet account, API wallet, running and reading the demo |
| [docs/BENCHMARKS.md](docs/BENCHMARKS.md) | Benchmark results and how to reproduce them |
| [CHANGELOG.md](CHANGELOG.md) | Release history |

Doxygen HTML: `scripts/docs.sh`.

## Integrating into your build

```cmake
include(FetchContent)
FetchContent_Declare(hyperliquid_cpp GIT_REPOSITORY <your-licensed-repo-url> GIT_TAG v1.1.0)
FetchContent_MakeAvailable(hyperliquid_cpp)      # or: add_subdirectory(third_party/hyperliquid-cpp)
target_link_libraries(my_bot PRIVATE hyperliquid::hyperliquid)
```

As a sub-project only the library is built (tests, benchmarks, examples and `-Werror` are off).
simdjson and libsecp256k1 are private, statically linked implementation details — their headers
never reach your code. `cmake --install` places headers, `libhyperliquid.a`, `libsecp256k1.a` and
`libsimdjson.a` for non-CMake builds (link all three plus OpenSSL).

## Repository layout

```
include/hl/          public headers (core/, crypto/, md/, om/, net/, WsMessages.h, hyperliquid.h)
src/                 implementation (+ private JSON helper)
tests/               unit, golden-vector and end-to-end tests; support/MockVenue; fixtures/
benchmarks/          Google Benchmark suite
examples/            book_printer/, testnet_quoter/
docs/                reference and guides
scripts/             build.sh, test.sh, bench.sh, docs.sh
```

## License

Proprietary — see [LICENSE](LICENSE). Third-party components: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
