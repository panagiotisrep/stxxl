/***************************************************************************
 *  examples/containers/map1.cpp
 *
 *  Part of the STXXL. See http://stxxl.sourceforge.net
 *
 *  Copyright (C) 2013 Daniel Feist <daniel.feist@student.kit.edu>
 *
 *  Distributed under the Boost Software License, Version 1.0.
 *  (See accompanying file LICENSE_1_0.txt or copy at
 *  http://www.boost.org/LICENSE_1_0.txt)
 **************************************************************************/

//! [example]
#include <chrono>
#include <stxxl/map>
#include <iostream>
#include <random>

//! [comparator]
struct CompareGreater
{
  bool operator ()(const int& a, const int& b) const
  {
    return a > b;
  }

  static int max_value()
  {
    return std::numeric_limits<int>::min();
  }
};

stxxl::stats* Stats;

std::tuple<size_t, size_t, size_t, size_t, size_t, size_t> get_reads_writes(const stxxl::stats_data& src)
{
  auto delta = (stxxl::stats_data(*Stats) - src);
  return {
    delta.get_reads(),
    delta.get_cached_reads(),
    delta.get_read_volume(),
    delta.get_writes(),
    delta.get_cached_writes(),
    delta.get_written_volume()
  };
}

bool file_exists(const std::string& filename)
{
  std::ifstream f(filename.c_str());
  return f.good();
}

std::string get_available_filename(const std::string& base_name)
{
  // if original name is free
  if (!file_exists(base_name))
    return base_name;

  // split "boa.txt" → "boa" + ".txt"
  std::string stem = base_name;
  std::string ext;

  std::size_t dot = base_name.find_last_of('.');
  if (dot != std::string::npos)
  {
    stem = base_name.substr(0, dot);
    ext = base_name.substr(dot);
  }

  int counter = 1;
  while (true)
  {
    std::ostringstream candidate;
    candidate << stem << counter << ext;

    if (!file_exists(candidate.str()))
      return candidate.str();

    ++counter;
  }
}

struct ProcIOStats
{
  uint64_t rchar = 0;
  uint64_t wchar = 0;
  uint64_t syscr = 0;
  uint64_t syscw = 0;
  uint64_t read_bytes = 0;
  uint64_t write_bytes = 0;
  uint64_t cancelled_write_bytes = 0;

  static ProcIOStats read_current()
  {
    ProcIOStats stats;
    std::ifstream file("/proc/self/io");
    std::string key;
    uint64_t value;

    while (file >> key >> value)
    {
      if (key == "rchar:") stats.rchar = value;
      else if (key == "wchar:") stats.wchar = value;
      else if (key == "syscr:") stats.syscr = value;
      else if (key == "syscw:") stats.syscw = value;
      else if (key == "read_bytes:") stats.read_bytes = value;
      else if (key == "write_bytes:") stats.write_bytes = value;
      else if (key == "cancelled_write_bytes:") stats.cancelled_write_bytes = value;
    }

    return stats;
  }

  ProcIOStats operator-(const ProcIOStats& other) const
  {
    ProcIOStats delta;
    delta.rchar = rchar - other.rchar;
    delta.wchar = wchar - other.wchar;
    delta.syscr = syscr - other.syscr;
    delta.syscw = syscw - other.syscw;
    delta.read_bytes = read_bytes - other.read_bytes;
    delta.write_bytes = write_bytes - other.write_bytes;
    delta.cancelled_write_bytes =
      cancelled_write_bytes - other.cancelled_write_bytes;
    return delta;
  }
};

#ifndef DATA_NODE_BLOCK_SIZE
#define DATA_NODE_BLOCK_SIZE (16 * 1024)
#endif

#ifndef DATA_LEAF_BLOCK_SIZE
#define DATA_LEAF_BLOCK_SIZE (128 * 1024)
#endif

#ifndef DATA_NODES_IN_CACHE
#define DATA_NODES_IN_CACHE (256)
#endif

#ifndef DATA_LEAVES_IN_CACHE
#define DATA_LEAVES_IN_CACHE (256)
#endif

