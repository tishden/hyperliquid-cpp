# Changelog

All notable changes to this project are documented here. The project follows
[Semantic Versioning](https://semver.org/).

## [1.5.0] — 2026-09-21

### Changed
- **Open source under the Apache License 2.0.** `LICENSE` is now the standard Apache-2.0 text and
  every source file carries `SPDX-License-Identifier: Apache-2.0`. The source-code licence agreement,
  its Russian version `LICENSE.ru` and `docs/LICENSING.md` are gone; copies licensed under them before
  this release keep their terms.
- `docs/COMPARISON.md` compares two open-source libraries: the price row and the commercial-terms
  section are removed.

### Added
- `NOTICE`, installed together with `LICENSE` and `THIRD_PARTY_NOTICES.md` by `cmake --install` and
  shipped in the runtime Docker image.
- `CONTRIBUTING.md`, `SECURITY.md` and a GitHub Actions workflow that runs the build, the test suite
  under GCC and under Clang with ASan+UBSan, and the repository checks.
- `scripts/ci.sh` fails when a source file lacks the SPDX licence header.

## [1.4.1] — 2026-09-20

### Fixed
- **Orders could be left resting on the venue with the client no longer managing them.** `modify`
  does not change an order: the venue cancels the one it has and opens a new one carrying the same
  cloid. An order is therefore a chain of generations, and every identifier names one link of it.
  The client acted on answers and updates that described a link the order had already left behind,
  and the result always looked the same from outside — a quote in the book that the strategy had
  stopped managing and could still be filled. Five paths did this, and an eight-hour testnet soak
  found all five: the venue closes each WebSocket about every ten minutes, so an action was in
  flight for one of them often enough.

  - Reconciliation asked `orderStatus` by cloid, which the venue answers about the *first* order
    placed under it. It now asks by oid wherever the order has one.
  - After a modify whose acknowledgement was lost, asking by that oid answers "canceled", because
    the amendment replaced it. An unresolved modify is now settled against `frontendOpenOrders` —
    the only answer that says which oid carries the cloid now.
  - The venue can answer a `batchModify` with an error for an amendment it has applied. Those errors
    are settled the same way.
  - A `canceled` update for a generation several amendments back was read as news about the live
    order. Updates naming an oid older than the tracked one are now dropped: the venue issues oids
    in increasing order, so they can only describe a link already left behind.
  - A placement whose acknowledgement was lost has no oid, so it can only be asked about by cloid —
    and the venue answers "unknown" until it has processed the action. That answer is no longer a
    verdict on the first ask: the order is asked about three times, a second apart.

  Two safety nets were added behind those fixes. After a reconnect, an open order that matches
  nothing in the table is adopted, and one the table has buried under a different oid is revived
  from the listing. `docs/ORDER_MANAGEMENT.md` §9 states the invariant all of this enforces.
- Amending a partially filled order carried its fills onto the replacement. The venue opens the new
  order with nothing filled, so `remainingSz()` understated what was resting, and once the carried
  fills reached the new `origSz` a working order was declared `Filled`. Fill accounting now belongs
  to the generation that earned it, and a late fill naming an earlier one moves the position without
  counting against what rests now.
- One venue event reached the strategy up to three times: a rejection arrives as an `orderUpdates`
  message, as the acknowledgement of the action that caused it, and as the reconciliation answer.
  `onOrderUpdate` now fires only when something observable changed, which is what its contract
  always said. The demo quoter counts a rejection once, too.
- Venue rejections carried inside an acknowledgement are logged (`exchange: <cloid> rejected by the
  venue: …`). They only reached `Order::lastError` before, so an order could change its fate with
  nothing in the log to explain why.
- `CMakeLists.txt` and `Doxyfile` still declared 1.3.0 while the library reported 1.4.0.

### Changed
- Every line from the default log sink, and every event line the demo quoter prints, now starts with
  `HH:MM:SS.mmm`. A trading log without a clock cannot be lined up with the venue's own record of the
  same moment, which is the first thing anyone reaches for when an order behaves unexpectedly. The
  end-of-run summary stays unstamped: it is one block, not a stream of events.
- Order state transitions are logged at debug level (cloid, oid, from, to, reason).

### Documentation
- `docs/ORDER_MANAGEMENT.md` §9 states the generation invariant; §10 covers what reconciliation may
  ask and when, and §11 records what the venue does with connections: testnet closes every WebSocket
  after 10–12 minutes (`code 1000: Expired`), busy or idle, pinged or not, while mainnet held the
  same sockets for a full 25-minute measurement. A heartbeat does not prevent that close — it
  prevents the other one, where an unpinged socket is dropped without a close frame (code 1006).
- `docs/BENCHMARKS.md` reports what the library does today rather than what it used to do, measured
  on one stand and stated with the environment that produced it.

## [1.4.0] — 2026-09-19

### Added
- `docs/COMPARISON.md` — a side-by-side with the free MIT-licensed C++ SDK for Hyperliquid, stating
  where that one is ahead (endpoint breadth, price) as plainly as where this one is: the stateful
  trading layer above a transport SDK, engineered latency, three private dependencies, live mainnet
  verification, warranty and maintenance.
- `scripts/ci.sh` — one command for everything that must be green before a release: the compiler and
  sanitizer matrix, documentation link checking, a scan for keys and absolute paths in tracked
  files, and a version/changelog consistency check. Toolchains the machine lacks are skipped rather
  than failed. `scripts/check-docs.py` does the link checking and is usable on its own.
- Copyright and `SPDX-License-Identifier: LicenseRef-hyperliquid-cpp` headers in every source file,
  script, CMake file and the Dockerfile; the licence files and the documentation now name the
  copyright holder.
- README: the testnet demo written out end to end — build, key-free dry run, credentials, a live
  testnet run and its real output, including the latency counters.
- `ExchangeClient::spotTokenBalance(token)` — the spot balance of one token, populated when
  `loadSpotAssets` is set. On a unified account (the venue default) the USDC balance here is the
  collateral behind both spot and perp trading, and is the figure to size against.
- The start-up line now says where the money is instead of one ambiguous number:
  `exchange: ready — perp collateral 0 USDC, 0 open position(s), spot USDC 28.41`, and without
  `loadSpotAssets` it explains that a zero there is normal on a unified account.
- **Order-path latency counters.** `ExchangeClient::Stats` gained `buildAndSign` and
  `orderRoundTrip` / `cancelRoundTrip` / `modifyRoundTrip` / `otherRoundTrip`, each a `LatencyStats`
  with count, mean, min, max and last in microseconds, measured on a steady clock. `hl_live_check`
  prints them per step and in its summary, `hl_testnet_quoter` in its status line and summary.
  Measured live on mainnet: build+sign 1 µs against a 311–763 ms venue round trip.
- `credentials.env.example` — a git-tracked, annotated template for `secrets/*.env`, including how to
  tell an agent wallet from the master account it trades for, and where the network is selected.
- `hl_live_check` understands spot: a `@<index>` or `A/B` coin name loads `spotMeta` automatically
  (`--spot` forces it), the batch step uses two bids because spot cannot short, the close step drops
  `reduceOnly`, `updateLeverage` is skipped, and a sub-lot remainder is reported as unsellable dust
  rather than a failure. Verified on mainnet against HYPE/USDC, UBTC/USDC and UETH/USDC.
- `L2BookOptions::fast` — subscribe to the venue's 5-level `l2Book` publish path (`subscribeL2Book`
  and `subscribeBook` both take it; `hl_book_printer --fast` demonstrates it). Measured on mainnet
  BTC and ETH on 2026-09-19: snapshots ~0.54 s apart against ~5.35 s for the default 20-level feed,
  with no dependable per-snapshot delivery lead (median 13–19 ms, spread −70..+58 ms). `docs/API.md` §4.1 has the
  numbers and says which of the three feeds to use for what.
