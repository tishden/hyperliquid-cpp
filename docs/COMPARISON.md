<!-- SPDX-License-Identifier: LicenseRef-hyperliquid-cpp -->
<!-- Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE. -->

# How this compares to the open-source C++ SDK

A free, MIT-licensed C++ SDK for Hyperliquid exists —
[TuxedoFish/hyperliquid-sdk-cpp](https://github.com/TuxedoFish/hyperliquid-sdk-cpp) — and it is a
competent project with wider endpoint coverage than this library. Anyone evaluating this one will
find it, so here is the comparison, including where the free one wins.

- [1. They are different kinds of thing](#1-they-are-different-kinds-of-thing)
- [2. Where the open SDK is ahead](#2-where-the-open-sdk-is-ahead)
- [3. Where this library is ahead](#3-where-this-library-is-ahead)
- [4. How this comparison was made](#4-how-this-comparison-was-made)

## 1. They are different kinds of thing

The open SDK is a **transport plus typed models**: you call an endpoint, you get a struct back.
This library is a **stateful trading client** — it keeps the order table, correlates
acknowledgements with fills, maintains positions and the book, reconciles after a disconnect and
accounts for the venue's rate limits. That is the layer *above* an SDK.

So the question is not "which SDK is better" but "how much of the trading client do you want to
write yourself".

| | Open SDK | This library |
|---|---|---|
| Shape | Transport + typed request/response models | Stateful client: order table, book, positions |
| Order state machine, cloid↔oid correlation | you write it | **included** |
| Maintained order book (`l2Book` + `bbo` overlay) | you write it | **included** |
| Positions from fills, stale-fill protection | you write it | **included** |
| Reconciliation after a reconnect | you write it | **included** |
| Rate-limit accounting, cancel-batch splitting | you write it | **included** |

## 2. Where the open SDK is ahead

| | Open SDK | This library |
|---|---|---|
| `/info` endpoints | **59 of 78** documented | 18 typed (+ `InfoClient::raw` for the rest) |
| `/exchange` actions | **41 of 68** documented | 9 typed (+ `submitAction` for the rest) |
| WebSocket channels | **24 of 24**, including `fastAssetCtxs` with zlib decompression | 14 typed (+ `subscribeRaw`); `fastAssetCtxs` not supported |
| Endpoints this one does not type at all | TWAP, staking, vaults, borrow/lend, deployment, prediction markets, HIP-3 | — |
| Price | **free** | not free |

If you need one of those endpoints, the open SDK has it today and this one does not. Both derive
their signing test vectors from the official `hyperliquid-python-sdk`, and both parse with simdjson.

## 3. Where this library is ahead

### The expensive part is already written

The middle rows of the first table are where connector bugs actually live. Two examples from this
library's own history, both found only by running against the live venue, neither caught by unit
tests until a regression test was written for it:

- readiness was announced before the start-up listing of already-resting orders had been applied,
  so an application that cancelled everything on "ready" silently left real orders working;
- spot balances were seeded only if the metadata response happened to arrive before the balance
  response.

Starting from a transport SDK means writing this layer from scratch and meeting the same class of
bug with no test suite waiting for it. That is the work being sold, not the HTTP calls.

### Latency is engineered, and measured

| | Open SDK | This library |
|---|---|---|
| Order → signed frame | not a stated goal | 15.9 µs, or **1.17 µs** with precomputed-nonce ECDSA |
| Event loop | Boost.Asio `io_context` | own epoll reactor, single thread, no allocations on the hot path |
| Published numbers | none in the README | benchmark suite plus live mainnet measurements ([BENCHMARKS.md](BENCHMARKS.md)) |

The open SDK's README makes no latency claim and points to a separate project of its author for
low-latency order routing. Here the signing path was profiled, rewritten and re-measured, and the
result is reproducible from the repository.

The honest bound, stated the same way in our own docs: the venue's round trip is ~435 ms, so the
client is never the bottleneck. What the engineering buys is a predictable, allocation-free,
single-threaded path you can reason about inside your own hot loop — not trading profit.

### It integrates into your process instead of bringing its own

| | Open SDK | This library |
|---|---|---|
| Dependencies | 9, incl. Boost.Asio, Boost.Beast, spdlog, nlohmann-json, zlib | 3: OpenSSL, libsecp256k1, simdjson |
| Dependency exposure | via vcpkg, headers on your include path | **all private** — no third-party header reaches your code, only opaque forward declarations |
| Standard | C++23 | C++20 |

If you already run your own event loop and your own JSON stack, adding Boost.Beast and spdlog to
the build is a real cost. This library links its three dependencies privately and never puts their
headers on your include path, so they cannot collide with the versions you already use.

### Verified against the live venue, not only against a mock

Perps and spot on **mainnet with real money**, across every size precision the venue uses, over
both transports, with real taker fills, `expiresAfter` and forced reconnects — each a scripted run
with a pass/fail verdict per step and a non-zero exit code on any failure
([ARCHITECTURE.md](ARCHITECTURE.md#verification-matrix)). The acceptance tool ships with the
library, so you can re-run it against your own account before every deployment.

### Documentation written for integration

An API reference in which every method carries its exact mapping to the venue's wire format, its
invariants and its failure modes; a coverage matrix that states what is *not* supported, so you do
not discover it in production; the signing specification with worked vectors. This is also what
makes the library workable for an LLM assistant — it can answer from the documentation instead of
guessing.

### Commercial terms

MIT gives you no warranty and nobody to ask. This library comes with a clean-provenance warranty, an
IP indemnity, a named counterparty and the option to buy maintenance — which matters for a venue
that changes as fast as Hyperliquid does. In one week of work on this library the venue's `l2Book`
publish rate turned out to be ten times slower than previously documented, a `fast` book variant
appeared, and unified accounts changed what the balance field means.

## 4. How this comparison was made

Checked on 2026-09-19 against the open SDK as it then stood. The endpoint, action and channel
counts are the figures its own README states; the structural claims — no order book, no order state
machine, no position tracking — come from reading its public headers and searching its source; the
dependency list comes from its `vcpkg.json`.

Its code was not audited line by line and its tests were not run, so the statements about what it
does **not** contain are reliable, while no claim is made here about the quality of what it does.
It is a moving target — re-check before relying on any row above.

---

© 2026 Denis Tishkov <denis8825@ya.ru>. hyperliquid-cpp is licensed, not sold — see [LICENSE](../LICENSE).
