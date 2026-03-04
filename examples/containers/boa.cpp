//! [example]

#define BOA_PAGES 8
#define BOA_PAGE_SIZE 1
#define BOA_BLOCK_SIZE 256*1024
#define BOA_BUFFER_SIZE 1024
#define BOA_LAMBDA 16

#include <iostream>
#include <stxxl/bits/utils/hash.h>
#include <stxxl/boa>
#include <stxxl/bits/utils/hash.h>
#include <type_traits>
#include <climits>
#include <fstream>
#include <string>
#include <stdexcept>
#include <tuple>

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


uint64_t reverse_bits(uint64_t const& x) noexcept
{
  using U = typename std::make_unsigned<uint64_t>::type;

  constexpr unsigned W = sizeof(U) * CHAR_BIT;

  U v = static_cast<U>(x);
  U r = 0;

  for (unsigned i = 0; i < W; ++i)
  {
    r <<= 1;
    r |= (v & 1);
    v >>= 1;
  }

  return static_cast<uint64_t>(r);
}

uint64_t HashFunction(uint32_t const& k)
{
  auto h = hash.hash_uint64(k);
  auto a = reverse_bits(h);
  return a;
}

stxxl::stats* Stats;

//
// int main()
// {
// #define SEARCH_
//   std::vector<unsigned long> data;
//   int size = 100000;
//   data.reserve(size);
//   for (int i = 1; i < size; ++i)
//   {
//     data.push_back(i);
//   }
//
//   std::mt19937 gen(12345); // fixed seed
//   std::shuffle(data.begin(), data.end(), gen);
//
//   using clock = std::chrono::high_resolution_clock; // or steady_clock
//   auto t0 = clock::now();
//
//   typedef stxxl::boa::boa<uint32_t, char, uint64_t, HashFunction, HashCompare, 8>
//     boa_type;
//
//   boa_type boa(1024);
//   boa.print_info();
//
//   // generate stats instance
//   stxxl::stats* Stats = stxxl::stats::get_instance();
//   // start measurement here
//   stxxl::stats_data stats_begin(*Stats);
//
//   auto start = std::chrono::high_resolution_clock::now();
//
//   stxxl::stats_data stats_insert(*Stats);
//   auto _start = std::chrono::high_resolution_clock::now();
//   int counter{0};
//   for (auto d : data)
//   {
//     ++counter;
//     if (counter % 100000 == 0)
//     {
//       auto end = std::chrono::high_resolution_clock::now();
//       std::chrono::duration<double, std::milli> elapsed = end - _start;
//       std::cout << counter << " " << elapsed.count() << std::endl;
//       _start = std::chrono::high_resolution_clock::now();
//     }
//
//     boa.insert(std::pair<unsigned long, char>(d, 'a'));
//   }
//
//   std::cout << "Insert\n" << (stxxl::stats_data(*Stats) - stats_insert);
//
//   auto end = std::chrono::high_resolution_clock::now();
//   std::chrono::duration<double, std::milli> elapsed = end - start;
//   std::cout << "Insert time: " << elapsed.count() << " ms\n";
//   boa.print_internal_structure();
//
//   stxxl::stats_data stats_search(*Stats);
//   start = std::chrono::high_resolution_clock::now();
// #ifdef SEARCH_
//   counter = 0;
//   for (auto d = data.begin(); d != data.end(); ++d)
//   {
//     ++counter;
//     if (counter % 5000 == 0)
//     {
//       auto end = std::chrono::high_resolution_clock::now();
//       std::chrono::duration<double, std::milli> elapsed = end - _start;
//       std::cout << counter << " " << elapsed.count() << std::endl;
//       _start = std::chrono::high_resolution_clock::now();
//       // break;
//     }
//
//     auto value = boa.find(*d);
//     if (!value)
//     {
//       std::cout << "did not find " << *d << std::endl;
//       return 0;
//     }
//     if (value->first != *d)
//     {
//       std::cout << "wrong result for key" << *d << std::endl;
//       return 0;
//     }
//   }
//
//   boa.stats.print();
//
//   std::cout << "Search\n" << (stxxl::stats_data(*Stats) - stats_search);
//
//   end = std::chrono::high_resolution_clock::now();
//   elapsed = end - start;
//   std::cout << "Search time: " << elapsed.count() << " ms\n";
// #endif
//   std::cout << "Total" << (stxxl::stats_data(*Stats) - stats_begin);
//
//   auto t1 = clock::now();
//   auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
//   std::cout << "Elapsed: " << ms << "ms\n";
//
//   return 0;
// }

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

int main()
{
#define IO_DETAILS
  using KeyType = uint32_t;
  using clock = std::chrono::high_resolution_clock; // or steady_clock

  const int pages = BOA_PAGES;
  const int page_size = BOA_PAGE_SIZE;
  const int block_size = BOA_BLOCK_SIZE;
  const int buffer_size = BOA_BUFFER_SIZE;
  const int lambda = BOA_LAMBDA;

  std::string tmp_filename = "boa_"
    + std::to_string(page_size)
    + "_" + std::to_string(pages)
    + "_" + std::to_string(block_size) + ".txt";

  const std::string filename = get_available_filename(tmp_filename);
  const int size = 5000000;
  const int output_every_inserts = 50000;
  const int output_every_queries = 1000;
  const int queries_no = 10000;

  std::ofstream out(filename, std::ios::app);
  if (!out)
    throw std::runtime_error("Cannot open results file");

  typedef stxxl::boa::boa<
    KeyType,
    char,
    uint64_t,
    HashFunction,
    HashCompare,
    lambda,
    pages,
    page_size,
    block_size
  > boa_type;

  /**************************** create data *****************************/
  std::vector<KeyType> data;
  data.reserve(size);
  for (int i = 1; i <= size; ++i)
  {
    data.push_back(i);
  }
  std::mt19937 gen(12345); // fixed seed
  std::shuffle(data.begin(), data.end(), gen);

  ProcIOStats os_before = ProcIOStats::read_current();

  /*************************** start experiment *************************/
  auto t_start = clock::now();

  out << "Start"
    << "\nElements: " << data.size()
    << "\nBuffer: " << buffer_size
    << "\nLambda " << lambda
    << "\nPages " << pages
    << "\nPage size " << page_size
    << "\nBlock size " << block_size
    << "\n";

  boa_type boa(buffer_size);

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

    boa.insert(std::pair<unsigned long, char>(d, 'a'));
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

    auto value = boa.find(d);
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

  boa.stats.print();

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
  std::cout << "Max internal memory bytes: " << boa.max_internal_memory() << "\n";
  std::cout << "Tiers no: " << boa.get_tiers_no() << "\n";

  out << "OS read_bytes:  " << os_delta.read_bytes << "\n";
  out << "OS write_bytes: " << os_delta.write_bytes << "\n";
  out << "OS read syscalls:  " << os_delta.syscr << "\n";
  out << "OS write syscalls: " << os_delta.syscw << "\n";
  out << "Max internal memory bytes: " << boa.max_internal_memory() << "\n";
  out << "Tiers no: " << boa.get_tiers_no() << "\n";

  std::cout << "Output file [" << filename << "]\n";
  out << "End\n";
  out.flush();
  out.close();

  return 0;
}

//! [example]