- `MarketDataClient::l2BookSubscriptionJson(coin, options)` — the exact string `subscribeL2Book`
  sends, so a subscription made with `nSigFigs` or `fast` can be reproduced for `unsubscribeRaw`.

### Documentation
- **Every benchmark and live-run figure in the documentation was re-measured on one stand** — an AWS
  `c8a.2xlarge` (AMD EPYC Zen 5, not bare metal) in Tokyo with cores 4–7 isolated (`isolcpus`,
  `nohz_full`, `rcu_nocbs`, IRQs moved off, `idle=poll`), Clang 21, median of five repetitions. Order
  entry is 15.9 µs by default and **1.17 µs** with precomputed nonces there (1.05 µs with
  `HL_NATIVE=ON`); parsing a `bbo` frame is 169 ns. The live figures come from runs made from that
  stand: 16/16 on testnet over both transports, and the eleven-instrument mainnet matrix with
  120 signed actions — build+sign 1 µs against a 311–763 ms venue round trip. `docs/BENCHMARKS.md`
  states the environment, the isolation settings and the reproduction command; no figure from the
  earlier development machine is quoted anywhere any more.
- Documented Hyperliquid's **unified account** mode, which is the venue default: one USDC balance in
  the spot clearinghouse backs spot and perps together, there is no spot↔perp transfer, and
  `clearinghouseState.accountValue` therefore reports only the collateral committed to perps — zero
  while flat — rather than the account's equity. Buying power comes from the spot balance;
  `positions` is correct in every mode, so seeding and reconciliation are unaffected. Verified on
  mainnet, including a restarted client seeding an open perp position.

