//
// Created by panag on 10/20/2026.
//

#ifndef STXXL_WORKLOADS_H
#define STXXL_WORKLOADS_H

// Workload generators for benchmarking.
//
// Two separate concerns:
//   1. KEY SETS   - which keys are stored (insertion order matters too).
//   2. ACCESS     - which keys get queried, drawn from the stored keys.
// Pre-generate everything before starting the timer.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

// ============================================================
// 1. KEY SETS  (all produce `size` unique keys)
// ============================================================

// Your original: keys 1..size, shuffled.
template <typename KeyType>
void uniform_random_data(std::vector<KeyType>& data, std::size_t size,
                         std::uint64_t seed = 12345) {
    data.clear();
    data.reserve(size);
    for (std::size_t i = 1; i <= size; ++i) data.push_back(static_cast<KeyType>(i));
    std::mt19937_64 gen(seed);
    std::shuffle(data.begin(), data.end(), gen);
}

// Sorted insertion: ascending or descending. Classic worst case for
// unbalanced trees and for structures that assume random insertion order.
template <typename KeyType>
void sequential_data(std::vector<KeyType>& data, std::size_t size, bool ascending = true) {
    data.clear();
    data.reserve(size);
    for (std::size_t i = 1; i <= size; ++i) data.push_back(static_cast<KeyType>(i));
    if (!ascending) std::reverse(data.begin(), data.end());
}

// Sorted, then `swap_fraction * size` random pairs swapped (e.g. 0.01 = 1%).
template <typename KeyType>
void nearly_sorted_data(std::vector<KeyType>& data, std::size_t size,
                        double swap_fraction = 0.01, std::uint64_t seed = 12345) {
    sequential_data(data, size, true);
    if (size < 2) return;
    std::mt19937_64 gen(seed);
    std::uniform_int_distribution<std::size_t> pick(0, size - 1);
    std::size_t swaps = static_cast<std::size_t>(swap_fraction * size);
    for (std::size_t i = 0; i < swaps; ++i) std::swap(data[pick(gen)], data[pick(gen)]);
}

// Keys concentrated around `num_clusters` centers (mixture of Gaussians),
// drawn from a universe of 10*size so there are gaps between clusters.
// Insertion order is shuffled.
template <typename KeyType>
void clustered_data(std::vector<KeyType>& data, std::size_t size,
                    int num_clusters = 4, std::uint64_t seed = 12345) {
    data.clear();
    data.reserve(size);
    std::mt19937_64 gen(seed);
    const double universe = 10.0 * static_cast<double>(size);
    const double sigma = universe / (num_clusters * 8.0);

    std::vector<double> centers;
    std::uniform_real_distribution<double> cdist(sigma * 4, universe - sigma * 4);
    for (int c = 0; c < num_clusters; ++c) centers.push_back(cdist(gen));

    std::uniform_int_distribution<int> which(0, num_clusters - 1);
    std::normal_distribution<double> noise(0.0, sigma);
    std::unordered_set<long long> seen;
    seen.reserve(size * 2);
    while (data.size() < size) {
        double v = centers[which(gen)] + noise(gen);
        long long k = static_cast<long long>(std::llround(v));
        if (k < 1 || k > static_cast<long long>(universe)) continue;
        if (seen.insert(k).second) data.push_back(static_cast<KeyType>(k));
    }
    std::shuffle(data.begin(), data.end(), gen);
}

// ============================================================
// 2. ACCESS DISTRIBUTIONS
//    Each takes the stored keys (in INSERTION order) and returns
//    `n_queries` keys to look up. All queried keys exist in the set.
// ============================================================

// Zipf sampler over ranks 0..n-1, P(rank i) ~ 1/(i+1)^theta.
// Uses an exact precomputed CDF + binary search (O(n) memory, O(log n) per sample).
// theta: 0 = uniform, ~0.99 = YCSB default, >1 = very skewed.
class ZipfSampler {
public:
    ZipfSampler(std::size_t n, double theta) : cdf_(n) {
        double sum = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            sum += 1.0 / std::pow(static_cast<double>(i + 1), theta);
            cdf_[i] = sum;
        }
        for (auto& c : cdf_) c /= sum;
    }
    template <typename Gen>
    std::size_t operator()(Gen& gen) const {
        double u = std::uniform_real_distribution<double>(0.0, 1.0)(gen);
        auto it = std::lower_bound(cdf_.begin(), cdf_.end(), u);
        return std::min<std::size_t>(it - cdf_.begin(), cdf_.size() - 1);
    }
