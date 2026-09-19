# Benchmarks

- [Environment](#environment)
- [Order entry: from 43 µs to 3.3 µs](#order-entry-from-43-µs-to-33-µs)
- [Market data](#market-data)
  - [How fast the venue actually feeds you](#how-fast-the-venue-actually-feeds-you)
  - [What the order path costs end to end, live](#what-the-order-path-costs-end-to-end-live)
- [Order book](#order-book)
- [Decimals](#decimals)
- [Cryptography](#cryptography)
- [WebSocket framing](#websocket-framing)
- [Reproducing](#reproducing)

## Environment

| | |
|---|---|
| CPU | Intel Core i7-3820 (Sandy Bridge-E, 2012), 3.6–3.7 GHz, L2 256 KiB, L3 10 MiB; `performance` governor, no core isolation |
| OS | Linux 5.14 |
| Compiler | Clang 21.1, `-O3 -DNDEBUG`, libstdc++ 15, no `-march=native` |
| Pinning | `taskset -c 2` |
| Framework | Google Benchmark 1.8, `--benchmark_min_time=0.5s` |

The reference machine is deliberately old: a current server core (Sapphire Rapids, Zen 4) is roughly 2× faster
per core on every row below. Frames are real mainnet captures from `tests/fixtures/`.

## Order entry: from 43 µs to 3.3 µs

"Order entry" = build the action for one order, hash it, sign it, and assemble the complete WebSocket `post`
frame (`RequestBuilder::wsPostAction`) — everything the client does before the bytes are handed to the socket.

### Stage breakdown

| Stage | Benchmark | v1.0.0 | v1.1.0 |
|---|---|---|---|
| MessagePack + JSON encoding | `Stage1_BuildAction` | 357 ns | 363 ns |
| Action hash — 1 Keccak-f permutation | `Stage2_ActionHash` | 1 135 ns | **757 ns** |
| EIP-712 digest — 2 permutations | `Stage3_AgentDigest` | 2 248 ns | **1 547 ns** |
| ECDSA, RFC 6979 (libsecp256k1) | `Sign_EcdsaSecp256k1` | 39 514 ns | 38 265 ns |
| ECDSA, **precomputed nonce** | `Sign_PrecomputedNonce` | — | **169 ns** |
| payload + frame assembly | (in end-to-end) | 2 allocations + copy | 1 allocation |
| WebSocket frame masking, 620 B | `WsEncode_OrderPost` | 565 ns | **59 ns** |

### End to end

| Benchmark | Signing | v1.0.0 | v1.1.0 |
|---|---|---|---|
| `Order_EndToEnd_SignedPayload` | RFC 6979 (default) | 43 528 ns | 42 190 ns |
| `Order_EndToEnd_Precomputed` | precomputed nonce | — | **3 353 ns** |
| `Cancel_EndToEnd_SignedPayload` | RFC 6979 | 43 179 ns | 41 798 ns |
| `Cancel_EndToEnd_Precomputed` | precomputed nonce | — | **3 101 ns** |

**13× faster order entry** with `ExchangeConfig::precomputedNonces > 0`.

Same measurement inside the Docker runtime image (Ubuntu 24.04, GCC 13 build, same CPU, no pinning):

| Benchmark | Docker |
|---|---|
| `Order_EndToEnd_SignedPayload` | 41 030 ns |
| `Order_EndToEnd_Precomputed` | **3 076 ns** (`deterministic_fallbacks=0`) |

### What was done

1. **Precomputed-nonce ECDSA (−38 µs).** An ECDSA signature is `R = k·G, r = R.x, s = k⁻¹(z + r·d) mod n`.
   The fixed-base scalar multiplication `k·G` is ~95 % of the cost and does not depend on the message. A
   background thread now prepares `(r, k⁻¹, r·d)` for fresh secret nonces; signing is one modular addition, one
   modular multiplication and low-s normalisation. This required constant-time 256-bit arithmetic modulo the
   curve order (`src/crypto/Scalar.h`: 4×64-bit limbs, 3-stage folding reduction with 2²⁵⁶ ≡ 2²⁵⁶ − n,
   masked final subtraction), cross-checked against OpenSSL BIGNUM on 20 000 random and edge-case inputs.
   Signatures are verified by libsecp256k1 in tests and accepted by the live testnet.
   Safety design (nonce uniqueness, hedged nonce generation, `fork()` protection): [API.md §6.1](API.md#precomputed-nonce-signing).
2. **Unrolled Keccak-f[1600] (−1.1 µs).** θ, ρ∘π and χ written out lane by lane, generated from the reference
   rotation/permutation tables. On this CPU it now matches OpenSSL's hand-written assembly (≈ 740 ns per
   64-byte hash); Clang generates ~15 % faster code than GCC for it.
3. **Word-wise WebSocket masking (−0.5 µs).** XOR 8 bytes at a time instead of per byte.
4. **Allocation-free decimal formatting and single-allocation frame building (−0.2 µs).**
   `Decimal::toChars` writes into a stack buffer; `wsPostAction` signs straight into the final frame string.

### Where the remaining 3.3 µs go

```
Keccak (3 permutations)  ████████████████████████████████████████████████  2.30 µs  69 %
encoding (msgpack+JSON)  ███████                                           0.36 µs  11 %
frame, hex, nonce (rest) █████████                                         0.46 µs  14 %
ECDSA online step        ███                                               0.17 µs   5 %
masking                  █                                                 0.06 µs   2 %
```

Keccak is at the hardware limit of this scalar implementation; the three permutations are sequential (each
input depends on the previous digest), so they cannot be parallelised. Build with `HL_NATIVE=ON` on the
deployment host: `-march=native` gave another ~8 % on Keccak with Clang.

### Nonce production

| Benchmark | Time | Throughput |
|---|---|---|
| `NoncePool_ProduceOne` (k via HMAC-SHA256, k·G, k⁻¹ by fixed-window Fermat, r·d) | 58 µs | ~17 000 nonces/s per refill thread |

A pool of 256 absorbs a burst of 256 signed actions; the refill thread restores it in ~15 ms. When the pool is
empty signing falls back to RFC 6979 (`signingStats().deterministic`).

## Market data

Frame copied into a padded buffer, full simdjson On-Demand parse, exact decimal conversion of every price and
size, dispatch to the handler.

| Benchmark | Frame | Time | Throughput |
|---|---|---|---|
| `Parse_Bbo` | 144 B | **336 ns** | 411 MiB/s |
| `Parse_OrderUpdate` (1 order) | 256 B | 461 ns | 532 MiB/s |
| `Parse_ActiveAssetCtx` | 311 B | 640 ns | 466 MiB/s |
| `Parse_UserFill` (1 fill) | 426 B | 779 ns | 525 MiB/s |
| `Parse_L2Book_20x20` | 1.6 KB | 3.97 µs | 385 MiB/s |
| `Parse_Trades_30` | 8.3 KB | 8.28 µs | 958 MiB/s |
| `Parse_MainnetSessionReplay` (758 frames: l2Book, bbo, trades, ctx) | 245 KB | 0.465 ms | **514 MiB/s · 1.64 M msg/s** |

### How fast the venue actually feeds you

Parsing is not the constraint on Hyperliquid — the publish rate is. Measured live on mainnet on
2026-09-19 from a single non-co-located host, 60–120 s per coin on BTC and ETH:

| Feed | Depth | Interval between messages (p50) |
|---|---|---|
| `l2Book`, default | 20 levels/side | ~5.3 s |
| `l2Book`, `fast` ([`L2BookOptions::fast`](API.md#41-marketdataconfig-and-l2bookoptions)) | 5 levels/side | ~0.54 s |
| `bbo` | best bid/offer | ~7 messages/s (~145 ms) |

Matching snapshots of the two `l2Book` feeds by their venue timestamp showed no consistent delivery
lead in either direction (within ±100 ms, sign varying between runs and coins), so `fast` buys rate,
not latency.

**Method.** Two `MarketDataClient`s on one event loop and two connections — one subscribed with
`L2BookOptions{.fast = true}`, one with the default — recording a steady-clock arrival time and the
level count for every `onL2Book`, plus `bbo` messages on the second client. Two clients rather than
one because both subscriptions answer on the same `l2Book` channel and would otherwise share (and
flip) a single maintained book. Ordinary retail connectivity, no co-location: the intervals are
venue-side behaviour and should reproduce anywhere, while any absolute one-way delay is not measured
here at all.

### What the order path costs end to end, live

The benchmarks above are local work. Measured against **mainnet** on 2026-09-19 from an ordinary
non-co-located host, with the client's own `Stats::…RoundTrip` counters
([API.md §5](API.md#5-order-management)):

Aggregated over runs on eleven instruments (perps and spot), 75 signed actions in total:

| Action | n | mean | min | max |
|---|---|---|---|---|
| build + sign (local only) | 75 | **0.008 ms** | 0.006 | 0.016 |
| order → venue response | 28 | 779 ms | 677 | 994 |
| cancel → venue response | 21 | 807 ms | 706 | 1135 |
| modify → venue response | 7 | 795 ms | 715 | 886 |

Roughly 0.001 % of the time an order takes is spent in this library; the rest is Hyperliquid producing
a block and the network getting there. That is the honest reason the optimisation work stopped where it
did: shaving the remaining microseconds would change nothing you can measure at the venue. The full run
is in [RUNNING.md §4](RUNNING.md#4-acceptance-run-against-a-live-venue).

## Order book

| Benchmark | Time |
|---|---|
| `Book_ApplySnapshot_20x20` | 52 ns |
| `Book_ApplyBbo_TopResize` | 76 ns |
| `Book_ApplyBbo_PriceWalk` (best ask moves through 2 levels) | 130 ns |
| `Book_Microprice` | 32 ns |
| `Book_VwapForSize_5Levels` | 38 ns |

## Decimals

| Benchmark | Time |
|---|---|
| `Decimal_Parse` (exact) | **19 ns** |
| `Strtod_Baseline` (inexact) | 101 ns |
| `Decimal_ToString` (wire form into `std::string`) | 36 ns |

## Cryptography

| Benchmark | Time |
|---|---|
| `Keccak256_64B` | 761 ns |
| `Keccak256_1KB` | 5.8 µs (170 MiB/s) |
| `Action_BuildOrder` | 354 ns |
| `Action_Hash_EIP712` (action hash + EIP-712 digest) | 2.27 µs |
| `Sign_EcdsaSecp256k1` (RFC 6979) | 38.3 µs |
| `Sign_PrecomputedNonce` | **169 ns** |

## WebSocket framing

| Benchmark | Time |
|---|---|
| `WsDecode_Bbo` | 18 ns (7.6 GiB/s) |
| `WsDecode_L2Book` (zero-copy) | 43 ns (34.5 GiB/s) |
| `WsEncode_OrderPost` (620 B, masked) | 59 ns |

## Reproducing

```bash
scripts/bench.sh                                            # all, pinned to core 2
scripts/bench.sh 3 --benchmark_filter='Stage|EndToEnd|Precomputed|Sign_'
build/release/benchmarks/hl_benchmarks --benchmark_format=json > bench.json
docker run --rm --cpuset-cpus=2 hyperliquid-cpp hl_benchmarks
```

Numbers vary by a few percent between runs; for comparisons pin to an isolated core, use the `performance`
governor and compare medians of several runs. The benchmarks that consume precomputed nonces refill the pool
outside the timed region (`deterministic_fallbacks=0` confirms every timed signature used the fast path).
