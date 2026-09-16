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

constexpr std::array<int, 24> kRotations = {1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14,
                                            27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44};
constexpr std::array<int, 24> kPi = {10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4,
                                     15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1};

void keccakF1600(std::array<std::uint64_t, 25>& s) noexcept {
    for (std::uint64_t rc : kRoundConstants) {
        // θ
        std::array<std::uint64_t, 5> c{};
        for (int x = 0; x < 5; ++x) {
            c[x] = s[x] ^ s[x + 5] ^ s[x + 10] ^ s[x + 15] ^ s[x + 20];
        }
        for (int x = 0; x < 5; ++x) {
            const std::uint64_t d = c[(x + 4) % 5] ^ std::rotl(c[(x + 1) % 5], 1);
            for (int y = 0; y < 25; y += 5) {
                s[y + x] ^= d;
            }
        }
        // ρ and π
        std::uint64_t current = s[1];
        for (int i = 0; i < 24; ++i) {
            const int j = kPi[i];
            const std::uint64_t tmp = s[j];
            s[j] = std::rotl(current, kRotations[i]);
            current = tmp;
        }
        // χ
        for (int y = 0; y < 25; y += 5) {
            const std::uint64_t a0 = s[y], a1 = s[y + 1], a2 = s[y + 2], a3 = s[y + 3], a4 = s[y + 4];
            s[y] = a0 ^ (~a1 & a2);
            s[y + 1] = a1 ^ (~a2 & a3);
            s[y + 2] = a2 ^ (~a3 & a4);
            s[y + 3] = a3 ^ (~a4 & a0);
            s[y + 4] = a4 ^ (~a0 & a1);
        }
        // ι
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
