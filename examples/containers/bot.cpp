//! [example]

#include <iostream>

// typedef std::pair<int, char> value_type;
// typedef std::pair<unsigned long long, value_type> element_type;

// bool operator==(const value_type& a, const value_type& b)
// {
  // return a.first == b.first;
// }

#include <stxxl/bits/utils/hash.h>
#include <stxxl/bot>
#include <stxxl/bits/utils/hash.h>

//! [comparator]
struct HashCompare
{
  bool operator ()(const unsigned long long& a, const unsigned long long& b) const
  {
    return a < b;
  }

  static unsigned long long max_value()
  {
    return std::numeric_limits<unsigned long long>::max();
  }
};

//! [comparator]
KWiseHash hash(30); // Example: aim for N ≈ 1e9 items → k ≈ ceil(log2 N) ≈ 30

uint64_t HashFunction(unsigned long const& k)
{
  return hash.hash_uint64(k);
}

int main()
{
  using clock = std::chrono::high_resolution_clock; // or steady_clock
  auto t0 = clock::now();

  typedef stxxl::bot::bot<uint64_t, char, uint64_t, HashFunction, HashCompare>
    bot_type;

  bot_type bot(100, 4, 1000);

  // generate stats instance
  stxxl::stats* Stats = stxxl::stats::get_instance();
  // start measurement here
  stxxl::stats_data stats_begin(*Stats);

  std::vector<unsigned long> data;
  int size = 50000;
  data.reserve(size);
  for (int i = 1; i < size; ++i)
  {
    data.push_back(i);
  }

  auto start = std::chrono::high_resolution_clock::now();

  stxxl::stats_data stats_insert(*Stats);
  for (auto d : data)
  {
    // if (d % 1000 == 0)
    // {
    //   std::cout << d << std::endl;
    // }

    bot.insert(std::pair<unsigned long, char>(d, 'a'));
  }

  std::cout << "Insert\n" << (stxxl::stats_data(*Stats) - stats_insert);

  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::milli> elapsed = end - start;
  std::cout << "Insert time: " << elapsed.count() << " ms\n";

  stxxl::stats_data stats_search(*Stats);
  start = std::chrono::high_resolution_clock::now();
  for (auto d : data)
  {
    if (d % 100 == 0)
    {
      std::cout << d << std::endl;
    }

    auto value = bot.find(d);
    if (!value)
    {
      std::cout << "did not find " << d << std::endl;
      return 0;
    }
    if (value->first != d)
    {
      std::cout << "wrong result for key" << d << std::endl;
      return 0;
    }
  }
  std::cout << "Search\n" << (stxxl::stats_data(*Stats) - stats_search);

  end = std::chrono::high_resolution_clock::now();
  elapsed = end - start;
  std::cout << "Search time: " << elapsed.count() << " ms\n";

  std::cout << "Total" << (stxxl::stats_data(*Stats) - stats_begin);

  auto t1 = clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
  std::cout << "Elapsed: " << ms << "ms\n";

  return 0;
}

//! [example]
