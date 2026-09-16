# Third-party notices

hyperliquid-cpp links against the following components. They are fetched at
configure time (or taken from the system) and are **not** modified.

| Component | Version | License | Used for | Linked |
|---|---|---|---|---|
| [OpenSSL](https://www.openssl.org/) | ≥ 3.0 (system) | Apache-2.0 | TLS, SHA-1 (WebSocket handshake), CSPRNG | dynamic/system |
| [libsecp256k1](https://github.com/bitcoin-core/secp256k1) | 0.6.0 | MIT | ECDSA signing (recoverable, RFC 6979) | static, private |
| [simdjson](https://github.com/simdjson/simdjson) | 3.12.3 | Apache-2.0 | JSON parsing | static, private |
| [GoogleTest](https://github.com/google/googletest) | 1.11+ | BSD-3-Clause | tests only | not shipped |
| [Google Benchmark](https://github.com/google/benchmark) | 1.8+ | Apache-2.0 | benchmarks only | not shipped |

Keccak-256, MessagePack, EIP-712, WebSocket (RFC 6455) and HTTP/1.1 are
implemented in this code base; no additional code is vendored.

The golden test vectors in `tests/signer_test.cpp` and `tests/actions_test.cpp`
were produced with the official `hyperliquid-python-sdk` (MIT). Test fixtures in
`tests/fixtures/` are captures of public Hyperliquid market data.