### Fixed
- **Spot balances were seeded only if `spotMeta` happened to arrive before
  `spotClearinghouseState`.** The two are independent requests and the seeding loop needed the
  markets from the first to place the balances from the second, so on the other ordering every spot
  position silently started at zero. Whichever response lands second now does the seeding. Found by
  the new `LoadSpotAssetsSeedsMarketsAndTokenBalances` test; mainnet happened to be ordered the
  favourable way, which is exactly why it had not shown up.
- **`onReady()` fired before orders already resting on the venue had been adopted.** The start-up
  `openOrders` listing is asynchronous, and readiness did not wait for it — measured at 3 s on
  mainnet. An application that cancels or reconciles on ready (`hl_live_check --flatten` does) saw an
  empty order table, reported success and left real orders working. Readiness now waits for the
  listing; a failed listing releases it with a warning. Regression test
  `OrdersOpenOnTheVenueAreAdoptedBeforeOnReadyFires`.
- `hl_live_check` dereferenced a null `AssetInfo` and crashed when the coin was unknown to the
  registry; it now stops after the first step with a message naming the likely cause.

### Changed
- All third-party dependencies are now `PRIVATE` in CMake, OpenSSL included. No public header
  includes one — they appear in the API only as opaque forward declarations — so consumers link
  them transitively but no longer inherit their include paths and cannot accidentally compile
  against a different OpenSSL than the library was built with. Verified with an out-of-tree
  consumer project. The test target now declares its own direct use of OpenSSL (`scalar_test`
  cross-checks the mod-n arithmetic against BIGNUM) instead of inheriting it.
- `THIRD_PARTY_NOTICES.md` now states what each dependency is actually used for: libsecp256k1 does
  all elliptic-curve point arithmetic (address derivation, `R = k·G` per precomputed nonce, and the
  RFC 6979 fallback), not only the signing the entry previously named, and OpenSSL covers TLS,
  the RFC 6455 handshake hash, the CSPRNG and the signer's HMAC.
- `unsubscribeRaw` now drops a maintained book by matching the exact subscription string — the rule
  `WsSession::unsubscribe` already used — instead of scanning the JSON for a coin name.
- `subscribeL2Book` warns when a coin already has a different `l2Book` subscription: both arrive on
  the same channel, so the shared book would flip between their depths.
- Corrected the documented `l2Book` cadence, which claimed "once per block, roughly every 0.5 s".
  The default feed is seconds apart, so levels below the top are correspondingly stale.
