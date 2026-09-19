# Benchmarks

- [Environment](#environment)
- [Order entry: from 15.9 µs to 1.17 µs](#order-entry-from-159-µs-to-117-µs)
- [Market data](#market-data)
  - [How fast the venue actually feeds you](#how-fast-the-venue-actually-feeds-you)
  - [What the order path costs end to end, live](#what-the-order-path-costs-end-to-end-live)
- [Order book](#order-book)
- [Decimals](#decimals)
- [Cryptography](#cryptography)
- [WebSocket framing](#websocket-framing)
- [Reproducing](#reproducing)

## Environment

Every number in this document comes from one stand, measured on 2026-09-19:

| | |
|---|---|
| Host | AWS EC2 `c8a.2xlarge` (virtualised, not bare metal), `ap-northeast-1` (Tokyo) |
| CPU | AMD EPYC 9R45 (Zen 5), 8 vCPU = 8 physical cores (no SMT), 4.5 GHz, L2 1 MiB/core, L3 32 MiB |
| OS | Ubuntu 24.04, Linux 7.0 (`-aws`) |
| Core isolation | `isolcpus=managed_irq,domain,4-7 nohz_full=4-7 rcu_nocbs=4-7 rcu_nocb_poll irqaffinity=0-1 idle=poll`, `irqbalance` off, all NIC and disk interrupts on cores 0–1, transparent huge pages off, NIC interrupt coalescing off |
| Pinning | benchmarks: `taskset -c 5` + `SCHED_FIFO` 50 on an isolated core; live runs: `taskset -c 4,5` |
| Compiler | Clang 21.1, `-O3 -DNDEBUG`, libstdc++ 13, no `-march=native` (see below for what it adds) |
| Framework | Google Benchmark 1.8, `--benchmark_min_time=0.5s`, **median of 5 repetitions** |

With the core isolated, the coefficient of variation across repetitions is below 1.5 % for every row
except the two ~10–30 ns WebSocket codec rows (≤ 5.5 %). Frames are real mainnet captures from
`tests/fixtures/`.

## Order entry: from 15.9 µs to 1.17 µs

"Order entry" = build the action for one order, hash it, sign it, and assemble the complete WebSocket `post`
frame (`RequestBuilder::wsPostAction`) — everything the client does before the bytes are handed to the socket.

### Stage breakdown

Both columns were measured on the stand above, back to back; v1.0.0 is the first release, before the
latency work.

| Stage | Benchmark | v1.0.0 | v1.4.0 |
|---|---|---|---|
| MessagePack + JSON encoding | `Action_BuildOrder` | 218 ns | **186 ns** |
| Action hash — 1 Keccak-f permutation | `Stage2_ActionHash` | — | 255 ns |
| EIP-712 digest — 2 permutations | `Stage3_AgentDigest` | — | 514 ns |
| Action hash + EIP-712 digest together | `Action_Hash_EIP712` | 774 ns | 764 ns |
| ECDSA, RFC 6979 (libsecp256k1) | `Sign_EcdsaSecp256k1` | 14 646 ns | 14 665 ns |
| ECDSA, **precomputed nonce** | `Sign_PrecomputedNonce` | — | **44 ns** |
| payload + frame assembly | (in end-to-end) | 2 allocations + copy | 1 allocation |
| WebSocket frame masking, 620 B | `WsEncode_OrderPost` | 147 ns | **28 ns** |

### End to end

| Benchmark | Signing | v1.0.0 | v1.4.0 |
|---|---|---|---|
| `Order_EndToEnd_SignedPayload` | RFC 6979 (default) | 15 867 ns | 15 920 ns |
| `Order_EndToEnd_Precomputed` | precomputed nonce | — | **1 169 ns** |
| `Cancel_EndToEnd_SignedPayload` | RFC 6979 | 15 720 ns | 15 756 ns |
| `Cancel_EndToEnd_Precomputed` | precomputed nonce | — | **1 059 ns** |

**13.6× faster order entry** with `ExchangeConfig::precomputedNonces > 0`.

The same build with `-DHL_NATIVE=ON` (`-march=native`) on this CPU:

| Benchmark | portable | `-march=native` |
|---|---|---|
| `Keccak256_64B` | 243 ns | 215 ns |
| `Order_EndToEnd_Precomputed` | 1 169 ns | **1 047 ns** |
| `Cancel_EndToEnd_Precomputed` | 1 059 ns | 933 ns |

And inside the Docker runtime image (Ubuntu 24.04, GCC 13 build, `--cpuset-cpus=5`, same stand):

| Benchmark | Docker |
|---|---|
| `Keccak256_64B` | 290 ns |
| `Order_EndToEnd_SignedPayload` | 16 049 ns |
| `Order_EndToEnd_Precomputed` | **1 278 ns** (`deterministic_fallbacks=0`) |

### What was done

1. **Precomputed-nonce ECDSA (−14.6 µs).** An ECDSA signature is `R = k·G, r = R.x, s = k⁻¹(z + r·d) mod n`.
   The fixed-base scalar multiplication `k·G` is almost all of the cost and does not depend on the message. A
   background thread now prepares `(r, k⁻¹, r·d)` for fresh secret nonces; signing is one modular addition, one
   modular multiplication and low-s normalisation. This required constant-time 256-bit arithmetic modulo the
   curve order (`src/crypto/Scalar.h`: 4×64-bit limbs, 3-stage folding reduction with 2²⁵⁶ ≡ 2²⁵⁶ − n,
   masked final subtraction), cross-checked against OpenSSL BIGNUM on 20 000 random and edge-case inputs.
   Signatures are verified by libsecp256k1 in tests and accepted by the live testnet and mainnet.
   Safety design (nonce uniqueness, hedged nonce generation, `fork()` protection): [API.md §6.1](API.md#precomputed-nonce-signing).
2. **Unrolled Keccak-f[1600].** θ, ρ∘π and χ written out lane by lane, generated from the reference
   rotation/permutation tables. On this stand it makes no measurable difference (`Keccak256_64B` 246 → 243 ns):
   Clang 21 on Zen 5 already runs the loop form as fast. GCC 13 is ~20 % slower on it (290 ns, Docker row
   above), and `-march=native` gains another 12 %.
3. **Word-wise WebSocket masking (−0.12 µs).** XOR 8 bytes at a time instead of per byte: 147 → 28 ns.
4. **Allocation-free decimal formatting and single-allocation frame building (−30 ns).**
   `Decimal::toChars` writes into a stack buffer; `wsPostAction` signs straight into the final frame string.

### Where the remaining 1.17 µs go

```
Keccak (3 permutations)  ██████████████████████████████████████████████████  0.77 µs  66 %
encoding (msgpack+JSON)  ████████████                                        0.19 µs  16 %
frame, hex, nonce (rest) █████████                                           0.14 µs  12 %
ECDSA online step        ███                                                 0.04 µs   4 %
masking                  █                                                   0.03 µs   2 %
```

Keccak is at the limit of this scalar implementation; the three permutations are sequential (each
input depends on the previous digest), so they cannot be parallelised. Build with `HL_NATIVE=ON` on the
deployment host for the last ~10 %.

### Nonce production

| Benchmark | Time | Throughput |
|---|---|---|
| `NoncePool_ProduceOne` (k via HMAC-SHA256, k·G, k⁻¹ by fixed-window Fermat, r·d) | 19.5 µs | ~51 000 nonces/s per refill thread |

A pool of 256 absorbs a burst of 256 signed actions; the refill thread restores it in ~5 ms. When the pool is
empty signing falls back to RFC 6979 (`signingStats().deterministic`).

## Market data

Frame copied into a padded buffer, full simdjson On-Demand parse, exact decimal conversion of every price and
size, dispatch to the handler.

| Benchmark | Frame | Time | Throughput |
|---|---|---|---|
| `Parse_Bbo` | 144 B | **169 ns** | 814 MiB/s |
| `Parse_OrderUpdate` (1 order) | 256 B | 177 ns | 1.35 GiB/s |
| `Parse_ActiveAssetCtx` | 311 B | 280 ns | 1.03 GiB/s |
| `Parse_UserFill` (1 fill) | 426 B | 306 ns | 1.30 GiB/s |
| `Parse_L2Book_20x20` | 1.6 KB | 2.54 µs | 599 MiB/s |
| `Parse_Trades_30` | 8.3 KB | 3.07 µs | 2.51 GiB/s |
| `Parse_MainnetSessionReplay` (758 frames: l2Book, bbo, trades, ctx) | 245 KB | 0.200 ms | **1.16 GiB/s · 3.80 M msg/s** |

### How fast the venue actually feeds you

Parsing is not the constraint on Hyperliquid — the publish rate is. Measured live on mainnet on
2026-09-19 from the stand above, 120 s per coin on BTC and ETH:

| Feed | Depth | Interval between messages (p50) | Venue timestamp → arrival (p50) |
|---|---|---|---|
| `l2Book`, default | 20 levels/side | ~5.35 s | 315–330 ms |
| `l2Book`, `fast` ([`L2BookOptions::fast`](API.md#41-marketdataconfig-and-l2bookoptions)) | 5 levels/side | ~0.54 s | ~245 ms |
| `bbo` | best bid/offer | 150–180 ms (~5 messages/s on BTC, ~3 on ETH) | ~232 ms |

Matching snapshots of the two `l2Book` feeds by their venue timestamp, the `fast` copy usually
arrived first — by a median of 13 ms (ETH) and 19 ms (BTC) — but the spread was −70 to +58 ms, so
neither feed is a dependable lead. `fast` buys rate, not latency.

The last column is the stand's clock (Amazon Time Sync, sub-microsecond offset) minus the timestamp
the venue puts in the message. The TCP round trip from the stand to `api.hyperliquid.xyz` is
**~2.4 ms**, so almost all of those ~230 ms are spent inside the venue between the timestamp and
publication — a floor no client can go below.

**Method.** Two `MarketDataClient`s on one event loop and two connections — one subscribed with
`L2BookOptions{.fast = true}`, one with the default — recording the wall-clock arrival time, the venue
timestamp and the level count for every `onL2Book`, plus `bbo` messages on the second client. Two
clients rather than one because both subscriptions answer on the same `l2Book` channel and would
otherwise share (and flip) a single maintained book.

### What the order path costs end to end, live

The benchmarks above are local work. Measured against **mainnet** on 2026-09-19 from the same stand,
with the client's own `Stats::…RoundTrip` counters ([API.md §5](API.md#5-order-management)):

Aggregated over runs on eleven instruments (perps and spot, both transports), 120 signed actions in
total:

| Action | n | mean | min | max |
|---|---|---|---|---|
| build + sign (local only) | 120 | **0.001 ms** | 0.001 | 0.009 |
| order → venue response | 48 | 435 ms | 311 | 763 |
| cancel → venue response | 33 | 402 ms | 329 | 545 |
| modify → venue response | 11 | 414 ms | 310 | 591 |
| `scheduleCancel` / `updateLeverage` → venue response | 28 | 715 ms | 597 | 954 |

Roughly 0.0002 % of the time an order takes is spent in this library; the rest is Hyperliquid producing
a block and the network getting there. That is the honest reason the optimisation work stopped where it
did: shaving the remaining nanoseconds would change nothing you can measure at the venue. The full run
is in [RUNNING.md §4](RUNNING.md#4-acceptance-run-against-a-live-venue).

The round trips above are from Tokyo, 2.4 ms of network away from the venue's edge; from a host
farther away, add that extra network time twice. What is left is block production, and no client
changes it.

## Order book

| Benchmark | Time |
|---|---|
| `Book_ApplySnapshot_20x20` | 11.5 ns |
| `Book_ApplyBbo_TopResize` | 25 ns |
| `Book_ApplyBbo_PriceWalk` (best ask moves through 2 levels) | 35 ns |
| `Book_Microprice` | 2.9 ns |
| `Book_VwapForSize_5Levels` | 5.0 ns |

## Decimals

| Benchmark | Time |
|---|---|
| `Decimal_Parse` (exact) | **7.8 ns** |
| `Strtod_Baseline` (inexact) | 27 ns |
| `Decimal_ToString` (wire form into `std::string`) | 12.3 ns |

## Cryptography

| Benchmark | Time |
|---|---|
| `Keccak256_64B` | 243 ns |
| `Keccak256_1KB` | 1.90 µs (514 MiB/s) |
| `Action_BuildOrder` | 186 ns |
| `Action_Hash_EIP712` (action hash + EIP-712 digest) | 764 ns |
| `Sign_EcdsaSecp256k1` (RFC 6979) | 14.7 µs |
| `Sign_PrecomputedNonce` | **44 ns** |

## WebSocket framing

| Benchmark | Time |
|---|---|
| `WsDecode_Bbo` | 7.7 ns (17.9 GiB/s) |
| `WsDecode_L2Book` (zero-copy) | 15.5 ns (96 GiB/s) |
| `WsEncode_OrderPost` (620 B, masked) | 28 ns |

## Reproducing

```bash
scripts/bench.sh                                            # all, pinned to core 2
scripts/bench.sh 3 --benchmark_filter='Stage|EndToEnd|Precomputed|Sign_'
build/release/benchmarks/hl_benchmarks --benchmark_format=json > bench.json
docker run --rm --cpuset-cpus=2 hyperliquid-cpp hl_benchmarks
```

The numbers above were taken as

```bash
sudo chrt -f 50 taskset -c 5 build/release/benchmarks/hl_benchmarks \
    --benchmark_min_time=0.5s --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

on a core isolated as in [Environment](#environment); [RUNNING.md](RUNNING.md) describes the same tuning
for a production host. Without isolation expect a few percent of run-to-run noise; compare medians of several
runs. The benchmarks that consume precomputed nonces refill the pool outside the timed region
(`deterministic_fallbacks=0` confirms every timed signature used the fast path), which is why they take
minutes of wall time.

---

© 2026 Denis Tishkov <denis8825@ya.ru>. hyperliquid-cpp is licensed, not sold — see [LICENSE](../LICENSE).
