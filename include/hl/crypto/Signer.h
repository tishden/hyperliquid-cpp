// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "hl/core/Types.h"
#include "hl/crypto/Keccak.h"

struct secp256k1_context_struct;

namespace hl {

/// Recoverable ECDSA signature in Ethereum form (`v` ∈ {27, 28}).
struct Signature {
    Hash256 r{};
    Hash256 s{};
    std::uint8_t v{};
};

/**
 * @brief Hyperliquid L1-action hash (a.k.a. phantom-agent `connectionId`).
 *
 * `keccak256( msgpack(action) ‖ nonce_be64 ‖ (0x00 | 0x01 ‖ vault20) [‖ 0x00 ‖ expiresAfter_be64] )`
 */
[[nodiscard]] Hash256 actionHash(std::span<const std::uint8_t> msgpackAction, const std::optional<Address>& vault,
                                 std::uint64_t nonce, const std::optional<std::uint64_t>& expiresAfter) noexcept;

/**
 * @brief EIP-712 digest of the phantom `Agent{source, connectionId}` struct.
 *
 * Domain `{name:"Exchange", version:"1", chainId:1337, verifyingContract:0x0}`;
 * `source` is `"a"` on mainnet and `"b"` on testnet.
 */
[[nodiscard]] Hash256 agentDigest(const Hash256& connectionId, bool isMainnet) noexcept;

/**
 * @brief Owns a secp256k1 private key and signs Hyperliquid L1 actions.
 *
 * Construct once from the 32-byte key of an **API (agent) wallet** — the
 * recommended setup: an agent can trade but can never withdraw, and can be
 * revoked from the HL UI. Signatures are deterministic (RFC 6979) and low-s
 * normalised, byte-identical to the official Python SDK.
 *
 * Thread-safety: `sign*` methods are const and may be called concurrently (also with the nonce
 * pool enabled). `enableNoncePool` / `disableNoncePool` must not race with signing.
 * The key is wiped from memory in the destructor.
 */
class Signer {
public:
    /**
     * @param privateKeyHex `0x`-prefixed or bare 64-hex-digit secp256k1 key.
     * @throws std::invalid_argument if the key is malformed or out of range.
     */
    explicit Signer(std::string_view privateKeyHex);
    ~Signer();

    Signer(const Signer&) = delete;
    Signer& operator=(const Signer&) = delete;

    /// EVM address of the key (`keccak256(uncompressedPubKey)[12..]`).
    [[nodiscard]] const Address& address() const noexcept { return address_; }

    /// Sign an arbitrary 32-byte digest.
    [[nodiscard]] Signature signDigest(const Hash256& digest) const;

    /// Full L1-action signature: actionHash → agentDigest → ECDSA.
    [[nodiscard]] Signature signL1Action(std::span<const std::uint8_t> msgpackAction,
                                         const std::optional<Address>& vault, std::uint64_t nonce,
                                         const std::optional<std::uint64_t>& expiresAfter, bool isMainnet) const;

    // ── precomputed-nonce ("offline/online") signing ────────────────────────

    /**
     * @brief Enable signing with precomputed nonces — cuts a signature from ~15 µs to well under 0.1 µs.
     *
     * ECDSA cost is dominated by the scalar multiplication R = k·G. With the pool enabled that work
     * is done ahead of time: each pool entry holds (r, k⁻¹, r·d) for a fresh secret nonce k, and
     * signing reduces to `s = k⁻¹·(z + r·d) mod n` plus low-s normalisation. Signatures are ordinary
     * valid secp256k1 signatures, but randomised (not RFC 6979) — every signature is different.
     *
     * Safety properties:
     *  - each entry is handed out exactly once (spin-locked pop) and wiped immediately;
     *  - k = HMAC-SHA256(privateKey, CSPRNG output ‖ counter) mod n, so a weak system RNG alone
     *    cannot make nonces predictable;
     *  - the pool is discarded in a `fork()`ed child, which would otherwise reuse parent nonces;
     *  - when the pool is empty signing silently falls back to the deterministic path.
     *
     * @param capacity          number of ready nonces to keep (memory: ~112 bytes each)
     * @param backgroundThread  start an internal thread that keeps the pool full (~33 000 nonces/s
     *                          per core); pass false and call refillNonces() yourself otherwise
     */
    void enableNoncePool(std::size_t capacity, bool backgroundThread = true);
    /// Stop the refill thread and wipe all precomputed nonces.
    void disableNoncePool() noexcept;
    /// Compute up to @p maxCount nonces on the calling thread (bounded by free capacity). Returns the number added.
    std::size_t refillNonces(std::size_t maxCount);
    /// Nonces currently ready (0 when the pool is disabled).
    [[nodiscard]] std::size_t noncePoolSize() const noexcept;

    /// Signature counters by path.
    struct SigningStats {
        std::uint64_t precomputed{0};    ///< signed with a pooled nonce
        std::uint64_t deterministic{0};  ///< signed with RFC 6979 (pool disabled or empty)
    };
    [[nodiscard]] SigningStats signingStats() const noexcept;

private:
    struct NoncePool;

    Hash256 key_{};
    Address address_{};
    ::secp256k1_context_struct* ctx_{nullptr};
    std::unique_ptr<NoncePool> pool_;
    mutable std::atomic<std::uint64_t> precomputedCount_{0};
    mutable std::atomic<std::uint64_t> deterministicCount_{0};
};

}  // namespace hl
