#pragma once

#include <cstdint>
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
 * Thread-safety: `sign*` methods are const and may be called concurrently.
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

private:
    Hash256 key_{};
    Address address_{};
    ::secp256k1_context_struct* ctx_{nullptr};
};

}  // namespace hl
