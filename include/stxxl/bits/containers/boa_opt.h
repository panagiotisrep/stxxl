//
// Created by panos on 6/23/25.
//

#ifndef BOA_NO_BUCKETS_H
#define BOA_H

#include <chrono>

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

// #define ROUTING_FILTER_MULT 1

STXXL_BEGIN_NAMESPACE
  namespace boa_opt
  {
    typedef int8_t run_index;
    typedef uint32_t index_in_run;

    template <class HashType, int Pages, int PageSize, int BlockSize>
    struct routing_filter
    {
      struct routing_element
      {
        run_index run;
        index_in_run index;
      };

      typedef typename VECTOR_GENERATOR<routing_element, PageSize, Pages, BlockSize, stxxl::RC, stxxl::lru>::result
      routing_external_vector;

      struct tier_info
      {
        unsigned int m_prefix_bits_length_{0};
        size_t prefix_combs;
        size_t m_entries_no{0};
        size_t m_offset{0};
      };

      std::vector<tier_info> m_tiers_info;
      std::unique_ptr<routing_external_vector> m_filter;

      // explicit routing_filter(const unsigned int entries)
      // {
      //   prefix_bits_length_ = std::floor(std::log2(entries));
      //   m_filter = std::unique_ptr<routing_external_vector>(
      //     new routing_external_vector(std::pow(2, prefix_bits_length_)));
      //   reset();
      // }

      routing_filter()
      {
        m_filter = std::unique_ptr<routing_external_vector>(new routing_external_vector());
      }

      void add_tier(const unsigned int entries)
      {
        tier_info info;
        info.m_prefix_bits_length_ = std::ceil(std::log2(entries));
        info.m_entries_no = std::pow(2, info.m_prefix_bits_length_);;
        info.prefix_combs = std::pow(2, info.m_prefix_bits_length_);

        for (auto const& ti : m_tiers_info)
        {
          info.m_offset += ti.m_entries_no;
        }

        m_tiers_info.push_back(info);
        m_filter->resize(info.m_entries_no + info.m_offset);
        reset(m_tiers_info.size() - 1);
      }

      void reset(unsigned int tier_no)
      {
        auto const& info = m_tiers_info[tier_no];

        routing_element element;
        element.run = 0;
        element.index = 0;
        std::fill(m_filter->begin() + info.m_offset, m_filter->begin() + info.m_offset + info.m_entries_no, element);
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

      size_t get_index_from_hash(HashType const& hash, tier_info const& info)
      {
        auto i = get_bits(hash, info.m_prefix_bits_length_);
        return (i * info.m_entries_no) / info.prefix_combs;
      }

      std::pair<run_index, index_in_run> get_run_index(HashType const& hash, unsigned int tier_no)
      {
        auto const& info = m_tiers_info[tier_no];
        auto index = get_index_from_hash(hash, info);
        routing_external_vector const& v = *m_filter;
        routing_element ret = v[index + info.m_offset];
        ret.run -= 1;
        return {ret.run, ret.index};
      }

      void insert(run_index r_index, index_in_run index_in_r, unsigned int tier_no, HashType const& hash)
      {
        auto const& info = m_tiers_info[tier_no];
        auto index = get_index_from_hash(hash, info);
        routing_element element;
        element.run = r_index + 1;
        element.index = index_in_r;
        (*m_filter)[index + info.m_offset] = element;
      }

      size_t get_size_bytes() const
      {
        return m_filter->size() * sizeof(routing_element);
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

        void print()
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
      };

      search_stats stats;

      size_t max_internal_memory()
      {
        size_t internal_memory = PageSize * (Pages + RunsPerTier + 3) * BlockSize;
        return internal_memory;
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

      static size_t get_element_footprint()
      {
        return sizeof(data_element) +
          sizeof(run_element) +
          std::pow(RunsPerTier, 0.5) * sizeof(typename routing_filter<
            HashType, Pages, PageSize, BlockSize>::routing_element);
      }

      static size_t get_run_element_footprint()
      {
        return sizeof(run_element);
      }

      size_t get_tiers_no()
      {
        return m_tiers.size();
      }

      size_t get_size_bytes() const
      {
        return m_elements_in_external_memory * sizeof(data_element)
          + m_run.size() * sizeof(run_element)
          + m_routing_filter->get_size_bytes()
          + m_in_memory_table.size() * sizeof(cache_element);
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

      struct data_element
      {
        KeyType m_key;
        DataType m_value;
      };

      typedef typename VECTOR_GENERATOR<data_element, PageSize, 3, BlockSize, stxxl::RC, stxxl::lru>::result
      external_data_vector;

      struct run_element
      {
        run_index m_prev_run = -1;
        index_in_run m_prev_index_in_run = 0;
        HashType m_hash{0};
        uint32_t m_data_vector_index{0};
      };

      typedef typename VECTOR_GENERATOR<run_element, PageSize, Pages, BlockSize, stxxl::RC, stxxl::lru>::result
      external_vector;

      struct run
      {
        size_t m_elements_number{}; // actual elements, not counting padding
        unsigned int m_run_size{};
        bool active{false};
      };

      external_vector m_run;
      external_data_vector m_elements;
      external_vector m_lazy_insertion_log;
      std::unique_ptr<routing_filter<HashType, 4, PageSize, BlockSize>> m_routing_filter;

      struct tier
      {
        // external_vector m_run;
        std::vector<run> m_runs;
        uint16_t m_level{0};
        bool m_dirty_routing_filter{false};
        bool m_empty_filter{true};

        size_t vector_run_offset(unsigned int run) const
        {
          return m_runs[0].m_run_size * run;
        }
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

      size_t get_run_offset(unsigned int tier, unsigned int run) const
      {
        size_t offset = 0;
        if (tier > 0)
        {
          offset = get_vector_size_for_n_tiers(tier - 1);
        }

        return offset + m_tiers[tier].vector_run_offset(run);
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

      void lazy_insert(const element_type& value) {
        lazy_insert_impl(value);
      }

      void consolidate_structure() {
        flush_to_boa_impl();
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

      //! find key-value corresponding to search key
      std::unique_ptr<element_type> find(const KeyType& k)
      {
        ++stats.searches;
        HashType hash;
        hash = HashFunction(k);

        //check external-memory
        int at_tier{-1};
        for (auto const& tier : m_tiers)
        {
          ++at_tier;
          if (active_runs(tier) == 0)
          {
            continue;
          }

          small_vec<int, RunsPerTier> visited_runs;
          auto run_to_search = m_routing_filter->get_run_index(hash, tier.m_level);
          ++stats.visited_routing_filter;

          while (run_to_search.first != -1)
          {
            visited_runs.push_unique(run_to_search.first);

            auto const& r = tier.m_runs[run_to_search.first];
            if (r.active)
            {
              if (stats.runs_per_tier.find(at_tier) == stats.runs_per_tier.end())
              {
                stats.runs_per_tier[at_tier] = 1;
                stats.elements_per_tier[at_tier] = 0;
              }
              else
              {
                stats.runs_per_tier[at_tier] += 1;
              }

              external_vector const* v = &m_run;
              auto const& data_v = m_elements;

              auto offset = get_run_offset(tier.m_level, run_to_search.first);
              auto prev_run = run_to_search.first;

              while (run_to_search.first != -1)
              {
                ++stats.visited_elements;

                if (prev_run != run_to_search.first)
                {
                  prev_run = run_to_search.first;
                  offset = get_run_offset(tier.m_level, run_to_search.first);
                }

                auto prev = (*v)[offset + run_to_search.second];

                if (prev.m_hash == hash)
                {
                  auto data = data_v[prev.m_data_vector_index];
                  if (data.m_key == k)
                  {
                    return std::unique_ptr<element_type>(new element_type(data.m_key, data.m_value));
                  }
                }
                if (!visited_runs.contains(prev.m_prev_run))
                {
                  visited_runs.push_unique(prev.m_prev_run);
                }
                run_to_search = {prev.m_prev_run, prev.m_prev_index_in_run};
              }
            } // if (r.active).
            else
            {
              run_to_search.first = -1;
            }
          } // while (run_to_search.first != -1).
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
      void print_internal_structure()
      {
        int tierNo{0};
        for (auto const& tier : m_tiers)
        {
          std::cout << "tier[" << tierNo << "] runs: " << tier.m_runs.size()
            << std::endl;
          tierNo++;
        }
      }

      void reserve_lazy_space_for_elements(size_t size) {
        m_lazy_insertion_log.reserve(size);
        m_elements.reserve(size);
      }

      void print_info()
      {
        std::cout << "Run element size: " << sizeof(run_element) << " bytes." << std::endl;
      }

      size_t get_merges_occurred() const
      {
        return m_merges_occurred;
      }

      void reset_merges_occurred()
      {
        m_merges_occurred = 0;
      }

    private:
      // routing_filter<HashType, Pages, PageSize, BlockSize>* create_routing_filter(
      //   uint32_t capacity, uint16_t tier_level = 0)
      // {
      //   auto run_size = m_in_memory_table_max_size * pow(RunsPerTier, tier_level);
      //   // auto hl = std::log2(run_size) / std::log2(RunsPerTier);
      //   // auto l_hl = p
      //
      //   // std::pow(RunsPerTier, ));
      //   // std::ceil(std::log2(RunsPerTier)),
      //   // run_size * pow(RunsPerTier, tier_level) + tier_level
      //   // ); //22
      // }

      void minor_flush_to_log_vector() {
        // TODO does this help stxxl sort?
        // std::sort(m_in_memory_table.begin(), m_in_memory_table.end(), [](cache_element const& a, cache_element const& b)
        // {
        //   HashCompare cmp{};
        //   return cmp(a.first, b.first);
        // });

        // if (m_elements.size() < get_vector_size_for_n_tiers(0))
        // {
          // m_elements.resize(get_vector_size_for_n_tiers(0)); // TODO this will also need resize
        // }

        for (auto& item : m_in_memory_table)
        {
          run_element elem;
          data_element data_elem;
          elem.m_hash = item.first;
          elem.m_data_vector_index = m_elements_in_external_memory++;
          data_elem.m_key = item.second.first;
          data_elem.m_value = item.second.second;

          m_lazy_insertion_log.push_back(elem);
          // m_elements[elem.m_data_vector_index] = data_elem;
          m_elements.push_back(data_elem);
        }
      }

      void sort_vector(typename external_vector::iterator it_begin, typename external_vector::iterator it_end) {
        struct my_less
        {
          typedef run_element value_type;
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
      std::pair<unsigned int, unsigned int> compute_space_for_lazy_insertion_log() {
        unsigned int max_tier{0};
        unsigned int max_run{0};

        unsigned int first_tier_runs = m_lazy_insertion_log.size() / m_in_memory_table_max_size;
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

      struct move {
        unsigned int m_from_tier{0};
        unsigned int m_to_tier{0};
        unsigned int m_from_run{0};

        move(unsigned int from_tier, unsigned int to_tier, unsigned int from_run) : m_from_run(from_run),
        m_from_tier(from_tier), m_to_tier(to_tier) {}
        
      };

      std::vector<move> compute_old_elements_moves(std::pair<unsigned int, unsigned int> const & new_space) {
        std::vector<move> existing_moves;

        unsigned int new_space_tier = new_space.first, new_space_run = new_space.second;

        for (auto const & tier : m_tiers) {
          if (tier.m_level < new_space_tier) {
            for (int r=0 ; r < tier.m_runs.size() ; r++) {
              if (tier.m_runs[r].active) {
                existing_moves.push_back({tier.m_level, tier.m_level, r});
              }
            }
          }
          else if (tier.m_level == new_space_tier) {
            auto existing_runs = active_runs(tier);
            auto total_runs = new_space_run + 1 + existing_runs;

            if (total_runs > RunsPerTier) {
              for (int r=0 ; r < tier.m_runs.size() ; r++) {
                if (tier.m_runs[r].active) {
                  existing_moves.push_back({tier.m_level, tier.m_level, r});
                  --total_runs;
                }
                if (total_runs == RunsPerTier) {
                  break;
                }
              }
            }
          } // else if (tier.m_level == new_space_tier).
          else {
            break;
          }
        } // for (auto const & tier : m_tiers).

        if (existing_moves.empty()) {
          return {};
        }

        unsigned int existing_elements_tier_dest = existing_moves.back().m_from_tier + 1;
        if (existing_elements_tier_dest <= new_space_tier) {
          existing_elements_tier_dest = new_space_run + 1 < RunsPerTier ? new_space_tier : new_space_tier + 1;
        }

        for (auto & move : existing_moves) {
          move.m_to_tier = existing_elements_tier_dest;
        }

        return existing_moves;
      }

      int first_non_active_run_in_tier(unsigned int tier) {
        for (int r=0 ; r < m_tiers[tier].m_runs.size() ; r++) {
          if (!m_tiers[tier].m_runs[r].active) {
            return r;
          }
        }

        return -1;
      }

      /**
       *
       * @param moves
       * @return #moved elements from log
       */
      unsigned int move_old_elements(std::vector<move> const & moves) {
        unsigned int target_tier = moves.back().m_to_tier;
        if (m_tiers.size() <= target_tier)
        {
          m_routing_filter->add_tier(routing_filter_entries_for_level(target_tier));
          m_tiers.push_back(tier());
          m_tiers[target_tier].m_runs.reserve(RunsPerTier);
          m_tiers[target_tier].m_level = target_tier;
          m_stats.tier_to_collisions[target_tier] = 0;
        }
        if (m_tiers[target_tier].m_runs.size() < RunsPerTier)
        {
          run r;
          m_tiers[target_tier].m_runs.push_back(std::move(r));
        }

        unsigned int old_elements{0};
        for (auto const & move : moves) {
          old_elements += m_tiers[move.m_from_tier].m_runs[move.m_from_run].m_run_size;
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

        run& final = m_tiers[target_tier].m_runs[target_run];
        final.active = true;
        final.m_run_size = target_tier_run_size;
        final.m_elements_number = target_tier_run_size;

        m_tiers[target_tier].m_dirty_routing_filter = true;

        // final.m_run = std::unique_ptr<external_vector>(new external_vector(final.m_run_size));
        if (m_run.size() < get_vector_size_for_n_tiers(target_tier))
        {
          m_run.resize(get_vector_size_for_n_tiers(target_tier));
        }
        if (m_elements.size() < get_vector_size_for_n_tiers(target_tier))
        {
          m_elements.resize(get_vector_size_for_n_tiers(target_tier));
        }
        auto target_run_offset = get_run_offset(target_tier, target_run);

        unsigned int moved_elements{0};
        for (auto const & move : moves) {
          auto offset = get_run_offset(move.m_from_tier, move.m_from_run);
          auto const & run = m_run;

          for (int i=0 ; i < m_tiers[move.m_from_tier].m_runs[move.m_from_run].m_run_size ; i++) {
            m_run[target_run_offset + moved_elements] = run[offset + i];
            ++moved_elements;
          }

          m_tiers[move.m_from_tier].m_runs[move.m_from_run].active = false;
        }

        auto const v = m_lazy_insertion_log;
        unsigned int moved_elements_from_log = target_tier_run_size - moved_elements;
        while (moved_elements < target_tier_run_size) {
          m_run[target_run_offset + moved_elements] = v[moved_elements];
          ++moved_elements;
        }

        auto it_begin = m_run.begin() + target_run_offset;
        auto it_end = m_run.begin() + target_run_offset + moved_elements;
        sort_vector(it_begin, it_end);

        return moved_elements_from_log;
      }


      /**
       *
       * @return #moved items from log
       */
      unsigned int make_space_for_new_inserts(std::pair<unsigned int, unsigned int> new_space) {
        auto old_elements_moves = compute_old_elements_moves(new_space);
        auto moved_elements = 0;

        if (!old_elements_moves.empty()) {
          moved_elements = move_old_elements(old_elements_moves);
        }

        auto highest_tier = new_space.first;
        auto last_run = new_space.second;

        for (int at_tier=0 ; at_tier <= highest_tier ; at_tier++) {
          if (m_tiers.size() <= at_tier)
          {
            m_routing_filter->add_tier(routing_filter_entries_for_level(at_tier));
            m_tiers.push_back(tier());
            m_tiers[at_tier].m_runs.reserve(RunsPerTier);
            m_tiers[at_tier].m_level = at_tier;
            m_stats.tier_to_collisions[at_tier] = 0;
          }

          unsigned int target_tier_run_size = m_in_memory_table_max_size * pow(
                                  RunsPerTier, at_tier);
          unsigned int till_run = at_tier == highest_tier ? last_run + active_runs(m_tiers[at_tier]) : RunsPerTier - 1;
          while (m_tiers[at_tier].m_runs.size() < till_run + 1)
          {
            run r;
            r.m_run_size = target_tier_run_size;
            r.m_elements_number = target_tier_run_size;
            m_tiers[at_tier].m_runs.push_back(std::move(r));
          }
        }

        if (m_run.size() < get_vector_size_for_n_tiers(highest_tier))
        {
          m_run.resize(get_vector_size_for_n_tiers(highest_tier));
        }
        if (m_elements.size() < get_vector_size_for_n_tiers(highest_tier))
        {
          m_elements.resize(get_vector_size_for_n_tiers(highest_tier));
        }

        return moved_elements;
      }

      void flush_from_log(unsigned int moved_first_elements) {
        unsigned int total_moved_elements = moved_first_elements;
        auto const & log = m_lazy_insertion_log;
        auto remaining_elements = log.size() - total_moved_elements;
        auto final_layout = compute_layout_for_lazy_insertion_log(remaining_elements);

        for (auto tier_layout : final_layout) {
          if (!m_tiers[tier_layout.first].m_empty_filter) {
            m_routing_filter->reset(tier_layout.first);
          }

          if (active_runs(m_tiers[tier_layout.first]) > 0) {
            update_routing_filter(m_tiers[tier_layout.first]);
            m_tiers[tier_layout.first].m_dirty_routing_filter = false;
          }

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

            // m_tiers[tier_layout.first].m_dirty_routing_filter = true;

            unsigned int target_tier_run_size = m_in_memory_table_max_size * pow(
                                 RunsPerTier, tier_layout.first);
            run& final = m_tiers[tier_layout.first].m_runs[first_inactive_run];
            final.active = true;
            final.m_run_size = target_tier_run_size;
            final.m_elements_number = target_tier_run_size;

            if (m_run.size() < get_vector_size_for_n_tiers(tier_layout.first))
            {
              m_run.resize(get_vector_size_for_n_tiers(tier_layout.first));
            }
            if (m_elements.size() < get_vector_size_for_n_tiers(tier_layout.first))
            {
              m_elements.resize(get_vector_size_for_n_tiers(tier_layout.first));
            }

            auto target_run_offset = get_run_offset(tier_layout.first, first_inactive_run);
            for (int i=0 ; i <final.m_run_size ; i++) {
              auto elem = log[total_moved_elements++];

              auto prev_route = m_routing_filter->get_run_index(elem.m_hash, tier_layout.first);
              if (prev_route.first > first_inactive_run)
              {
                prev_route.first = 0;
                prev_route.second = 0;
              }
              elem.m_prev_run = prev_route.first;
              elem.m_prev_index_in_run = prev_route.second;
              m_routing_filter->insert(first_inactive_run, i, tier_layout.first, elem.m_hash);

              m_run[target_run_offset + i] = elem;
            }
          }
        } // for (auto tier_layout : final_layout).

        if (total_moved_elements != log.size()) {
          std::cout << "Error: total moved elements (" << total_moved_elements << ") != log size (" << log.size() << ")" << std::endl;
          throw std::runtime_error("Error: total moved elements != log size");
        }
      }

      void flush_to_boa_impl() {
        sort_vector(m_lazy_insertion_log.begin(), m_lazy_insertion_log.end());
        auto new_space = compute_space_for_lazy_insertion_log();
        auto moved_from_log = make_space_for_new_inserts(new_space);
        flush_from_log(moved_from_log);
        std::cout << "Flushed lazy insertion log size: " << m_lazy_insertion_log.size() << std::endl;
        m_lazy_insertion_log.clear();
        compact_tiers();
        update_routing_filters();
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

      size_t routing_filter_entries_for_level(uint16_t tier_level = 0)
      {
        size_t run_size = m_in_memory_table_max_size * pow(RunsPerTier, tier_level + ROUTING_FILTER_MULT);
        return run_size;
      }

      //! initialize the lsm tree
      void init()
      {
        m_routing_filter.reset(new routing_filter<HashType, 4, PageSize, BlockSize>());
        m_routing_filter->add_tier(routing_filter_entries_for_level(0));
        m_stats.tier_to_collisions[0] = 0;

        tier t0;
        m_tiers.push_back(std::move(t0));
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
        // find hashes range [min, max]
        std::sort(m_in_memory_table.begin(), m_in_memory_table.end(), [](cache_element const& a, cache_element const& b)
        {
          HashCompare cmp{};
          return cmp(a.first, b.first);
        });


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
        r.m_elements_number = 0;
        // r.m_run = std::unique_ptr<external_vector>(new external_vector(r.m_run_size));
        if (m_run.size() < get_vector_size_for_n_tiers(0))
        {
          m_run.resize(get_vector_size_for_n_tiers(0));
        }
        if (m_elements.size() < get_vector_size_for_n_tiers(0))
        {
          m_elements.resize(get_vector_size_for_n_tiers(0));
        }
        auto offset = get_run_offset(0, first_inactive_index);
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

          auto prev_route = m_routing_filter->get_run_index(elem.m_hash, 0);
          if (prev_route.first > first_inactive_index)
          {
            prev_route.first = 0;
            prev_route.second = 0;
          }
          if (prev_route.first > -1)
          {
            m_stats.tier_to_collisions[0] += 1;
          }

          elem.m_prev_run = prev_route.first;
          elem.m_prev_index_in_run = prev_route.second;
          auto index_in_array = index;
          m_routing_filter->insert(first_inactive_index, index_in_array, 0, elem.m_hash);

          m_run[offset + index++] = elem;
          m_elements[elem.m_data_vector_index] = data_elem;

          ++it;
        } // while (it != m_in_memory_table.end()).
        // m_tiers[0].m_runs.push_back(std::move(r));
        m_tiers[0].m_empty_filter = false;
        compact_tiers();
      }

      int active_runs(tier const& t)
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

    public:
      int active_runs(unsigned int tier)
      {
        return active_runs(m_tiers[tier]);
      }

    private:
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
              m_routing_filter->add_tier(routing_filter_entries_for_level(new_tier_no));
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

            merge_runs(m_tiers[tier_no], m_tiers[new_tier_no]);
            ++m_merges_occurred;
            for (auto& run : m_tiers[tier_no].m_runs)
            {
              run.active = false;
            }
            m_routing_filter->reset(tier_no);
            m_tiers[tier_no].m_empty_filter = true;
            m_stats.tier_to_collisions[tier_no] = 0;
          }
        } // for (auto & tier : m_tiers).
      }

      //! merge runs into next tier
      void merge_runs(tier const& source_tier, tier& dest_tier)
      {
        size_t total_elements{0};

        for (auto const& source : source_tier.m_runs)
        {
          total_elements += source.m_elements_number;
        }

        // find buckets info
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
        final.m_elements_number = 0;
        // final.m_run = std::unique_ptr<external_vector>(new external_vector(final.m_run_size));
        if (m_run.size() < get_vector_size_for_n_tiers(dest_tier.m_level))
        {
          m_run.resize(get_vector_size_for_n_tiers(dest_tier.m_level));
        }
        if (m_elements.size() < get_vector_size_for_n_tiers(dest_tier.m_level))
        {
          m_elements.resize(get_vector_size_for_n_tiers(dest_tier.m_level));
        }

        auto offset = get_run_offset(dest_tier.m_level, first_inactive_index);
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
        heap.reserve(RunsPerTier);

        // add heads in heap
        for (int i = 0; i < RunsPerTier; ++i)
        {
          HeapItem item;
          auto end = get_run_offset(source_tier.m_level, i) + source_tier.m_runs[i].m_run_size;

          auto const& v = m_run;
          for (int j = get_run_offset(source_tier.m_level, i); j < end; ++j)
          {
            item.elem = v[j];
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
        auto const& v = m_run;

        // loop rest items
        while (!heap.empty())
        {
          if (index >= final.m_run_size)
          {
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

          auto prev_route = m_routing_filter->get_run_index(min_key.m_hash, dest_tier.m_level);
          if (prev_route.first > first_inactive_index)
          {
            prev_route.first = 0;
            prev_route.second = 0;
          }
          if (prev_route.first > -1)
          {
            m_stats.tier_to_collisions[dest_tier.m_level] += 1;
          }
          min_key.m_prev_run = prev_route.first;
          min_key.m_prev_index_in_run = prev_route.second;
          auto index_in_array = total_added;
          m_routing_filter->insert(first_inactive_index, index_in_array, dest_tier.m_level, min_key.m_hash);

          m_run[offset + index++] = min_key;
          ++(final.m_elements_number);

          ++total_added;
          HeapItem item;
          bool _added{false};
          auto end = get_run_offset(source_tier.m_level, min_item.src) + source_tier.m_runs[min_item.src].m_run_size;
          for (int j = min_item.index + 1; j < end; ++j)
          {
            item.elem = v[j];
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

        m_tiers[dest_tier.m_level].m_empty_filter = false;

        if (index != final.m_run_size)
        {
          exit(0);
        }
      }

      void update_routing_filters() {
        for (auto& t : m_tiers) {
          if (t.m_dirty_routing_filter) {
            m_routing_filter->reset(t.m_level);
            update_routing_filter(t);
            t.m_dirty_routing_filter = false;
          }
        }
      }

      void update_routing_filter(tier& t)
      {
        for (int r=0 ; r<RunsPerTier ; ++r) {
          auto const& run = t.m_runs[r];
          if (!run.active) {
            continue;
          }

          auto offset = get_run_offset(t.m_level, r);

          for (int i=0 ; i<run.m_run_size ; ++i) {
            auto elem = m_run[offset + i];

            auto prev_route = m_routing_filter->get_run_index(elem.m_hash, t.m_level);
            if (prev_route.first > r)
            {
              prev_route.first = 0;
              prev_route.second = 0;
            }
            elem.m_prev_run = prev_route.first;
            elem.m_prev_index_in_run = prev_route.second;
            m_routing_filter->insert(r, i, t.m_level, elem.m_hash);

            m_run[offset + i] = elem;
          }

        } // for (int r=0 ; r<RunsPerTier ; ++r).

      }
    };
  } // namespace boa.

STXXL_END_NAMESPACE

#endif // BOA_NO_BUCKETS_H
