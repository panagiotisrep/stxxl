//
// Created by panos on 6/23/25.
//

#ifndef BOA_H
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

STXXL_BEGIN_NAMESPACE
  namespace boa
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
      // , 4, 8, 1 * 1024 * 1024, stxxl::RC, stxxl::lru
      // typedef typename VECTOR_GENERATOR<
      // routing_element,
      // 8, // larger page subdivision
      // 16, // modest pager (important!)
      // 2 * 1024 * 1024, // larger blocks (2MB)
      // stxxl::RC,
      // stxxl::lru
      // >::result routing_external_vector;

      unsigned int prefix_bits_length_;
      std::unique_ptr<routing_external_vector> m_filter;

      // explicit routing_filter(const unsigned int prefix_bits_length) : prefix_bits_length_(prefix_bits_length)
      // {
      //   m_filter = std::unique_ptr<routing_external_vector>(new routing_external_vector(static_cast<size_t>(std::pow(2, prefix_bits_length))));
      //   // m_filter->resize(static_cast<size_t>(std::pow(2, prefix_bits_length)));
      //   // std::fill(m_filter->begin(), m_filter->end(), std::pair<run_index, unsigned int>(-1, 0));
      // }

      explicit routing_filter(const unsigned int entries)
      {
        m_filter = std::unique_ptr<routing_external_vector>(new routing_external_vector(entries));
        prefix_bits_length_ = std::floor(std::log2(entries));
      }

      // routing_filter(unsigned int char_bits_length, unsigned int number_of_brackets)
      // {
      //   prefix_bits_length_ = std::ceil(std::log2(char_bits_length * number_of_brackets));
      //   m_filter = std::unique_ptr<routing_external_vector>(new routing_external_vector(char_bits_length * number_of_brackets));
      //   // m_filter->resize(static_cast<size_t>(std::pow(2, prefix_bits_length)));
      //   // std::fill(m_filter->begin(), m_filter->end(), std::pair<run_index, unsigned int>(-1, 0));
      // }

      void reset()
      {
        routing_element element;
        element.run = 0;
        element.index = 0;
        std::fill(m_filter->begin(), m_filter->end(), element);
        // TODO need this?
      }

      // size_t get_bits(HashType const& x, const unsigned group_index, unsigned group_size) noexcept
      // {
      //   typedef typename std::make_unsigned<HashType>::type U;
      //   U mask = (U(1) << group_size) - U(1);
      //   return (U(x) >> (group_index * group_size)) & mask;
      // }

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
        // return get_bits(h1, 0, m_char_bits_length * m_chars_no) == get_bits(h2, 0, m_char_bits_length * m_chars_no);
        return get_bits(h1, prefix_bits_length_) == get_bits(h2, prefix_bits_length_);
      }

      size_t get_index_from_hash(HashType const& hash)
      {
        return get_bits(hash, prefix_bits_length_);
      }

      std::pair<run_index, index_in_run> get_run_index(HashType const& hash)
      {
        auto index = get_index_from_hash(hash);
        routing_external_vector const& v = *m_filter;
        routing_element ret = v[index];
        ret.run -= 1;
        return {ret.run, ret.index};
      }

      void insert(run_index r_index, index_in_run index_in_r, HashType const& hash)
      {
        auto index = get_index_from_hash(hash);
        routing_element element;
        element.run = r_index + 1;
        element.index = index_in_r;
        (*m_filter)[index] = element;
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
        size_t internal_memory = (m_tiers.size() * RunsPerTier + 1) * PageSize * Pages * BlockSize;
        return internal_memory;
      }

      size_t get_tiers_no()
      {
        return m_tiers.size();
      }

    private:
      //! In-memory cache
      typedef std::pair<HashType, element_type> cache_element;
      // std::map<HashType, element_type, HashCompare> m_in_memory_table;
      std::vector<cache_element> m_in_memory_table;

      unsigned int m_in_memory_table_max_size;

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

      struct run_element
      {
        KeyType m_key;
        DataType m_value;
        run_index m_prev_run = -1;
        index_in_run m_prev_index_in_run = 0;
        HashType m_hash;
      };

      typedef typename VECTOR_GENERATOR<run_element, PageSize, Pages, BlockSize, stxxl::RC, stxxl::lru>::result
      external_vector;

      // typedef typename VECTOR_GENERATOR<
      // run_element,
      // 8, // larger page subdivision
      // 16, // modest pager (important!)
      // 2 * 1024 * 1024, // larger blocks (2MB)
      // stxxl::RC,
      // stxxl::lru
      // >::result external_vector;

      struct run
      {
        external_vector m_run;
        unsigned int m_buckets_no{}; // number of buckets in run
        double m_bucket_interval{}; // offset between buckets w.r.t. hash
        size_t m_elements_number{}; // actual elements, not counting padding
        HashType m_min_hash;
        HashType m_max_hash;
        unsigned int m_run_size{};
        unsigned int m_bucket_size{}; // elements per bucket
        bool active{false};
        size_t m_size{0};

        // ~run()
        // {
        //   delete m_run;
        // }
      };

      struct tier
      {
        std::vector<run> m_runs;
        std::unique_ptr<routing_filter<HashType, Pages, PageSize, BlockSize>> m_routing_filter;
        uint16_t m_level;
      };

      //! The external memory runs
      std::vector<tier> m_tiers;

    public:
      boa(unsigned int buffer_size) :
        m_in_memory_table_max_size(buffer_size) { init(); }

      //! Insert a key-value pair in the lsm tree
      void insert(const element_type& value)
      {
        HashType hash;
        hash = HashFunction(value.first);
        // m_in_memory_table[hash] = value;
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
          // TODO maybe this helps speed?
          // if (active_runs(tier) == 0)
          // {
          //   continue;
          // }

          small_vec<int, RunsPerTier> visited_runs;
          auto run_to_search = tier.m_routing_filter->get_run_index(hash);
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

              ++stats.visited_runs;
              auto bucket_no = bucket_index(hash, r);
              std::size_t start = bucket_no * r.m_bucket_size;
              std::size_t end = start + r.m_bucket_size;

              std::size_t stop = r.m_run.size() < end ? r.m_run.size() : end;

              // external_vector const & v = r.m_run;
              // auto prev = v[run_to_search.second];
              // if (prev.m_hash == hash)
              // {
              // return std::unique_ptr<element_type>(new element_type(prev.m_key, prev.m_value));
              // }

              external_vector const* v = &(tier.m_runs[run_to_search.first].m_run);
              auto prev_run = run_to_search.first;
              while (run_to_search.first != -1)
              {
                ++stats.visited_elements;

                if (prev_run != run_to_search.first)
                {
                  prev_run = run_to_search.first;
                  v = &(tier.m_runs[run_to_search.first].m_run);
                }

                auto prev = (*v)[run_to_search.second];
                if (prev.m_hash == hash)
                {
                  return std::unique_ptr<element_type>(new element_type(prev.m_key, prev.m_value));
                }
                if (!visited_runs.contains(prev.m_prev_run))
                {
                  visited_runs.push_unique(prev.m_prev_run);
                }
                run_to_search = {prev.m_prev_run, prev.m_prev_index_in_run};
              }

              // for (int at = start; at < stop; at++)
              // {
              //   ++stats.visited_elements;
              //   stats.elements_per_tier[at_tier] += 1;
              //   run_element e = v[at];
              //   auto h = e.m_hash;
              //   if (h == 0)
              //   {
              //     ++stats.empty_elements;
              //   }
              //   if (h > hash)
              //   {
              //     break;
              //   }
              //   else if (h == hash)
              //   {
              //     return std::unique_ptr<element_type>(new element_type(e.m_key, e.m_value));
              //   }
              // } // for (int at = start; at < stop; at++).
              //
              // // auto prev = v[run_to_search.second];
              // if (prev.m_prev_run != -1 && !visited_runs.contains(prev.m_prev_run))
              // {
              //   visited_runs.push_unique(prev.m_prev_run);
              //   run_to_search = {prev.m_prev_run, prev.m_prev_index_in_run};
              // }
              // else
              // {
              //   run_to_search.first = -1;
              // }
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

    private:
      routing_filter<HashType, Pages, PageSize, BlockSize>* create_routing_filter(
        uint32_t capacity, uint16_t tier_level = 0)
      {
        auto run_size = m_in_memory_table_max_size * pow(RunsPerTier, tier_level);
        // auto hl = std::log2(run_size) / std::log2(RunsPerTier);
        // auto l_hl = p
        return new routing_filter<HashType, Pages, PageSize, BlockSize>(run_size);
        // std::pow(RunsPerTier, ));
        // std::ceil(std::log2(RunsPerTier)),
        // run_size * pow(RunsPerTier, tier_level) + tier_level
        // ); //22
      }

      //! initialize the lsm tree
      void init()
      {
        tier t0;
        t0.m_routing_filter.reset(create_routing_filter(m_in_memory_table_max_size * RunsPerTier));
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
        if (!m_tiers[0].m_routing_filter)
        {
          m_tiers[0].m_routing_filter.reset(create_routing_filter(m_in_memory_table_max_size * RunsPerTier));
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

        // find buckets info
        const unsigned int intervals_no = get_buckets_no(m_in_memory_table.size());
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

        std::vector<unsigned int> buckets_size(intervals_no, 0);
        for (auto const& pair : m_in_memory_table)
        {
          ++buckets_size[bucket_index(pair.first)];
        }

        const auto max_bucket_size = *std::max_element(buckets_size.begin(), buckets_size.end());

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
        r.m_run_size = max_bucket_size * intervals_no;
        r.m_bucket_size = max_bucket_size;
        r.m_max_hash = max;
        r.m_min_hash = min;
        r.m_bucket_interval = interval_size;
        r.m_buckets_no = intervals_no;
        r.m_elements_number = 0;
        // r.m_run = std::unique_ptr<external_vector>(new external_vector(r.m_run_size));
        r.m_run.resize(r.m_run_size, true);
        std::fill(r.m_run.begin(), r.m_run.end(), empty_element);
        r.m_size = 0;

        int at_bucket{0};
        unsigned int added{0};

        auto it = m_in_memory_table.begin();
        size_t total_added{0};
        size_t index{0};

        while (it != m_in_memory_table.end())
        {
          bool inserted_new_element{false};
          if (bucket_index(it->first) == at_bucket)
          {
            inserted_new_element = true;
            ++(r.m_elements_number);

            run_element elem;
            elem.m_hash = it->first;
            elem.m_key = it->second.first;
            elem.m_value = it->second.second;

            auto prev_route = m_tiers[0].m_routing_filter->get_run_index(elem.m_hash);
            elem.m_prev_run = prev_route.first;
            elem.m_prev_index_in_run = prev_route.second;
            auto index_in_array = total_added;
            // if (prev_route.first == first_inactive_index)
            // {
            // index_in_array = prev_route.second;
            // }
            m_tiers[0].m_routing_filter->insert(first_inactive_index, index_in_array, elem.m_hash);

            (r.m_run)[index++] = elem;
            ++r.m_size;
          } //if (bucket_index(it->first) == at_bucket).
          else
          {
            (r.m_run)[index++] = empty_element; // fill bucket
            // index++;
            ++r.m_size;
          }

          ++added;
          ++total_added;
          if (added == max_bucket_size)
          {
            ++at_bucket;
            added = 0;
          }

          if (inserted_new_element)
          {
            ++it;
          }
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
            }

            if (m_tiers[new_tier_no].m_runs.size() < RunsPerTier)
            {
              run r;
              m_tiers[new_tier_no].m_runs.push_back(std::move(r));
            }

            merge_runs(m_tiers[tier_no].m_runs, m_tiers[new_tier_no]);
            for (auto& run : m_tiers[tier_no].m_runs)
            {
              run.active = false;
            }
            m_tiers[tier_no].m_routing_filter->reset();
          }
        } // for (auto & tier : m_tiers).
      }

      //! merge runs into next tier
      // void merge_runs(std::vector<run> const& sources, tier& dest_tier)
      // {
      //   using vec_it = typename external_vector::const_iterator;
      //
      //   std::vector<std::pair<vec_it, vec_it>> iters;
      //   size_t total_elements{0};
      //   HashType min = std::numeric_limits<HashType>::max();
      //   HashType max = std::numeric_limits<HashType>::min();
      //   for (auto& source : sources)
      //   {
      //     if (min > source.m_min_hash)
      //     {
      //       min = source.m_min_hash;
      //     }
      //     if (max < source.m_max_hash)
      //     {
      //       max = source.m_max_hash;
      //     }
      //
      //     iters.emplace_back(source.m_run.begin(), source.m_run.end());
      //     total_elements += source.m_elements_number;
      //   }
      //
      //   if (!dest_tier.m_routing_filter)
      //   {
      //     // std::cout << "Creating new filter size " << 4 + std::log2(total_elements) << std::endl;
      //     dest_tier.m_routing_filter.reset(create_routing_filter(total_elements));
      //   }
      //
      //   // find buckets info
      //   const unsigned int intervals_no = get_buckets_no(total_elements);
      //   const double interval_size = static_cast<double>(max - min) / static_cast<double>(intervals_no);
      //   auto bucket_index = [&interval_size, &min, &intervals_no](HashType const& h)
      //   {
      //     auto index = static_cast<int>(static_cast<double>((h - min)) / interval_size);
      //     // ensure max value falls in the last interval
      //     if (index >= intervals_no)
      //     {
      //       index = intervals_no - 1;
      //     }
      //     return index;
      //   };
      //
      //   std::vector<unsigned int> buckets_size(intervals_no, 0);
      //   auto iters_temp = iters;
      //   run_element min_key;
      //   while (!iters_temp.empty())
      //   {
      //     min_key.m_hash = std::numeric_limits<KeyType>::max();
      //     bool add{false};
      //     auto iter_pair = iters_temp.begin();
      //     auto* vec_it_to_move = &(iter_pair->first);
      //     while (iter_pair != iters_temp.end())
      //     {
      //       if (iter_pair->first == iter_pair->second)
      //       {
      //         iter_pair = iters_temp.erase(iter_pair);
      //         continue;
      //       }
      //       if (iter_pair->first->m_hash == 0) // empty element
      //       {
      //         ++(iter_pair->first);
      //         continue;
      //       }
      //       if (iter_pair->first->m_hash < min_key.m_hash)
      //       {
      //         min_key.m_hash = iter_pair->first->m_hash;
      //         add = true;
      //         vec_it_to_move = &(iter_pair->first);
      //       }
      //       ++iter_pair;
      //     }
      //     if (add)
      //     {
      //       auto a = bucket_index(min_key.m_hash);
      //       ++buckets_size[bucket_index(min_key.m_hash)];
      //     }
      //     ++(*vec_it_to_move);
      //   }
      //
      //   const auto max_bucket_size = *std::max_element(buckets_size.begin(), buckets_size.end());
      //
      //   int first_inactive_index = 0;
      //   for (auto const & run : dest_tier.m_runs)
      //   {
      //     if (!run.active)
      //     {
      //       break;
      //     }
      //     ++first_inactive_index;
      //   }
      //   run_element empty_element;
      //   empty_element.m_hash = 0;
      //
      //   run & final = dest_tier.m_runs[first_inactive_index];
      //   final.active = true;
      //   final.m_run_size = max_bucket_size * intervals_no;
      //   final.m_bucket_size = max_bucket_size;
      //   final.m_max_hash = max;
      //   final.m_min_hash = min;
      //   final.m_bucket_interval = interval_size;
      //   final.m_buckets_no = intervals_no;
      //   final.m_elements_number = 0;
      //   // final.m_run = std::unique_ptr<external_vector>(new external_vector(final.m_run_size));
      //   final.m_run.resize(final.m_run_size, true);
      //   std::fill( final.m_run.begin(),  final.m_run.end(), empty_element);
      //
      //   unsigned int at_bucket{0};
      //   unsigned int added{0};
      //   size_t total_added{0};
      //   size_t index{0};
      //
      //   while (!iters.empty())
      //   {
      //     min_key.m_hash = std::numeric_limits<KeyType>::max();
      //     bool add{false};
      //     auto iter_pair = iters.begin();
      //     auto* vec_it_to_move = &(iter_pair->first);
      //     while (iter_pair != iters.end())
      //     {
      //       if (iter_pair->first == iter_pair->second)
      //       {
      //         iter_pair = iters.erase(iter_pair);
      //         continue;
      //       }
      //       if (iter_pair->first->m_hash == 0)
      //       {
      //         ++(iter_pair->first);
      //         continue;
      //       }
      //       if (iter_pair->first->m_hash < min_key.m_hash)
      //       {
      //         min_key = *(iter_pair->first);
      //         add = true;
      //         vec_it_to_move = &(iter_pair->first);
      //       }
      //       ++iter_pair;
      //     }
      //     if (add)
      //     {
      //       while (bucket_index(min_key.m_hash) != at_bucket)
      //       {
      //         (*final.m_run)[index++] = empty_element; // fill bucket
      //         ++added;
      //         ++total_added;
      //         if (added == max_bucket_size)
      //         {
      //           ++at_bucket;
      //           added = 0;
      //         }
      //       }
      //
      //       auto prev_route = dest_tier.m_routing_filter->get_run_index(min_key.m_hash);
      //       min_key.m_prev_run = prev_route.first;
      //       min_key.m_prev_index_in_run = prev_route.second;
      //       auto index_in_array = total_added;
      //       if (prev_route.first == first_inactive_index)
      //       {
      //         index_in_array = prev_route.second;
      //       }
      //       dest_tier.m_routing_filter->insert(first_inactive_index, index_in_array, min_key.m_hash);
      //
      //       (*final.m_run)[index++] = min_key;
      //       ++(final.m_elements_number);
      //       ++(*vec_it_to_move);
      //
      //       ++total_added;
      //       ++added;
      //       if (added == max_bucket_size)
      //       {
      //         ++at_bucket;
      //         added = 0;
      //       }
      //     }
      //   }
      //
      //   // dest_tier.m_runs.push_back(std::move(final));
      // }

      void merge_runs(std::vector<run> const& sources, tier& dest_tier)
      {
        // std::vector<std::pair<vec_it, vec_it>> iters;
        size_t total_elements{0};
        HashType min = std::numeric_limits<HashType>::max();
        HashType max = std::numeric_limits<HashType>::min();
        for (auto& source : sources)
        {
          if (min > source.m_min_hash)
          {
            min = source.m_min_hash;
          }
          if (max < source.m_max_hash)
          {
            max = source.m_max_hash;
          }

          // iters.emplace_back(source.m_run.begin(), source.m_run.end());
          total_elements += source.m_elements_number;
        }

        if (!dest_tier.m_routing_filter)
        {
          // std::cout << "Creating new filter size " << 4 + std::log2(total_elements) << std::endl;
          dest_tier.m_routing_filter.reset(create_routing_filter(total_elements, dest_tier.m_level));
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

        std::vector<unsigned int> buckets_size(intervals_no, 0);
        run_element min_key;

        for (auto& source : sources)
        {
          min_key.m_hash = std::numeric_limits<HashType>::max();
          external_vector const& v = source.m_run;

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

        const auto max_bucket_size = *std::max_element(buckets_size.begin(), buckets_size.end());

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
        final.m_run_size = max_bucket_size * intervals_no;
        final.m_bucket_size = max_bucket_size;
        final.m_max_hash = max;
        final.m_min_hash = min;
        final.m_bucket_interval = interval_size;
        final.m_buckets_no = intervals_no;
        final.m_elements_number = 0;
        // final.m_run = std::unique_ptr<external_vector>(new external_vector(final.m_run_size));
        final.m_run.resize(final.m_run_size, true);
        std::fill(final.m_run.begin(), final.m_run.end(), empty_element);

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

        // std::make_heap(heap.begin(), heap.end(), ByHash());

        // loop rest items
        while (!heap.empty())
        {
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

          while (bucket_index(min_key.m_hash) != at_bucket)
          {
            // (*final.m_run)[index++] = empty_element; // fill bucket
            index++;
            ++added;
            ++total_added;
            if (added == max_bucket_size)
            {
              ++at_bucket;
              added = 0;
            }
          }

          auto prev_route = dest_tier.m_routing_filter->get_run_index(min_key.m_hash);
          if (prev_route.first > first_inactive_index)
          {
            prev_route.first = 0;
            prev_route.second = 0;
          }
          min_key.m_prev_run = prev_route.first;
          min_key.m_prev_index_in_run = prev_route.second;
          auto index_in_array = total_added;
          // if (prev_route.first == first_inactive_index)
          // {
          // index_in_array = prev_route.second;
          // }
          dest_tier.m_routing_filter->insert(first_inactive_index, index_in_array, min_key.m_hash);

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
      }
    };
  } // namespace boa.

STXXL_END_NAMESPACE

#endif // BOA_H
