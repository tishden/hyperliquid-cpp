<!-- SPDX-License-Identifier: LicenseRef-hyperliquid-cpp -->
<!-- Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE. -->

# How this compares to the open-source C++ SDK

There is a free, MIT-licensed C++ SDK for Hyperliquid —
[TuxedoFish/hyperliquid-sdk-cpp](https://github.com/TuxedoFish/hyperliquid-sdk-cpp) — and it is a
competent project. Anyone evaluating this library will find it, so here is the comparison written
out, including the parts where the free one wins.

- [1. They are different kinds of thing](#1-they-are-different-kinds-of-thing)
- [2. Where the open SDK is ahead](#2-where-the-open-sdk-is-ahead)
- [3. Where this library is ahead](#3-where-this-library-is-ahead)
- [4. Which one to pick](#4-which-one-to-pick)
- [5. How this comparison was made](#5-how-this-comparison-was-made)

## 1. They are different kinds of thing

The open SDK's public surface is `RestApi`, `WebsocketApi`, `RequestTypes`, `ResponseTypes`,
`Keys`: a transport plus typed models. You call an endpoint, you get a struct back. It covers a lot
of endpoints and it covers them carefully.

This library's public surface is `ExchangeClient`, `MarketDataClient`, `OrderBook`, `InfoClient`:
a **stateful trading client**. It keeps the order table, correlates acknowledgements with fills,
maintains positions and the book, reconciles after a disconnect, and accounts for the venue's rate
limits. That is the layer *above* an SDK.

So the question is not "which SDK is better" but "how much of the trading client do you want to
write yourself".

| | Open SDK | This library |
|---|---|---|
| Shape | Transport + typed request/response models | Stateful client: order table, book, positions |
| Order state machine, cloid↔oid correlation | you write it | included |
| Maintained order book (`l2Book` + `bbo` overlay) | you write it | included |
| Positions from fills, stale-fill protection | you write it | included |
| Reconciliation after a reconnect | you write it | included |
| Rate-limit accounting, cancel-batch splitting | you write it | included |
| Endpoint breadth | **much wider** | narrower, plus raw escape hatches |
| Licence | MIT, free | commercial, with warranty and indemnity |

## 2. Where the open SDK is ahead

Stated plainly, because it is true and you would find it anyway.

| | Open SDK | This library |
|---|---|---|
| `/info` endpoints | **59 of 78** documented | 18 typed (+ `InfoClient::raw` for the rest) |
| `/exchange` actions | **41 of 68** documented | 9 typed (+ `submitAction` for the rest) |
| WebSocket channels | **24 of 24**, including `fastAssetCtxs` with zlib decompression | 14 typed (+ `subscribeRaw`); `fastAssetCtxs` explicitly not supported |
| Test cases | **406** | 190 |
| Project hygiene | fuzzing corpus, mkdocs site, `SECURITY.md`, `CONTRIBUTING.md`, codecov | none of these |
| Exotica | prediction markets, HIP-3, borrow/lend, vaults, deployment, staking | not covered |
| Price | **free** | not free |

If what you need is coverage of an endpoint this library does not type — TWAP, staking, vault
management, prediction markets — the open SDK has it today and this one does not.

Two things are a genuine tie: both derive their signing test vectors from the official
`hyperliquid-python-sdk`, and both parse with simdjson.

## 3. Where this library is ahead

### The expensive part is already written

Everything in the first table's middle rows is where connector bugs actually live. Two examples
from this library's own history, both found only by running against the live venue, neither caught
by unit tests until a regression test was written for it:

- readiness was announced before the start-up listing of already-resting orders had been applied,
  so an application that cancelled everything on "ready" silently left real orders working;
- spot balances were seeded only if the metadata response happened to arrive before the balance
  response.

A buyer starting from a transport SDK writes this layer from scratch and meets the same class of
bug with no test suite waiting for it. That is the work being sold, not the HTTP calls.

### Latency is engineered, and measured

| | Open SDK | This library |
|---|---|---|
| Signing an order | not a stated goal | **3.3 µs** (from 43 µs) via precomputed-nonce ECDSA |
| Event loop | Boost.Asio `io_context` | own epoll reactor, single thread, no allocations on the hot path |
| Published numbers | none in the README | full benchmark suite plus live mainnet measurements ([BENCHMARKS.md](BENCHMARKS.md)) |

The open SDK's README makes no latency claim and points to a separate project of its author for
low-latency order routing. This library treats latency as the product: the signing path was
profiled, rewritten and re-measured, and the result is reproducible from the repository.

Note the honest bound, stated the same way in our own docs: the venue's round trip is ~780 ms, so
the client is never the bottleneck. What the engineering buys is a predictable, allocation-free,
single-threaded path you can reason about inside your own hot loop — not trading profit.

### It integrates into your process instead of bringing its own

| | Open SDK | This library |
|---|---|---|
| Dependencies | 9: Boost.Asio, Boost.Beast, Boost.System, spdlog, nlohmann-json, zlib, simdjson, OpenSSL, secp256k1 | 3: OpenSSL, libsecp256k1, simdjson |
| Dependency exposure | via vcpkg, headers in your include path | **all private** — no third-party header reaches your code, only opaque forward declarations |
| Standard | C++23 | C++20 |

If you already run your own event loop and your own JSON stack, adding Boost.Beast and spdlog to
the build is a real cost. This library links three libraries privately and never puts their headers
on your include path, so it cannot collide with the versions you already use.

### Verified against the live venue, not only against a mock

Twelve instruments on **mainnet with real money**: every perp `szDecimals` from 0 to 5, spot pairs,
both transports, real taker fills, `expiresAfter`, forced reconnects — each a scripted run with a
pass/fail verdict per step and a non-zero exit code on any failure
([ARCHITECTURE.md](ARCHITECTURE.md#verification-matrix)). The acceptance tool ships with the
library, so you can re-run it against your own account before every deployment.

### Documentation written for integration

2 500 lines of API reference in which every method carries its exact mapping to the venue's wire
format, its invariants and its failure modes; a coverage matrix that states what is *not* supported
so you do not discover it in production; the signing specification with worked vectors. This is
also what makes the library workable for an LLM assistant — it can answer from the documentation
instead of guessing.

### Commercial terms

MIT gives you no warranty and nobody to ask. This library comes with a clean-provenance warranty, an
IP indemnity, a named counterparty and the option to buy maintenance — which matters for a venue
that changes as fast as Hyperliquid does. In one week of work on this library the venue's `l2Book`
publish rate turned out to be ten times slower than previously documented, a `fast` book variant
appeared, and unified accounts changed what the balance field means.

## 4. Which one to pick

**Take the open SDK if** you need breadth of endpoint coverage, you are happy to write the order
state machine and book yourself, Boost is already in your build, and latency is not a design
constraint. It is free, actively developed and reasonable.

**Take this library if** you are building a latency-sensitive trading process, you want the order
lifecycle and book handled and tested rather than written again, you care about what your build
pulls in, and you want a warranty and someone accountable for keeping up with the venue.

**Both is legitimate too**: nothing stops you using the open SDK for operational endpoints it
covers and this library for the trading path.

## 5. How this comparison was made

Checked on 2026-09-19 against the open SDK at its then-current state (repository started
2026-09-05, last commit 2026-09-14, 79 commits, MIT). Endpoint, action and channel counts are the
figures its own README states. Structural claims — the absence of an order book, an order state
machine and position tracking — come from reading its public headers and searching the source; its
dependency list comes from its `vcpkg.json`; its test-case count from counting `TEST`/`TEST_F`
macros.

Its code was not audited line by line and its tests were not run. So the statements about what it
does **not** contain are reliable; statements about the quality of what it does contain are not
made here. It is a moving target — re-check before relying on any row above.

---

© 2026 Denis Tishkov <denis8825@ya.ru>. hyperliquid-cpp is licensed, not sold — see [LICENSE](../LICENSE).