private:
    std::vector<double> cdf_;
};

// Zipfian access.
// scramble = true : hot keys are spread randomly across the key space
//                   (isolates skew from key-locality effects).
// scramble = false: hot keys are the smallest keys (skew + locality combined).
template <typename KeyType>
void zipf_queries(const std::vector<KeyType>& keys, std::vector<KeyType>& queries,
                  std::size_t n_queries, double theta = 0.99, bool scramble = true,
                  std::uint64_t seed = 54321) {
    std::mt19937_64 gen(seed);
    std::vector<KeyType> ranked(keys);
    if (scramble) std::shuffle(ranked.begin(), ranked.end(), gen);
    else          std::sort(ranked.begin(), ranked.end());

    ZipfSampler zipf(ranked.size(), theta);
    queries.clear();
    queries.reserve(n_queries);
    for (std::size_t i = 0; i < n_queries; ++i) queries.push_back(ranked[zipf(gen)]);
}

// Hotspot: `hot_op_frac` of operations hit a random `hot_key_frac` of the keys
// (80/20 by default); the rest are uniform over the cold keys.
template <typename KeyType>
void hotspot_queries(const std::vector<KeyType>& keys, std::vector<KeyType>& queries,
                     std::size_t n_queries, double hot_key_frac = 0.2,
                     double hot_op_frac = 0.8, std::uint64_t seed = 54321) {
    std::mt19937_64 gen(seed);
    std::vector<KeyType> shuffled(keys);
    std::shuffle(shuffled.begin(), shuffled.end(), gen);

    std::size_t hot_n = std::max<std::size_t>(1, static_cast<std::size_t>(hot_key_frac * shuffled.size()));
    hot_n = std::min(hot_n, shuffled.size());
    std::uniform_real_distribution<double> coin(0.0, 1.0);
    std::uniform_int_distribution<std::size_t> hot(0, hot_n - 1);
    std::uniform_int_distribution<std::size_t> cold(hot_n < shuffled.size() ? hot_n : 0, shuffled.size() - 1);

    queries.clear();
    queries.reserve(n_queries);
    for (std::size_t i = 0; i < n_queries; ++i)
        queries.push_back(shuffled[coin(gen) < hot_op_frac ? hot(gen) : cold(gen)]);
}

// Latest: the most recently inserted keys are the most popular (Zipf over recency).
// `keys` must be in insertion order.
template <typename KeyType>
void latest_queries(const std::vector<KeyType>& keys, std::vector<KeyType>& queries,
                    std::size_t n_queries, double theta = 0.99, std::uint64_t seed = 54321) {
    std::mt19937_64 gen(seed);
    ZipfSampler zipf(keys.size(), theta);
    queries.clear();
    queries.reserve(n_queries);
    for (std::size_t i = 0; i < n_queries; ++i) queries.push_back(keys[keys.size() - 1 - zipf(gen)]);
}

// Exponential skew over sorted key order: smoother, lighter tail than Zipf.
// `mean_frac` = mean position as a fraction of the key count (smaller = more skew).
template <typename KeyType>
void exponential_queries(const std::vector<KeyType>& keys, std::vector<KeyType>& queries,
                         std::size_t n_queries, double mean_frac = 0.05,
                         std::uint64_t seed = 54321) {
    std::mt19937_64 gen(seed);
    std::vector<KeyType> sorted(keys);
    std::sort(sorted.begin(), sorted.end());
    std::exponential_distribution<double> expo(1.0 / (mean_frac * sorted.size()));

    queries.clear();
    queries.reserve(n_queries);
    for (std::size_t i = 0; i < n_queries; ++i) {
        std::size_t idx = static_cast<std::size_t>(expo(gen));
        queries.push_back(sorted[std::min(idx, sorted.size() - 1)]);
    }
}

// Uniform access, for the baseline.
template <typename KeyType>
void uniform_queries(const std::vector<KeyType>& keys, std::vector<KeyType>& queries,
                     std::size_t n_queries, std::uint64_t seed = 54321) {
    std::mt19937_64 gen(seed);
    std::uniform_int_distribution<std::size_t> pick(0, keys.size() - 1);
    queries.clear();
    queries.reserve(n_queries);
    for (std::size_t i = 0; i < n_queries; ++i) queries.push_back(keys[pick(gen)]);
}