- Replaced the placeholder licence with a full source-code licence agreement: perpetual, non-exclusive,
  commercial use and modification allowed, distribution only in compiled form; no resale, no source
  publication, no connector/SDK redistribution, no patenting of the embodied algorithms; provenance warranty
  and IP indemnity from the licensor, who keeps the right to license, resell or open-source the library.
- Simplified the licence wording and removed every placeholder: the parties come from the Order and the
  governing law defaults to the licensor's country. Added `LICENSE.ru`, a Russian version of equal force;
  the parties sign one of the two.
- Added `docs/LICENSING.md` — the licence in plain language, with an FAQ and a pre-signature checklist.

## [1.3.0] — 2026-09-19

Result of a full code review and an audit of the connector against the Hyperliquid API documentation,
the official Python SDK and the live venue.

### Added
- **Venue coverage**: TP/SL grouping and order-priority fees (`grouping:{"p":rate}`), builder codes,
  `updateIsolatedMargin`, `noop`, `reserveRequestWeight`, the `fast` flag on cancels, `expiresAfter` on every
  action (`ExchangeConfig::actionExpiryMs`).
- **Info**: `userRole`, `spotBalances`, `rateLimit`, `userFillsByTime`, `userFunding`, `historicalOrders`,
  `perpContexts` (`metaAndAssetCtxs`), `fundingHistory`, `predictedFundings`, `candles`.
- **WebSocket**: typed `candle`, `userEvents` (liquidations, venue cancels, funding — channel `user`),
  `userFundings`, `activeAssetData`, `notification`, with subscription helpers.
- **Rate limits**: address-budget tracking (`rateLimitStatus()`, `Stats::addressUnitsUsed`), periodic refresh
  from the venue, a warning before exhaustion, 429 + `Retry-After` handling that pauses the queue, and cancel
  batches split at 40 entries.
- **Start-up**: adopts orders the venue already has open (`adoptExistingOrders`), seeds spot balances when
  `loadSpotAssets` is set, and detects an API-wallet/master-account mismatch.
- `ExchangeListener::onLiquidation` / `onFunding`; `WsSession::reconnectNow`/`clearSubscriptions`;
  shared `NonceGenerator` for clients that use one signing key; `docs/COVERAGE.md`.

### Fixed
- `modify()` silently dropped a trigger order's stop/take-profit specification, turning it into a plain limit
  order; a new overload can also move the trigger.
- A venue error reported once for a whole batch left the other orders of the batch pending forever.
- Undefined behaviour when re-subscribing user channels after adopting an agent's master account.
- `WsSession::reconnectNow()` during a connect left the session dead with no retry timer.
- `~Signer` called `join()` on a thread that does not exist in a forked child (`std::terminate`).
- A replayed old fill could rewind the tracked position; positions now only move forward in venue time.
- Re-entrancy: running the event loop from inside a callback corrupted the WebSocket, JSON and HTTP buffers.
  The decoder, parser, TLS read path and HTTP client are now re-entrancy safe.
- `Decimal::mul`/`div` wrapped on overflow; they now saturate (`max()`, `min()`, `isSaturated()`).
- Reconciliation after a reconnect used one request per live order; it now lists open orders once.
- TLS handshakes inherited the per-address connect budget; a shared `SSL_CTX` replaces one per stream;
  reads are bounded per readiness event; `scheduleCancel` validates the venue's 5-second rule.
- Coin names are validated before they are interpolated into JSON.

### Changed
- 186 tests (from 138), including re-entrancy, restart, eviction, stale fills, fast cancels, batch errors,
  rate limits, parser fuzzing and the new info endpoints.

## [1.2.0] — 2026-09-19

### Added
- `hl_live_check` — scripted acceptance run of the whole order-management contract against a live venue
  (16 steps, pass/fail table, non-zero exit on failure), plus `--flatten` to cancel everything and close a
  position. Validated on testnet over both WebSocket and HTTP with real taker and maker fills.
