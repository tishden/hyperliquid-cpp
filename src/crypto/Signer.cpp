// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
#include "hl/crypto/Signer.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <secp256k1.h>
#include <secp256k1_recovery.h>

#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

#include "crypto/Scalar.h"
#include "hl/core/Hex.h"
#include "hl/core/Log.h"

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

// ── precomputed nonce pool ──────────────────────────────────────────────────

namespace {

std::atomic<std::uint64_t> gForkGeneration{0};
std::atomic<bool> gAtforkRegistered{false};

void onForkChild() noexcept { gForkGeneration.fetch_add(1, std::memory_order_relaxed); }

void registerAtfork() noexcept {
    if (!gAtforkRegistered.exchange(true)) {
        (void)::pthread_atfork(nullptr, nullptr, &onForkChild);
    }
}

// One ready-to-use nonce: everything that depends on k and the key but not on the message.
struct NonceEntry {
    detail::Limbs r{};
    detail::Limbs kInv{};
    detail::Limbs rd{};  // r·d mod n
    std::uint8_t recid{0};
};

}  // namespace

struct Signer::NoncePool {
    std::vector<NonceEntry> ring;
    std::size_t mask{0};
    std::uint64_t head{0};  // next entry to consume
    std::uint64_t tail{0};  // next slot to fill
    std::atomic_flag lock{};
    std::atomic<std::size_t> size{0};
    std::atomic<bool> stop{false};
    std::uint64_t forkGeneration{0};
    std::uint64_t counter{0};  // producer-side
    ::secp256k1_context_struct* ctx{nullptr};
    const Hash256* key{nullptr};  // owned by the Signer, which outlives the pool
    std::atomic_flag refilling{};
    std::thread thread;

    std::size_t refill(std::size_t maxCount);

    struct Guard {
        std::atomic_flag& f;
        explicit Guard(std::atomic_flag& flag) noexcept : f(flag) {
            while (f.test_and_set(std::memory_order_acquire)) {
            }
        }
        ~Guard() { f.clear(std::memory_order_release); }
    };

    ~NoncePool() {
        stop.store(true);
        if (thread.joinable()) {
            if (forkGeneration == gForkGeneration.load()) {
                thread.join();
            } else {
                // In a forked child the refill thread does not exist; joining it would throw from a
                // destructor. The child never uses the pool (generation mismatch), so let it go.
                thread.detach();
            }
        }
        OPENSSL_cleanse(ring.data(), ring.size() * sizeof(NonceEntry));
        if (ctx != nullptr) {
            secp256k1_context_destroy(ctx);
        }
    }

    bool push(const NonceEntry& e) noexcept {
        Guard g(lock);
        if (tail - head > mask) {
            return false;
        }
        ring[static_cast<std::size_t>(tail & mask)] = e;
        ++tail;
        size.store(static_cast<std::size_t>(tail - head), std::memory_order_relaxed);
        return true;
    }

    bool pop(NonceEntry& out) noexcept {
        if (size.load(std::memory_order_relaxed) == 0) {
            return false;
        }
        Guard g(lock);
        if (head == tail) {
            return false;
        }
        NonceEntry& slot = ring[static_cast<std::size_t>(head & mask)];
        out = slot;
        OPENSSL_cleanse(&slot, sizeof(slot));
        ++head;
        size.store(static_cast<std::size_t>(tail - head), std::memory_order_relaxed);
        return true;
    }
};

namespace {

// Produce one nonce entry; returns false in the (astronomically unlikely) rejection cases.
bool produceNonce(::secp256k1_context_struct* ctx, const Hash256& key, const detail::Limbs& d, std::uint64_t counter,
                  NonceEntry& out) {
    std::uint8_t input[40];
    if (RAND_bytes(input, 32) != 1) {
        return false;
    }
    for (int i = 0; i < 8; ++i) {
        input[32 + i] = static_cast<std::uint8_t>(counter >> (8 * i));
    }
    std::uint8_t kBytes[32];
    unsigned int kLen = sizeof(kBytes);
    const bool mac = HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), input, sizeof(input), kBytes, &kLen) != nullptr;
    OPENSSL_cleanse(input, sizeof(input));
    detail::Limbs k = detail::fromBytes(kBytes);
    bool ok = mac && !detail::isZero(k) && detail::lessThan(k, detail::kOrderN);
    secp256k1_pubkey R;
    if (ok) {
        ok = secp256k1_ec_pubkey_create(ctx, &R, kBytes) == 1;
    }
    if (ok) {
        std::uint8_t ser[65];
        std::size_t len = sizeof(ser);
        secp256k1_ec_pubkey_serialize(ctx, ser, &len, &R, SECP256K1_EC_UNCOMPRESSED);
        const detail::Limbs x = detail::fromBytes(ser + 1);
        // x ≥ n would need recovery ids 2/3, which Ethereum-style v cannot express: reject.
        ok = detail::lessThan(x, detail::kOrderN);
        if (ok) {
            out.r = x;
            out.recid = static_cast<std::uint8_t>(ser[64] & 1U);
            out.kInv = detail::invMod(k);
            out.rd = detail::mulMod(x, d);
        }
        OPENSSL_cleanse(ser, sizeof(ser));
    }
    OPENSSL_cleanse(kBytes, sizeof(kBytes));
    OPENSSL_cleanse(k.data(), sizeof(k));
    return ok;
}

}  // namespace

