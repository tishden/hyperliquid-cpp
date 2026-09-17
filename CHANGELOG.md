# Changelog

All notable changes to this project are documented here. The project follows
[Semantic Versioning](https://semver.org/).

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
- Stage benchmarks for order entry; nonce-pool, scalar and address-fallback tests (135 tests; ThreadSanitizer clean).

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