- **Agent-wallet detection**: `InfoClient::userRole` and a start-up check in `ExchangeClient` — with no
  `accountAddress` the master account reported by the venue is adopted; with a wrong one the mismatch is
  logged and reported through `onError` (previously orders rested while fills, positions and balance stayed
  empty).
- `WsSession::reconnectNow()` and `ExchangeClient::session()`.
- `docs/TESTNET.md`: agent-wallet vs master account, acceptance-check section, real run outputs, observed
  testnet fees; `docs/ORDER_MANAGEMENT.md`: acknowledgement-vs-fill-stream timing, agent detection.

## [1.1.0] — 2026-09-17

### Added
- **Precomputed-nonce ECDSA** (`Signer::enableNoncePool`, `refillNonces`, `noncePoolSize`, `signingStats`;
  `ExchangeConfig::precomputedNonces`; quoter `--presign`): signing 14.7 µs → 44 ns, order entry 15.9 µs → 1.17 µs
  (figures from the current benchmark stand, `docs/BENCHMARKS.md`).
  Single-use hedged nonces, fork protection, automatic RFC 6979 fallback.
- Constant-time arithmetic modulo the secp256k1 order, cross-checked against OpenSSL BIGNUM.
- `RequestBuilder::wsPostAction` / `appendPayload` (single-allocation signed frames), `Decimal::toChars`.
- `ExchangeClient::signingStats()`; CMake option `HL_NATIVE`.
- Documentation: `docs/RUNNING.md`, `docs/ORDER_MANAGEMENT.md`; precomputed-nonce section in `docs/SIGNING.md`;
  benchmark breakdown of the order path.
- Stage benchmarks for order entry; nonce-pool, scalar and address-fallback tests (ThreadSanitizer clean).

### Changed
- Keccak-f[1600] unrolled. On the current benchmark stand (Zen 5, Clang 21) the loop form is already as
  fast, so the gain there is nil.
- WebSocket masking 8 bytes at a time: 147 ns → 28 ns for an order frame.

### Fixed
- `TlsStream` only tried the first resolved address; an unreachable CDN edge IP stalled connects and every
  reconnect until timeout. It now falls through to the next address and rotates the starting address.

## [1.0.0] — 2026-09-16

### Added
- **Market data**: `MarketDataClient` with `l2Book`, `bbo`, `trades`, `activeAssetCtx`,
  `allMids` and raw subscriptions; automatic reconnect with subscription replay;
  application-level heartbeat and stale-connection detection.
- **Order book**: allocation-free `OrderBook` maintained from full `l2Book` snapshots with
  a `bbo` top-of-book overlay; mid, microprice, spread, cumulative depth, VWAP-for-size.
- **Order management**: `ExchangeClient` — order / batch / cancel / cancel-all / modify /
  scheduleCancel / updateLeverage; WebSocket `post` or HTTP transport with automatic
  fallback; order table merging acknowledgements, `orderUpdates` and `userFills`;
  position tracking; reconciliation through `orderStatus` after timeouts, failed cancels
  and reconnects; agent-wallet and vault support.
- **Info**: `InfoClient` — asset metadata (perp + spot), clearinghouse state, open orders,
  order status, user fills, L2 snapshot, all mids, raw requests.
- **Signing**: EIP-712 L1-action signing (msgpack + Keccak-256 + secp256k1) byte-identical
  to the official Python SDK; price/size rounding per venue rules (`AssetInfo`).
- **Transport**: epoll `EventLoop`, non-blocking TLS stream, RFC 6455 WebSocket client,
  HTTP/1.1 keep-alive client.
- 120 tests (unit, golden vectors, end-to-end against an in-process mock venue),
  25 benchmarks, ASan/UBSan clean, GCC 15 and Clang 21.
- Examples: `hl_book_printer`, `hl_testnet_quoter`.

---

© 2026 Denis Tishkov <denis8825@ya.ru>. Licensed under the [Apache License 2.0](LICENSE).