std::size_t Signer::NoncePool::refill(std::size_t maxCount) {
    // Producers are serialised: the counter and the secp256k1 context are not shared-safe.
    if (refilling.test_and_set(std::memory_order_acquire)) {
        return 0;
    }
    const detail::Limbs d = detail::fromBytes(key->data());
    std::size_t added = 0;
    // Bound the attempts: a persistently failing CSPRNG must not spin the refill thread on a core.
    std::size_t attemptsLeft = 4 * maxCount + 8;
    while (added < maxCount && attemptsLeft > 0 && size.load(std::memory_order_relaxed) <= mask &&
           !stop.load(std::memory_order_relaxed)) {
        --attemptsLeft;
        NonceEntry e;
        if (!produceNonce(ctx, *key, d, counter++, e)) {
            logf(LogLevel::Warn, "nonce pool: could not produce a nonce (CSPRNG or HMAC failure)");
            continue;
        }
        const bool pushed = push(e);
        OPENSSL_cleanse(&e, sizeof(e));
        if (!pushed) {
            break;
        }
        ++added;
    }
    refilling.clear(std::memory_order_release);
    return added;
}

void Signer::enableNoncePool(std::size_t capacity, bool backgroundThread) {
    disableNoncePool();
    if (capacity == 0) {
        return;
    }
    registerAtfork();
    std::size_t pow2 = 1;
    while (pow2 < capacity) {
        pow2 <<= 1;
    }
    auto pool = std::make_unique<NoncePool>();
    pool->ring.assign(pow2, NonceEntry{});
    pool->mask = pow2 - 1;
    pool->forkGeneration = gForkGeneration.load();
    pool->key = &key_;
    pool->ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (pool->ctx == nullptr) {
        throw std::runtime_error("hl::Signer: secp256k1_context_create failed");
    }
    std::uint8_t seed[32];
    if (RAND_bytes(seed, sizeof(seed)) == 1 && secp256k1_context_randomize(pool->ctx, seed) != 1) {
        // Hardening only; the pool stays correct without it.
    }
    OPENSSL_cleanse(seed, sizeof(seed));
    if (backgroundThread) {
        NoncePool* raw = pool.get();  // the thread is joined in ~NoncePool, before the pool memory goes away
        pool->thread = std::thread([raw] {
            while (!raw->stop.load(std::memory_order_relaxed)) {
                if (raw->refill(64) == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        });
    }
    pool_ = std::move(pool);
}

void Signer::disableNoncePool() noexcept { pool_.reset(); }

std::size_t Signer::refillNonces(std::size_t maxCount) { return pool_ ? pool_->refill(maxCount) : 0; }

std::size_t Signer::noncePoolSize() const noexcept {
    return pool_ ? pool_->size.load(std::memory_order_relaxed) : 0;
}

Signer::SigningStats Signer::signingStats() const noexcept {
    return SigningStats{precomputedCount_.load(), deterministicCount_.load()};
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
    pool_.reset();
    OPENSSL_cleanse(key_.data(), key_.size());
    if (ctx_ != nullptr) {
        secp256k1_context_destroy(ctx_);
    }
}

Signature Signer::signDigest(const Hash256& digest) const {
    NonceEntry e;
    if (pool_ && pool_->forkGeneration == gForkGeneration.load(std::memory_order_relaxed) && pool_->pop(e)) {
        // Online step: s = k⁻¹ · (z + r·d) mod n, then low-s normalisation (flips the recovery id).
        const detail::Limbs z = detail::reduce256(detail::fromBytes(digest.data()));
        detail::Limbs sScalar = detail::mulMod(e.kInv, detail::addMod(z, e.rd));
        if (!detail::isZero(sScalar)) {
            detail::Limbs negated{};
            detail::sub(detail::kOrderN, sScalar, negated);
            const bool high = detail::lessThan(detail::kOrderHalf, sScalar);
            const std::uint64_t pick = 0ULL - static_cast<std::uint64_t>(high);
            for (std::size_t i = 0; i < 4; ++i) {
                sScalar[i] = (negated[i] & pick) | (sScalar[i] & ~pick);
            }
            Signature out;
            detail::toBytes(e.r, out.r.data());
            detail::toBytes(sScalar, out.s.data());
            out.v = static_cast<std::uint8_t>(27 + (e.recid ^ static_cast<std::uint8_t>(high)));
            OPENSSL_cleanse(&e, sizeof(e));
            OPENSSL_cleanse(sScalar.data(), sizeof(sScalar));
            precomputedCount_.fetch_add(1, std::memory_order_relaxed);
            return out;
        }
        OPENSSL_cleanse(&e, sizeof(e));
    }
    deterministicCount_.fetch_add(1, std::memory_order_relaxed);
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
