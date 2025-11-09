//! [example]

#include <iostream>

#define VECTOR_LSTREE
#ifdef VECTOR_LSTREE
typedef std::pair<int, char> value_type;
bool operator==(const value_type &a, const value_type &b) {
  return a.first == b.first;
}
#endif

#include <chrono>
#include <stxxl/lsm_tree>

#define DATA_NODE_BLOCK_SIZE (4096)
#define DATA_LEAF_BLOCK_SIZE (4096)

#define DATA_NODE_BLOCK_SIZE (4096)
#define DATA_LEAF_BLOCK_SIZE (4096)

//! [comparator]
struct CompareGreater
{
  bool operator () (const int& a, const int& b) const
  { return a > b; }

  static int max_value()
  { return std::numeric_limits<int>::min(); }
};
//! [comparator]

int main() {
  using clock = std::chrono::high_resolution_clock;   // or steady_clock
  auto t0 = clock::now();

  // template parameter <KeyType, DataType, CompareType, RawNodeSize,
  // RawLeafSize, PDAllocStrategy (optional)>
  typedef stxxl::lsm_tree<int, char, CompareGreater, DATA_NODE_BLOCK_SIZE,
                          DATA_LEAF_BLOCK_SIZE>
      lsmt_type;

  lsmt_type tree;

  // generate stats instance
  stxxl::stats *Stats = stxxl::stats::get_instance();
  // start measurement here
  stxxl::stats_data stats_begin(*Stats);

  std::vector<int> data;
  int size = 100000;
  data.reserve(size);
  for (int i = 0; i < size; ++i)
  {
    data.push_back(i);
  }

  // shuffle elements
  for (int i = 0; i < size; ++i)
  {
    auto temp = data[i];
    auto new_index = rand() % size;
    data[i] = data[new_index];
    data[new_index] = temp;
  }

  stxxl::stats_data stats_insert(*Stats);
  for (auto d : data) {
    tree.insert(std::pair<int, char>(d, 'a'));
  }
  std::cout << "Insert\n" << (stxxl::stats_data(*Stats) - stats_insert);

  stxxl::stats_data stats_search(*Stats);
  for (auto d : data) {
    auto value = tree.find(d);
    if (!value) {
      std::cout << "did not find " << d << std::endl;
      return 0;
    }
  }

  std::cout << "Search\n" << (stxxl::stats_data(*Stats) - stats_search);

  std::cout << "Total" << (stxxl::stats_data(*Stats) - stats_begin);
  tree.print_internal_structure();
  std::cout << "tree size " << tree.size() << std::endl;

  auto t1 = clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
  std::cout << "Elapsed: " << ms << "ms\n";

  return 0;
}
//! [example]
