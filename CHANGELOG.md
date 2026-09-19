# Changelog

All notable changes to this project are documented here. The project follows
[Semantic Versioning](https://semver.org/).

## [1.4.0] — 2026-09-19

### Added
- `docs/COMPARISON.md` — a side-by-side with the free MIT-licensed C++ SDK for Hyperliquid, stating
  where that one is ahead (endpoint breadth, price) as plainly as where this one is: the stateful
  trading layer above a transport SDK, engineered latency, three private dependencies, live mainnet
  verification, warranty and maintenance. A condensed version is a section of the Russian offer PDF.
- `scripts/ci.sh` — one command for everything that must be green before a release: the compiler and
  sanitizer matrix, documentation link checking, a scan for keys and absolute paths in tracked
  files, and a version/changelog consistency check. Toolchains the machine lacks are skipped rather
  than failed. `scripts/check-docs.py` does the link checking and is usable on its own.
- Copyright and `SPDX-License-Identifier: LicenseRef-hyperliquid-cpp` headers in every source file,
  script, CMake file and the Dockerfile; the licence files and the documentation now name the
  copyright holder.
- `docs/hyperliquid-cpp-offer-ru.pdf` — a commercial one-pager in Russian describing what
  is being sold, positioned as a fast Hyperliquid client rather than a trading platform. Source in
  `docs/offer-ru.html`, rebuilt with `scripts/offer-pdf.sh`.
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
  Measured live on mainnet: build+sign 9 µs against a 673–1076 ms venue round trip.
- `credentials.env.example` — a git-tracked, annotated template for `secrets/*.env`, including how to
  tell an agent wallet from the master account it trades for, and where the network is selected.
- `hl_live_check` understands spot: a `@<index>` or `A/B` coin name loads `spotMeta` automatically
  (`--spot` forces it), the batch step uses two bids because spot cannot short, the close step drops
  `reduceOnly`, `updateLeverage` is skipped, and a sub-lot remainder is reported as unsellable dust
  rather than a failure. Verified on mainnet against HYPE/USDC, UBTC/USDC and UETH/USDC.
- `L2BookOptions::fast` — subscribe to the venue's 5-level `l2Book` publish path (`subscribeL2Book`
  and `subscribeBook` both take it; `hl_book_printer --fast` demonstrates it). Measured on mainnet
  BTC and ETH on 2026-09-19: snapshots ~0.54 s apart against ~5.3 s for the default 20-level feed,
  with no consistent per-snapshot delivery lead in either direction. `docs/API.md` §4.1 has the
  numbers and says which of the three feeds to use for what.
- `MarketDataClient::l2BookSubscriptionJson(coin, options)` — the exact string `subscribeL2Book`
  sends, so a subscription made with `nSigFigs` or `fast` can be reproduced for `unsubscribeRaw`.

### Documentation
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
  `ExchangeConfig::precomputedNonces`; quoter `--presign`): signing 38 µs → 0.17 µs, order entry 42 µs → 3.3 µs.
  Single-use hedged nonces, fork protection, automatic RFC 6979 fallback.
- Constant-time arithmetic modulo the secp256k1 order, cross-checked against OpenSSL BIGNUM.
- `RequestBuilder::wsPostAction` / `appendPayload` (single-allocation signed frames), `Decimal::toChars`.
- `ExchangeClient::signingStats()`; CMake option `HL_NATIVE`.
- Documentation: `docs/RUNNING.md`, `docs/ORDER_MANAGEMENT.md`; precomputed-nonce section in `docs/SIGNING.md`;
  benchmark breakdown of the order path.
- Stage benchmarks for order entry; nonce-pool, scalar and address-fallback tests (ThreadSanitizer clean).

### Changed
- Keccak-f[1600] unrolled: −33 % per hash (at parity with OpenSSL assembly).
- WebSocket masking 8 bytes at a time: 565 ns → 59 ns for an order frame.

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

© 2026 Denis Tishkov <denis8825@ya.ru>. hyperliquid-cpp is licensed, not sold — see [LICENSE](LICENSE).
