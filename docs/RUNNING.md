# Running hyperliquid-cpp

How to build, run and operate the library and its applications — natively, in Docker and as a service.

- [1. Requirements](#1-requirements)
- [2. Building](#2-building)
- [3. Running the examples](#3-running-the-examples)
- [4. Docker](#4-docker)
- [5. Credentials](#5-credentials)
- [6. Configuration reference for your own application](#6-configuration-reference-for-your-own-application)
- [7. Low-latency deployment](#7-low-latency-deployment)
- [8. Running as a service (systemd)](#8-running-as-a-service-systemd)
- [9. Logging and monitoring](#9-logging-and-monitoring)
- [10. Shutdown and restarts](#10-shutdown-and-restarts)
- [11. Production checklist](#11-production-checklist)
- [12. Troubleshooting](#12-troubleshooting)

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
| `build/release/examples/hl_live_check` | scripted acceptance run of the whole order-management contract against the live venue; exit code 0 only if every step passed | yes |
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

All quoter options are listed by `--help` and in [TESTNET.md](TESTNET.md#4-run-the-demo); account setup is
described there as well. The quoter enables the precomputed-nonce signer by default (`--presign 256`).

## 4. Docker

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
- Use `--network host` and `--cpuset-cpus` for latency-sensitive runs ([§7](#7-low-latency-deployment)).

## 5. Credentials

- Use an **API (agent) wallet** per environment (testnet and mainnet are separate): it can trade but not
  withdraw and can be revoked. `privateKey` = agent key, `accountAddress` = master account.
- Supply keys via environment variables, a key file with mode `600`, or your secret manager — never as
  command-line arguments (visible in `ps`) and never in images or git.
- Two processes must not share one signing key: nonces are generated per process and could collide. Use one
  agent wallet per process.

## 6. Configuration reference for your own application

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
| `ExchangeConfig::transport` | `WebSocket` (one connection for posts, order updates and fills) |
| `ExchangeConfig::requestTimeoutMs` | 3–10 s; shorter means faster reconciliation of lost actions |
| `WsSessionOptions::pingIntervalMs` / `staleTimeoutMs` | 20 s / 60 s (HL closes idle sockets after 60 s) |
| `WsSessionOptions::reconnectMinDelayMs` / `reconnectMaxDelayMs` | 250 ms / 10 s |
| `TlsOptions::connectTimeoutMs` | 10 s total, split across resolved addresses |
| `hl::setLogLevel` | `Info` in production, `Debug` when diagnosing |

## 7. Low-latency deployment

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

## 8. Running as a service (systemd)

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

## 9. Logging and monitoring

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
| `ExchangeClient::isReady()` | readiness | false for longer than the reconnect backoff |

## 10. Shutdown and restarts

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

## 11. Production checklist

- [ ] `hl_live_check --taker` passes against the target network with the production configuration
- [ ] Separate agent wallet per environment and per process; keys outside images and repositories
- [ ] `accountAddress` is the **master** account of the agent wallet (the client logs a mismatch at start-up)
- [ ] Tested on testnet with the same binary and configuration
- [ ] `scheduleCancel` dead-man's switch refreshed by the strategy
- [ ] Position and loss limits enforced in the strategy (the connector does not impose risk limits)
- [ ] Rate limits respected: order volume budget, ≤ 40 orders per batch, WebSocket ≤ 2 000 messages/min
- [ ] Counters from §9 monitored and alerted
- [ ] Clean shutdown path cancels orders (SIGTERM tested)
- [ ] CA bundle available (`SSL_CERT_FILE`) on minimal hosts
- [ ] Clock synchronised (chrony/NTP) — nonces are wall-clock milliseconds
- [ ] `precomputedNonces` sized so `signingStats().deterministic` stays flat

## 12. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `TLS handshake failed … certificate` | CA bundle not found — `export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt` (Debian/Ubuntu/Alpine) or `/etc/pki/tls/certs/ca-bundle.crt` (RHEL) |
| `connect … failed on all addresses` | no route / firewall / proxy; test with `curl https://api.hyperliquid.xyz/info` |
| repeated `ws: … closed` then `connected` | network instability; the client recovers and reconciles — check `reconnectCount()` |
| `User or API Wallet 0x… does not exist` | agent not authorised on this network, or wrong `accountAddress` — see [TESTNET.md](TESTNET.md#7-troubleshooting) |
| `Invalid nonce` | two processes signing with the same key, or clock far off |
| orders rejected with `invalid price` | price not rounded with `AssetInfo::roundPx` |
| no fills / updates although orders rest | `accountAddress` set to the agent instead of the master account |
| quoter exits immediately in Docker on Ctrl-C without cancel output | run with `--init` |
