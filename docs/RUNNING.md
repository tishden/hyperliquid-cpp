# Running hyperliquid-cpp

How to build, run and operate the library and its applications — natively, in Docker and as a service.

- [1. Requirements](#1-requirements)
- [2. Building](#2-building)
- [3. Running the examples](#3-running-the-examples)
- [4. Acceptance run against a live venue](#4-acceptance-run-against-a-live-venue)
- [5. Docker](#5-docker)
- [6. Credentials](#6-credentials)
- [7. Configuration reference for your own application](#7-configuration-reference-for-your-own-application)
- [8. Low-latency deployment](#8-low-latency-deployment)
- [9. Running as a service (systemd)](#9-running-as-a-service-systemd)
- [10. Logging and monitoring](#10-logging-and-monitoring)
- [11. Shutdown and restarts](#11-shutdown-and-restarts)
- [12. Production checklist](#12-production-checklist)
- [13. Troubleshooting](#13-troubleshooting)

## 1. Requirements

| | Minimum | Tested |
|---|---|---|
| OS | Linux (epoll), x86-64 or aarch64 | RHEL/AlmaLinux 9, Ubuntu 24.04 |
| Compiler | GCC 11 / Clang 14, C++20 | GCC 11.5, GCC 15, Clang 21 |
| CMake | 3.21 | 3.26, 3.28 |
| OpenSSL | 3.0 (headers + libs) | 3.0, 3.5 |
| Build tools | Ninja, git (dependencies are fetched at configure time) | |
| Tests / benchmarks | GoogleTest, Google Benchmark (system packages or fetched) | |
| Runtime | `libssl`, CA certificates | |

## 2. Building

```bash
scripts/build.sh release     # build/release — optimised, portable
scripts/build.sh debug       # build/debug
scripts/build.sh asan        # build/asan — AddressSanitizer + UBSan
scripts/test.sh release      # build + all tests
scripts/bench.sh             # build + benchmarks pinned to core 2
```

Equivalent raw CMake:

```bash
cmake --preset release && cmake --build --preset release && ctest --preset release
```

| CMake option | Default | Effect |
|---|---|---|
| `HL_BUILD_TESTS` / `HL_BUILD_BENCHMARKS` / `HL_BUILD_EXAMPLES` | ON top-level, OFF as sub-project | |
| `HL_WARNINGS_AS_ERRORS` | ON top-level | `-Werror` |
| `HL_SANITIZE` | OFF | ASan + UBSan |
| `HL_NATIVE` | OFF | `-march=native` for the library, tests, examples — faster Keccak on modern CPUs, binaries not portable |
| `CMAKE_BUILD_TYPE` | Release | |

Choose the compiler with `CXX=clang++ CC=clang` (or `CXX=g++`) on the first configure. Clang produced ~15 %
faster Keccak than GCC on the reference machine.

## 3. Running the examples

| Binary | Purpose | Needs a key |
|---|---|---|
| `build/release/examples/hl_book_printer` | top of book, spread, microprice, funding, last trade for coins; mainnet by default, `--testnet` | no |
| `build/release/examples/hl_testnet_quoter` | two-sided post-only market maker on testnet | no with `--dry-run`, yes otherwise |
| `build/release/examples/hl_live_check` | scripted acceptance run of the whole order-management contract against the live venue; exit code 0 only if every step passed. Testnet by default, mainnet behind two flags — see [§4](#4-acceptance-run-against-a-live-venue) | yes |
| `build/release/tests/hl_tests` | full test suite (runs offline against an in-process mock venue) | no |
| `build/release/benchmarks/hl_benchmarks` | Google Benchmark suite | no |

```bash
build/release/examples/hl_book_printer BTC ETH SOL
build/release/examples/hl_testnet_quoter --dry-run --coin ETH

export HL_PRIVATE_KEY=0x…  HL_ACCOUNT_ADDRESS=0x…
build/release/examples/hl_testnet_quoter --coin ETH --notional 20 --half-spread-bps 8 --duration 600
```

```bash
# acceptance run before deploying a new build or configuration
build/release/examples/hl_live_check --key-file secrets/testnet.env --coin ETH --taker
build/release/examples/hl_live_check --key-file secrets/testnet.env --coin ETH --transport http
# emergency: cancel everything and close the position of one coin
build/release/examples/hl_live_check --key-file secrets/testnet.env --coin ETH --flatten
```

All quoter options are listed by `--help` and in [TESTNET.md](TESTNET.md#5-run-the-demo); account setup is
described there as well. The quoter enables the precomputed-nonce signer by default (`--presign 256`).


## 4. Acceptance run against a live venue

`hl_live_check` walks the whole order-management contract step by step and exits non-zero if any step
fails. It defaults to testnet; mainnet needs two flags, one of which spells out what it means:

```bash
# testnet
build/release/examples/hl_live_check --key-file secrets/testnet.env --coin ETH --taker

# mainnet, real money — resting orders sit 2 % away from mid unless --taker is given
build/release/examples/hl_live_check --key-file secrets/prod.env --coin @107 --notional 11 \
    --taker --expiry-ms 30000 --mainnet --i-understand-this-trades-real-money
```

Spot pairs are named `@<index>` (`PURR/USDC` is the exception, index 0); passing such a name loads
`spotMeta` automatically, and `--spot` forces it. Perp-only steps are skipped for a spot pair.

### What a real mainnet run looks like

Below is an unedited run against mainnet spot HYPE/USDC on 2026-09-19 from an ordinary, non-co-located
host, with a $30 account. Only the two addresses are redacted.

```text
hyperliquid-cpp 1.3.0 — live acceptance check on MAINNET (@107, WebSocket transport, presign 64, expiresAfter)
[hl][INFO] exchange: starting (mainnet, account 0x<master>, signer 0x<agent>)

▶ connect, load metadata, subscribe user streams
[hl][INFO] exchange: account value 0 USDC, 0 open positions
[hl][INFO] exchange: 563 assets loaded
[hl][INFO] exchange: ready
   PASS — account 0x<master>, 563 assets, @107 asset=10107 szDecimals=2

▶ market data: order book
   PASS — bid 93.116 / ask 93.117, spread 0.11 bps, size to use 0.12

▶ place post-only order far from mid → Open with oid
   PASS — px 91.254, state Open, oid 549961856293
   ⏱  order ×1 706.9 ms | build+sign ×1 0.016 ms | step 707 ms

▶ info: orderStatus and frontendOpenOrders see the order
   PASS — orderStatus: status open, oid 549961856293 | frontendOpenOrders: listed: oid 549961856293 px 91.254 sz 0.12 tif Alo

▶ modify price and size in place (cloid preserved)
   PASS — px 90.323, sz 0.24, oid 549961856293 → 549961870250, state Open
   ⏱  modify ×1 688.7 ms | build+sign ×1 0.008 ms | step 689 ms

▶ cancel by cloid → Canceled
   PASS — state Canceled
   ⏱  cancel ×1 708.9 ms | build+sign ×1 0.008 ms | step 709 ms

▶ batch of 2 orders in one action, then cancelAll
   PASS — states Open Open → cancelAll cleared all
   ⏱  order ×1 722.1 ms, cancel ×1 711.2 ms | build+sign ×2 0.009 ms | step 1433 ms

▶ post-only order that crosses → rejected by the venue
   PASS — Rejected: Post only order would have immediately matched, bbo was 93.116@93.124. asset=10107
   ⏱  order ×1 672.6 ms | build+sign ×1 0.007 ms | step 673 ms

▶ local validation rejects invalid price/size/coin before signing
   PASS — unknown coin 'NOSUCHCOIN' | invalid price 1234.56789 for @107 (nearest valid 1234.6) | price and size must be positive

▶ IOC order that crosses → fill, position and fees
      · fill Buy 0.12 @ 93.125 (taker, fee 0.000084)
   PASS — Filled, filled 0.12 @ 93.125, position 0.12991601, fills 1, fee 0.000084 HYPE
   ⏱  order ×1 944.2 ms | build+sign ×1 0.007 ms | step 1236 ms

▶ sell the acquired spot balance back with an IOC
      · fill Sell 0.12 @ 93.124 (taker, fee 0.00782241)
   PASS — Filled, position now 0.00983201 (below one lot — unsellable dust, the spot buy fee was charged in the base token)
   ⏱  order ×1 985.7 ms | build+sign ×1 0.007 ms | step 1057 ms

▶ scheduleCancel (dead-man's switch): arm and clear
[hl][WARN] exchange: action failed (Venue): Cannot set scheduled cancel time until enough volume traded. Required: $1000000. Traded: $43.73.
   PASS — arm: … | clear: …
   ⏱  other ×2 1010.0 ms | build+sign ×2 0.013 ms | step 2020 ms

▶ updateLeverage
   SKIP — leverage is a perp-only action; @107 is a spot pair

▶ reconnect: drop the private socket, reconcile a live order
[hl][WARN] ws: wss://api.hyperliquid.xyz/ws closed: live-check forced reconnect
[hl][INFO] exchange: ready
   PASS — reconnected, reconciles 0 → 1, order Open
   ⏱  order ×1 694.2 ms, cancel ×1 694.7 ms | build+sign ×2 0.008 ms | step 2783 ms

▶ no orders left on the venue
   PASS — 0 open orders on the venue

▶ no leftover position (a resting test order may have been filled)
   PASS — only 0.00983201 left — below one lot, cannot be sold

══ live check summary ═══════════════════════════════════
  … 15 steps, all PASS …
  ---------------------------------------------------
  actions 12 (0 via HTTP), errors 2, timeouts 0, reconciles 1
  signatures 12 precomputed-nonce / 0 deterministic
  --- latency (round trip includes the network to the venue) ---
  build+sign   n=12   mean    0.009 ms   min    0.007   max    0.019   last    0.009
  order        n=6    mean  787.619 ms   min  672.600   max  985.703   last  694.201
  cancel       n=3    mean  704.963 ms   min  694.739   max  711.225   last  694.739
  modify       n=1    mean  688.715 ms   min  688.715   max  688.715   last  688.715
  other        n=2    mean 1010.007 ms   min 1002.368   max 1017.647   last 1002.368
  order updates 28, fills 2, md messages 66 (parse errors 0)
  0 of 15 steps failed
═════════════════════════════════════════════════════════
```

**Read the latency block, not the marketing.** `build+sign` is everything this library does for an
action — encode, keccak, EIP-712, ECDSA, frame — and it is **9 µs**. The round trip is **700–1000 ms**,
because that is the venue: block production plus the network. The library is four orders of magnitude
away from being the bottleneck, which is exactly why the signing work went into the precomputed-nonce
path and no further. Budget your own strategy against ~0.8 s to know an order rested, not against µs.

Three venue behaviours the run makes concrete, all of them surprises for someone arriving from a CEX:

- **The spot taker fee on a buy is charged in the base token.** Buy 0.12 HYPE and 0.11991601 arrives.
  Selling "everything back" therefore always leaves a remainder, and when that remainder is smaller
  than one lot it cannot be sold at all. Above it is 0.0098 HYPE, worth about \$0.92, permanently stuck.
  Size spot round trips with this in mind.
- **`scheduleCancel` needs \$1 M of traded volume.** The dead-man's switch is not available to a new
  account; the run treats the refusal as a pass because the action, signature and error path are what
  it is checking.
- **Spot has no shorting and no leverage.** A sell needs the base token in the balance, and
  `updateLeverage` does not apply — the run skips it rather than failing.

### Same run, other instruments

Twelve instruments were run this way on mainnet, covering every perp `szDecimals` from 0 to 5, both
product types and both transports — see the table in
[ARCHITECTURE.md](ARCHITECTURE.md#verification-matrix). Every run ended flat with no orders left.
The interesting ones:

| Coin | What it exercises | Result |
|---|---|---|
| `BTC` | **perps**: `updateLeverage`, reduce-only close, real taker fill (fee in USDC, so no dust) | 16/16 |
| `XRP` | perps with `szDecimals 0` — sizes must be whole units, the rounding edge case | 14/14 |
| `@107` HYPE/USDC | spot, WebSocket, `--taker` with real fills, `expiresAfter` | 15/15 |
| `@151` UETH/USDC | spot, **`--transport http`** (10 of 10 actions over HTTP) | 13/13 |
| `PURR/USDC` | the one spot pair the venue names by pair instead of `@index` | 13/13 |

Perp coins take the same run and need no separate funding: on a unified account the spot USDC
balance already collateralises them (see "Account modes" in [§6](#6-credentials)). Only in the older
Manual/Standard mode is a perp balance separate, and this library cannot move funds into it —
`usdClassTransfer` is a user-signed action it deliberately does not implement
([COVERAGE.md §5](COVERAGE.md#5-deliberately-excluded)), so use the UI.

## 5. Docker

The `Dockerfile` has three stages:

| Target | Contents | Use |
|---|---|---|
| `build` | Ubuntu 24.04 toolchain; configures, builds and **runs the test suite** — the image build fails if a test fails | CI |
| `dev` | `build` + library installed in `/opt/hyperliquid-cpp` (headers, `libhyperliquid.a`, `libsecp256k1.a`, `libsimdjson.a`) and binaries on `PATH` | developing against the library |
| `runtime` (default) | Ubuntu 24.04 + `libssl3`, CA certificates, the four binaries and test fixtures; runs as unprivileged user `trader` | running |

```bash
docker build -t hyperliquid-cpp .                                   # runtime image (~133 MB)
docker build --build-arg RUN_TESTS=OFF -t hyperliquid-cpp .         # skip tests during the build
docker build --target dev -t hyperliquid-cpp:dev .

docker run --rm hyperliquid-cpp hl_book_printer BTC ETH
docker run --rm hyperliquid-cpp hl_testnet_quoter --dry-run --coin ETH
docker run --rm -e HL_PRIVATE_KEY -e HL_ACCOUNT_ADDRESS hyperliquid-cpp \
    hl_testnet_quoter --coin ETH --duration 600
docker run --rm hyperliquid-cpp hl_tests
docker run --rm --cpuset-cpus=2 hyperliquid-cpp hl_benchmarks
```

Key file instead of environment variables:

```bash
docker run --rm -v "$HOME/.config/hl/testnet.env:/run/secrets/hl.env:ro" hyperliquid-cpp \
    hl_testnet_quoter --key-file /run/secrets/hl.env --coin ETH
```

Notes:
- `docker run --init` (or `--init` in compose) forwards Ctrl-C/SIGTERM properly so the quoter can cancel its
  orders before exiting; `docker stop` sends SIGTERM and waits 10 s by default — enough for the 5 s cancel drain.
- The image sets `SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt`.
- Use `--network host` and `--cpuset-cpus` for latency-sensitive runs ([§8](#8-low-latency-deployment)).

## 6. Credentials

Start from the annotated template in the repository root:

```bash
cp credentials.env.example secrets/prod.env    # secrets/ is gitignored
chmod 600 secrets/prod.env
```

- Use an **API (agent) wallet** per environment (testnet and mainnet are separate): it can trade but not
  withdraw and can be revoked. `privateKey` = agent key, `accountAddress` = **master account**.
- `HL_ACCOUNT_ADDRESS` is the account the agent trades *for*, never the agent's own address. Get it wrong
  and orders go to the master while fills, positions and balances are read from the agent address, which
  holds nothing: the account looks empty while real orders rest on the venue. The client logs an error
  naming both addresses, but it does not stop. Ask the venue which is which:
  `{"type":"userRole","user":"<agent>"}` answers `{"role":"agent","data":{"user":"<master>"}}`.
  Omitting `HL_ACCOUNT_ADDRESS` entirely is safe — the client then adopts the master it is told about.
- Supply keys via environment variables, a key file with mode `600`, or your secret manager — never as
  command-line arguments (visible in `ps`) and never in images or git.
- Two processes must not share one signing key: nonces are generated per process and could collide. Use one
  agent wallet per process.

### Account modes: where the money actually is

Hyperliquid's default is the **Unified Account**: a single USDC balance, held in the *spot*
clearinghouse, collateralises spot trading and perp margin at once. There is no spot↔perp transfer
to perform — the UI has no such button, and `usdClassTransfer` is meaningless in this mode. The
older *Manual/Standard* mode, aimed at market makers, does keep separate perp and spot balances.

The consequence for anyone reading balances through the API:

| Question | Where to look |
|---|---|
| How much can I trade with? | **spot USDC balance** — `InfoClient::spotBalances`, or `ExchangeConfig::loadSpotAssets = true` so the client seeds it |
| What perp margin is committed right now? | `clearinghouseState.accountValue` — **0 while flat**, even on a funded account |
| What positions do I have? | `clearinghouseState.assetPositions` — correct in every mode |

So `exchange: account value 0 USDC, 0 open positions` in the start-up log is not a misconfiguration
on a unified account: it is the venue reporting that no collateral is committed to perps yet.
Measured on mainnet with 28.4 USDC on the account: `accountValue` read `0.0` while flat and `2.288`
while an \$11.4 BTC perp position was open, and a restarted client seeded that position correctly.

### Choosing the network

The network is a program setting, not part of the credentials file, so the same file cannot be pointed at
the wrong venue by accident.

| | Testnet | Mainnet |
|---|---|---|
| `ExchangeConfig::network`, `MarketDataConfig::network` | `hl::Network::Testnet` (**the default**) | `hl::Network::Mainnet` |
| REST (`restUrl(network)`) | `https://api.hyperliquid-testnet.xyz` | `https://api.hyperliquid.xyz` |
| WebSocket (`wsUrl(network)`) | `wss://api.hyperliquid-testnet.xyz/ws` | `wss://api.hyperliquid.xyz/ws` |
| `hl_live_check`, `hl_testnet_quoter` | default | `--mainnet --i-understand-this-trades-real-money` |
| `hl_book_printer` (read-only) | `--testnet` | default |

To reach anything else — a proxy, a recorded mock, a private gateway — set the overrides instead of the
network: `ExchangeConfig::restUrlOverride` and `wsUrlOverride`, `MarketDataConfig::urlOverride`. They take
precedence over `network`; an invalid `restUrlOverride` throws from the `ExchangeClient` constructor.

## 7. Configuration reference for your own application

Minimal wiring of both clients on one loop:

```cpp
hl::EventLoop loop;
MyStrategy strategy;                                      // MarketDataListener + ExchangeListener

hl::MarketDataConfig md;  md.network = hl::Network::Testnet;
hl::MarketDataClient marketData(loop, strategy, md);
marketData.subscribeBook("ETH");

hl::ExchangeConfig ex;
ex.network            = hl::Network::Testnet;
ex.privateKey         = std::getenv("HL_PRIVATE_KEY");
ex.accountAddress     = std::getenv("HL_ACCOUNT_ADDRESS");
ex.transport          = hl::ActionTransport::WebSocket;   // HTTP fallback is automatic
ex.precomputedNonces  = 512;                              // ~3 µs instead of ~42 µs per signed action
ex.requestTimeoutMs   = 5'000;
hl::ExchangeClient exchange(loop, strategy, ex);

marketData.start();
exchange.start();
loop.run();
```

| Setting | Recommendation |
|---|---|
| `ExchangeConfig::precomputedNonces` | 256–1024 for quoting; 0 if you need deterministic (reproducible) signatures |
| `ExchangeConfig::fastCancels` | leave on; the venue plans to prioritise flagged cancels in the mempool |
| `ExchangeConfig::actionExpiryMs` | 2 000–10 000 ms while quoting, so a stalled action cannot arrive late |
| `ExchangeConfig::adoptExistingOrders` | leave on, so a restart sees what the previous process left resting |
| `ExchangeConfig::subscribeUserEvents` | leave on: liquidations arrive in no other stream |
| `ExchangeConfig::nonces` | share one generator between clients that sign with the same key |
| `ExchangeConfig::rateLimitRefreshMs` | 30–60 s while quoting |
| `ExchangeConfig::transport` | `WebSocket` (one connection for posts, order updates and fills) |
| `ExchangeConfig::requestTimeoutMs` | 3–10 s; shorter means faster reconciliation of lost actions |
| `WsSessionOptions::pingIntervalMs` / `staleTimeoutMs` | 20 s / 60 s (HL closes idle sockets after 60 s) |
| `WsSessionOptions::reconnectMinDelayMs` / `reconnectMaxDelayMs` | 250 ms / 10 s |
| `TlsOptions::connectTimeoutMs` | 10 s total, split across resolved addresses |
| `hl::setLogLevel` | `Info` in production, `Debug` when diagnosing |

## 8. Low-latency deployment

The venue's block time (~0.2 s) bounds end-to-end latency, but a quiet, predictable client keeps queue
position and reaction time consistent:

1. **Location.** Hyperliquid's API is served through CloudFront; measure RTT from candidate regions
   (`curl -w '%{time_connect} %{time_starttransfer}\n' -o /dev/null -X POST https://api.hyperliquid.xyz/info -d '{"type":"meta"}' -H 'Content-Type: application/json'`)
   and deploy where it is lowest (typically Tokyo).
2. **Dedicated core for the loop.** Isolate a core (`isolcpus=` / `nohz_full=` or cgroup cpusets) and run the
   event loop there, busy-polling:
   ```cpp
   std::thread io([&] {
       cpu_set_t set; CPU_ZERO(&set); CPU_SET(3, &set);
       pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
       while (!loop.stopped()) { loop.runOnce(0); }
   });
   ```
   Pass work from other threads with `loop.postThreadSafe`.
3. **Nonce pool thread elsewhere.** The refill thread started by `precomputedNonces` inherits the creating
   thread's affinity — construct `ExchangeClient` on a thread pinned to a *non*-critical core (or call
   `pthread_setaffinity_np` before construction) so refills never compete with the loop.
4. **`HL_NATIVE=ON`** when building on the deployment hardware.
5. **Market data and orders on separate loops/cores** if you subscribe to many coins: parsing a large
   `allMids`/`trades` burst then cannot delay an order.
6. **Performance governor**, no frequency scaling on the critical cores; disable transparent huge page
   compaction jitter if observed.
7. Host networking in containers (`--network host`).

## 9. Running as a service (systemd)

```ini
# /etc/systemd/system/hl-quoter.service
[Unit]
Description=Hyperliquid quoter
After=network-online.target
Wants=network-online.target

[Service]
User=trader
EnvironmentFile=/etc/hl/testnet.env          # HL_PRIVATE_KEY=…, HL_ACCOUNT_ADDRESS=… (mode 600)
ExecStart=/usr/local/bin/hl_testnet_quoter --coin ETH --dead-man-switch
KillSignal=SIGTERM
TimeoutStopSec=15                            # quoter drains cancels for up to 5 s
Restart=on-failure
RestartSec=5
CPUAffinity=2 3
NoNewPrivileges=yes
ProtectSystem=strict
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload && sudo systemctl enable --now hl-quoter
journalctl -u hl-quoter -f
```

## 10. Logging and monitoring

- Library log lines go through `hl::setLogSink` (default: stderr, `[hl][LEVEL] message`). The library logs
  only on the control path: connects, reconnects, failed actions, reconciliation problems — never per message.
- Poll counters periodically and export them to your metrics system:

| Source | Counter | Alert when |
|---|---|---|
| `MarketDataClient::parserStats()` | `messages`, `parseErrors`, `unhandled` | `parseErrors` grows |
| `MarketDataClient::reconnectCount()` | reconnects | grows repeatedly |
| `OrderBook::timeMs()` | exchange time of last update | older than a few seconds while connected |
| `ExchangeClient::stats()` | `actionsSent`, `actionsViaHttp`, `actionErrors`, `timeouts`, `fills`, `reconciles` | `timeouts`/`reconciles` grow; `actionsViaHttp` grows with WS transport (socket flapping) |
| `ExchangeClient::signingStats()` | `precomputed`, `deterministic` | `deterministic` grows with the pool enabled (pool too small) |
| `ExchangeClient::rateLimitStatus()` | the venue's `requestsUsed`/`requestsCap` plus local submissions | less than ~20 % of the budget remains (running out throttles the account to one request per 10 s) |
| `ExchangeClient::stats().rateLimitHits` | HTTP 429 responses | any sustained growth |
| `ExchangeClient::stats().addressUnitsUsed` | order/cancel entries submitted | compare with traded volume: the budget grows by 1 per USDC |
| `ExchangeClient::isReady()` | readiness | false for longer than the reconnect backoff |

## 11. Shutdown and restarts

1. Stop generating new orders.
2. `exchange.cancelAll()` and run the loop until `liveOrders()` is empty (bounded wait).
3. Optionally clear the dead-man's switch: `scheduleCancel(std::nullopt)`.
4. `exchange.stop()`, `marketData.stop()`.

`EventLoop::stop()` is async-signal-safe; call it from a SIGINT/SIGTERM handler, then run the steps above
after `run()` returns (the quoter's `main.cpp` is a reference implementation).

After a crash or restart the client starts clean: positions come from `clearinghouseState`, and orders left
resting by the previous process appear as **external** orders once they change (or query
`info().openOrders(user)` at start-up and cancel them). With `scheduleCancel` active, orphaned orders are
removed by the venue automatically.

## 12. Production checklist

- [ ] `hl_live_check --taker` passes against the target network with the production configuration
- [ ] Separate agent wallet per environment and per process; keys outside images and repositories
- [ ] `accountAddress` is the **master** account of the agent wallet, confirmed with `{"type":"userRole"}` — the client logs a mismatch at start-up but still trades
- [ ] Tested on testnet with the same binary and configuration
- [ ] `scheduleCancel` dead-man's switch refreshed by the strategy
- [ ] Position and loss limits enforced in the strategy (the connector does not impose risk limits)
- [ ] Rate limits watched: `rateLimitStatus()` monitored; cancel batches are split at 40 entries automatically; WebSocket ≤ 2 000 messages/min
- [ ] Liquidation handling wired (`ExchangeListener::onLiquidation`)
- [ ] Counters from §10 monitored and alerted, including the `…RoundTrip` latency counters
- [ ] Order-path latency budget set against the venue's real round trip (~0.7–1.0 s), not against the library's microseconds
- [ ] Clean shutdown path cancels orders (SIGTERM tested)
- [ ] CA bundle available (`SSL_CERT_FILE`) on minimal hosts
- [ ] Clock synchronised (chrony/NTP) — nonces are wall-clock milliseconds
- [ ] `precomputedNonces` sized so `signingStats().deterministic` stays flat

## 13. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `TLS handshake failed … certificate` | CA bundle not found — `export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt` (Debian/Ubuntu/Alpine) or `/etc/pki/tls/certs/ca-bundle.crt` (RHEL) |
| `connect … failed on all addresses` | no route / firewall / proxy; test with `curl https://api.hyperliquid.xyz/info` |
| repeated `ws: … closed` then `connected` | network instability; the client recovers and reconciles — check `reconnectCount()` |
| `User or API Wallet 0x… does not exist` | agent not authorised on this network, or wrong `accountAddress` — see [TESTNET.md](TESTNET.md#8-troubleshooting) |
| `Invalid nonce` | two processes signing with the same key, or clock far off |
| orders rejected with `invalid price` | price not rounded with `AssetInfo::roundPx` |
| no fills / updates although orders rest | `accountAddress` set to the agent instead of the master account |
| quoter exits immediately in Docker on Ctrl-C without cancel output | run with `--init` |
