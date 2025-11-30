//
// Created by panos on 6/23/25.
//

#ifndef BOT_H
#define BOT_H

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
  namespace bot
  {
    typedef uint32_t prefix_t;
    typedef uint8_t char_t;

    class settings
    {
    public:
      settings* get_instance()
      {
        static std::unique_ptr<settings> instance;
        if (!instance)
        {
          instance.reset(new settings());
        }
        return instance.get();
      }

    private:
      settings() = default;
    };

    namespace bits_manipulation
    {
      template <typename HashType>
      size_t get_bits(HashType const& x, const unsigned group_index, unsigned group_size) noexcept
      {
        typedef typename std::make_unsigned<HashType>::type U;
        U mask = (U(1) << group_size) - U(1);
        return (U(x) >> (group_index * group_size)) & mask;
      }

      template <typename HashType>
      size_t get_bits(HashType const& x, const unsigned bits_length) noexcept
      {
        using U = typename std::make_unsigned<HashType>::type;
        U mask = (U(1) << bits_length) - U(1);
        return static_cast<size_t>(U(x) & mask);
      }

      template <typename NT>
      size_t get_high_bits(NT const& x, unsigned group_index, unsigned group_size) noexcept
      {
        using U = typename std::make_unsigned<NT>::type;
        constexpr unsigned W = sizeof(U) * CHAR_BIT;

        if (group_size == 0)
          return 0;

        // starting bit from the *left* (highest bits)
        // shift may become negative → guard before computing
        unsigned block_size = (group_index + 1) * group_size;

        // If the requested high-bit block starts left of bit 0 → result is 0
        if (block_size > W)
          return 0;

        unsigned shift = W - block_size; // safe now

        // safe mask
        U mask;
        if (group_size >= W)
          mask = U(~U(0));
        else
          mask = (U(1) << group_size) - 1;

        U ux = static_cast<U>(x);

        // Move desired high-bit group down to lowest bits, mask it
        return static_cast<size_t>((ux >> shift) & mask);
      }


      template <class NT>
      size_t get_high_bits(NT const& x, unsigned bits_length) noexcept
      {
        using U = typename std::make_unsigned<NT>::type;
        constexpr unsigned width = sizeof(U) * CHAR_BIT;

        U ux = U(x);
        unsigned shift = width - bits_length;
        U high = ux >> shift;
        U mask = (U(1) << bits_length) - U(1);
        return static_cast<size_t>(high & mask);
      }

      template <typename HashType>
      size_t get_prefix(HashType const& hash, unsigned int prefix_bits_length)
      {
        return get_high_bits(hash, prefix_bits_length) << (sizeof(prefix_t) * 8 - prefix_bits_length);
      }

      template <typename HashType>
      char_t get_char_of_hash(HashType const& hash, unsigned int char_no, unsigned int char_bits_length)
      {
        return get_high_bits(hash, char_no, char_bits_length);
      }

      template <typename HashType>
      char_t get_char_of_hash_from_the_end(HashType const& hash, unsigned int char_no, unsigned int char_bits_length)
      {
        return get_bits(hash, char_no, char_bits_length);
      }
    } // namespace bits_manipulation.


    template <class KeyType, class DataType, class HashType>
    struct external_memory_log
    {
      struct log_element
      {
        KeyType m_key;
        DataType m_value;
        HashType m_hash;

        log_element() = default;

        log_element(const KeyType& key, const DataType& value, const HashType& hash) : m_key(key), m_value(value),
          m_hash(hash)
        {
        }
      };

      typedef typename VECTOR_GENERATOR<log_element, 4, 8, 1 * 1024 * 1024, stxxl::RC,
                                        stxxl::lru>::result external_vector;

      external_vector m_log{};
      size_t added{};
      uint32_t block_size{};
    };

    template <class HashType>
    struct character_queue
    {
      struct sketch
      {
        prefix_t m_prefix[10];
        char_t m_next_char[10]{};
        char_t m_check_char[10]{};
      };

      typedef typename VECTOR_GENERATOR<sketch, 4, 8, 1 * 1024 * 1024, stxxl::RC, stxxl::lru>::result sketches;

      std::map<unsigned char, std::unique_ptr<sketches>> m_sketches{};
      unsigned int m_q;
      unsigned int m_char_size;

      character_queue(unsigned int q, unsigned int char_size) : m_q(q), m_char_size(char_size)
      {
        for (unsigned int i = 0; i < q; ++i)
        {
          m_sketches[pow(2, i)].reset(new sketches());
        }
      }

      void insert_hash(HashType const& hash)
      {
        using namespace bits_manipulation;

        // todo i=0

        for (unsigned int q = 0; q < m_q; ++q)
        {
          auto i_from = static_cast<int>(pow(2, q));
          auto i_to = static_cast<int>(pow(2, q+1)) - 1;

          auto & sigma_j = m_sketches[i_from];

          sketch s;
          for (int j = i_from; j < i_to; ++j)
          {
            s.m_prefix = get_prefix(hash, (j + 1) * m_char_size);
            s.m_check_char[i] = get_char_of_hash_from_the_end(hash, i, m_char_size);
            s.m_next_char[i] = get_char_of_hash(hash, i + 1, m_char_size);
            sigma_j->push_back(s);
          }
        }
      }
    };

    struct sketch
    {
      unsigned char m_routing_child_ptr{};
      bool m_false_positive{};
      uint8_t m_next_char{};
      uint32_t m_prefix{};
      uint8_t m_prefix_last_char{};
    };

    template <class HashType>
    struct routing_filter
    {
      struct list_node
      {
        int32_t m_next_node{-1};
        prefix_t m_prefix{};
        uint8_t m_routing_child_ptr{};
        // HashType m_hash{};
        char_t m_check_char{};
        char_t m_next_char{};
      };

      struct list
      {
        typedef typename VECTOR_GENERATOR<list_node, 4, 8, 1 * 1024 * 1024, stxxl::RC, stxxl::lru>::result list_vector;
        list_vector m_list;
        size_t m_size{};
        size_t m_tail{};

        size_t push_back(const list_node& node)
        {
          if (m_size > 0)
          {
            m_list[m_tail].m_next_node = m_size;
          }

          m_list.push_back(node);
          m_tail = m_size;
          ++m_size;
          return m_size - 1;
        }

        size_t insert_after(const list_node& node, size_t const& pos)
        {
          auto next = m_list[pos].m_next_node;;
          m_list[pos].m_next_node = m_size;

          auto tmp = node;
          tmp.m_next_node = next;
          m_list.push_back(tmp);

          if (next == -1)
          {
            m_tail = m_size;
          }
          ++m_size;
          return m_size - 1;
        }
      };

      typedef int32_t list_pointer;
      typedef VECTOR_GENERATOR<list_pointer, 4, 8, 1 * 1024 * 1024, stxxl::RC,
                               stxxl::lru>::result routing_external_vector;

      uint32_t m_prefix_bits_length;
      uint32_t m_char_bits_length;
      uint32_t m_chars_no;
      uint32_t m_pivot_prefix_bits_length;
      list m_sketches;
      std::unique_ptr<routing_external_vector> m_sketches_hashmap;
      uint32_t m_check_char_index;

      routing_filter(uint32_t chars_no, uint32_t char_bits_length, uint32_t check_char_index,
                     const uint32_t pivot_prefix_bits_length = 10) :
        m_char_bits_length(char_bits_length), m_chars_no(chars_no),
        m_pivot_prefix_bits_length(pivot_prefix_bits_length),
        m_check_char_index(check_char_index)
      {
        m_prefix_bits_length = char_bits_length * chars_no;
        m_sketches_hashmap = std::unique_ptr<routing_external_vector>(new routing_external_vector());
        m_sketches_hashmap->resize(static_cast<size_t>(std::pow(2, pivot_prefix_bits_length)));
        std::fill(m_sketches_hashmap->begin(), m_sketches_hashmap->end(), -1);
      }

      bool equal_prefixes(HashType const& h1, HashType const& h2)
      {
        // return get_bits(h1, 0, m_char_bits_length * m_chars_no) == get_bits(h2, 0, m_char_bits_length * m_chars_no);
        return bits_manipulation::get_high_bits(h1, m_prefix_bits_length) == get_high_bits(h2, m_prefix_bits_length);
      }

      size_t get_prefix(HashType const& hash)
      {
        return bits_manipulation::get_high_bits(hash, m_prefix_bits_length) << (sizeof(uint32_t) * 8 - m_prefix_bits_length);
      }

      char_t get_char_after_prefix(HashType const& hash)
      {
        return bits_manipulation::get_high_bits(hash, m_chars_no, m_char_bits_length);
        // << (sizeof(uint32_t) * 8 - ((m_chars_no) * m_char_bits_length));
      }

      char_t get_prefix_last_char(uint32_t prefix)
      {
        auto h = prefix >> (sizeof(uint32_t) * 8 - m_prefix_bits_length);
        return bits_manipulation::get_bits(h, m_char_bits_length);
        // << (sizeof(uint32_t) * 8 - ((m_chars_no) * m_char_bits_length));
      }

      std::list<sketch> get_sketches(HashType const& hash)
      {
        using namespace bits_manipulation;

        if (hash == 630133364300331789)
        {
          auto a = 4;
        }
        auto query_prefix = get_prefix(hash);
        std::list<sketch> sketches;

        auto pivot_prefix = get_high_bits(hash, m_pivot_prefix_bits_length);
        auto pivot_prefix_index_in_list = (*m_sketches_hashmap)[pivot_prefix];
        auto next_pivot_prefix_index_in_list = (*m_sketches_hashmap)[pivot_prefix + 1];
        uint8_t check_char = get_bits_translated_lowest_position(hash, m_check_char_index, m_char_bits_length);

        if (pivot_prefix_index_in_list != -1)
        {
          auto next_index = pivot_prefix_index_in_list;

          do
          {
            auto node = m_sketches.m_list[next_index];
            // prefix_t node_prefix = get_prefix(node.m_hash);
            if (node.m_prefix == query_prefix)
            {
              sketch s;
              s.m_routing_child_ptr = node.m_routing_child_ptr;
              s.m_next_char = node.m_next_char;
              s.m_prefix = node.m_prefix;
              s.m_prefix_last_char = get_prefix_last_char(node.m_prefix);
              s.m_false_positive = check_char != node.m_check_char; //get_bits_translated_lowest_position(
              // node.m_hash, m_check_char_index, m_char_bits_length);
              sketches.push_back(s);
            }

            if (get_high_bits(node.m_prefix, m_pivot_prefix_bits_length) != pivot_prefix)
            {
              break;
            }
            next_index = node.m_next_node;
          }
          while (next_index != -1 && next_index != next_pivot_prefix_index_in_list);
        }

        return sketches;
      }

      size_t get_bits_translated_lowest_position(HashType const& x, unsigned group_index, unsigned group_size) noexcept
      {
        using U = typename std::make_unsigned<HashType>::type;
        constexpr unsigned W = sizeof(U) * CHAR_BIT;

        if (group_size == 0)
          return 0;

        const unsigned shift = group_index * group_size;

        // If the group starts beyond the value's width, result is always 0
        if (shift >= W)
          return 0;

        // Construct mask safely (no UB)
        U mask;
        if (group_size >= W)
          mask = U(~U(0));
        else
          mask = (U(1) << group_size) - 1;

        // Extract and shift down so bits are at LSB
        U val = static_cast<U>(x);
        U result = (val >> shift) & mask;

        return static_cast<size_t>(result);
      }


      void insert_hash(HashType const& hash, unsigned char routing_child_ptr)
      {
        using namespace bits_manipulation;
        if (hash == 630133364300331789)
        {
          auto a = 3;
        }
        list_node node;
        node.m_routing_child_ptr = routing_child_ptr;
        node.m_prefix = get_prefix(hash);
        // node.m_hash = hash;
        node.m_check_char = get_bits_translated_lowest_position(hash, m_check_char_index, m_char_bits_length);
        node.m_next_char = get_char_after_prefix(hash);

        auto pivot_prefix = get_high_bits(hash, m_pivot_prefix_bits_length);
        auto pivot_prefix_index_in_list = (*m_sketches_hashmap)[pivot_prefix];

        if (pivot_prefix_index_in_list == -1)
        {
          auto index_in_list = m_sketches.push_back(node);
          (*m_sketches_hashmap)[pivot_prefix] = index_in_list;
        }
        else
        {
          m_sketches.insert_after(node, pivot_prefix_index_in_list);
        }
      }

      void merge(routing_filter const& f, character_queue<HashType> const& char_queue, uint32_t new_child_ptr)
      {
        for (list_node sketch : f.m_sketches.m_list)
        {
          insert_hash(sketch.m_hash, new_child_ptr);
        }
      }
    };


    template <class KeyType, class DataType, class HashType>
    struct routing_tree
    {
      using log_element_t = typename external_memory_log<KeyType, DataType, HashType>::log_element;
      using external_memory_log_t = external_memory_log<KeyType, DataType, HashType>;

      struct abstract_node
      {
        virtual ~abstract_node();
        virtual std::unique_ptr<log_element_t> search(KeyType const& k, HashType const& hash,
                                                      external_memory_log_t& log,
                                                      std::list<sketch> const& sketches) = 0;
      };

      struct node_t : public abstract_node
      {
        std::vector<abstract_node*> m_children;
        uint32_t m_max_fan_out;
        std::unique_ptr<routing_filter<HashType>> m_routing_filter;
        std::unique_ptr<character_queue<HashType>> m_character_queue;
        size_t m_log_ptr{};
        uint32_t m_height_in_tree;

        node_t(uint32_t max_fan_out, uint32_t height_in_tree) : m_max_fan_out(max_fan_out),
                                                                m_height_in_tree(height_in_tree)
        {
          m_routing_filter.reset(new routing_filter<HashType>(m_height_in_tree, 3, m_height_in_tree - 1));
          m_character_queue.reset(new character_queue<HashType>(7, 3));
        }

        ~node_t() override
        {
          for (auto child : m_children)
          {
            delete child;
          }
        }

        std::unique_ptr<log_element_t>
        search(KeyType const& k, HashType const& hash,
               external_memory_log_t& log,
               std::list<sketch> const& sketches) override
        {
          auto new_sketches = m_routing_filter->get_sketches(hash);
          std::unordered_map<uint8_t, std::list<sketch>> child_to_sketches;
          auto tmp = new_sketches;
          if (!sketches.empty())
          {
            std::unordered_set<uint8_t> last_chars;
            for (auto const& s : sketches)
            {
              last_chars.insert(s.m_prefix_last_char);
            }

            auto it = new_sketches.begin();
            while (it != new_sketches.end())
            {
              if (it->m_next_char != sketches.begin()->m_prefix_last_char) //last_chars.count(it->m_next_char) == 0)
              {
                it = new_sketches.erase(it);
                // it->m_false_positive = true;
              }
              else
              {
                ++it;
              }
            }
          }

          // if(!sketches.empty() && new_sketches.size() != sketches.size())
          // {
          // exit(0);
          // }
          for (auto const& sketch : new_sketches)
          {
            child_to_sketches[sketch.m_routing_child_ptr].push_back(sketch);
          }

          for (auto const& s : child_to_sketches)
          {
            bool should_search{false};
            for (auto const& sketch : s.second)
            {
              if (!sketch.m_false_positive)
              {
                should_search = true;
                break;
              }
            }

            if (should_search)
            {
              if (auto ret = m_children[s.first]->search(k, hash, log, s.second))
              {
                return ret;
              }
            }
          }
          return nullptr;
        }
      };

      struct leaf_t : public abstract_node
      {
        size_t m_log_ptr{};

        explicit leaf_t(size_t log_ptr) : m_log_ptr(log_ptr)
        {
        }

        ~leaf_t() override = default;

        std::unique_ptr<log_element_t>
        search(KeyType const& k, HashType const& hash, external_memory_log_t& log,
               std::list<sketch> const& sketches) override
        {
          if (m_log_ptr >= log.m_log.size())
          {
            return nullptr;
          }

          auto end = m_log_ptr + log.block_size;

          for (size_t i = m_log_ptr; i < log.m_log.size() && i < end; ++i)
          {
            const auto& e = log.m_log[i];
            if (e.m_key == k)
            {
              return std::unique_ptr<log_element_t>(new log_element_t(e.m_key, e.m_value, e.m_hash));
            }
          }
          return nullptr;
        }
      };

      node_t* m_root;
      uint32_t m_max_fan_out;
      uint32_t m_height;

      explicit routing_tree(uint32_t max_fan_out, uint32_t m_height) : m_max_fan_out(max_fan_out), m_height(m_height)
      {
        m_root = new node_t(max_fan_out, m_height);
      }

      ~routing_tree()
      {
        delete m_root;
      }

      /**
       *
       * @param node
       * @return pos of inserted node under root
       */
      uint32_t insert_node_under_root(abstract_node* node)
      {
        m_root->m_children.push_back(node);
        return m_root->m_children.size() - 1;
      }

      bool is_filled()
      {
        return m_root->m_children.size() > m_max_fan_out;
      }

      void clear_root_children()
      {
        m_root->m_children.clear();
      }

      size_t root_children_number()
      {
        return m_root->m_children.size();
      }

      void merge(routing_tree const& other)
      {
        auto new_child_ptr = insert_node_under_root(other.m_root);
        m_root->m_routing_filter->merge(*(other.m_root->m_routing_filter),
                                        *(other.m_root->m_character_queue),
                                        new_child_ptr);
      }

      std::unique_ptr<log_element_t> search(KeyType const& k, HashType const& hash, external_memory_log_t& log)
      {
        std::list<sketch> sketches;
        return m_root->search(k, hash, log, sketches);
      }

      void insert_hash(HashType const& hash)
      {
        auto new_leaf_index_in_root = root_children_number();
        m_root->m_routing_filter->insert_hash(hash, new_leaf_index_in_root);
        m_root->m_character_queue->insert_hash(hash);
      }
    };

    template <class KeyType, class DataType, class HashType>
    routing_tree<KeyType, DataType, HashType>::abstract_node::~abstract_node() = default;

    template <class KeyType, class DataType, class HashType, HashType (*HashFunction)(KeyType const&), class
              HashCompare>
    class bot : private noncopyable
    {
    public:
      typedef std::pair<KeyType, DataType> element_type;
      using log_element_t = typename external_memory_log<KeyType, DataType, HashType>::log_element;
      using routing_tree_t = routing_tree<KeyType, DataType, HashType>;

    private:
      struct tier
      {
        uint32_t tier_no{};
        std::unique_ptr<routing_tree<KeyType, DataType, HashType>> m_routing_tree;
        // character queue
      };

      //! In-memory cache
      std::map<HashType, log_element_t> m_in_memory_log;
      size_t m_block_size;
      size_t m_lambda;
      size_t m_beta;
      std::vector<tier> m_tiers;
      external_memory_log<KeyType, DataType, HashType> m_log;

    public:
      bot(size_t block_size, size_t lambda, size_t beta) :
        m_block_size(block_size), m_lambda(lambda), m_beta(beta)
      {
        m_log.block_size = block_size;
        init();
      }

      //! Insert a key-value pair in the lsm tree
      void insert(const element_type& value)
      {
        HashType hash;
        hash = HashFunction(value.first);
        log_element_t element;
        element.m_key = value.first;
        element.m_value = value.second;
        element.m_hash = hash;

        m_in_memory_log.insert({hash, element});

        if (m_in_memory_log.size() >= m_block_size)
        {
          flush_in_external_memory();
          m_in_memory_log.clear();
        }
      }

      //! find key-value corresponding to search key
      std::unique_ptr<element_type> find(const KeyType& k)
      {
        if (k == 19)
        {
          auto a = 4;
        }
        // search in memory
        auto h = HashFunction(k);
        for (auto const& element : m_in_memory_log)
        {
          if (element.second.m_key == k)
          {
            return std::unique_ptr<element_type>(new element_type({element.second.m_key, element.second.m_value}));
          }
        }

        // search in external memory
        for (auto const& tier : m_tiers)
        {
          auto ret = tier.m_routing_tree->search(k, h, m_log);
          if (ret)
          {
            return std::unique_ptr<element_type>(new element_type({ret->m_key, ret->m_value}));
          }
        }

        return nullptr;
      }

    private:
      routing_tree_t* create_routing_tree(uint32_t height)
      {
        static_cast<void>(height);
        return new routing_tree_t(m_lambda, height);
      }

      //! initialize the lsm tree
      void init()
      {
        tier t0;
        t0.tier_no = 1;
        t0.m_routing_tree.reset(create_routing_tree(1));
        m_tiers.push_back(std::move(t0));
      }

      //! Flush in memory log to external memory
      void flush_in_external_memory()
      {
        if (!m_tiers[0].m_routing_tree)
        {
          m_tiers[0].m_routing_tree.reset(create_routing_tree(1));
        }

        auto leaf = new typename routing_tree_t::leaf_t(m_log.added);

        for (auto const& element : m_in_memory_log)
        {
          m_log.m_log.push_back(element.second);
          ++(m_log.added);
          m_tiers[0].m_routing_tree->insert_hash(element.second.m_hash);
        } // for (auto const & element : m_in_memory_log).

        m_tiers[0].m_routing_tree->insert_node_under_root(leaf);
        m_log.m_log.flush();
        compact_tiers();
      }

      //! merges runs of tiers if needed
      void compact_tiers()
      {
        for (int32_t tier_no = 0; tier_no < m_tiers.size(); ++tier_no)
        {
          if (m_tiers[tier_no].m_routing_tree->is_filled())
          {
            int32_t new_tier_no = tier_no + 1;
            if (m_tiers.size() <= new_tier_no)
            {
              auto t = tier();
              t.tier_no = new_tier_no + 1;
              t.m_routing_tree.reset(create_routing_tree(new_tier_no + 1));
              m_tiers.push_back(std::move(t));
            }

            merge_routing_trees(m_tiers[tier_no], m_tiers[new_tier_no]);
            m_tiers[tier_no].m_routing_tree->m_root = nullptr;
            m_tiers[tier_no].m_routing_tree.reset(create_routing_tree(tier_no + 1));
          }
        } // for (int32_t tier_no = 0; tier_no < m_tiers.size(); ++tier_no).
      }

      //! merge runs into next tier
      void merge_routing_trees(tier& source_tier, tier& dest_tier)
      {
        dest_tier.m_routing_tree->merge(*(source_tier.m_routing_tree));
      }
    };
  } // namespace bot.

STXXL_END_NAMESPACE

#endif // BOT_H
