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

STXXL_BEGIN_NAMESPACE
  namespace boa
  {
    typedef char run_index;
    typedef unsigned int index_in_run;

    template <class HashType>
    struct routing_filter
    {
      typedef VECTOR_GENERATOR<std::pair<run_index, index_in_run>, 4, 8, 1 * 1024 * 1024, stxxl::RC,
                               stxxl::lru>::result
      routing_external_vector;

      unsigned int prefix_bits_length_;
      std::unique_ptr<routing_external_vector> m_filter;

      explicit routing_filter(const unsigned int prefix_bits_length) : prefix_bits_length_(prefix_bits_length)
      {
        m_filter = std::unique_ptr<routing_external_vector>(new routing_external_vector());
        m_filter->resize(static_cast<size_t>(std::pow(2, prefix_bits_length)));
        std::fill(m_filter->begin(), m_filter->end(), std::pair<run_index, unsigned int>(-1, 0));
      }

      size_t get_bits(HashType const& x, const unsigned group_index, unsigned group_size) noexcept
      {
        typedef typename std::make_unsigned<HashType>::type U;
        U mask = (U(1) << group_size) - U(1);
        return (U(x) >> (group_index * group_size)) & mask;
      }

      size_t get_bits(HashType const& x, const unsigned bits_length) noexcept
      {
        using U = typename std::make_unsigned<HashType>::type;
        U mask = (U(1) << bits_length) - U(1);
        return static_cast<size_t>(U(x) & mask);
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
        return (*m_filter)[index];
      }

      void insert(run_index r_index, index_in_run index_in_r, HashType const& hash)
      {
        auto index = get_index_from_hash(hash);
        (*m_filter)[index] = std::pair<run_index, index_in_run>(r_index, index_in_r);
      }

      void flush_to_external_memory() const
      {
        this->m_filter->flush();
      }
    };


    template <class KeyType, class DataType, class HashType, HashType (*HashFunction)(KeyType const&), class
              HashCompare>
    class boa : private noncopyable
    {
    public:
      typedef std::pair<KeyType, DataType> element_type;

    private:
      //! In-memory cache
      std::map<HashType, element_type, HashCompare> m_in_memory_table;
      unsigned int m_in_memory_table_max_size;
      unsigned int m_runs_per_tier;

      struct run_element
      {
        KeyType m_key;
        DataType m_value;
        run_index m_prev_run = -1;
        index_in_run m_prev_index_in_run = 0;
        HashType m_hash;
      };

      typedef typename VECTOR_GENERATOR<run_element, 4, 8, 1 * 1024 * 1024, stxxl::RC, stxxl::lru>::result
      external_vector;

      struct run
      {
        std::unique_ptr<external_vector> m_run;
        unsigned int m_buckets_no{}; // number of buckets in run
        double m_bucket_interval{}; // offset between buckets w.r.t. hash
        size_t m_elements_number{}; // actual elements, not counting padding
        HashType m_min_hash;
        HashType m_max_hash;
        unsigned int m_run_size{};
        unsigned int m_bucket_size{}; // elements per bucket
        bool active{false};
      };

      struct tier
      {
        std::vector<run> m_runs;
        std::unique_ptr<routing_filter<HashType>> m_routing_filter;
      };

      //! The external memory runs
      std::vector<tier> m_tiers;

    public:
      boa(unsigned int buffer_size, unsigned int runs_per_tier) :
        m_in_memory_table_max_size(buffer_size), m_runs_per_tier(runs_per_tier) { init(); }

      //! Insert a key-value pair in the lsm tree
      void insert(const element_type& value)
      {
        HashType hash;
        hash = HashFunction(value.first);
        m_in_memory_table[hash] = value;

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
            total += run.m_run->size();
          }
        }
        return total;
      }

      //! find key-value corresponding to search key
      std::unique_ptr<element_type> find(const KeyType& k)
      {
        HashType hash;
        hash = HashFunction(k);

        // check in-memory
        if (m_in_memory_table.find(hash) != m_in_memory_table.end())
        {
          return std::unique_ptr<element_type>(
            new element_type(m_in_memory_table[hash].first, m_in_memory_table[hash].second));
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

        for (auto const& tier : m_tiers)
        {
          if (tier.m_runs.empty())
          {
            continue;
          }

          std::set<run_index> visited_runs;
          auto run_to_search = tier.m_routing_filter->get_run_index(hash);

          while (run_to_search.first != -1)
          {
            visited_runs.insert(run_to_search.first);

            auto const& r = tier.m_runs[run_to_search.first];

            auto bucket_no = bucket_index(hash, r);
            std::size_t start = bucket_no * r.m_bucket_size;
            std::size_t end = start + r.m_bucket_size;

            auto it = r.m_run->begin() + start;
            while (it < r.m_run->end())
            {
              if (it->m_hash != hash)
              {
                ++it;
                if (it == r.m_run->begin() + end)
                {
                  break;
                }
              }
              else
              {
                return std::unique_ptr<element_type>(new element_type(it->m_key, it->m_value));
              }
            } // while (it < r.m_run->end()).


            auto prev = (*tier.m_runs[run_to_search.first].m_run)[run_to_search.second];
            if (prev.m_prev_run != -1 && visited_runs.count(prev.m_prev_run) == 0)
            {
              visited_runs.insert(prev.m_prev_run);
              run_to_search = {prev.m_prev_run, prev.m_prev_index_in_run};
            }
            else
            {
              run_to_search.first = -1;
            }
          } // while (run_to_search.first != -1).
        } // for (auto const& tier : m_tiers).

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
      routing_filter<HashType>* create_routing_filter()
      {
        return new routing_filter<HashType>(22);
      }

      //! initialize the lsm tree
      void init()
      {
        tier t0;
        t0.m_routing_filter.reset(create_routing_filter());
        m_tiers.push_back(std::move(t0));
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
          m_tiers[0].m_routing_filter.reset(create_routing_filter());
        }

        // find hashes range [min, max]
        HashType min = std::numeric_limits<HashType>::max();
        HashType max = std::numeric_limits<HashType>::min();
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
        run r;
        r.m_run_size = max_bucket_size * intervals_no;
        r.m_bucket_size = max_bucket_size;
        r.m_max_hash = max;
        r.m_min_hash = min;
        r.m_bucket_interval = interval_size;
        r.m_buckets_no = intervals_no;
        r.m_elements_number = 0;
        r.m_run = std::unique_ptr<external_vector>(new external_vector());
        r.m_run->resize(r.m_run_size);

        int at_bucket{0};
        unsigned int added{0};

        run_element empty_element;
        empty_element.m_hash = 0;

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
            if (prev_route.first == m_tiers[0].m_runs.size())
            {
              index_in_array = prev_route.second;
            }
            m_tiers[0].m_routing_filter->insert(m_tiers[0].m_runs.size(), index_in_array, elem.m_hash);

            (*r.m_run)[index++] = elem;
          } //if (bucket_index(it->first) == at_bucket).
          else
          {
            (*r.m_run)[index++] = empty_element; // fill bucket
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

        m_tiers[0].m_runs.push_back(std::move(r));
        compact_tiers();
      }

      //! merges runs of tiers if needed
      void compact_tiers()
      {
        for (int tier_no = 0; tier_no < m_tiers.size(); ++tier_no)
        {
          if (m_tiers[tier_no].m_runs.size() >= m_runs_per_tier)
          {
            int new_tier_no = tier_no + 1;
            if (m_tiers.size() <= new_tier_no)
            {
              m_tiers.push_back(tier());
            }

            merge_runs(m_tiers[tier_no].m_runs, m_tiers[new_tier_no]);
            m_tiers[tier_no].m_runs.clear();
            m_tiers[tier_no].m_routing_filter.reset();
          }
        } // for (auto & tier : m_tiers).
      }

      //! merge runs into next tier
      void merge_runs(std::vector<run> const& sources, tier& dest_tier)
      {
        using vec_it = typename external_vector::const_iterator;

        if (!dest_tier.m_routing_filter)
        {
          dest_tier.m_routing_filter.reset(create_routing_filter());
        }
        run final;

        std::vector<std::pair<vec_it, vec_it>> iters;
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

          iters.emplace_back(source.m_run->begin(), source.m_run->end());
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

        std::vector<unsigned int> buckets_size(intervals_no, 0);
        auto iters_temp = iters;
        run_element min_key;
        while (!iters_temp.empty())
        {
          min_key.m_hash = std::numeric_limits<KeyType>::max();
          bool add{false};
          auto iter_pair = iters_temp.begin();
          auto* vec_it_to_move = &(iter_pair->first);
          while (iter_pair != iters_temp.end())
          {
            if (iter_pair->first == iter_pair->second)
            {
              iter_pair = iters_temp.erase(iter_pair);
              continue;
            }
            if (iter_pair->first->m_hash == 0) // empty element
            {
              ++(iter_pair->first);
              continue;
            }
            if (iter_pair->first->m_hash < min_key.m_hash)
            {
              min_key.m_hash = iter_pair->first->m_hash;
              add = true;
              vec_it_to_move = &(iter_pair->first);
            }
            ++iter_pair;
          }
          if (add)
          {
            auto a = bucket_index(min_key.m_hash);
            ++buckets_size[bucket_index(min_key.m_hash)];
          }
          ++(*vec_it_to_move);
        }

        const auto max_bucket_size = *std::max_element(buckets_size.begin(), buckets_size.end());

        final.m_run_size = max_bucket_size * intervals_no;
        final.m_bucket_size = max_bucket_size;
        final.m_max_hash = max;
        final.m_min_hash = min;
        final.m_bucket_interval = interval_size;
        final.m_buckets_no = intervals_no;
        final.m_elements_number = 0;
        final.m_run = std::unique_ptr<external_vector>(new external_vector());
        final.m_run->resize(final.m_run_size);

        unsigned int at_bucket{0};
        run_element empty_element;
        empty_element.m_hash = 0;
        unsigned int added{0};
        size_t total_added{0};
        size_t index{0};

        while (!iters.empty())
        {
          min_key.m_hash = std::numeric_limits<KeyType>::max();
          bool add{false};
          auto iter_pair = iters.begin();
          auto* vec_it_to_move = &(iter_pair->first);
          while (iter_pair != iters.end())
          {
            if (iter_pair->first == iter_pair->second)
            {
              iter_pair = iters.erase(iter_pair);
              continue;
            }
            if (iter_pair->first->m_hash == 0)
            {
              ++(iter_pair->first);
              continue;
            }
            if (iter_pair->first->m_hash < min_key.m_hash)
            {
              min_key = *(iter_pair->first);
              add = true;
              vec_it_to_move = &(iter_pair->first);
            }
            ++iter_pair;
          }
          if (add)
          {
            while (bucket_index(min_key.m_hash) != at_bucket)
            {
              (*final.m_run)[index++] = empty_element; // fill bucket
              ++added;
              ++total_added;
              if (added == max_bucket_size)
              {
                ++at_bucket;
                added = 0;
              }
            }

            auto prev_route = dest_tier.m_routing_filter->get_run_index(min_key.m_hash);
            min_key.m_prev_run = prev_route.first;
            min_key.m_prev_index_in_run = prev_route.second;
            auto index_in_array = total_added;
            if (prev_route.first == dest_tier.m_runs.size())
            {
              index_in_array = prev_route.second;
            }
            dest_tier.m_routing_filter->insert(dest_tier.m_runs.size(), index_in_array, min_key.m_hash);

            (*final.m_run)[index++] = min_key;
            ++(final.m_elements_number);
            ++(*vec_it_to_move);

            ++total_added;
            ++added;
            if (added == max_bucket_size)
            {
              ++at_bucket;
              added = 0;
            }
          }
        }

        dest_tier.m_runs.push_back(std::move(final));
      }
    };
  } // namespace boa.

STXXL_END_NAMESPACE

#endif // BOA_H
