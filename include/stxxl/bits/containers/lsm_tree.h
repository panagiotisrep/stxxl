//
// Created by panos on 6/23/25.
//

#ifndef LSM_TREE_H
#define LSM_TREE_H

#include "stxxl/bits/stream/sort_stream.h"
#include "stxxl/bits/stream/stream.h"

#include <limits>
#include <map>
#include <stxxl/bits/containers/btree/iterator.h>
#include <stxxl/bits/containers/btree/iterator_map.h>
#include <stxxl/bits/namespace.h>
#include <stxxl/map>
#include <stxxl/types>
#include <stxxl/vector>

STXXL_BEGIN_NAMESPACE
  template <class KeyType, class DataType, class CompareType,
            unsigned RawNodeSize = 16 * 1024, // 16 KBytes default
            unsigned RawLeafSize = 128 * 1024, // 128 KBytes default
            class PDAllocStrategy = stxxl::SR>
  class lsm_tree : private noncopyable
  {
  public:
    typedef KeyType key_type;
    typedef DataType data_type;
    typedef CompareType key_compare;
    typedef std::pair<key_type, data_type> value_type;

    typedef map<KeyType, DataType, CompareType, RawNodeSize, RawLeafSize,
                PDAllocStrategy>
    map_type;

  private:
    //! In-memory cache
    std::map<key_type, data_type, key_compare> m_memtable;
    unsigned int m_memtable_max_size = 1000;
    unsigned int m_run_size = 1000;
    unsigned int m_runs_per_tier = 4;

#ifdef VECTOR_LSTREE
    typedef typename VECTOR_GENERATOR<value_type, 4, 8, 1 * 1024 * 1024, stxxl::RC, stxxl::lru>::result vector;
#endif

    struct run
    {
#ifdef VECTOR_LSTREE
      std::unique_ptr<vector> m_run;
#else
      std::unique_ptr<map_type> m_run;
#endif
      unsigned int m_run_size;
    };

    struct tier
    {
      std::vector<run> m_runs;
    };

    //! The external memory runs
    std::vector<tier> m_tiers;

    struct internal_stats
    {
      int reads;
      int inserts;

      internal_stats() : reads(0), inserts(0)
      {
      }
    };

    internal_stats m_stats;

  public:
    lsm_tree() { init(); }

    //! Insert a key-value pair in the lsm tree
    void insert(const value_type& value)
    {
      ++(m_stats.inserts);
      m_memtable[value.first] = value.second;

      if (m_memtable.size() >= m_memtable_max_size)
      {
        minor_flush();
        m_memtable.clear();
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
    std::unique_ptr<value_type> find(const key_type& k)
    {
      ++(m_stats.reads);

      for (auto const& tier : m_tiers)
      {
        for (auto const& run : tier.m_runs)
        {
#ifdef VECTOR_LSTREE
          value_type v;
          v.first = k;
          auto it = std::lower_bound(run.m_run->begin(), run.m_run->end(), v);
#else
          auto it = run.m_run->find(k);
#endif
          if (it != run.m_run->end())
          {
            return std::unique_ptr<value_type>(
              new value_type(it->first, it->second));
          }
        }
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

  private:
    //! init a run at given tier
    void init_run(run& r, unsigned int tier_no)
    {
      r.m_run_size = m_run_size * std::pow(m_runs_per_tier, tier_no);
#ifdef VECTOR_LSTREE
      r.m_run = std::unique_ptr<vector>(new vector());
      r.m_run->reserve(r.m_run_size);
#else
      r.m_run = std::unique_ptr<map_type>(
        new map_type((map_type::node_block_type::raw_size) * 3,
                     (map_type::leaf_block_type::raw_size) * 3));
#endif
    }

    //! initialize the lsm tree
    void init()
    {
      tier t0;
      // t0.m_runs_num = 1;
      m_tiers.push_back(std::move(t0));
    }

    //! Flush memtable to a run in
    void minor_flush()
    {
      run r;
      init_run(r, 0);
      // typename vector::bufwriter_type bw(r.m_run);
      for (auto const& pair : m_memtable)
      {
        // bw << pair.first;
#ifdef VECTOR_LSTREE
        r.m_run->push_back(pair);
#else
        r.m_run->insert(r.m_run->end(), pair);
#endif
      }

#ifdef VECTOR_LSTREE
      r.m_run->flush();
#endif

      // bw.finish();
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
          run r;
          int new_tier_no = tier_no + 1;
          init_run(r, new_tier_no);
          merge_runs(r, m_tiers[tier_no].m_runs);

          if (m_tiers.size() <= new_tier_no)
          {
            m_tiers.push_back(tier());
          }
          m_tiers[new_tier_no].m_runs.push_back(std::move(r));
          m_tiers[tier_no].m_runs.clear();
        }
      } // for (auto & tier : m_tiers).
    }

    //! merge runs into a new one
    void merge_runs(run& final, std::vector<run> const& sources)
    {
#ifdef VECTOR_LSTREE
      using vec_it = typename vector::const_iterator;
#else
      using vec_it = typename map_type::iterator;
#endif

      std::vector<std::pair<vec_it, vec_it>> iters;
      size_t total{0};
      for (auto& source : sources)
      {
#ifndef VECTOR_LSTREE
        // source.m_run->enable_prefetching();
#endif
        iters.emplace_back(source.m_run->begin(), source.m_run->end());
        total += source.m_run->size();
      }

      // vector out(total);
      // std::cout << total << std::endl;
      value_type min_key;

      int added{0};
#ifdef VECTOR_LSTREE
      while (!iters.empty())
      {
        min_key.first = std::numeric_limits<KeyType>::max();
        bool add{false};
        auto iter_pair = iters.begin();
        while (iter_pair != iters.end())
        {
          if (iter_pair->first == iter_pair->second)
          {
            iter_pair = iters.erase(iter_pair);
            continue;
          }
          if (iter_pair->first->first < min_key.first)
          {
            min_key.first = iter_pair->first->first;
            add = true;
            ++(iter_pair->first);
          }
          ++iter_pair;
        }
        if (add)
        {
          final.m_run->push_back(min_key);
          ++added;
        }
      }
      final.m_run->flush();
#else
      for (auto& iter : iters)
      {
        while (iter.first != iter.second)
        {
          final.m_run->insert(final.m_run->end(),
                              {iter.first->first, iter.first->second});
          ++added;
          ++iter.first;
        }
      }
#endif

      // std::cout << final.m_run->size() << std::endl;
      // std::cout << "added " << added << std::endl;
    }

public:
    void reset_stats()
    {
      m_stats = internal_stats();
    }

    void print_stats()
    {
      std::cout << "LSM Tree stats"
        << "\n\tNumber of searches: " << m_stats.reads
        << "\n\t Number of inserts" << m_stats.inserts << std::endl;
    }
  };

STXXL_END_NAMESPACE

#endif // LSM_TREE_H
