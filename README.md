# hyperliquid-cpp

**A C++20 trading client for [Hyperliquid](https://hyperliquid.xyz): market data, order books, order
management and EIP-712 signing in one static library with three private dependencies.**

```
 public WS ──► MarketDataClient ──► OrderBook (l2Book + bbo overlay) ──► your strategy
                                                                              │
 private WS / HTTP ◄── ExchangeClient ◄── Signer (msgpack·keccak·EIP-712·secp256k1)
        │                   ▲
        └── orderUpdates ───┘ order table · fills · positions · reconciliation
```

| | |
|---|---|
| **Market data** | `l2Book` (20-level, aggregated, or `fast` 5-level at ~10× the rate), `bbo`, `trades`, `activeAssetCtx`, `allMids`, `candle` · auto-reconnect with subscription replay · heartbeat + stale detection |
| **Order book** | allocation-free, 64 levels/side · snapshot + best-bid/offer overlay · mid, microprice, spread, depth, VWAP |
| **Order management** | place / batch / cancel / cancel-all / modify (incl. stops) / scheduleCancel / updateLeverage / updateIsolatedMargin · TP-SL grouping, builder codes, `expiresAfter`, fast cancels, order-priority fees · WebSocket `post` **or** HTTP · unified order state from acks + `orderUpdates` + `userFills` · positions (perp **and** spot) · automatic reconciliation · adopts orders left by a previous process · agent-wallet/master detection |
| **Account & risk** | liquidations and venue-initiated cancels (`userEvents`) · funding payments · request-budget tracking with 429 / `Retry-After` handling · account state, open orders, order status, fills by time, historical orders, funding history, predicted fundings, candles, spot balances, rate limits |
| **Signing** | byte-identical to the official Python SDK (golden-vector tested) · agent (API) wallets · vaults / sub-accounts · `expiresAfter` · optional precomputed-nonce ECDSA when a signature per 15 µs is too slow |
| **Venue rules** | asset ids resolved from `meta`/`spotMeta` · exact price (5 significant figures) and size rounding |
| **Engineering** | exact fixed-point decimals (no floating point on the wire path) · single-threaded epoll reactor, re-entrancy-safe callbacks · 200 tests incl. end-to-end against a mock venue and a 16-step live acceptance run · ASan/UBSan/TSan clean · GCC 11/15, Clang 21 · `-Werror` |
| **Not a general SDK** | a stateful trading client — order table, book, positions, reconciliation — not a thin endpoint wrapper. A free MIT SDK with wider endpoint coverage exists; the side-by-side, including where it wins, is in [docs/COMPARISON.md](docs/COMPARISON.md) |
| **Not included, by design** | the library cannot move funds: withdrawals, transfers and staking need EIP-712 user-signed actions it does not implement, so a compromised strategy process cannot drain the account ([docs/COVERAGE.md](docs/COVERAGE.md)) |

## Performance

Measured on an AWS `c8a.2xlarge` (AMD EPYC Zen 5, not bare metal), one isolated core, Clang 21 `-O3` —
environment, full table and methodology in [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

| Operation | Latency |
|---|---|
| Parse `bbo` frame → `BboMsg` | **169 ns** |
| Parse `l2Book` (20×20 levels, 1.6 KB) | 2.5 µs |
| Apply snapshot to `OrderBook` | 12 ns |
| Replay of a real mainnet session | 1.16 GiB/s · 3.8 M msg/s |
| Exact decimal parse (`strtod` on the same input: 27 ns) | **7.8 ns** |
| Order → signed WebSocket frame (msgpack, Keccak, EIP-712, ECDSA) | **15.9 µs**, or **1.17 µs** with precomputed nonces |

**How it is verified.** 200 tests on Clang 21, GCC 11 and GCC 15, clean under ASan+UBSan and
ThreadSanitizer. A scripted acceptance run walks the whole order-management contract against the
live venue: 16 of 16 steps on testnet over both transports, and the same run on mainnet with real
money across eleven instruments, covering every size precision Hyperliquid uses, both product types
and both transports. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#verification-matrix) has the
matrix; a full mainnet log is in
[docs/RUNNING.md §4](docs/RUNNING.md#4-acceptance-run-against-a-live-venue).

## Quick start

Requirements: Linux x86-64/aarch64, CMake ≥ 3.21, a C++20 compiler (GCC ≥ 11, Clang ≥ 14),
OpenSSL ≥ 3.0 development headers, Ninja, network access at configure time (libsecp256k1 and
simdjson are fetched and built statically).

```bash
scripts/build.sh release           # or: cmake --preset release && cmake --build --preset release
scripts/test.sh release            # 200 tests, ~20 s
scripts/ci.sh                      # everything: compilers, sanitizers, doc links, secret scan
build/release/examples/hl_book_printer BTC ETH SOL
build/release/examples/hl_testnet_quoter --dry-run --coin ETH
```

### Docker

```bash
docker build -t hyperliquid-cpp .                  # compiles, runs all 200 tests, produces a ~142 MB runtime image
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

## Acceptance check against the live venue

`hl_live_check` runs the order-management contract end to end against Hyperliquid and prints a pass/fail table
(exit code 0 only if every step passes) — use it after every build, configuration change or credential rotation:

```bash
build/release/examples/hl_live_check --key-file secrets/testnet.env --coin ETH --taker
build/release/examples/hl_live_check --key-file secrets/testnet.env --coin ETH --flatten   # emergency stop
```

Full output of a real run: [docs/TESTNET.md](docs/TESTNET.md#4-acceptance-check).

## The testnet demo, end to end

`hl_testnet_quoter` is a complete, readable market maker built only on the public API: it maintains
the book from `l2Book` + `bbo`, quotes both sides post-only around the microprice with inventory skew
and a hard position limit, amends quotes in place, backs off on rejections, optionally keeps a
dead-man's switch, and cancels everything on Ctrl-C with a PnL summary.

**1. Build.**

```bash
git clone https://github.com/tishden/hyperliquid-cpp && cd hyperliquid-cpp
cmake --preset release && cmake --build --preset release
```

**2. Watch it work without a key.** `--dry-run` runs the full market-data path and the quoting
maths, and prints the quotes it *would* send. Nothing is signed and nothing is sent.

```bash
build/release/examples/hl_testnet_quoter --dry-run --coin ETH
```

**3. Add credentials.** Create an API (agent) wallet in the Hyperliquid testnet UI, fund the account
from the faucet, then:

```bash
cp credentials.env.example secrets/testnet.env   # secrets/ is gitignored
chmod 600 secrets/testnet.env
$EDITOR secrets/testnet.env                      # agent key + the MASTER account address
```

**4. Quote for ten minutes on testnet.**

```bash
build/release/examples/hl_testnet_quoter --key-file secrets/testnet.env \
    --coin ETH --notional 20 --half-spread-bps 8 --duration 600
```

Real output from that command (trimmed):

```text
hyperliquid-cpp 1.4.1 — ETH quoter on testnet (live orders)
19:37:49.508 [ex] ready: account 0x…, signer 0x…, ETH asset=4 szDecimals=4, position 0
19:37:59.126 [status] ETH mid=2627.6 spread=1.52bps bid=2625.4(Open) ask=2629.8(Open) pos=0 fills=0 vol=$0 pnl≈$0 order_rt=753/753ms(mean/last) sign=0.01ms
19:41:09.165 [status] ETH mid=2634.05 spread=0.38bps bid=2631.9(Open) ask=2636.3(Open) pos=0 fills=0 vol=$0 pnl≈$0 order_rt=753/753ms(mean/last) sign=0.01ms
19:46:09.210 [status] ETH mid=2635.95 spread=1.14bps bid=2634.2(Open) ask=2638(Open) pos=0 fills=0 vol=$0 pnl≈$0 order_rt=753/753ms(mean/last) sign=0.01ms
…
stopping…
[shutdown] canceling 2 live order(s)…

══ summary ══════════════════════════════════════════════
  coin            ETH
  orders placed   2   amendments 136   rejects 0
  fills           0 (maker 0)   volume $0
  position        0 → 0
  pnl (mark@mid)  $0
  actions         139 sent, 0 via HTTP, 0 errors, 0 timeouts, 0 reconciles
  signatures      139 precomputed-nonce, 0 deterministic
  build+sign      n=139  mean   0.007 ms   min   0.003   max   0.036
  order rt        n=2    mean 753.105 ms   min 753.090   max 753.121
  cancel rt       n=1    mean 773.620 ms   min 773.620   max 773.620
  modify rt       n=136  mean 855.011 ms   min 737.131   max 2408.963
  md messages     747 (parse errors 0, reconnects 0)
═════════════════════════════════════════════════════════
```

Three numbers in that output matter. `build+sign 0.007 ms` is everything the library does per
action: encode, keccak, EIP-712, ECDSA, frame. `order rt 753 ms` is the venue — block
production plus the network, and nothing a client can shorten. And `amendments 136` against
`orders placed 2` is the quoting model working: quotes are moved with `modify` in place, which keeps
the order id and its queue position instead of cancelling and re-placing.

Credentials go in a `secrets/*.env` file (gitignored); the annotated
[credentials.env.example](credentials.env.example) explains how to tell an agent wallet from the
master account it trades for — the most common way to get this wrong. Setting up a testnet account
and API wallet: [docs/TESTNET.md](docs/TESTNET.md); running against mainnet and what a real run
looks like: [docs/RUNNING.md §4](docs/RUNNING.md#4-acceptance-run-against-a-live-venue).

## Documentation

| Document | Contents |
|---|---|
| [docs/API.md](docs/API.md) | Complete API reference: every class, method, field, callback, state transition and error |
| [docs/COVERAGE.md](docs/COVERAGE.md) | What of the Hyperliquid API is covered: every info endpoint, exchange action and WebSocket channel, with its status and how it was verified |
| [docs/RUNNING.md](docs/RUNNING.md) | Building, running natively and in Docker, credentials, low-latency deployment, systemd, monitoring, production checklist |
| [docs/ORDER_MANAGEMENT.md](docs/ORDER_MANAGEMENT.md) | The order-management algorithm step by step: submission, correlation, merging acks/updates/fills, modify, reconciliation, reconnects, latency |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Layers, threading, data flow, order state machine, reliability design |
| [docs/SIGNING.md](docs/SIGNING.md) | Exact Hyperliquid L1-action signing specification with worked vectors; precomputed-nonce ECDSA |
| [docs/TESTNET.md](docs/TESTNET.md) | Testnet account, API wallet, running and reading the demo |
| [docs/BENCHMARKS.md](docs/BENCHMARKS.md) | Benchmark results and how to reproduce them |
| [docs/COMPARISON.md](docs/COMPARISON.md) | Side-by-side with the other open-source C++ SDK: what each one is, where it is ahead and where this one is |
| [CHANGELOG.md](CHANGELOG.md) | Release history |

Doxygen HTML: `scripts/docs.sh`.

## Integrating into your build

```cmake
include(FetchContent)
FetchContent_Declare(hyperliquid_cpp GIT_REPOSITORY https://github.com/tishden/hyperliquid-cpp.git GIT_TAG v1.5.0)
FetchContent_MakeAvailable(hyperliquid_cpp)      # or: add_subdirectory(third_party/hyperliquid-cpp)
target_link_libraries(my_bot PRIVATE hyperliquid::hyperliquid)
```

As a sub-project only the library is built (tests, benchmarks, examples and `-Werror` are off).
Every dependency is private — OpenSSL, simdjson and libsecp256k1 — so their headers never reach
your code and your target does not inherit their include paths; CMake still links them for you.
`cmake --install` places headers, `libhyperliquid.a`, `libsecp256k1.a` and `libsimdjson.a` for
non-CMake builds (link all three plus OpenSSL).

## Repository layout

```
include/hl/          public headers (core/, crypto/, md/, om/, net/, WsMessages.h, hyperliquid.h)
src/                 implementation (+ private JSON helper)
tests/               unit, golden-vector and end-to-end tests; support/MockVenue; fixtures/
benchmarks/          Google Benchmark suite
examples/            book_printer/, testnet_quoter/
docs/                reference and guides
scripts/             build.sh, test.sh, bench.sh, docs.sh, ci.sh, check-docs.py
```

## Licence

[Apache License 2.0](LICENSE): use it commercially, modify it, ship it in open or closed products. Keep the
copyright and [NOTICE](NOTICE) in what you distribute. Third-party components and their licences are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Contributing

Bug reports, venue quirks and pull requests are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md). Security
issues go through [SECURITY.md](SECURITY.md), not public issues.

---

© 2026 Denis Tishkov <denis8825@ya.ru>. Licensed under the [Apache License 2.0](LICENSE).
