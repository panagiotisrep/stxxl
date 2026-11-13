//
// Created by panos on 6/23/25.
//

#ifndef BOA_PV_H
#define BOA_PV_H

#include "stxxl/bits/stream/sort_stream.h"
#include "stxxl/bits/stream/stream.h"

#include <limits>
#include <map>
#include <stxxl/bits/containers/btree/iterator.h>
#include <stxxl/bits/containers/btree/iterator_map.h>
#include <stxxl/bits/namespace.h>
#include <stxxl/map>
#include <stxxl/vector>
#include <queue>
#include <ips2ra.hpp>
#include <stxxl/bits/containers/boa_params.h>

// #ifdef SORT_BASED_MATERIALIZATION
//   #define IN_PLACE_SORT
// #endif

STXXL_BEGIN_NAMESPACE
  namespace boa_pv
  {
    typedef int8_t run_index;
    typedef uint32_t index_in_run;

    template <class HashType, int Pages, int PageSize, int BlockSize>
    struct routing_filter
    {
      // 4-byte entry: bits 0-6 hold run + 1 (0 = empty), bit 7 is a flag (used by run elements, see
      // first_in_run_flag), bits 8-31 a signed delta between the position in the run and its estimate
      // (boa::estimate_position). escape_delta = position unknown. run + 1 <= 127 (RunsPerTier <= 127).
      static constexpr int run_bits = 7;
      static constexpr uint32_t flag_bit = 1u << run_bits;
      static constexpr int delta_shift = 8;
      static constexpr int delta_bits = 24;
      static constexpr int32_t delta_min = -(1 << (delta_bits - 1));
      static constexpr int32_t delta_max = (1 << (delta_bits - 1)) - 1;
      static constexpr int32_t escape_delta = delta_min;

      struct routing_element
      {
        uint32_t bits;
      };
      static_assert(sizeof(routing_element) == 4, "routing_element must be 4 bytes");

      static routing_element encode(run_index run, int32_t delta)
      {
        return {(uint32_t(run + 1) & ((1u << run_bits) - 1))
                | ((uint32_t(delta) & ((1u << delta_bits) - 1)) << delta_shift)};
      }

      static std::pair<run_index, int32_t> decode(routing_element const& element)
      {
        uint32_t v = element.bits;
        run_index run = run_index(int(v & ((1u << run_bits) - 1)) - 1);
        int32_t delta = int32_t(v) >> delta_shift; // arithmetic shift sign-extends the 24-bit delta
        return {run, delta};
      }

      typedef typename VECTOR_GENERATOR<routing_element, PageSize, Pages, BlockSize, stxxl::RC, stxxl::lru>::result
      routing_external_vector;

      unsigned int prefix_bits_length_;
      std::unique_ptr<routing_external_vector> m_filter;

      explicit routing_filter(const unsigned int entries)
      {
        prefix_bits_length_ = std::ceil(std::log2(entries));
        m_filter = std::unique_ptr<routing_external_vector>(
          new routing_external_vector(std::pow(2, prefix_bits_length_)));
        reset();
      }

      void reset()
      {
        routing_element element{0}; // run + 1 == 0: empty
        std::fill(m_filter->begin(), m_filter->end(), element);
      }

      size_t get_bits(HashType const& x, const unsigned bits_length) noexcept
      {
        // using U = typename std::make_unsigned<HashType>::type;
        // U mask = (U(1) << bits_length) - U(1);
        // return static_cast<size_t>(U(x) & mask);
        using U = typename std::make_unsigned<HashType>::type;
        constexpr unsigned W = sizeof(U) * 8;

        // Assume bits_length <= W
        U ux = U(x);

        unsigned shift = W - bits_length;
        U mask = (bits_length == 0) ? U(0) : ((U(1) << bits_length) - U(1));

        return static_cast<size_t>((ux >> shift) & mask);
      }

      bool equal_prefixes(HashType const& h1, HashType const& h2)
      {
        return get_bits(h1, prefix_bits_length_) == get_bits(h2, prefix_bits_length_);
      }

      bool smaller_prefixes(HashType const& h1, HashType const& h2)
      {
        return get_bits(h1, prefix_bits_length_) < get_bits(h2, prefix_bits_length_);
      }

      size_t get_index_from_hash(HashType const& hash)
      {
        return get_bits(hash, prefix_bits_length_);
      }

      size_t get_size_bytes() const
      {
        return m_filter->size() * sizeof(routing_element);
      }

      //! smallest hash with the same prefix as hash
      HashType lowest_hash_with_prefix(HashType const& hash) const noexcept
      {
        using U = typename std::make_unsigned<HashType>::type;
        constexpr unsigned W = sizeof(U) * 8;
        if (prefix_bits_length_ == 0)
          return 0;
        unsigned shift = W - prefix_bits_length_;
        return shift == 0 ? hash : HashType((U(hash) >> shift) << shift);
      }

      //! (run, delta) for the bucket of hash; run == -1 if empty. Positions are decoded by boa.
      std::pair<run_index, int32_t> get(HashType const& hash)
      {
        auto index = get_index_from_hash(hash);
        routing_external_vector const& v = *m_filter;
        return decode(v[index]);
      }

      void insert(run_index r_index, int32_t delta, HashType const& hash)
      {
        auto index = get_index_from_hash(hash);
        (*m_filter)[index] = encode(r_index, delta);
      }

      void flush_to_external_memory() const
      {
        this->m_filter->flush();
      }
    };


    template <class KeyType, class DataType, class HashType, HashType (*HashFunction)(KeyType const&), class
              HashCompare, size_t RunsPerTier, int Pages = 4, int PageSize = 8, int BlockSize = 1 * 1024 * 1024>
    class boa : private noncopyable
    {
    public:
      typedef std::pair<KeyType, DataType> element_type;

      struct stats
      {
        std::unordered_map<uint16_t, size_t> tier_to_collisions;
        size_t pointer_escapes{0}; // directory/element positions too far from their estimate to encode
        int64_t pointer_max_abs_delta{0}; // largest |position - estimate| stored (directory and elements)
      };

      stats m_stats;

      struct search_stats
      {
        size_t visited_runs{0};
        size_t visited_elements{0};
        size_t visited_routing_filter{0};
        size_t empty_elements{0};
        size_t searches{0};

        std::unordered_map<int32, size_t> elements_per_tier;
        std::unordered_map<int32, size_t> runs_per_tier;

        void print_details()
        {
          std::cout << "Searches [" << searches
            << "], Visited runs [" << visited_runs
            << "], Visited total elements [" << visited_elements
            << "], Visited empty elements [" << empty_elements
            << "], Visited routing filter [" << visited_routing_filter
            << "]" << std::endl;
          for (int i = 0; i < runs_per_tier.size(); ++i)
          {
            std::cout << "Tier [" << i
              << "] visited runs [" << runs_per_tier[i]
              << "], Visited Elements [" << elements_per_tier[i]
              << "]" << std::endl;
          }
        }

        std::string info() const
        {
          std::stringstream ss;
          ss << "Searches [" << searches
            << "], Visited runs [" << visited_runs
            << "], Visited total elements [" << visited_elements
            << "], Directory reads [" << visited_routing_filter
            << "]\n";
          return ss.str();
        }

        void reset()
        {
          visited_runs = 0;
          visited_elements = 0;
          visited_routing_filter = 0;
          empty_elements = 0;
          searches = 0;
          elements_per_tier.clear();
          runs_per_tier.clear();
        }
      };

      search_stats stats;

      size_t max_internal_memory()
      {
        size_t internal_memory = ((m_tiers.size() * (RunsPerTier + 1))) * PageSize * Pages * BlockSize +
          3 * PageSize * BlockSize;
        return internal_memory;
      }

      size_t get_tiers_no()
      {
        return m_tiers.size();
      }

      size_t get_merges_occurred() const
      {
        return m_merges_occurred;
      }

      void reset_merges_occurred()
      {
        m_merges_occurred = 0;
      }

      static size_t get_run_element_footprint()
      {
        return sizeof(run_element);
      }

      static size_t get_element_footprint()
      {
        return sizeof(data_element) +
          sizeof(run_element) +
          std::pow(RunsPerTier, 0.5) * sizeof(typename routing_filter<
            HashType, Pages, PageSize, BlockSize>::routing_element);
      }

      size_t get_size_bytes() const
      {
        size_t size = m_elements_in_external_memory * sizeof(data_element) + m_in_memory_table.size() * sizeof(
          cache_element);
        for (auto const& tier : m_tiers)
        {
          for (auto const& run : tier.m_runs)
          {
            size += run.m_run.size() * sizeof(run_element);
          }
        }
        return size;
      }

      size_t get_directory_size_bytes() const
      {
        size_t size = 0;
        for (auto const& tier : m_tiers)
        {
          if (tier.m_routing_filter)
            size += tier.m_routing_filter->get_size_bytes();
        }
        return size;
      }

    private:
      //! In-memory cache
      typedef std::pair<HashType, element_type> cache_element;
      // std::map<HashType, element_type, HashCompare> m_in_memory_table;
      std::vector<cache_element> m_in_memory_table;

      unsigned int m_in_memory_table_max_size;
      size_t m_elements_in_external_memory;
      size_t m_merges_occurred{0};

      template <class T, size_t N>
      struct small_vec
      {
        std::array<T, N> buf{};
        size_t sz = 0;

        bool contains(const T& x) const
        {
          for (size_t i = 0; i < sz; ++i)
            if (buf[i] == x) return true;
          return false;
        }

        void push_unique(const T& x)
        {
          if (contains(x)) return;
          buf[sz++] = x; // assert(sz <= N)
        }

        void clear() { sz = 0; }
      };

      // on-disk structs below are packed to avoid alignment padding
      struct __attribute__((packed)) data_element
      {
        KeyType m_key;
        DataType m_value;
      };

      typedef typename VECTOR_GENERATOR<data_element, PageSize, 3, BlockSize, stxxl::RC, stxxl::lru>::result
      external_data_vector;

      struct __attribute__((packed)) lazy_run_element
      {
        HashType m_hash{0};
        uint32_t m_data_vector_index{0};
      };

      // fields ordered so the struct has no padding without packing
      struct run_element
      {
        HashType m_hash;
        // newest element of the bucket in the next older run containing it, encoded like a directory
        // entry (run + 1, position delta); shared by all elements of the bucket in this run. The flag
        // bit marks the bucket's first (lowest) element in this run. Use set_prev / get_prev.
        uint32_t m_prev{0};
        uint32_t m_data_vector_index{0};

        run_element() = default;
        run_element(lazy_run_element const& lazy) : m_hash(lazy.m_hash), m_data_vector_index(lazy.m_data_vector_index) {
        }

        run_element(std::pair<HashType, cache_element> const& pair) : m_hash(pair.second.first), m_data_vector_index(pair.first) {
        }
      };

      typedef typename VECTOR_GENERATOR<run_element, PageSize, Pages, BlockSize, stxxl::RC, stxxl::lru>::result
      external_vector;

      external_data_vector m_elements;

      struct run
      {
        external_vector m_run;
        unsigned int m_buckets_no{}; // number of buckets in run
        double m_bucket_interval{}; // offset between buckets w.r.t. hash
        size_t m_elements_number{}; // actual elements, not counting padding
        unsigned int m_run_size{};
        unsigned int m_bucket_size{}; // elements per bucket
        bool active{false};
        HashType m_min_hash{0};
        HashType m_max_hash{0};
        size_t m_size{0};
      };

      struct tier
      {
        std::vector<run> m_runs;
        std::unique_ptr<routing_filter<HashType, Pages, PageSize, BlockSize>> m_routing_filter;
        uint16_t m_level;
        bool m_dirty_routing_filter{false};
      };

      //! The external memory runs
      std::vector<tier> m_tiers;

      size_t get_vector_size_for_n_tiers(unsigned int tier) const
      {
        size_t size = 0;
        for (unsigned int i = 0; i <= tier; ++i)
        {
          size += m_tiers[i].m_runs[0].m_run_size;
        }
        return size * RunsPerTier;
      }

    public:
      explicit boa(unsigned int buffer_size) :
        m_in_memory_table_max_size(buffer_size)
      {
        m_elements_in_external_memory = 0;
        init();
      }

      //! Insert a key-value pair in the lsm tree
      void insert(const element_type& value)
      {
        HashType hash;
        hash = HashFunction(value.first);
        m_in_memory_table.push_back({hash, value});

        if (m_in_memory_table.size() >= m_in_memory_table_max_size)
        {
          minor_flush();
          m_in_memory_table.clear();
        }
      }

      //! get number of elements in all tiers
      uint64 size() const
      {
        uint64 total = 0;
        for (auto const& tier : m_tiers)
        {
          for (auto const& run : tier.m_runs)
          {
            total += run.m_run.size();
          }
        }
        return total;
      }

      std::string get_structure_info() const
      {
        std::stringstream info;
        int tierNo{0};
        for (auto const& tier : m_tiers)
        {
          info << "tier[" << tierNo << "] runs: " << active_runs(tier)
            << "\n";
          tierNo++;
        }
        return info.str();
      }

      //! find key-value corresponding to search key
      std::unique_ptr<element_type> find(const KeyType& k)
      {
        ++stats.searches;
        HashType hash;
        hash = HashFunction(k);

        if (k == 1697085)
        {
          auto a = 2;
        }
        //check external-memory
        auto bucket_index = [](HashType const& h, run const& r)
        {
          if (h - r.m_min_hash < 0)
          {
            return 0;
          }
          auto index = static_cast<int>(static_cast<double>((h - r.m_min_hash)) / r.m_bucket_interval);
          // ensure max value falls in the last interval
          if (index >= r.m_buckets_no)
          {
            index = r.m_buckets_no - 1;
          }
          return index;
        };

        int at_tier{-1};
        for (auto const& tier : m_tiers)
        {
          ++at_tier;
          if (active_runs(tier) == 0)
          {
            continue;
          }

          auto pos = directory_get(tier, hash);
          ++stats.visited_routing_filter;
          auto const& data_v = m_elements;

          // pos is the newest (highest) element of the bucket in a run. The bucket's elements in that run
          // are contiguous and sorted by hash, and all share the pointer to the next older run: scan down
          // until hash is passed or the bucket's first element in the run is reached, then jump.
          while (pos.first != -1)
          {
            auto const& r = tier.m_runs[pos.first];
            if (!r.active)
              break;
            ++stats.visited_runs;
            stats.runs_per_tier[at_tier] += 1;

            external_vector const& v = r.m_run;
            std::pair<run_index, index_in_run> next{-1, 0};
            for (int64_t i = pos.second; i >= 0; --i)
            {
              run_element e = v[i];
              ++stats.visited_elements;
              stats.elements_per_tier[at_tier] += 1;
              if (e.m_hash == 0)
              {
                ++stats.empty_elements; // bin padding
                continue;
              }
              if (e.m_hash == hash)
              {
                auto data = data_v[e.m_data_vector_index];
                if (data.m_key == k)
                {
                  return std::unique_ptr<element_type>(new element_type(KeyType(data.m_key), DataType(data.m_value)));
                }
              }
              if (e.m_hash < hash || first_in_run(e))
              {
                next = get_prev(e, tier);
                break;
              }
            }
            if (next.first != -1 && next.first >= pos.first)
              throw std::logic_error("bucket pointer does not lead to an older run");
            pos = next;
          }
        } // for (auto const& tier : m_tiers).

        // check in-memory
        auto it = std::find_if(m_in_memory_table.begin(), m_in_memory_table.end(),
                               [&](cache_element const& item)
                               {
                                 return item.first == hash;
                               });
        if (it != m_in_memory_table.end())
        {
          return std::unique_ptr<element_type>(
            new element_type(it->second.first, it->second.second));
        }

        return nullptr;
      }

      //! print runs per tiers
      void print_internal_structure() const
      {
        std::cout << get_structure_info();
      }

      void print_info()
      {
        std::cout << "Run element size: " << sizeof(run_element) << " bytes." << std::endl;
      }

      int active_runs(unsigned int tier) const
      {
        return active_runs(m_tiers[tier]);
      }

    private:
      routing_filter<HashType, Pages, PageSize, BlockSize>* create_routing_filter(uint16_t tier_level = 0)
      {
        auto run_size = m_in_memory_table_max_size * pow(RunsPerTier, tier_level + 1) * routing_filter_mult;
        // auto hl = std::log2(run_size) / std::log2(RunsPerTier);
        // auto l_hl = p
        return new routing_filter<HashType, Pages, PageSize, BlockSize>(run_size);
        // std::pow(RunsPerTier, ));
        // std::ceil(std::log2(RunsPerTier)),
        // run_size * pow(RunsPerTier, tier_level) + tier_level
        // ); //22
      }

      typedef routing_filter<HashType, Pages, PageSize, BlockSize> routing_filter_type;
      static_assert(RunsPerTier <= std::numeric_limits<run_index>::max(),
                    "run indices must fit in run_index (and so in the directory entry)");

      //! Expected position of hash `low` in run r, assuming uniform hashes over [min, max]. Integer-only,
      //! so directory encoding and decoding compute exactly the same value. Needs r's min/max/size set.
      static index_in_run estimate_position(run const& r, HashType low)
      {
        if (r.m_run_size == 0 || low <= r.m_min_hash)
          return 0;
        if (low >= r.m_max_hash)
          return r.m_run_size - 1;
        __extension__ typedef unsigned __int128 uint128; // avoids overflow of (low - min) * (size - 1)
        return index_in_run(uint128(low - r.m_min_hash) * (r.m_run_size - 1) / (r.m_max_hash - r.m_min_hash));
      }

      //! Position idx of hash's bucket in run r of tier t, as a delta from its estimate
      //! (escape_delta if it doesn't fit). Shared by directory entries and run element pointers.
      int32_t encode_position(tier const& t, run_index r, index_in_run idx, HashType const& hash)
      {
        int64_t est = estimate_position(t.m_runs[r], t.m_routing_filter->lowest_hash_with_prefix(hash));
        int64_t delta = int64_t(idx) - est;
        m_stats.pointer_max_abs_delta = std::max(m_stats.pointer_max_abs_delta, delta < 0 ? -delta : delta);
        if (delta <= routing_filter_type::escape_delta || delta > routing_filter_type::delta_max)
        {
          ++m_stats.pointer_escapes;
          return routing_filter_type::escape_delta;
        }
        return int32_t(delta);
      }

      //! Inverse of encode_position; escaped deltas fall back to escape_search.
      static index_in_run decode_position(tier const& t, run_index r, int32_t delta, HashType const& hash)
      {
        auto& filter = *t.m_routing_filter;
        run const& rn = t.m_runs[r];
        if (delta == routing_filter_type::escape_delta)
          return escape_search(rn, filter, hash);
        return index_in_run(estimate_position(rn, filter.lowest_hash_with_prefix(hash)) + delta);
      }

      //! Directory entry for hash's bucket: run r, position idx (stored relative to its estimate).
      void directory_insert(tier& t, run_index r, index_in_run idx, HashType const& hash)
      {
        int32_t delta = encode_position(t, r, idx, hash);
        t.m_routing_filter->insert(r, delta, hash);
#ifdef BOA_PV_CHECK_DIRECTORY
        if (delta != routing_filter_type::escape_delta)
        {
          auto back = directory_get(t, hash);
          if (back.first != r || back.second != idx)
            throw std::logic_error("directory entry does not decode to the stored position");
        }
#endif
      }

      //! (run, position) of the newest element of hash's bucket in tier t; run == -1 if none.
      static std::pair<run_index, index_in_run> directory_get(tier const& t, HashType const& hash)
      {
        auto entry = t.m_routing_filter->get(hash);
        if (entry.first == -1)
          return {-1, 0};
        return {entry.first, decode_position(t, entry.first, entry.second, hash)};
      }

      //! Sets the pointer of elem, about to be written to run cur of tier t. prev is the bucket's newest
      //! element so far (from the directory; prev.first == -1 for none). If it is in cur, an earlier element
      //! of the bucket was just written there and elem shares its pointer; otherwise elem is the bucket's
      //! first element in cur and points to prev.
      void set_prev(run_element& elem, tier const& t, run_index cur, std::pair<run_index, index_in_run> prev)
      {
        if (prev.first == cur)
        {
          run_element const before = t.m_runs[cur].m_run[prev.second];
          elem.m_prev = before.m_prev & ~routing_filter_type::flag_bit;
          return;
        }
        int32_t delta = prev.first == -1 ? 0 : encode_position(t, prev.first, prev.second, elem.m_hash);
        elem.m_prev = routing_filter_type::encode(prev.first, delta).bits | routing_filter_type::flag_bit;
#ifdef BOA_PV_CHECK_DIRECTORY
        if (delta != routing_filter_type::escape_delta && get_prev(elem, t) != prev && prev.first != -1)
          throw std::logic_error("element pointer does not decode to the stored position");
#endif
      }

      //! True if elem is the first (lowest) element of its bucket in its run.
      static bool first_in_run(run_element const& elem)
      {
        return elem.m_prev & routing_filter_type::flag_bit;
      }

      //! (run, position) of the newest element of elem's bucket in the next older run; run == -1 if none.
      static std::pair<run_index, index_in_run> get_prev(run_element const& elem, tier const& t)
      {
        auto entry = routing_filter_type::decode({elem.m_prev});
        if (entry.first == -1)
          return {-1, 0};
        return {entry.first, decode_position(t, entry.first, entry.second, elem.m_hash)};
      }

      //! Fallback for escaped entries: last (newest) element of hash's bucket in run r.
      static index_in_run escape_search(run const& r, routing_filter_type& filter, HashType const& hash)
      {
        external_vector const& v = r.m_run;
        for (size_t i = v.size(); i-- > 0;)
        {
          run_element e = v[i];
          if (e.m_hash != 0 && filter.equal_prefixes(e.m_hash, hash))
            return index_in_run(i);
        }
        return 0;
      }

      //! initialize the lsm tree
      void init()
      {
        tier t0;
        t0.m_routing_filter.reset(create_routing_filter(0));
        m_tiers.push_back(std::move(t0));
        m_tiers[0].m_level = 0;
        m_tiers[0].m_runs.reserve(RunsPerTier);

        for (int i = 0; i < RunsPerTier; i++)
        {
          run r;
          // r.m_run = new external_vector();
          m_tiers[0].m_runs.push_back(std::move(r));
        }

        m_in_memory_table.reserve(m_in_memory_table_max_size);
      }

      static size_t get_buckets_no(size_t array_size)
      {
        return array_size / static_cast<size_t>(std::log2(array_size));
      }

      //! Flush memtable to a run in external memory
      void minor_flush()
      {
        if (!m_tiers[0].m_routing_filter)
        {
          m_tiers[0].m_routing_filter.reset(create_routing_filter(0));
        }

        // find hashes range [min, max]
        HashType min = std::numeric_limits<HashType>::max();
        HashType max = std::numeric_limits<HashType>::min();

        std::sort(m_in_memory_table.begin(), m_in_memory_table.end(), [](cache_element const& a, cache_element const& b)
        {
          HashCompare cmp{};
          return cmp(a.first, b.first);
        });

        for (auto const& pair : m_in_memory_table)
        {
          if (pair.first > max)
          {
            max = pair.first;
          }
          if (pair.first < min)
          {
            min = pair.first;
          }
        }

        // init run and flush elements
        int first_inactive_index = 0;
        for (auto const& run : m_tiers[0].m_runs)
        {
          if (!run.active)
          {
            break;
          }
          ++first_inactive_index;
        }

        run_element empty_element;
        empty_element.m_hash = 0;

        run& r = m_tiers[0].m_runs[first_inactive_index];
        r.active = true;
        r.m_run_size = m_in_memory_table.size();
        r.m_bucket_size = 0;
        r.m_max_hash = max;
        r.m_min_hash = min;
        r.m_elements_number = 0;
        // r.m_run = std::unique_ptr<external_vector>(new external_vector(r.m_run_size));
        r.m_run.resize(r.m_run_size);
        if (m_elements.size() < get_vector_size_for_n_tiers(0))
        {
          m_elements.resize(get_vector_size_for_n_tiers(0));
        }
        // std::fill(r.m_run.begin(), r.m_run.end(), empty_element);
        r.m_size = 0;

        int at_bucket{0};
        unsigned int added{0};

        auto it = m_in_memory_table.begin();
        size_t total_added{0};
        size_t index{0};

        while (it != m_in_memory_table.end())
        {
          ++(r.m_elements_number);

          run_element elem;
          data_element data_elem;
          elem.m_hash = it->first;
          elem.m_data_vector_index = m_elements_in_external_memory++;
          data_elem.m_key = it->second.first;
          data_elem.m_value = it->second.second;


          auto prev_route = directory_get(m_tiers[0], elem.m_hash);
          if (prev_route.first > first_inactive_index)
          {
            prev_route.first = 0;
            prev_route.second = 0;
          }
          set_prev(elem, m_tiers[0], first_inactive_index, prev_route);
          auto index_in_array = index;
          directory_insert(m_tiers[0], first_inactive_index, index_in_array, elem.m_hash);

          (r.m_run)[index++] = elem;
          m_elements[elem.m_data_vector_index] = data_elem;
          ++r.m_size;
          ++it;
        } // while (it != m_in_memory_table.end()).


        // m_tiers[0].m_runs.push_back(std::move(r));
        compact_tiers();
      }

      int active_runs(tier const& t) const
      {
        int count{0};
        for (auto const& r : t.m_runs)
        {
          if (r.active)
          {
            ++count;
          }
        }
        return count;
      }

      //! merges runs of tiers if needed
      void compact_tiers()
      {
        for (int tier_no = 0; tier_no < m_tiers.size(); ++tier_no)
        {
          if (active_runs(m_tiers[tier_no]) >= RunsPerTier)
          {
            int new_tier_no = tier_no + 1;
            if (m_tiers.size() <= new_tier_no)
            {
              m_tiers.push_back(tier());
              m_tiers[new_tier_no].m_runs.reserve(RunsPerTier);
              m_tiers[new_tier_no].m_level = new_tier_no;
              m_stats.tier_to_collisions[new_tier_no] = 0;
            }

            if (m_tiers[new_tier_no].m_runs.size() < RunsPerTier)
            {
              run r;
              m_tiers[new_tier_no].m_runs.push_back(std::move(r));
            }

            merge_runs(m_tiers[tier_no].m_runs, m_tiers[new_tier_no]);
            ++m_merges_occurred;
            for (auto& run : m_tiers[tier_no].m_runs)
            {
              run.active = false;
            }
            m_tiers[tier_no].m_routing_filter->reset();
            m_stats.tier_to_collisions[tier_no] = 0;
          }
        } // for (auto & tier : m_tiers).
      }

      //! merge runs into next tier
      void merge_runs(std::vector<run> const& sources, tier& dest_tier)
      {
        size_t total_elements{0};
        HashType min = std::numeric_limits<HashType>::max();
        HashType max = std::numeric_limits<HashType>::min();
        for (auto const& source : sources)
        {
          if (min > source.m_min_hash)
          {
            min = source.m_min_hash;
          }
          if (max < source.m_max_hash)
          {
            max = source.m_max_hash;
          }

          total_elements += source.m_elements_number;
        }

        if (!dest_tier.m_routing_filter)
        {
          dest_tier.m_routing_filter.reset(create_routing_filter(dest_tier.m_level));
        }

        // find buckets info
        const unsigned int intervals_no = get_buckets_no(total_elements);
        const double interval_size = static_cast<double>(max - min) / static_cast<double>(intervals_no);
        auto bucket_index = [&interval_size, &min, &intervals_no](HashType const& h)
        {
          auto index = static_cast<int>(static_cast<double>((h - min)) / interval_size);
          // ensure max value falls in the last interval
          if (index >= intervals_no)
          {
            index = intervals_no - 1;
          }
          return index;
        };

        run_element min_key;

        int first_inactive_index = 0;
        for (auto const& run : dest_tier.m_runs)
        {
          if (!run.active)
          {
            break;
          }
          ++first_inactive_index;
        }
        run_element empty_element;
        empty_element.m_hash = 0;

        run& final = dest_tier.m_runs[first_inactive_index];
        final.active = true;
        final.m_run_size = total_elements;
        final.m_bucket_size = 0;
        final.m_max_hash = max;
        final.m_min_hash = min;
        final.m_bucket_interval = interval_size;
        final.m_buckets_no = intervals_no;
        final.m_elements_number = 0;
        // final.m_run = std::unique_ptr<external_vector>(new external_vector(final.m_run_size));
        final.m_run.resize(final.m_run_size);
        if (m_elements.size() < get_vector_size_for_n_tiers(dest_tier.m_level))
        {
          m_elements.resize(get_vector_size_for_n_tiers(dest_tier.m_level));
        }
        // std::fill(final.m_run.begin(), final.m_run.end(), empty_element);

        unsigned int at_bucket{0};
        unsigned int added{0};
        size_t total_added{0};
        size_t index{0};

        struct HeapItem
        {
          run_element elem;
          size_t src; // which run
          size_t index;
        };
        struct ByHash
        {
          bool operator()(HeapItem const& a, HeapItem const& b) const
          {
            return a.elem.m_hash > b.elem.m_hash; // min-heap via priority_queue
          }
        };
        // std::priority_queue<HeapItem, std::vector<HeapItem>, ByHash> heap;
        std::vector<HeapItem> heap;
        heap.reserve(sources.size());

        std::vector<external_vector const*> const_sources;
        const_sources.resize(sources.size());
        for (int i = 0; i < sources.size(); ++i)
        {
          const_sources[i] = &(sources[i].m_run);
        }

        // add heads in heap
        for (int i = 0; i < const_sources.size(); ++i)
        {
          HeapItem item;

          for (int j = 0; j < const_sources[i]->size(); ++j)
          {
            item.elem = (*const_sources[i])[j];
            if (item.elem.m_hash == 0)
            {
              continue;
            }
            else
            {
              item.src = i;
              item.index = j;
              heap.push_back(item);
              break;
            }
          }
        }

        size_t empty_elements_added{0};

        // loop rest items
        while (!heap.empty())
        {
          if (index >= final.m_run.size())
          {
            std::cout << "index >= final.m_run.size()" << std::endl;
            exit(0);
          }

          // std::pop_heap(heap.begin(), heap.end(), ByHash());
          HeapItem min_item = heap[0];
          int min_index_in_heap = 0;
          for (int i = 1; i < heap.size(); ++i)
          {
            if (heap[i].elem.m_hash < min_item.elem.m_hash)
            {
              min_item = heap[i];
              min_index_in_heap = i;
            }
          }

          min_key = min_item.elem;

          auto prev_route = directory_get(dest_tier, min_key.m_hash);
          if (prev_route.first > first_inactive_index)
          {
            prev_route.first = 0;
            prev_route.second = 0;
            std::cerr << "ERROR: " << prev_route.first << " " << prev_route.second << std::endl;
            throw std::logic_error("invalid filter");
          }
          if (prev_route.first > -1)
          {
            m_stats.tier_to_collisions[dest_tier.m_level] += 1;
          }
          set_prev(min_key, dest_tier, first_inactive_index, prev_route);
          auto index_in_array = total_added;

          directory_insert(dest_tier, first_inactive_index, index_in_array, min_key.m_hash);

          (final.m_run)[index++] = min_key;
          ++(final.m_elements_number);

          ++total_added;

          HeapItem item;
          bool _added{false};
          for (int j = min_item.index + 1; j < const_sources[min_item.src]->size(); ++j)
          {
            item.elem = (*const_sources[min_item.src])[j];
            if (item.elem.m_hash != 0)
            {
              item.src = min_item.src;
              item.index = j;
              heap[min_index_in_heap] = item;
              _added = true;
              break;
            }
          }
          if (!_added)
          {
            heap.erase(heap.begin() + min_index_in_heap);
          }
        }

        if (index != final.m_run.size())
        {
          exit(0);
        }
      }

    public:
      void lazy_insert(const element_type& value) {
        lazy_insert_impl(value);
      }

      void consolidate_structure() {
        flush_to_boa_impl();
      }

      void reserve_lazy_space_for_elements(size_t size) {
        m_lazy_insertion_log.reserve(size);
#ifdef IN_MEMORY_SORT
        m_in_memory_lazy_table.reserve(size);
#endif
        m_elements.reserve(size);
      }

    private:
      typedef typename VECTOR_GENERATOR<lazy_run_element, PageSize, 5, 1024*1024, stxxl::RC, stxxl::lru>::result
      external_lazy_run_item_vector;

      std::vector<std::pair<size_t, cache_element>> m_in_memory_lazy_table;
      external_lazy_run_item_vector m_lazy_insertion_log;

      void minor_flush_to_log_vector() {
        if (stxxl::materialization == stxxl::materialization_strategy::merge) {
        std::sort(m_in_memory_table.begin(), m_in_memory_table.end(),
                  [](cache_element const &a, cache_element const &b) {
                    HashCompare cmp{};
                    return cmp(a.first, b.first);
                  });
        }

        // if (m_elements.size() < get_vector_size_for_n_tiers(0))
        // {
        // m_elements.resize(get_vector_size_for_n_tiers(0)); // TODO this will also need resize
        // }

        data_element data_elem;
        lazy_run_element elem;
        for (auto& item : m_in_memory_table)
        {
          auto pos = m_elements_in_external_memory++;
          data_elem.m_key = item.second.first;
          data_elem.m_value = item.second.second;

#ifdef  IN_MEMORY_SORT
          m_in_memory_lazy_table.push_back(std::make_pair(pos, std::move(item)));
#else
          elem.m_hash = item.first;
          elem.m_data_vector_index = pos;

          m_lazy_insertion_log.push_back(elem);
#endif
          if (pos >= m_elements.size()) {
            m_elements.push_back(data_elem);
          }
          else {
            m_elements[pos] = data_elem;
          }
        }
      }

      template<class vector_type, class element_type>
      void sort_vector(typename vector_type::iterator it_begin, typename vector_type::iterator it_end) {
        struct my_less
        {
          typedef element_type value_type;
          bool operator() (const value_type & a, const value_type & b) const
          {
            return a.m_hash < b.m_hash;
          }
          value_type min_value() const { value_type v; v.m_hash = std::numeric_limits<HashType>::min(); return v;};
          value_type max_value() const { value_type v; v.m_hash = std::numeric_limits<HashType>::max(); return v; };
        };

        std::cout << "Sorting lazy insertion log" << std::endl;
        std::cout << "Log size: " << m_lazy_insertion_log.size() << std::endl;

        auto start = std::chrono::high_resolution_clock::now();

        stxxl::sort<BlockSize>(it_begin,
                          it_end,
                               my_less(),
                               256 * 1024 * 1024,
                               stxxl::RC());

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cout << "Sort completed in " << duration.count() << " milliseconds" << std::endl;
      }

      /**
       * If in empty boa, how would it be after insertions
       * @return tier, run where tier max tier reached, run max run in last tier
       */
      std::pair<unsigned int, unsigned int> compute_space_for_lazy_insertion_log(size_t log_size) {
        unsigned int max_tier{0};
        unsigned int max_run{0};

        unsigned int first_tier_runs = log_size / m_in_memory_table_max_size;
        while (first_tier_runs > pow(RunsPerTier, max_tier+1)) {
          ++max_tier;
        }

        unsigned int first_tier_runs_per_max_tier_run = pow(RunsPerTier, max_tier);
        max_run = (first_tier_runs / first_tier_runs_per_max_tier_run) - 1;

        return std::make_pair(max_tier, max_run);
      }

      std::map<unsigned int, unsigned int> compute_layout_for_lazy_insertion_log(unsigned int log_size) {
        std::map<unsigned int, unsigned int> tier_to_max_run;
        unsigned int first_tier_runs = log_size / m_in_memory_table_max_size;

        while (first_tier_runs > 0) {
          unsigned int max_tier{0};
          unsigned int max_run{0};
          while (first_tier_runs > pow(RunsPerTier, max_tier+1)) {
            ++max_tier;
          }
          unsigned int first_tier_runs_per_max_tier_run = pow(RunsPerTier, max_tier);
          max_run = (first_tier_runs / first_tier_runs_per_max_tier_run) - 1;
          tier_to_max_run[max_tier] = max_run;

          first_tier_runs -= (max_run + 1) * pow(RunsPerTier, max_tier);
        }

        return tier_to_max_run;
      }


      bool empty_run_in_tier(unsigned int tier) {
        if (m_tiers[tier].m_runs.size() < RunsPerTier) {
          return true;
        }

        for (auto const & run : m_tiers[tier].m_runs) {
          if (!run.active) {
            return true;
          }
        }

        return false;
      }

      struct displacement {
        unsigned int m_from_tier{0};
        unsigned int m_to_tier{0};
        unsigned int m_from_run{0};

        displacement(unsigned int from_tier, unsigned int to_tier, unsigned int from_run) : m_from_run(from_run),
        m_from_tier(from_tier), m_to_tier(to_tier) {}

      };

      std::vector<displacement> compute_runs_displacements(std::pair<unsigned int, unsigned int> const & new_space) {
        std::vector<displacement> displacements;

        unsigned int new_space_tier = new_space.first, new_space_run = new_space.second;
        bool changed_target_tier{false};

        for (auto const & tier : m_tiers) {
          if (tier.m_level < new_space_tier) {
            for (int r=0 ; r < tier.m_runs.size() ; r++) {
              if (tier.m_runs[r].active) {
                displacements.push_back({tier.m_level, tier.m_level, r});
              }
            }
          }
          else if (tier.m_level == new_space_tier) {
            auto existing_runs = active_runs(tier);
            auto total_runs = new_space_run + 1 + existing_runs;
            if (total_runs >= RunsPerTier) {
              changed_target_tier = true;
              for (int r=0 ; r < tier.m_runs.size() ; r++) { // only existing runs (was std::max(RunsPerTier, size): read past the end)
                if (tier.m_runs[r].active) {
                  displacements.push_back({tier.m_level, tier.m_level, r});
                }
              }
            }
          } // else if (tier.m_level == new_space_tier).
          else if (changed_target_tier) {
            changed_target_tier = false;
            auto existing_runs = active_runs(tier);
            if (existing_runs + 1 >= RunsPerTier) {
              changed_target_tier = true;
              for (int r=0 ; r < tier.m_runs.size(); r++) { // only existing runs (was std::max(RunsPerTier, size): read past the end)
                if (tier.m_runs[r].active) {
                  displacements.push_back({tier.m_level, tier.m_level, r});
                }
              }
            }
          }
          else {
            break;
          }
        } // for (auto const & tier : m_tiers).

        if (displacements.empty()) {
          return {};
        }

        unsigned int existing_elements_tier_dest = displacements.back().m_from_tier + 1;
        if (existing_elements_tier_dest <= new_space_tier) {
          existing_elements_tier_dest = new_space_run + 1 < RunsPerTier ? new_space_tier : new_space_tier + 1;
        }

        for (auto & move : displacements) {
          move.m_to_tier = existing_elements_tier_dest;
        }

        return displacements;
      }

      int first_non_active_run_in_tier(unsigned int tier) {
        for (int r=0 ; r < m_tiers[tier].m_runs.size() ; r++) {
          if (!m_tiers[tier].m_runs[r].active) {
            return r;
          }
        }

        return -1;
      }

      void allocate_space_for_runs(unsigned int tiers) {
        while (m_tiers.size() < tiers) {
          m_tiers.push_back(tier());
          unsigned int i = m_tiers.size() - 1;
          m_tiers[i].m_runs.reserve(RunsPerTier);
          m_tiers[i].m_level = i;
          m_tiers[i].m_routing_filter.reset(create_routing_filter(i));

          while (m_tiers[i].m_runs.size() < RunsPerTier)
          {
            run r;
            m_tiers[i].m_runs.push_back(std::move(r));
          }
        }
      }

      /**
       *
       * @param moves
       * @return #moved elements from log
       */
      unsigned int displace_old_runs(std::vector<displacement> const & displacements) {
        unsigned int const target_tier = displacements.back().m_to_tier;
        allocate_space_for_runs(target_tier + 1);

        unsigned int old_elements{0};
        for (auto const & move : displacements) {
          old_elements += m_tiers[move.m_from_tier].m_runs[move.m_from_run].m_elements_number;
        }
        unsigned int target_tier_run_size = m_in_memory_table_max_size * pow(
                                            RunsPerTier, target_tier);

        if (m_lazy_insertion_log.size() + old_elements < target_tier_run_size) {
          std::cout << "Error: Lazy insertion log size (" << m_lazy_insertion_log.size() << ") + old elements (" <<
              old_elements << ") is less than target tier run size (" << target_tier_run_size << ")" << std::endl;
          throw std::runtime_error("Error: Lazy insertion log size + old elements is less than target tier run size");
        }

        auto target_run = first_non_active_run_in_tier(target_tier);
        if (target_run == -1) {
          std::cout << "Error: No non-active run in target tier" << std::endl;
          throw std::runtime_error("Error: No non-active run in target tier");
        }

        unsigned int consumed_log_elements{0};
        if (stxxl::materialization == stxxl::materialization_strategy::sort) {
#ifdef IN_PLACE_SORT
        run& final = m_tiers[target_tier].m_runs[target_run];
        final.active = true;
        final.m_run_size = target_tier_run_size;
        final.m_elements_number = target_tier_run_size;
        final.m_run.resize(target_tier_run_size);
        auto & tmp = final.m_run;
#else
        external_vector tmp;
        tmp.resize(target_tier_run_size);
#endif
        unsigned int displaced_elements{0};
        unsigned int index{0};
        bool removed_from_target_tier{false};

        for (auto const & move : displacements) {
          auto const & run = m_tiers[move.m_from_tier].m_runs[move.m_from_run].m_run;

          if (move.m_from_tier == target_tier) {
            removed_from_target_tier = true;
          }

          // for (int i=0 ; i < m_tiers[move.m_from_tier].m_runs[move.m_from_run].m_run_size ; i++) {
          for (int i=0 ; i < run.size() ; i++) {
            if (run[i].m_hash == 0) {
              continue;
            }

            tmp[index++] = run[i];
            ++displaced_elements;
          }

          m_tiers[move.m_from_tier].m_runs[move.m_from_run].active = false;
          m_tiers[move.m_from_tier].m_runs[move.m_from_run].m_run.resize(0);
          m_tiers[move.m_from_tier].m_dirty_routing_filter = true;
        }

        if (removed_from_target_tier) {
          auto & tier = m_tiers[target_tier];
          for (int i=0 ; i < tier.m_runs.size() ; ++i) {
            if (!tier.m_runs[i].active) {
              int first_active_run = -1;

              for (int j=i+1 ; j < tier.m_runs.size() ; ++j) {
                if (tier.m_runs[j].active) {
                  first_active_run = j;
                  break;
                }
              }
              if (first_active_run == -1) {
                break;
              }
              else {
                std::cout << "Error: First active run is not the first active run in the tier" << std::endl;
                std::throw_with_nested(std::runtime_error("Error: First active run is not the first active run in the tier"));
                // auto tmp = std::move(tier.m_runs[first_active_run]);
                // tier.m_runs[first_active_run] = std::move(tier.m_runs[i]);
                // tier.m_runs[i] = std::move(tmp);
              }
            }
          }
        }

        auto const v = m_lazy_insertion_log;
        consumed_log_elements = target_tier_run_size - displaced_elements;
        for (int i=0 ; i < consumed_log_elements ; i++) {
          tmp[index++] = v[i];
        }

        sort_vector<external_vector, run_element>(tmp.begin(), tmp.end());

#ifdef IN_PLACE_SORT
        add_in_place_sorted_run(target_tier, target_run);
#else //IN_PLACE_SORT

        auto flush_start = std::chrono::high_resolution_clock::now();

        sorted_vector_to_run<run_element, external_vector>(target_tier,
            target_run,
            tmp,
            0,
            [](run_element const &e) {
            return e.m_hash;
            });

        auto flush_end = std::chrono::high_resolution_clock::now();
        auto flush_duration = std::chrono::duration_cast<std::chrono::milliseconds>(flush_end - flush_start);
        std::cout << "sorted_vector_to_run completed in " << flush_duration.count() << " milliseconds" << std::endl;

#endif // IN_PLACE_SORT
        } else {

        unsigned int displaced_elements{0};
        unsigned int index{0};
        bool removed_from_target_tier{false};

        std::vector<run*> displaced_runs;

        for (auto const & move : displacements) {
          if (move.m_from_tier == target_tier) {
            removed_from_target_tier = true;
          }

          displaced_elements += m_tiers[move.m_from_tier].m_runs[move.m_from_run].m_elements_number;
          // for (int i=0 ; i < m_tiers[move.m_from_tier].m_runs[move.m_from_run].m_run_size ; i++) {
          displaced_runs.push_back(&m_tiers[move.m_from_tier].m_runs[move.m_from_run]);
        }

        if (removed_from_target_tier) {
          auto & tier = m_tiers[target_tier];
          for (int i=0 ; i < tier.m_runs.size() ; ++i) {
            if (!tier.m_runs[i].active) {
              int first_active_run = -1;

              for (int j=i+1 ; j < tier.m_runs.size() ; ++j) {
                if (tier.m_runs[j].active) {
                  first_active_run = j;
                  break;
                }
              }
              if (first_active_run == -1) {
                break;
              }
              else {
                std::cout << "Error: First active run is not the first active run in the tier" << std::endl;
                std::throw_with_nested(std::runtime_error("Error: First active run is not the first active run in the tier"));
                // auto tmp = std::move(tier.m_runs[first_active_run]);
                // tier.m_runs[first_active_run] = std::move(tier.m_runs[i]);
                // tier.m_runs[i] = std::move(tmp);
              }
            }
          }
        }

        std::vector<size_t> lazy_runs_beginnings;
        consumed_log_elements = target_tier_run_size - displaced_elements;
        for (int i=0 ; i < consumed_log_elements ; i+=m_in_memory_table_max_size) {
          lazy_runs_beginnings.push_back(i);
        }


        auto flush_start = std::chrono::high_resolution_clock::now();
        std::cout << "starting merge runs" << std::endl;

        merge_runs(displaced_runs, lazy_runs_beginnings, target_tier, target_run);

        auto flush_end = std::chrono::high_resolution_clock::now();
        auto flush_duration = std::chrono::duration_cast<std::chrono::milliseconds>(flush_end - flush_start);
        std::cout << "merge_runs completed in " << flush_duration.count() << " milliseconds" << std::endl;


        for (auto const & move : displacements) {
          m_tiers[move.m_from_tier].m_runs[move.m_from_run].active = false;
          m_tiers[move.m_from_tier].m_runs[move.m_from_run].m_run.resize(0);
          m_tiers[move.m_from_tier].m_dirty_routing_filter = true;
        }

        }

        for (int i=0 ; i<target_tier ; ++i) {
          m_tiers[i].m_dirty_routing_filter = false;
          m_tiers[i].m_routing_filter->reset();
        }
        return consumed_log_elements;
      }


      /**
       *
       * @return #moved items from log
       */
      unsigned int promote_existing_runs(std::pair<unsigned int, unsigned int> new_space) {
        auto displacements = compute_runs_displacements(new_space);
        auto consumed_log_elements = 0;

        if (!displacements.empty()) {
          consumed_log_elements = displace_old_runs(displacements);
        }

        auto highest_tier = new_space.first;
        auto last_run = new_space.second;

        allocate_space_for_runs(highest_tier+1);
        for (int at_tier=0 ; at_tier <= highest_tier ; at_tier++) {
          // if (m_tiers.size() <= at_tier)
          // {
          //   m_tiers.push_back(tier());
          //   m_tiers[at_tier].m_runs.reserve(RunsPerTier);
          //   m_tiers[at_tier].m_level = at_tier;
          //   m_stats.tier_to_collisions[at_tier] = 0;
          //   if (!m_tiers[at_tier].m_routing_filter) {
          //     m_tiers[at_tier].m_routing_filter.reset(create_routing_filter(at_tier));
          //   }
          // }

          unsigned int target_tier_run_size = m_in_memory_table_max_size * pow(
                                  RunsPerTier, at_tier);
          unsigned int till_run = at_tier == highest_tier ? last_run + active_runs(m_tiers[at_tier]) : RunsPerTier - 1;
          while (m_tiers[at_tier].m_runs.size() < till_run + 1)
          {
            run r;
            r.m_elements_number = target_tier_run_size;
            m_tiers[at_tier].m_runs.push_back(std::move(r));
          }
        }

        // if (m_run.size() < get_vector_size_for_n_tiers(highest_tier))
        // {
        //   m_run.resize(get_vector_size_for_n_tiers(highest_tier));
        // }
        // if (m_elements.size() < get_vector_size_for_n_tiers(highest_tier))
        // {
        //   m_elements.resize(get_vector_size_for_n_tiers(highest_tier));
        // }

        return consumed_log_elements;
      }

      template<class ElementType, class InputLog, class HashExtractor>
      unsigned int sorted_vector_to_run(unsigned int at_tier,
                                int run_index,
                                InputLog const &log,
                                unsigned int log_offset,
                                HashExtractor hash_extractor) {
        unsigned int target_tier_run_size = m_in_memory_table_max_size * pow(RunsPerTier, at_tier);
        run &final = m_tiers[at_tier].m_runs[run_index];
        auto const &routing_filter = m_tiers[at_tier].m_routing_filter;

        final.active = true;
        final.m_elements_number = target_tier_run_size;

        auto min = hash_extractor(log[log_offset]);
        auto max = hash_extractor(log[log_offset + target_tier_run_size - 1]);

        // std::cout << "Min " << min << " Max " << max << std::endl;
        // std::cout << max - min << " " << "max - min" << std::endl;

        const unsigned int intervals_no = get_buckets_no(target_tier_run_size);
        const double interval_size = static_cast<double>(max - min) / static_cast<double>(intervals_no);
        auto bucket_index = [&interval_size, &min, &intervals_no](HashType const &h) {
          auto index = static_cast<int>(static_cast<double>((h - min)) / interval_size);
          // ensure max value falls in the last interval
          if (index >= intervals_no) {
            index = intervals_no - 1;
          }
          return index;
        };

        // run_element min_key;
        // min_key.m_hash = std::numeric_limits<HashType>::max();
        std::vector<unsigned int> buckets_size(intervals_no, 0);

        for (uint i = 0; i < target_tier_run_size; i++) {
          auto h = hash_extractor(log[log_offset + i]);
          if (h == 0) {
            continue;
          }
          ++buckets_size[bucket_index(h)];
        }

        const auto max_bucket_size = *std::max_element(buckets_size.begin(), buckets_size.end());
        final.m_run_size = max_bucket_size * intervals_no;
        final.m_elements_number = target_tier_run_size;
        final.m_bucket_size = max_bucket_size;
        final.m_run.resize(max_bucket_size * intervals_no);

        final.m_max_hash = max;
        final.m_min_hash = min;
        final.m_bucket_interval = interval_size;
        final.m_buckets_no = intervals_no;

        unsigned int at_bucket{0};
        unsigned int added{0};
        size_t total_added{0};
        run_element empty_element;
        unsigned int index{0};
        empty_element.m_hash = 0;

        for (uint i = 0; i < target_tier_run_size; i++) {
          if (index >= final.m_run.size()) {
            std::cout << "index >= final.m_run.size()" << std::endl;
            exit(0);
          }

          auto elem = run_element(log[log_offset++]);

          while (bucket_index(elem.m_hash) != at_bucket) {
            final.m_run[index++] = empty_element; // fill bucket
            // index++;
            // empty_elements_added++;
            ++added;
            ++total_added;
            if (added == max_bucket_size) {
              ++at_bucket;
              added = 0;
            }
          }

          auto prev_route = directory_get(m_tiers[at_tier], elem.m_hash);
          if (prev_route.first > run_index) {
            prev_route.first = 0;
            prev_route.second = 0;
            std::cout << "Error: Routing filter is not consistent: run_index=" << run_index << ", prev_route.first=" << prev_route.first << std::endl;
            throw std::logic_error("Error: Routing filter is not consistent");
          }
          set_prev(elem, m_tiers[at_tier], run_index, prev_route);
          directory_insert(m_tiers[at_tier], run_index, index, elem.m_hash);

          final.m_run[index++] = elem;

          ++added;

          if (added == max_bucket_size) {
            ++at_bucket;
            added = 0;
          }
        } // for (uint i=0 ; i <target_tier_run_size ; i++).

        while (index < final.m_run.size()) {
          final.m_run[index++] = empty_element;
        }

        return log_offset;
      }

      /**
       * at tier, run index there is an added sorted vector, make it boa run
       * @param at_tier
       * @param run_index
       */
      void add_in_place_sorted_run(unsigned int at_tier, int run_index) {
        unsigned int target_tier_run_size = m_in_memory_table_max_size * pow(RunsPerTier, at_tier);
        auto const &routing_filter = m_tiers[at_tier].m_routing_filter;

        auto & final = m_tiers[at_tier].m_runs[run_index];
        final.active = true;
        final.m_elements_number = target_tier_run_size;

        auto min = final.m_run[0].m_hash;
        auto max = final.m_run[target_tier_run_size - 1].m_hash;

        const unsigned int intervals_no = get_buckets_no(target_tier_run_size);
        const double interval_size = static_cast<double>(max - min) / static_cast<double>(intervals_no);
        auto bucket_index = [&interval_size, &min, &intervals_no](HashType const &h) {
          auto index = static_cast<int>(static_cast<double>((h - min)) / interval_size);
          // ensure max value falls in the last interval
          if (index >= intervals_no) {
            index = intervals_no - 1;
          }
          return index;
        };

        // run_element min_key;
        // min_key.m_hash = std::numeric_limits<HashType>::max();
        std::vector<unsigned int> buckets_size(intervals_no, 0);
        auto const & u = final.m_run;
        for (uint i = 0; i < target_tier_run_size; i++) {
          auto h = u[i].m_hash;
          if (h == 0) {
            continue;
          }
          ++buckets_size[bucket_index(h)];
        }

        const auto max_bucket_size = *std::max_element(buckets_size.begin(), buckets_size.end());
        final.m_run_size = max_bucket_size * intervals_no;
        final.m_elements_number = target_tier_run_size;
        final.m_bucket_size = max_bucket_size;
        final.m_run.resize(max_bucket_size * intervals_no);

        final.m_max_hash = max;
        final.m_min_hash = min;
        final.m_bucket_interval = interval_size;
        final.m_buckets_no = intervals_no;

        unsigned int at_bucket{0};
        unsigned int added{0};
        size_t total_added{0};
        run_element empty_element;
        // unsigned int index{0};
        empty_element.m_hash = 0;

        std::deque<run_element> q;

        for (unsigned int i = 0; i < final.m_run.size(); i++) {
          const std::size_t cur_bin = i / max_bucket_size;

          // Step 1: consume original input at this position, if any.
          if (i < target_tier_run_size) {
            auto cur = final.m_run[i];
            if (cur.m_hash != 0) {
              const std::size_t b = bucket_index(cur.m_hash);

              if (b >= buckets_size.size()) {
                throw std::runtime_error("bucket(elem) out of range");
              }

              // Optional safety check: input must be sorted by bucket.
              if (!q.empty() && bucket_index(q.back().m_hash) > b) {
                throw std::runtime_error("input is not sorted by bucket");
              }

              q.push_back(std::move(cur));
            }
            else {
              std::cout << "Error: hash is 0" << std::endl;
              throw std::runtime_error("hash is 0");
            }
          }

          // Step 2: write the correct value for this slot.
          // Step 2: write output for this slot.
            if (!q.empty() && bucket_index(q.front().m_hash) == cur_bin) {
              auto elem = q.front();

              auto prev_route = directory_get(m_tiers[at_tier], elem.m_hash);
              if (prev_route.first > run_index) {
                std::cout << "Error: Routing filter is not consistent: run_index="
                          << run_index
                          << ", prev_route.first=" << prev_route.first
                          << std::endl;
                throw std::runtime_error("Error: Routing filter is not consistent");
              }

              set_prev(elem, m_tiers[at_tier], run_index, prev_route);
              directory_insert(m_tiers[at_tier], run_index, i, elem.m_hash);

              final.m_run[i] = elem;
              q.pop_front();
            } else {
              final.m_run[i] = empty_element;
            }
        } // for (uint i=0 ; i <target_tier_run_size ; i++).

        if (!q.empty()) {
          std::cout << "Error: q.size() != 0" << std::endl;
          throw std::runtime_error("Error: q.size() != 0");
        }
      }

      void flush_from_log(unsigned int moved_first_elements) {
        unsigned int total_moved_elements = moved_first_elements;
#ifdef IN_MEMORY_SORT
        auto const & log = m_in_memory_lazy_table;
#else
        auto const & log = m_lazy_insertion_log;
#endif
        auto remaining_elements = log.size() - total_moved_elements;
        auto final_layout = compute_layout_for_lazy_insertion_log(remaining_elements);

        for (auto tier_layout : final_layout) { //TODO reverse order
          // if (m_tiers[tier_layout.first].m_dirty_routing_filter) {
          //   auto last_tier = final_layout.rbegin()->first;
          //
          //   if (tier_layout.first <= last_tier) {
          //     m_tiers[tier_layout.first].m_routing_filter->reset();
          //     m_tiers[tier_layout.first].m_dirty_routing_filter = false;
          //   }
          //   else {
          //     // auto start = std::chrono::high_resolution_clock::now();
          //     // update_routing_filter_translated_runs(m_tiers[tier_layout.first]);
          //     // auto end = std::chrono::high_resolution_clock::now();
          //     // auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
          //     // std::cout << "update_routing_filter completed in " << duration.count() << " milliseconds" << std::endl;
          //   }
          // }

          // if (active_runs(m_tiers[tier_layout.first]) > 0) {
            // update_routing_filter(m_tiers[tier_layout.first]);
          // }

          for (int at_run=0 ; at_run <= tier_layout.second ; at_run++) {
            if (total_moved_elements == log.size()) {
              std::cout << "Have moved all elements from log" << std::endl;
              continue;
            }

            int first_inactive_run = first_non_active_run_in_tier(tier_layout.first);

            if (first_inactive_run == -1) {
              std::cout << "Error: In tier " << tier_layout.first << " no free run" << std::endl;
              throw std::runtime_error("Error: Tier run is active");
            }

            if (stxxl::materialization != stxxl::materialization_strategy::merge) {
#ifdef IN_MEMORY_SORT
            total_moved_elements = sorted_vector_to_run<lazy_run_element, std::vector<std::pair<size_t, cache_element>>>(tier_layout.first,
              first_inactive_run,
              log,
              total_moved_elements,
              [](std::pair<size_t, cache_element> const &e) {
                return e.second.first;
              });
#else
            total_moved_elements = sorted_vector_to_run<lazy_run_element, external_lazy_run_item_vector>(tier_layout.first,
              first_inactive_run,
              log,
              total_moved_elements,
              [](lazy_run_element const &log) {
                return log.m_hash;
              });
            // auto min = log[total_moved_elements].m_hash;
            // auto max = log[total_moved_elements + target_tier_run_size - 1].m_hash;
#endif

            } else {

            std::vector<size_t> lazy_runs_beginnings;
            std::vector<run*> displaced_runs;
            int target_tier_run_size = m_in_memory_table_max_size * pow(RunsPerTier, tier_layout.first);
            if (target_tier_run_size > log.size() - total_moved_elements) {
              std::cout << "target_tier_run_size is larger than available elements in log" << std::endl;
              std::throw_with_nested(std::runtime_error("Error: target_tier_run_size is larger than available elements in log"));
            }
            for (int i=0 ; i < target_tier_run_size ; i+=m_in_memory_table_max_size) {
              lazy_runs_beginnings.push_back(total_moved_elements + i);
            }
            total_moved_elements += target_tier_run_size;

            merge_runs(displaced_runs, lazy_runs_beginnings, tier_layout.first, first_inactive_run);
            }

          } // for (int at_run=0 ; at_run <= tier_layout.second ; at_run++).
        } // for (auto tier_layout : final_layout).

        if (total_moved_elements != log.size()) {
          std::cout << "Error: total moved elements (" << total_moved_elements << ") != log size (" << log.size() << ")" << std::endl;
          throw std::runtime_error("Error: total moved elements != log size");
        }
      }

      void flush_to_boa_impl() {
#ifdef IN_MEMORY_SORT
        auto & log = m_in_memory_lazy_table;
#else
        auto & log = m_lazy_insertion_log;
#endif

        if (log.empty()) {
          return;
        }
        if (stxxl::materialization != stxxl::materialization_strategy::merge) {
#ifdef IN_MEMORY_SORT
        std::cout << "Sorting lazy insertion log" << std::endl;
        std::cout << "Log size: " << log.size() << std::endl;
        auto start = std::chrono::high_resolution_clock::now();
        auto hashFunction = [](const std::pair<size_t, cache_element> &x) -> HashType {
          return x.second.first;
        };
        ips2ra::sort(m_in_memory_lazy_table.begin(), m_in_memory_lazy_table.end(), hashFunction);
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cout << "Sort completed in " << duration.count() << " milliseconds" << std::endl;
#else
        sort_vector<external_lazy_run_item_vector, lazy_run_element>(m_lazy_insertion_log.begin(), m_lazy_insertion_log.end());
#endif
        }

        auto new_space = compute_space_for_lazy_insertion_log(log.size());
        auto consumed_log_elements = promote_existing_runs(new_space);

        auto flush_start = std::chrono::high_resolution_clock::now();
        flush_from_log(consumed_log_elements);
        auto flush_end = std::chrono::high_resolution_clock::now();
        auto flush_duration = std::chrono::duration_cast<std::chrono::milliseconds>(flush_end - flush_start);
        std::cout << "flush_from_log completed in " << flush_duration.count() << " milliseconds" << std::endl;

        std::cout << "Flushed lazy insertion log size: " << log.size() << std::endl;

        log.resize(0);
        compact_tiers();
        // update_routing_filters();
      }

      void lazy_insert_impl(element_type const &value) {
        HashType hash;
        hash = HashFunction(value.first);

        m_in_memory_table.push_back({hash, value});

        if (m_in_memory_table.size() >= m_in_memory_table_max_size)
        {
          minor_flush_to_log_vector();
          m_in_memory_table.clear();
        }
      }

      void update_routing_filter_translated_runs(tier& t)
      {
        t.m_dirty_routing_filter = false;
        t.m_routing_filter->reset();

        for (int r=0 ; r<t.m_runs.size() ; ++r) {
          auto const& run = t.m_runs[r];
          if (!run.active) {
            break;
          }

          auto& v = t.m_runs[r].m_run;

          for (int i=0 ; i<v.size() ; ++i) {
            auto elem = v[i];

            if (elem.m_hash == 0) {
              continue;
            }
            
            auto prev_route = directory_get(t, elem.m_hash);
            if (prev_route.first > r)
            {
              prev_route.first = 0;
              prev_route.second = 0;
            }
            set_prev(elem, t, r, prev_route);
            directory_insert(t, r, i, elem.m_hash);

            v[i] = elem;
          }

        } // for (int r=0 ; r<RunsPerTier ; ++r).
      }

void merge_runs(std::vector<run*> const& displaced_runs, std::vector<size_t> lazy_runs_beginnings,
  int dest_tier, int run_index)
      {
        size_t total_elements{0};
        HashType min = std::numeric_limits<HashType>::max();
        HashType max = std::numeric_limits<HashType>::min();
        for (auto source : displaced_runs)
        {
          if (min > source->m_min_hash)
          {
            min = source->m_min_hash;
          }
          if (max < source->m_max_hash)
          {
            max = source->m_max_hash;
          }

          total_elements += source->m_elements_number;
        }

        auto const & log = m_lazy_insertion_log;
        if (!lazy_runs_beginnings.empty()) {
          for (int i = 0; i < m_in_memory_table_max_size + lazy_runs_beginnings.back(); ++i) {
            auto h = log[i].m_hash;
            if (min > h)
            {
              min = h;
            }
            if (max < h)
            {
              max = h;
            }

            ++total_elements;
          }
        }

        // find buckets info
        const unsigned int intervals_no = get_buckets_no(total_elements);
        const double interval_size = static_cast<double>(max - min) / static_cast<double>(intervals_no);
        auto bucket_index = [&interval_size, &min, &intervals_no](HashType const& h)
        {
          auto index = static_cast<int>(static_cast<double>((h - min)) / interval_size);
          // ensure max value falls in the last interval
          if (index >= intervals_no)
          {
            index = intervals_no - 1;
          }
          return index;
        };

        run_element min_key;

        std::vector<unsigned int> buckets_size(intervals_no, 0);
        for (auto const& source : displaced_runs)
        {
          min_key.m_hash = std::numeric_limits<HashType>::max();
          external_vector const& v = source->m_run;

          for (int i = 0; i < v.size(); ++i)
          {
            auto h = v[i].m_hash;
            if (h == 0)
            {
              continue;
            }
            ++buckets_size[bucket_index(h)];
          }
        }

        if (!lazy_runs_beginnings.empty()) {
          for (int i = 0; i < m_in_memory_table_max_size + lazy_runs_beginnings.back(); ++i) {
            auto h = log[i].m_hash;
            if (h == 0) {
              continue;
            }
            ++buckets_size[bucket_index(h)];
          }
        }
        const auto max_bucket_size = *std::max_element(buckets_size.begin(), buckets_size.end());

        run_element empty_element;
        empty_element.m_hash = 0;

        run& final = m_tiers[dest_tier].m_runs[run_index];
        final.active = true;
        final.m_run_size = max_bucket_size * intervals_no;
        final.m_bucket_size = max_bucket_size;
        final.m_max_hash = max;
        final.m_min_hash = min;
        final.m_bucket_interval = interval_size;
        final.m_buckets_no = intervals_no;
        final.m_elements_number = 0;
        // final.m_run = std::unique_ptr<external_vector>(new external_vector(final.m_run_size));
        final.m_run.resize(final.m_run_size);
        // std::fill(final.m_run.begin(), final.m_run.end(), empty_element);

        unsigned int at_bucket{0};
        unsigned int added{0};
        size_t total_added{0};
        size_t index{0};

        struct HeapItem
        {
          run_element elem;
          size_t src; // which run
          size_t index;
        };
        struct ByHash
        {
          bool operator()(HeapItem const& a, HeapItem const& b) const
          {
            return a.elem.m_hash > b.elem.m_hash; // min-heap via priority_queue
          }
        };
        std::priority_queue<HeapItem, std::vector<HeapItem>, ByHash> heap;
        // std::vector<HeapItem> heap;
        // heap.reserve(displaced_runs.size() + lazy_runs_beginnings.size());

        std::vector<external_vector const*> const_sources;
        const_sources.resize(displaced_runs.size());
        for (int i = 0; i < displaced_runs.size(); ++i)
        {
          const_sources[i] = &(displaced_runs[i]->m_run);
        }

        // add heads in heap
        for (int i = 0; i < const_sources.size(); ++i)
        {
          HeapItem item;

          for (int j = 0; j < const_sources[i]->size(); ++j)
          {
            item.elem = (*const_sources[i])[j];
            if (item.elem.m_hash == 0)
            {
              continue;
            }
            else
            {
              item.src = i;
              item.index = j;
              heap.push(item);
              break;
            }
          }
        }

        for (int i = 0; i< lazy_runs_beginnings.size() ; ++i) {
          HeapItem item;
          auto begin = lazy_runs_beginnings[i];

          for (int j = begin; j < begin + m_in_memory_table_max_size; ++j) {
            item.elem = log[j];
            if (item.elem.m_hash == 0)
            {
              continue;
            }
            else
            {
              item.src = 1000000 + i;
              item.index = j;
              heap.push(item);
              break;
            }
          }
        }

        size_t empty_elements_added{0};

        // loop rest items
        while (!heap.empty())
        {

          if (index >= final.m_run.size())
          {
            std::cout << "index >= final.m_run.size()" << std::endl;
            throw std::runtime_error("index >= final.m_run.size()");
          }

          // std::pop_heap(heap.begin(), heap.end(), ByHash());
          HeapItem min_item = heap.top();
          int min_index_in_heap = 0;
          heap.pop();
          // for (int i = 1; i < heap.size(); ++i)
          // {
            // if (heap[i].elem.m_hash < min_item.elem.m_hash)
            // {
              // min_item = heap[i];
              // min_index_in_heap = i;
            // }
          // }

          min_key = min_item.elem;
          while (bucket_index(min_key.m_hash) != at_bucket)
          {
            final.m_run[index++] = empty_element; // fill bucket
            // index++;
            empty_elements_added++;
            ++added;
            ++total_added;
            if (added == max_bucket_size)
            {
              ++at_bucket;
              added = 0;
            }
          }

          auto prev_route = directory_get(m_tiers[dest_tier], min_key.m_hash);
          if (prev_route.first > run_index)
          {
            prev_route.first = 0;
            prev_route.second = 0;
            std::cerr << "ERROR: " << prev_route.first << " " << prev_route.second << std::endl;
            throw std::runtime_error("invalid filter");
          }
          if (prev_route.first > -1)
          {
            m_stats.tier_to_collisions[m_tiers[dest_tier].m_level] += 1;
          }
          set_prev(min_key, m_tiers[dest_tier], run_index, prev_route);

          // the directory points to the newest element of the bucket (this one); older elements,
          // including those earlier in this run, are reached through the element pointers
          directory_insert(m_tiers[dest_tier], run_index, index, min_key.m_hash);

          (final.m_run)[index++] = min_key;
          ++(final.m_elements_number);

          ++total_added;

          ++added;

          if (added == max_bucket_size)
          {
            ++at_bucket;
            added = 0;
          }


          HeapItem item;
          bool _added{false};
          if (min_item.src < 1000000) {
            for (int j = min_item.index + 1; j < const_sources[min_item.src]->size(); ++j)
            {
              item.elem = (*const_sources[min_item.src])[j];
              if (item.elem.m_hash != 0)
              {
                item.src = min_item.src;
                item.index = j;
                // heap[min_index_in_heap] = item;
                heap.push(item);
                _added = true;
                break;
              }
            }
          }
          else {
            auto begin = lazy_runs_beginnings[min_item.src - 1000000];
            for (int j = min_item.index + 1 ; j < begin + m_in_memory_table_max_size; ++j) {
              item.elem = log[j];
              if (item.elem.m_hash != 0)
              {
                item.src = min_item.src;
                item.index = j;
                // heap[min_index_in_heap] = item;
                heap.push(item);
                _added = true;
                break;
              }
            }
          }
          if (!_added)
          {
            // heap.erase(heap.begin() + min_index_in_heap);
          }
        }
        while (index < final.m_run.size())
        {
          final.m_run[index++] = empty_element;
        }
        if (!heap.empty()) {
          std::cout << "ERROR: heap not empty" << std::endl;
          throw std::runtime_error("ERROR: heap not empty");
        }
      }
    };
  } // namespace boa.

STXXL_END_NAMESPACE

#endif // BOA_PV_H
