//
// Created by panos on 8/21/25.
//

#ifndef STXXL_HASH_H
#define STXXL_HASH_H

#include <bits/stdc++.h>

// -------- Field math over p = 2^61 - 1 (Mersenne) --------
static const unsigned long long P = (1ULL << 61) - 1;

static inline unsigned long long reduce_mersenne(__uint128_t x) {
    // Fast reduction mod (2^61 - 1)
    unsigned long long lo = (unsigned long long)(x & P);
    unsigned long long hi = (unsigned long long)(x >> 61);
    unsigned long long res = lo + hi;
    if (res >= P) res -= P;
    return res;
}

static inline unsigned long long add_mod(unsigned long long a, unsigned long long b) {
    unsigned long long s = a + b;
    if (s >= P) s -= P;
    return s;
}

static inline unsigned long long mul_mod(unsigned long long a, unsigned long long b) {
    return reduce_mersenne((__uint128_t)a * b);
}

// -------- Base hash (maps arbitrary bytes to 64-bit, then into field) --------
static inline uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return x;
}

// Hash a byte span into a field element in [0, P-1]
static inline unsigned long long to_field(const void* data, size_t len, uint64_t seed = 0x123456789abcdef0ULL) {
    // Simple streaming SplitMix64; for stronger mixing, use SipHash/HighwayHash.
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t acc = seed ^ (uint64_t)len;
    while (len >= 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        acc = splitmix64(acc ^ w);
        p += 8; len -= 8;
    }
    uint64_t tail = 0;
    for (size_t i = 0; i < len; ++i) tail |= (uint64_t)p[i] << (8 * i);
    acc = splitmix64(acc ^ tail);
    // Map to field element
    return acc & P;
}

// -------- k-wise independent hash family: degree-(k-1) polynomial --------
struct KWiseHash {
    // h(x) = a0 + a1 x + ... + a_{k-1} x^{k-1} (mod P)
    std::vector<unsigned long long> a; // coefficients
    size_t k;

    // k = ceil(c * log2(N)); rng_seed deterministic for reproducibility
    KWiseHash(size_t k_, uint64_t rng_seed = 0xF00DF00DCAFEBABEULL) : k(k_) {
        a.resize(k);
        std::mt19937_64 rng(rng_seed);
        std::uniform_int_distribution<unsigned long long> dist(0, P - 1);
        for (size_t i = 0; i < k; ++i) a[i] = dist(rng);
    }

    // Hash raw bytes -> [0, P-1]
    unsigned long long operator()(const void* data, size_t len) const {
        unsigned long long x = to_field(data, len);
        // Horner's rule
        unsigned long long h = 0;
        for (size_t i = k; i-- > 0; ) {
            h = add_mod(mul_mod(h, x), a[i]);
        }
        return h; // in [0, P-1]
    }

    // Convenience: hash a 64-bit key
    unsigned long long hash_uint64(uint64_t key) const {
        unsigned long long x = (splitmix64(key) & P);
        unsigned long long h = 0;
        for (size_t i = k; i-- > 0; ) {
            h = add_mod(mul_mod(h, x), a[i]);
        }
        return h;
    }

    // Map to table range [0, M)
    uint64_t to_range_uint64(uint64_t key, uint64_t M) const {
        return (uint64_t)(hash_uint64(key) % M);
    }
};

#endif //STXXL_HASH_H