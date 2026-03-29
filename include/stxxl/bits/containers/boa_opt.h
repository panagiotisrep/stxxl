//
// Created by panos on 6/23/25.
//

#ifndef BOA_NO_BUCKETS_H
#define BOA_H

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
        unsigned int m_buckets_no{}; // number of buckets in run
        double m_bucket_interval{}; // offset between buckets w.r.t. hash
        size_t m_elements_number{}; // actual elements, not counting padding
        HashType m_min_hash;
        HashType m_max_hash;
        unsigned int m_run_size{};
        unsigned int m_bucket_size{}; // elements per bucket
        bool active{false};
        size_t m_size{0};
      };

      external_vector m_run;
      external_data_vector m_elements;
      std::unique_ptr<routing_filter<HashType, Pages, PageSize, BlockSize>> m_routing_filter;

      struct tier
      {
        // external_vector m_run;
        std::vector<run> m_runs;
        uint16_t m_level{0};

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

      size_t routing_filter_entries_for_level(uint16_t tier_level = 0)
      {
        size_t run_size = m_in_memory_table_max_size * pow(RunsPerTier, tier_level + ROUTING_FILTER_MULT);
        return run_size;
      }

      //! initialize the lsm tree
      void init()
      {
        m_routing_filter.reset(new routing_filter<HashType, RunsPerTier, PageSize, BlockSize>());
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
        if (m_run.size() < get_vector_size_for_n_tiers(0))
        {
          m_run.resize(get_vector_size_for_n_tiers(0));
        }
        if (m_elements.size() < get_vector_size_for_n_tiers(0))
        {
          m_elements.resize(get_vector_size_for_n_tiers(0));
        }
        auto offset = get_run_offset(0, first_inactive_index);
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

          ++r.m_size;
          ++it;
        } // while (it != m_in_memory_table.end()).
        // m_tiers[0].m_runs.push_back(std::move(r));
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
            m_stats.tier_to_collisions[tier_no] = 0;
          }
        } // for (auto & tier : m_tiers).
      }

      //! merge runs into next tier
      void merge_runs(tier const& source_tier, tier& dest_tier)
      {
        size_t total_elements{0};
        HashType min = std::numeric_limits<HashType>::max();
        HashType max = std::numeric_limits<HashType>::min();
        for (auto const& source : source_tier.m_runs)
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

        if (index != final.m_run_size)
        {
          exit(0);
        }
      }
    };
  } // namespace boa.

STXXL_END_NAMESPACE

#endif // BOA_NO_BUCKETS_H
