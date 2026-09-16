#include "hl/crypto/Signer.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <secp256k1.h>
#include <secp256k1_recovery.h>

#include <stdexcept>

#include "hl/core/Hex.h"

namespace hl {

namespace {

void putBe64(std::uint8_t* out, std::uint64_t v) noexcept {
    for (int i = 7; i >= 0; --i) {
        *out++ = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

void updateBe64(Keccak256& k, std::uint64_t v) noexcept {
    std::uint8_t buf[8];
    putBe64(buf, v);
    k.update(buf, sizeof(buf));
}

// EIP-712 constants are hashed once.
struct Eip712Constants {
    Hash256 domainSeparator{};
    Hash256 agentTypeHash{};
    Hash256 sourceMainnet{};
    Hash256 sourceTestnet{};

    Eip712Constants() noexcept {
        Keccak256 domain;
        const Hash256 typeHash =
            keccak256("EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)");
        domain.update(typeHash.data(), typeHash.size());
        const Hash256 name = keccak256("Exchange");
        domain.update(name.data(), name.size());
        const Hash256 version = keccak256("1");
        domain.update(version.data(), version.size());
        std::uint8_t chainId[32] = {};
        putBe64(chainId + 24, 1337);
        domain.update(chainId, sizeof(chainId));
        const std::uint8_t zero[32] = {};
        domain.update(zero, sizeof(zero));
        domainSeparator = domain.finalize();

        agentTypeHash = keccak256("Agent(string source,bytes32 connectionId)");
        sourceMainnet = keccak256("a");
        sourceTestnet = keccak256("b");
    }
};

const Eip712Constants& eip712() noexcept {
    static const Eip712Constants constants;
    return constants;
}

}  // namespace

Hash256 actionHash(std::span<const std::uint8_t> msgpackAction, const std::optional<Address>& vault,
                   std::uint64_t nonce, const std::optional<std::uint64_t>& expiresAfter) noexcept {
    Keccak256 k;
    k.update(msgpackAction.data(), msgpackAction.size());
    updateBe64(k, nonce);
    if (vault.has_value()) {
        const std::uint8_t flag = 0x01;
        k.update(&flag, 1);
        k.update(vault->data(), vault->size());
    } else {
        const std::uint8_t flag = 0x00;
        k.update(&flag, 1);
    }
    if (expiresAfter.has_value()) {
        const std::uint8_t flag = 0x00;
        k.update(&flag, 1);
        updateBe64(k, *expiresAfter);
    }
    return k.finalize();
}

Hash256 agentDigest(const Hash256& connectionId, bool isMainnet) noexcept {
    const auto& c = eip712();
    Keccak256 structHasher;
    structHasher.update(c.agentTypeHash.data(), c.agentTypeHash.size());
    const Hash256& source = isMainnet ? c.sourceMainnet : c.sourceTestnet;
    structHasher.update(source.data(), source.size());
    structHasher.update(connectionId.data(), connectionId.size());
    const Hash256 structHash = structHasher.finalize();

    Keccak256 digest;
    const std::uint8_t prefix[2] = {0x19, 0x01};
    digest.update(prefix, sizeof(prefix));
    digest.update(c.domainSeparator.data(), c.domainSeparator.size());
    digest.update(structHash.data(), structHash.size());
    return digest.finalize();
}

Signer::Signer(std::string_view privateKeyHex) {
    if (!fromHex(privateKeyHex, key_.data(), key_.size())) {
        throw std::invalid_argument("hl::Signer: private key must be 32 bytes of hex");
    }
    ctx_ = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (ctx_ == nullptr) {
        throw std::runtime_error("hl::Signer: secp256k1_context_create failed");
    }
    // Side-channel hardening recommended by libsecp256k1 (best effort).
    std::uint8_t seed[32];
    if (RAND_bytes(seed, sizeof(seed)) == 1) {
        if (secp256k1_context_randomize(ctx_, seed) != 1) {
            // Randomization only hardens against side channels; signing stays correct without it.
        }
    }
    OPENSSL_cleanse(seed, sizeof(seed));
    if (secp256k1_ec_seckey_verify(ctx_, key_.data()) != 1) {
        secp256k1_context_destroy(ctx_);
        ctx_ = nullptr;
        OPENSSL_cleanse(key_.data(), key_.size());
        throw std::invalid_argument("hl::Signer: private key is not a valid secp256k1 scalar");
    }
    secp256k1_pubkey pub;
    if (secp256k1_ec_pubkey_create(ctx_, &pub, key_.data()) != 1) {
        throw std::runtime_error("hl::Signer: secp256k1_ec_pubkey_create failed");
    }
    std::uint8_t uncompressed[65];
    std::size_t len = sizeof(uncompressed);
    secp256k1_ec_pubkey_serialize(ctx_, uncompressed, &len, &pub, SECP256K1_EC_UNCOMPRESSED);
    const Hash256 h = keccak256(uncompressed + 1, 64);
    std::copy(h.begin() + 12, h.end(), address_.begin());
}

Signer::~Signer() {
    OPENSSL_cleanse(key_.data(), key_.size());
    if (ctx_ != nullptr) {
        secp256k1_context_destroy(ctx_);
    }
}

Signature Signer::signDigest(const Hash256& digest) const {
    secp256k1_ecdsa_recoverable_signature sig;
    if (secp256k1_ecdsa_sign_recoverable(ctx_, &sig, digest.data(), key_.data(), nullptr, nullptr) != 1) {
        throw std::runtime_error("hl::Signer: signing failed");
    }
    std::uint8_t compact[64];
    int recid = 0;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(ctx_, compact, &recid, &sig);
    Signature out;
    std::copy(compact, compact + 32, out.r.begin());
    std::copy(compact + 32, compact + 64, out.s.begin());
    out.v = static_cast<std::uint8_t>(27 + recid);
    return out;
}

Signature Signer::signL1Action(std::span<const std::uint8_t> msgpackAction, const std::optional<Address>& vault,
                               std::uint64_t nonce, const std::optional<std::uint64_t>& expiresAfter,
                               bool isMainnet) const {
    return signDigest(agentDigest(actionHash(msgpackAction, vault, nonce, expiresAfter), isMainnet));
}

}  // namespace hl