int main()
{
#define IO_DETAILS


  using KeyType = uint32_t;
  using clock = std::chrono::high_resolution_clock; // or steady_clock

  const int node_cache = DATA_NODE_BLOCK_SIZE * DATA_NODES_IN_CACHE;
  const int leaf_cache = DATA_LEAF_BLOCK_SIZE * DATA_LEAVES_IN_CACHE;
  const int node_block_size = DATA_NODE_BLOCK_SIZE;
  const int leaf_block_size = DATA_LEAF_BLOCK_SIZE;

  auto tmp = std::string("stxxl_map_n")
    + std::to_string(node_block_size) + "_" + std::to_string(node_cache) + "_l_"
    + std::to_string(leaf_block_size) + "_" + std::to_string(leaf_cache) + ".txt";

  const std::string filename = get_available_filename(tmp);
// ... existing code ...
struct ProcIOStats
{
  // ... existing code ...
};

void print_os_io_stats(std::ostream& os, const ProcIOStats& os_delta, int total_memory_bytes)
{
  os << "OS read_bytes:  " << os_delta.read_bytes << "\n";
  os << "OS write_bytes: " << os_delta.write_bytes << "\n";
  os << "OS read syscalls:  " << os_delta.syscr << "\n";
  os << "OS write syscalls: " << os_delta.syscw << "\n";
  os << "Max internal memory bytes: " << total_memory_bytes << "\n";
}

#ifndef DATA_NODE_BLOCK_SIZE
// ... existing code ...

  ProcIOStats os_after = ProcIOStats::read_current();
  ProcIOStats os_delta = os_after - os_before;

  const int total_memory_bytes = node_cache + leaf_cache;
  print_os_io_stats(std::cout, os_delta, total_memory_bytes);
  print_os_io_stats(out, os_delta, total_memory_bytes);

  std::cout << "Output file [" << filename << "]\n";
  out << "End\n";
  out.flush();
  out.close();

  return 0;
}
// ... existing code ...const int size = 5000000;
  const int output_every_inserts = 50000;
  const int output_every_queries = 1000;
  const int queries_no = 10000;

  std::ofstream out(filename, std::ios::app);
  if (!out)
    throw std::runtime_error("Cannot open results file");

  typedef stxxl::map<uint32_t, char, CompareGreater, node_block_size, leaf_block_size> map_type;

  map_type my_map(node_cache, leaf_cache);

  /**************************** create data *****************************/
  std::vector<KeyType> data;
  data.reserve(size);
  for (int i = 1; i <= size; ++i)
  {
    data.push_back(i);
  }
  std::mt19937 gen(12345); // fixed seed
  std::shuffle(data.begin(), data.end(), gen);

  std::vector<map_type::value_type> value_array;
  for (auto d : data)
    value_array.emplace_back(static_cast<int>(d), (char)'a');

  /*************************** start experiment *************************/
  auto t_start = clock::now();

  out << "Start Map"
    << "\nElements: " << data.size()
    << "\nnode_cache: " << node_cache
    << "\nleaf_cache " << leaf_cache
    << "\nnode_block_size " << node_block_size
    << "\nPage leaf_block_size " << leaf_block_size
    << "\n";


  ProcIOStats os_before = ProcIOStats::read_current();

  Stats = stxxl::stats::get_instance();
  stxxl::stats_data stats_begin(*Stats);


  /************************* Insertions ***********************************/

  stxxl::stats_data stats_insert_begin(*Stats);
  auto t_start_insertions = std::chrono::high_resolution_clock::now();

  std::cout << "Start Insertions\n";
  out << "Start Insertions\n";

  auto start_iteration = std::chrono::high_resolution_clock::now();
  int counter{0};
  for (auto d : data)
  {
    ++counter;
    if (counter % output_every_inserts == 0)
    {
      auto end = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double, std::milli> elapsed = end - start_iteration;
      std::cout << counter << " " << elapsed.count() << std::endl;
#ifndef IO_DETAILS
      out << "Insertions " << counter << ", time " << elapsed.count() << "\n";
#else
      auto details = get_reads_writes(stats_insert_begin);
      out << "Insertions " << counter
        << ", time " << elapsed.count() << ", details: "
        << "reads " << std::get<0>(details) << ", "
        << "cached_reads " << std::get<1>(details) << ", "
        << "bytes read " << std::get<2>(details) << ", "
        << "writes " << std::get<3>(details) << ", "
        << "cached writes " << std::get<4>(details) << ", "
        << "bytes written " << std::get<5>(details) << "\n";
#endif
      start_iteration = std::chrono::high_resolution_clock::now();
    }

    my_map.insert(std::pair<unsigned long, char>(d, 'a'));
  }

  std::chrono::duration<double, std::milli> insertions_total = std::chrono::high_resolution_clock::now() -
    t_start_insertions;
  out << "End Insertions, time " << insertions_total.count() << "\n";
  out << (stxxl::stats_data(*Stats) - stats_insert_begin);

  /******************************** Queries ***********************************/

  std::shuffle(data.begin(), data.end(), gen);

  stxxl::stats_data stats_queries_begin(*Stats);
  auto t_start_queries = std::chrono::high_resolution_clock::now();

  std::cout << "Start Queries\n";
  out << "Start Queries\n";

  counter = 0;
  start_iteration = std::chrono::high_resolution_clock::now();
  for (unsigned int& d : data)
  {
    if (counter == queries_no)
    {
      break;
    }
    ++counter;
    if (counter % output_every_queries == 0)
    {
      auto end = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double, std::milli> elapsed = end - start_iteration;
      std::cout << counter << " " << elapsed.count() << std::endl;
#ifndef IO_DETAILS
      out << "Queries " << counter << ", time " << elapsed.count() << "\n";
#else
      auto details = get_reads_writes(stats_queries_begin);
      out << "Queries " << counter
        << ", time " << elapsed.count() << ", details: "
        << "reads " << std::get<0>(details) << ", "
        << "cached_reads " << std::get<1>(details) << ", "
        << "bytes read " << std::get<2>(details) << ", "
        << "writes " << std::get<3>(details) << ", "
        << "cached writes " << std::get<4>(details) << ", "
        << "bytes written " << std::get<5>(details) << "\n";
#endif
      start_iteration = std::chrono::high_resolution_clock::now();
      // break;
    }

    if (my_map.find(d) == my_map.end())
    {
      std::cout << "wrong result for key" << d << std::endl;
      return 0;
    }
  }

  std::chrono::duration<double, std::milli> queries_total = std::chrono::high_resolution_clock::now() - t_start_queries;
  out << "End Queries, time " << queries_total.count() << "\n";
  out << (stxxl::stats_data(*Stats) - stats_queries_begin);

  out << "Total\n"
    << (stxxl::stats_data(*Stats) - stats_begin)
    << std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t_start).count()
    << "\n";

  std::cout << "Total\n"
    << (stxxl::stats_data(*Stats) - stats_begin)
    << std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t_start).count()
    << "\n";

  ProcIOStats os_after = ProcIOStats::read_current();
  ProcIOStats os_delta = os_after - os_before;

  std::cout << "OS read_bytes:  " << os_delta.read_bytes << "\n";
  std::cout << "OS write_bytes: " << os_delta.write_bytes << "\n";
  std::cout << "OS read syscalls:  " << os_delta.syscr << "\n";
  std::cout << "OS write syscalls: " << os_delta.syscw << "\n";
  std::cout << "Max internal memory bytes: " << node_cache + leaf_cache << "\n";

  out << "OS read_bytes:  " << os_delta.read_bytes << "\n";
  out << "OS write_bytes: " << os_delta.write_bytes << "\n";
  out << "OS read syscalls:  " << os_delta.syscr << "\n";
  out << "OS write syscalls: " << os_delta.syscw << "\n";
  out << "Max internal memory bytes: " << node_cache + leaf_cache << "\n";

  std::cout << "Output file [" << filename << "]\n";
  out << "End\n";

  out << "End\n";
  out.flush();
  out.close();

  return 0;
}

//! [example]
