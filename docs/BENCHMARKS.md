# Benchmarks

## Environment

| | |
|---|---|
| CPU | desktop x86-64, 4 cores / 8 threads, 3.7 GHz, L2 256 KiB, L3 10 MiB (no isolation, turbo on) |
| OS | Linux 5.14 |
| Compiler | Clang 21.1, `-O3 -DNDEBUG`, libstdc++ 15; no `-march=native` |
| Pinning | `taskset -c 2` |
| Build | `scripts/bench.sh` (Google Benchmark 1.8, `--benchmark_min_time=0.3s`) |

Frames are real mainnet captures from `tests/fixtures/` (BTC, September 2026).

## Results

### WebSocket message parsing (`hl::WsMessageParser`)

Includes copying the frame into the padded parse buffer, full simdjson On-Demand parse, exact decimal
conversion of every price/size, and dispatch to the handler.

| Benchmark | Frame | Time | Throughput |
|---|---|---|---|
| `bbo` | 144 B | **402 ns** | 341 MiB/s |
| `activeAssetCtx` | 311 B | 809 ns | 366 MiB/s |
| `orderUpdates` (1 order) | 256 B | 556 ns | 439 MiB/s |
| `userFills` (1 fill) | 426 B | 750 ns | 541 MiB/s |
| `l2Book` 20×20 levels | 1.6 KB | 5.2 µs | 291 MiB/s |
| `trades` (30 trades) | 8.3 KB | 10.2 µs | 770 MiB/s |
| Replay of a 25 s mainnet session (758 frames: l2Book, bbo, trades, ctx) | 245 KB | 0.485 ms | **489 MiB/s · 1.56 M msg/s** |

### Order book (`hl::OrderBook`)

| Benchmark | Time |
|---|---|
| `applySnapshot` 20×20 | **27.5 ns** |
| `applyBbo` — resize top level | 52 ns |
| `applyBbo` — best ask walks 2 levels | 88 ns |
| `microprice()` | 31.5 ns |
| `vwapForSize` over 5 levels | 38.6 ns |

### Decimals (`hl::Decimal`)

| Benchmark | Time |
|---|---|
| `Decimal::parse` (exact) | **18 ns** |
| `strtod` baseline (inexact) | 95 ns |
| `Decimal::appendTo` (wire form) | 29 ns |

### Signing and order entry

| Benchmark | Time |
|---|---|
| Keccak-256, 64 B | 617 ns |
| Keccak-256, 1 KB | 4.8 µs (202 MiB/s) |
| Build `order` action (msgpack + JSON) | 554 ns |
| Action hash + EIP-712 digest | 1.98 µs |
| secp256k1 ECDSA sign (recoverable) | 39.5 µs |
| **Order → signed WebSocket post frame** (end to end) | **43.4 µs** |
| Cancel-by-cloid → signed payload | 43.0 µs |

ECDSA dominates order-entry CPU cost; 43 µs is ~0.02 % of Hyperliquid's ~200 ms block time.

### WebSocket framing

| Benchmark | Time |
|---|---|
| Decode `bbo` frame | 22.6 ns (6.1 GiB/s) |
| Decode `l2Book` frame (zero-copy) | 47.8 ns (31 GiB/s) |
| Encode + mask 620 B order post | 443 ns |

## Reproducing

```bash
scripts/bench.sh                    # pinned to core 2
scripts/bench.sh 3 --benchmark_filter='Parse_|Book_'
build/release/benchmarks/hl_benchmarks --benchmark_format=json > bench.json
```

For stable numbers: disable frequency scaling (`cpupower frequency-set -g performance`), pin to an
isolated core, and compare medians of several runs.
