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
| **Market data** | `l2Book` (20-level, aggregated, or `fast` 5-level at ~10× the rate), `bbo`, `trades`, `activeAssetCtx`, `allMids`, `candle` · auto-reconnect with subscription replay · heartbeat + stale detection |
| **Order book** | allocation-free, 64 levels/side · snapshot + best-bid/offer overlay · mid, microprice, spread, depth, VWAP |
| **Order management** | place / batch / cancel / cancel-all / modify (incl. stops) / scheduleCancel / updateLeverage / updateIsolatedMargin · TP-SL grouping, builder codes, `expiresAfter`, fast cancels, order-priority fees · WebSocket `post` **or** HTTP · unified order state from acks + `orderUpdates` + `userFills` · positions (perp **and** spot) · automatic reconciliation · adopts orders left by a previous process · agent-wallet/master detection |
| **Account & risk** | liquidations and venue-initiated cancels (`userEvents`) · funding payments · request-budget tracking with 429 / `Retry-After` handling · account state, open orders, order status, fills by time, historical orders, funding history, predicted fundings, candles, spot balances, rate limits |
| **Signing** | byte-identical to the official Python SDK (golden-vector tested) · optional precomputed-nonce ECDSA: 0.17 µs per signature · agent (API) wallets · vaults / sub-accounts · `expiresAfter` |
| **Venue rules** | asset ids resolved from `meta`/`spotMeta` · exact price (5 significant figures) and size rounding |
| **Engineering** | exact fixed-point decimals (no floating point on the wire path) · single-threaded epoll reactor, re-entrancy-safe callbacks · 190 tests incl. end-to-end against a mock venue and a 17-step live acceptance run · ASan/UBSan/TSan clean · GCC 11/15, Clang 21 · `-Werror` |
| **Not included, by design** | the library cannot move funds: withdrawals, transfers and staking need EIP-712 user-signed actions it does not implement, so a compromised strategy process cannot drain the account ([docs/COVERAGE.md](docs/COVERAGE.md)) |

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

Verified: 190 tests on Clang 21 / GCC 11 / GCC 15, ASan+UBSan and ThreadSanitizer clean, plus scripted live
acceptance runs — **17/17 steps on testnet perps** and **twelve instruments on mainnet with real money** (every perp `szDecimals` from 0 to 5, spot and perps, both transports) (resting
orders, amendments, cancels, batches, post-only rejection, real taker fills with fees, `expiresAfter`, forced
reconnect with reconciliation) over both WebSocket and HTTP — see
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#verification-matrix) and a full mainnet log in
[docs/RUNNING.md §4](docs/RUNNING.md#4-acceptance-run-against-a-live-venue).

Hyperliquid's own latency floor is block time (~0.2 s), so the connector never is the bottleneck —
it leaves the whole budget to your strategy.

## Quick start

Requirements: Linux x86-64/aarch64, CMake ≥ 3.21, a C++20 compiler (GCC ≥ 11, Clang ≥ 14),
OpenSSL ≥ 3.0 development headers, Ninja, network access at configure time (libsecp256k1 and
simdjson are fetched and built statically).

```bash
scripts/build.sh release           # or: cmake --preset release && cmake --build --preset release
scripts/test.sh release            # 190 tests, ~5 s
build/release/examples/hl_book_printer BTC ETH SOL
build/release/examples/hl_testnet_quoter --dry-run --coin ETH
```

### Docker

```bash
docker build -t hyperliquid-cpp .                  # compiles, runs all 190 tests, produces a ~138 MB runtime image
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
hyperliquid-cpp 1.3.0 — ETH quoter on testnet (live orders)
[ex] ready: account 0x…, signer 0x…, ETH asset=1 szDecimals=4, position 0
[status] ETH mid=2645.4 spread=1.51bps bid=2643.9(Open) ask=2648.3(Open) pos=0 fills=0 vol=$0 pnl≈$0 order_rt=801/801ms(mean/last) sign=0.01ms
[status] ETH mid=2644.8 spread=1.51bps bid=2643.1(Open) ask=2647.5(Open) pos=0 fills=0 vol=$0 pnl≈$0 order_rt=801/801ms(mean/last) sign=0.01ms
[status] ETH mid=2646.25 spread=1.13bps bid=2643.8(Open) ask=2648.1(Open) pos=0 fills=0 vol=$0 pnl≈$0 order_rt=801/801ms(mean/last) sign=0.01ms
^C
stopping…
[shutdown] canceling 2 live order(s)…

══ summary ══════════════════════════════════════════════
  coin            ETH
  orders placed   2   amendments 10   rejects 0
  fills           0 (maker 0)   volume $0
  position        0 → 0
  pnl (mark@mid)  $0
  actions         13 sent, 0 via HTTP, 0 errors, 0 timeouts, 0 reconciles
  signatures      13 precomputed-nonce, 0 deterministic
  build+sign      n=13   mean   0.008 ms   min   0.003   max   0.016
  order rt        n=2    mean 801.017 ms   min 800.995   max 801.040
  cancel rt       n=1    mean 809.695 ms   min 809.695   max 809.695
  modify rt       n=10   mean 840.116 ms   min 752.675   max 1255.894
  md messages     63 (parse errors 0, reconnects 0)
═════════════════════════════════════════════════════════
```

Two things in that output are the point of this library. **`build+sign 0.008 ms`** is everything it
does per action — encode, keccak, EIP-712, ECDSA, frame. **`order rt 801 ms`** is the venue: block
production plus the network. And **`amendments 10` against `orders placed 2`** is the quoting model
working — quotes are moved with `modify` in place, keeping the order id and its queue position
instead of cancelling and re-placing.

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
| [docs/LICENSING.md](docs/LICENSING.md) | The licence in plain language: what you may and may not do, warranties, FAQ, pre-signature checklist |
| [CHANGELOG.md](CHANGELOG.md) | Release history |
| [docs/hyperliquid-cpp-offer-ru.pdf](docs/hyperliquid-cpp-offer-ru.pdf) | Коммерческое предложение (RU) — what is being sold, in three pages. Source: [docs/offer-ru.html](docs/offer-ru.html), rebuild with `scripts/offer-pdf.sh` |

Doxygen HTML: `scripts/docs.sh`.

## Integrating into your build

```cmake
include(FetchContent)
FetchContent_Declare(hyperliquid_cpp GIT_REPOSITORY <your-licensed-repo-url> GIT_TAG v1.3.0)
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
scripts/             build.sh, test.sh, bench.sh, docs.sh
```

## Licence

A perpetual, worldwide, **non-exclusive source-code licence**: use it commercially, modify it, and ship it
inside your own products in compiled form, with no seat count and no royalty. You may not resell it, publish
the source, distribute it as a connector or SDK, or patent what it embodies. The licensor warrants the code's
provenance — its own code, no hidden copyleft, no back doors — and indemnifies against third-party IP claims,
while keeping the right to license, resell or open-source the library to others.

Full terms: [LICENSE](LICENSE) (English) · [LICENSE.ru](LICENSE.ru) (Russian, equal force — the parties sign
one of them) · plain-language explanation and FAQ: [docs/LICENSING.md](docs/LICENSING.md) · third-party
components: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

---

© 2026 Denis Tishkov <denis8825@ya.ru>. hyperliquid-cpp is licensed, not sold — see [LICENSE](LICENSE).
