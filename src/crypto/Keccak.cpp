#include "hl/crypto/Keccak.h"

#include <bit>
#include <cstring>

namespace hl {

namespace {

constexpr std::array<std::uint64_t, 24> kRoundConstants = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL,
    0x000000000000808bULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008aULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
    0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL,
};


// Keccak-f[1600], rounds unrolled per step (θ, ρ∘π into a scratch state, χ, ι).
// The ρ/π lane mapping is generated from the reference tables:
//   B[y, 2x+3y] = rotl(A[x, y] ^ D[x], r[x][y]).
void keccakF1600(std::array<std::uint64_t, 25>& s) noexcept {
    for (std::uint64_t rc : kRoundConstants) {
        const std::uint64_t c0 = s[0] ^ s[5] ^ s[10] ^ s[15] ^ s[20];
        const std::uint64_t c1 = s[1] ^ s[6] ^ s[11] ^ s[16] ^ s[21];
        const std::uint64_t c2 = s[2] ^ s[7] ^ s[12] ^ s[17] ^ s[22];
        const std::uint64_t c3 = s[3] ^ s[8] ^ s[13] ^ s[18] ^ s[23];
        const std::uint64_t c4 = s[4] ^ s[9] ^ s[14] ^ s[19] ^ s[24];
        const std::uint64_t d0 = c4 ^ std::rotl(c1, 1);
        const std::uint64_t d1 = c0 ^ std::rotl(c2, 1);
        const std::uint64_t d2 = c1 ^ std::rotl(c3, 1);
        const std::uint64_t d3 = c2 ^ std::rotl(c4, 1);
        const std::uint64_t d4 = c3 ^ std::rotl(c0, 1);
        const std::uint64_t b0 = (s[0] ^ d0);
        const std::uint64_t b1 = std::rotl(s[6] ^ d1, 44);
        const std::uint64_t b2 = std::rotl(s[12] ^ d2, 43);
        const std::uint64_t b3 = std::rotl(s[18] ^ d3, 21);
        const std::uint64_t b4 = std::rotl(s[24] ^ d4, 14);
        const std::uint64_t b5 = std::rotl(s[3] ^ d3, 28);
        const std::uint64_t b6 = std::rotl(s[9] ^ d4, 20);
        const std::uint64_t b7 = std::rotl(s[10] ^ d0, 3);
        const std::uint64_t b8 = std::rotl(s[16] ^ d1, 45);
        const std::uint64_t b9 = std::rotl(s[22] ^ d2, 61);
        const std::uint64_t b10 = std::rotl(s[1] ^ d1, 1);
        const std::uint64_t b11 = std::rotl(s[7] ^ d2, 6);
        const std::uint64_t b12 = std::rotl(s[13] ^ d3, 25);
        const std::uint64_t b13 = std::rotl(s[19] ^ d4, 8);
        const std::uint64_t b14 = std::rotl(s[20] ^ d0, 18);
        const std::uint64_t b15 = std::rotl(s[4] ^ d4, 27);
        const std::uint64_t b16 = std::rotl(s[5] ^ d0, 36);
        const std::uint64_t b17 = std::rotl(s[11] ^ d1, 10);
        const std::uint64_t b18 = std::rotl(s[17] ^ d2, 15);
        const std::uint64_t b19 = std::rotl(s[23] ^ d3, 56);
        const std::uint64_t b20 = std::rotl(s[2] ^ d2, 62);
        const std::uint64_t b21 = std::rotl(s[8] ^ d3, 55);
        const std::uint64_t b22 = std::rotl(s[14] ^ d4, 39);
        const std::uint64_t b23 = std::rotl(s[15] ^ d0, 41);
        const std::uint64_t b24 = std::rotl(s[21] ^ d1, 2);
        s[0] = b0 ^ (~b1 & b2);
        s[1] = b1 ^ (~b2 & b3);
        s[2] = b2 ^ (~b3 & b4);
        s[3] = b3 ^ (~b4 & b0);
        s[4] = b4 ^ (~b0 & b1);
        s[5] = b5 ^ (~b6 & b7);
        s[6] = b6 ^ (~b7 & b8);
        s[7] = b7 ^ (~b8 & b9);
        s[8] = b8 ^ (~b9 & b5);
        s[9] = b9 ^ (~b5 & b6);
        s[10] = b10 ^ (~b11 & b12);
        s[11] = b11 ^ (~b12 & b13);
        s[12] = b12 ^ (~b13 & b14);
        s[13] = b13 ^ (~b14 & b10);
        s[14] = b14 ^ (~b10 & b11);
        s[15] = b15 ^ (~b16 & b17);
        s[16] = b16 ^ (~b17 & b18);
        s[17] = b17 ^ (~b18 & b19);
        s[18] = b18 ^ (~b19 & b15);
        s[19] = b19 ^ (~b15 & b16);
        s[20] = b20 ^ (~b21 & b22);
        s[21] = b21 ^ (~b22 & b23);
        s[22] = b22 ^ (~b23 & b24);
        s[23] = b23 ^ (~b24 & b20);
        s[24] = b24 ^ (~b20 & b21);
        s[0] ^= rc;
    }
}

void absorbBlock(std::array<std::uint64_t, 25>& state, const std::uint8_t* block, std::size_t rate) noexcept {
    for (std::size_t i = 0; i < rate / 8; ++i) {
        std::uint64_t lane = 0;
        std::memcpy(&lane, block + 8 * i, 8);  // little-endian host assumed (x86-64 / aarch64)
        state[i] ^= lane;
    }
    keccakF1600(state);
}

}  // namespace

void Keccak256::update(const std::uint8_t* data, std::size_t len) noexcept {
    if (buffered_ != 0) {
        const std::size_t take = std::min(len, kRate - buffered_);
        std::memcpy(buffer_.data() + buffered_, data, take);
        buffered_ += take;
        data += take;
        len -= take;
        if (buffered_ == kRate) {
            absorbBlock(state_, buffer_.data(), kRate);
            buffered_ = 0;
        }
    }
    while (len >= kRate) {
        absorbBlock(state_, data, kRate);
        data += kRate;
        len -= kRate;
    }
    if (len != 0) {
        std::memcpy(buffer_.data(), data, len);
        buffered_ = len;
    }
}

Hash256 Keccak256::finalize() noexcept {
    std::memset(buffer_.data() + buffered_, 0, kRate - buffered_);
    buffer_[buffered_] ^= 0x01;
    buffer_[kRate - 1] ^= 0x80;
    absorbBlock(state_, buffer_.data(), kRate);
    Hash256 out{};
    std::memcpy(out.data(), state_.data(), out.size());
    return out;
}

Hash256 keccak256(const std::uint8_t* data, std::size_t len) noexcept {
    Keccak256 k;
    k.update(data, len);
    return k.finalize();
}

}  // namespace hl
