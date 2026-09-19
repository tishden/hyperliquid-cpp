# Changelog

All notable changes to this project are documented here. The project follows
[Semantic Versioning](https://semver.org/).

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