// Your original query phase: every key exactly once, shuffled again.
template <typename KeyType>
void shuffled_queries(const std::vector<KeyType>& keys, std::vector<KeyType>& queries,
                      std::uint64_t seed = 54321) {
    queries = keys;
    std::mt19937_64 gen(seed);
    std::shuffle(queries.begin(), queries.end(), gen);
}

// ============================================================
// 3. SELECTION BY ENUM  (default parameters of each generator)
// ============================================================

enum class KeyDistribution {
    UniformRandom,
    SequentialAscending,
    SequentialDescending,
    NearlySorted,
    Clustered
};

enum class QueryDistribution {
    Shuffled,       // every key exactly once; ignores n_queries
    Uniform,
    Zipf,           // hot keys scattered over the key space
    ZipfSorted,     // hot keys are the smallest keys
    Hotspot,
    Latest,
    Exponential
};

inline const char* to_string(KeyDistribution d) {
    switch (d) {
    case KeyDistribution::UniformRandom:        return "uniform";
    case KeyDistribution::SequentialAscending:  return "seq_asc";
    case KeyDistribution::SequentialDescending: return "seq_desc";
    case KeyDistribution::NearlySorted:         return "nearly_sorted";
    case KeyDistribution::Clustered:            return "clustered";
    }
    return "unknown";
}

inline const char* to_string(QueryDistribution d) {
    switch (d) {
    case QueryDistribution::Shuffled:    return "shuffled";
    case QueryDistribution::Uniform:     return "uniform";
    case QueryDistribution::Zipf:        return "zipf";
    case QueryDistribution::ZipfSorted:  return "zipf_sorted";
    case QueryDistribution::Hotspot:     return "hotspot";
    case QueryDistribution::Latest:      return "latest";
    case QueryDistribution::Exponential: return "exponential";
    }
    return "unknown";
}

const KeyDistribution all_key_distributions[] = {
    KeyDistribution::UniformRandom, KeyDistribution::SequentialAscending,
    KeyDistribution::SequentialDescending, KeyDistribution::NearlySorted,
    KeyDistribution::Clustered
};

const QueryDistribution all_query_distributions[] = {
    QueryDistribution::Shuffled, QueryDistribution::Uniform, QueryDistribution::Zipf,
    QueryDistribution::ZipfSorted, QueryDistribution::Hotspot, QueryDistribution::Latest,
    QueryDistribution::Exponential
};

// Inverse of to_string; returns false if `name` is not a known distribution.
inline bool from_string(const std::string& name, KeyDistribution& out) {
    for (KeyDistribution d : all_key_distributions)
        if (name == to_string(d)) { out = d; return true; }
    return false;
}

inline bool from_string(const std::string& name, QueryDistribution& out) {
    for (QueryDistribution d : all_query_distributions)
        if (name == to_string(d)) { out = d; return true; }
    return false;
}

template <typename KeyType>
void generate_data(KeyDistribution dist, std::vector<KeyType>& data, std::size_t size) {
    switch (dist) {
    case KeyDistribution::UniformRandom:        uniform_random_data(data, size); break;
    case KeyDistribution::SequentialAscending:  sequential_data(data, size, true); break;
    case KeyDistribution::SequentialDescending: sequential_data(data, size, false); break;
    case KeyDistribution::NearlySorted:         nearly_sorted_data(data, size); break;
    case KeyDistribution::Clustered:            clustered_data(data, size); break;
    }
}

template <typename KeyType>
void generate_queries(QueryDistribution dist, const std::vector<KeyType>& keys,
                      std::vector<KeyType>& queries, std::size_t n_queries) {
    switch (dist) {
    case QueryDistribution::Shuffled:    shuffled_queries(keys, queries); break;
    case QueryDistribution::Uniform:     uniform_queries(keys, queries, n_queries); break;
    case QueryDistribution::Zipf:        zipf_queries(keys, queries, n_queries, 0.99, true); break;
    case QueryDistribution::ZipfSorted:  zipf_queries(keys, queries, n_queries, 0.99, false); break;
    case QueryDistribution::Hotspot:     hotspot_queries(keys, queries, n_queries); break;
    case QueryDistribution::Latest:      latest_queries(keys, queries, n_queries); break;
    case QueryDistribution::Exponential: exponential_queries(keys, queries, n_queries); break;
    }
}

#endif //STXXL_WORKLOADS_H
