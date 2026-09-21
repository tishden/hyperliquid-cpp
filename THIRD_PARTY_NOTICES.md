# Third-party notices

hyperliquid-cpp links against the following components. They are fetched at
configure time (or taken from the system) and are **not** modified.

| Component | Version | License | Used for | Linked |
|---|---|---|---|---|
| [OpenSSL](https://www.openssl.org/) | ≥ 3.0 (system) | Apache-2.0 | TLS (`TlsStream`); SHA-1 for the `Sec-WebSocket-Accept` handshake, which RFC 6455 mandates; CSPRNG for WebSocket frame masks and cloids; HMAC-SHA256 and secret wiping in the signer | dynamic/system, private |
| [libsecp256k1](https://github.com/bitcoin-core/secp256k1) | 0.6.0 | MIT | All elliptic-curve point arithmetic: public key and address derivation at start-up, `R = k·G` for each precomputed nonce, and recoverable RFC 6979 signing as the fallback when the nonce pool is empty. Not called on the hot signing path, which is our own mod-n scalar arithmetic. | static, private |
| [simdjson](https://github.com/simdjson/simdjson) | 3.12.3 | Apache-2.0 | JSON parsing | static, private |
| [GoogleTest](https://github.com/google/googletest) | 1.11+ | BSD-3-Clause | tests only | not shipped |
| [Google Benchmark](https://github.com/google/benchmark) | 1.8+ | Apache-2.0 | benchmarks only | not shipped |

Keccak-256, MessagePack, EIP-712, WebSocket (RFC 6455) and HTTP/1.1 are
implemented in this code base; no additional code is vendored.

hyperliquid-cpp itself is licensed under the Apache License 2.0 ([LICENSE](LICENSE), [NOTICE](NOTICE)).
Anyone redistributing a product built with it also carries the attribution requirements of the components
above.

The golden test vectors in `tests/signer_test.cpp` and `tests/actions_test.cpp`
were produced with the official `hyperliquid-python-sdk` (MIT). Test fixtures in
`tests/fixtures/` are captures of public Hyperliquid market data.

---

© 2026 Denis Tishkov <denis8825@ya.ru>. Licensed under the [Apache License 2.0](LICENSE).
