/***************************************************************************
 *  examples/containers/unordered_map1.cpp
 *
 *  Part of the STXXL. See http://stxxl.sourceforge.net
 *
 *  Copyright (C) 2013 Daniel Feist <daniel.feist@student.kit.edu>
 *  Copyright (C) 2014 Timo Bingmann <tb@panthema.net>
 *
 *  Distributed under the Boost Software License, Version 1.0.
 *  (See accompanying file LICENSE_1_0.txt or copy at
 *  http://www.boost.org/LICENSE_1_0.txt)
 **************************************************************************/

//! [example]
#include <stxxl/unordered_map>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

//! [hash]
struct HashFunctor
{
    size_t operator () (uint32_t key) const
    {
        return static_cast<size_t>(key * 2654435761u);
    }
};
//! [hash]

//! [comparator]
struct CompareLess
{
    bool operator () (const uint32_t& a, const uint32_t& b) const
    { return a < b; }

    static uint32_t min_value() { return std::numeric_limits<uint32_t>::min(); }
    static uint32_t max_value() { return std::numeric_limits<uint32_t>::max(); }
};
//! [comparator]

stxxl::stats* Stats;

std::tuple<size_t, size_t, size_t, size_t, size_t, size_t, size_t, size_t> get_reads_writes(const stxxl::stats_data& src)
{
    auto delta = (stxxl::stats_data(*Stats) - src);
    return {
        delta.get_reads(),
        delta.get_cached_reads(),
        delta.get_read_volume(),
        delta.get_writes(),
        delta.get_cached_writes(),
        delta.get_written_volume(),
        delta.get_wait_read_time(),
        delta.get_wait_write_time()
    };
}

bool file_exists(const std::string& filename)
{
    std::ifstream f(filename.c_str());
    return f.good();
}

std::string get_available_filename(const std::string& base_name)
{
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
        candidate << stem << "_exp_" << counter << ext;

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

#ifndef SUB_BLOCK_SIZE
#define SUB_BLOCK_SIZE 8192
#endif

#ifndef SUB_BLOCKS_PER_BLOCK
#define SUB_BLOCKS_PER_BLOCK 256
#endif

#ifndef UNORDERED_MAP_INITIAL_BUCKETS
#define UNORDERED_MAP_INITIAL_BUCKETS 10000
#endif

#ifndef UNORDERED_MAP_BUFFER_SIZE
#define UNORDERED_MAP_BUFFER_SIZE (1024 * 1024 * 1024)
#endif

#define IO_DETAILS

#ifndef EXPERIMENTS_NO
#define EXPERIMENTS_NO 1
#endif

#ifndef UNORDERED_MAP_SIZE
#define UNORDERED_MAP_SIZE 10000000
#endif

using KeyType = uint32_t;
using clock_type = std::chrono::high_resolution_clock;

const int sub_block_size = SUB_BLOCK_SIZE;
const int sub_blocks_per_block = SUB_BLOCKS_PER_BLOCK;
const int initial_buckets = UNORDERED_MAP_INITIAL_BUCKETS;
const int buffer_size = UNORDERED_MAP_BUFFER_SIZE;
const int size = UNORDERED_MAP_SIZE;

typedef stxxl::unordered_map<
        KeyType, char, HashFunctor, CompareLess, SUB_BLOCK_SIZE, SUB_BLOCKS_PER_BLOCK> unordered_map_type;

std::string base_result_filename(const char* prefix)
{
    return std::string(prefix)
        + "_sb_" + std::to_string(sub_block_size)
        + "_spb_" + std::to_string(sub_blocks_per_block)
        + "_b_" + std::to_string(initial_buckets)
        + "_buf_" + std::to_string(buffer_size);
}

std::vector<KeyType> create_data(const int size)
{
    std::vector<KeyType> data;
    data.reserve(size);
    for (int i = 1; i <= size; ++i)
    {
        data.push_back(i);
    }
    std::mt19937 gen(12345);
    std::shuffle(data.begin(), data.end(), gen);
    return data;
}

void write_header(std::ostream& out, const std::string& start_msg, const size_t elements)
{
    out << start_msg
        << "\nElements: " << elements
        << "\nsub_block_size: " << sub_block_size
        << "\nsub_blocks_per_block: " << sub_blocks_per_block
        << "\ninitial_buckets: " << initial_buckets
        << "\nbuffer_size: " << buffer_size
        << "\n";
}

void write_map_summary(std::ostream& out, const unordered_map_type& my_map)
{
    out << "Map size: " << my_map.size() << "\n";
    out << "Bucket count: " << my_map.bucket_count() << "\n";
    out << "Load factor: " << my_map.load_factor() << "\n";
}

void insertions_then_queries_benchmark()
{
    unordered_map_type my_map(initial_buckets, HashFunctor(), CompareLess(), buffer_size);

    const int output_every_inserts = 100000;
    const int output_every_queries = 1000;
    const int queries_no = 20000;

    /**************************** create data *****************************/
    std::vector<KeyType> data = create_data(size);

    const std::string filename = get_available_filename(base_result_filename("stxxl_unordered_map") + ".txt");

    std::ofstream out(filename, std::ios::app);
    if (!out)
        throw std::runtime_error("Cannot open results file");

    /*************************** start experiment *************************/
    auto t_start = clock_type::now();

    write_header(out, "Start Unordered Map", data.size());
    write_header(std::cout, "Start Unordered Map", data.size());

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
                << "bytes written " << std::get<5>(details) << ", "
                << "read wait time " << std::get<6>(details) << ", "
                << "write wait time " << std::get<7>(details) << "\n";
#endif
            start_iteration = std::chrono::high_resolution_clock::now();
        }

        my_map.insert(std::pair<KeyType, char>(d, 'a'));
    }

    std::chrono::duration<double, std::milli> insertions_total = std::chrono::high_resolution_clock::now() -
        t_start_insertions;
    out << "End Insertions, time " << insertions_total.count() << "\n";
    std::cout << "End Insertions, time " << insertions_total.count() << "\n";
    out << (stxxl::stats_data(*Stats) - stats_insert_begin);

    /******************************** Queries ***********************************/

    std::mt19937 gen(12345);
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
                << "bytes written " << std::get<5>(details) << ", "
                << "read wait time " << std::get<6>(details) << ", "
                << "write wait time " << std::get<7>(details) << "\n";
#endif
            start_iteration = std::chrono::high_resolution_clock::now();
        }

        auto value = my_map.find(d);
        if (value == my_map.end() || value->first != d)
        {
            std::cout << "wrong result for key " << d << std::endl;
            out << "wrong result for key " << d << std::endl;
            out.flush();
            out.close();
            return;
        }
    }

    std::chrono::duration<double, std::milli> queries_total = std::chrono::high_resolution_clock::now() - t_start_queries;
    out << "End Queries, time " << queries_total.count() << "\n";
    std::cout << "End Queries, time " << queries_total.count() << "\n";
    out << (stxxl::stats_data(*Stats) - stats_queries_begin);

    out << "Total\n"
        << (stxxl::stats_data(*Stats) - stats_begin)
        << std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - t_start).count()
        << "\n";

    std::cout << "Total\n"
        << (stxxl::stats_data(*Stats) - stats_begin)
        << std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - t_start).count()
        << "\n";

    ProcIOStats os_after = ProcIOStats::read_current();
    ProcIOStats os_delta = os_after - os_before;

    std::cout << "OS read_bytes:  " << os_delta.read_bytes << "\n";
    std::cout << "OS write_bytes: " << os_delta.write_bytes << "\n";
    std::cout << "OS read syscalls:  " << os_delta.syscr << "\n";
    std::cout << "OS write syscalls: " << os_delta.syscw << "\n";
    std::cout << "Max internal memory bytes: " << buffer_size << "\n";
    write_map_summary(std::cout, my_map);

    out << "OS read_bytes:  " << os_delta.read_bytes << "\n";
    out << "OS write_bytes: " << os_delta.write_bytes << "\n";
    out << "OS read syscalls:  " << os_delta.syscr << "\n";
    out << "OS write syscalls: " << os_delta.syscw << "\n";
    out << "Max internal memory bytes: " << buffer_size << "\n";
    write_map_summary(out, my_map);

    std::cout << "Output file [" << filename << "]\n";
    out << "End\n";
    out.flush();
    out.close();
}

void insertions_with_queries_benchmark(const int n_insertions_per_batch, const int k_queries_per_batch)
{
    unordered_map_type my_map(initial_buckets, HashFunctor(), CompareLess(), buffer_size);

    /**************************** create data *****************************/
    std::vector<KeyType> data = create_data(size);

    const std::string filename = get_available_filename(
        base_result_filename("stxxl_unordered_map_interleaved")
        + "_" + std::to_string(n_insertions_per_batch)
        + "_" + std::to_string(k_queries_per_batch) + ".txt");

    std::ofstream out(filename, std::ios::app);
    if (!out)
        throw std::runtime_error("Cannot open results file");

    ProcIOStats os_before = ProcIOStats::read_current();

    /*************************** start experiment *************************/
    auto t_start = clock_type::now();

    const std::string start_msg = "Interleaved Start Unordered Map";
    write_header(out, start_msg, data.size());
    out << "Insertions per batch: " << n_insertions_per_batch
        << "\nQueries per batch: " << k_queries_per_batch
        << "\n";

    write_header(std::cout, start_msg, data.size());
    std::cout << "Insertions per batch: " << n_insertions_per_batch
        << "\nQueries per batch: " << k_queries_per_batch
        << "\n";

    Stats = stxxl::stats::get_instance();
    stxxl::stats_data stats_begin(*Stats);

    std::mt19937 gen(12345);
    int total_processed = 0;
    int batch_no = 0;

    while (total_processed < size)
    {
        ++batch_no;
        stxxl::stats_data stats_begin_batch(*Stats);

        /************************* Combined Insertions and Queries ***************************/
        auto t_start_batch = std::chrono::high_resolution_clock::now();

        int insert_end = std::min(total_processed + n_insertions_per_batch, size);
        int n_inserted = 0;

        for (int i = total_processed; i < insert_end; ++i)
        {
            my_map.insert(std::pair<KeyType, char>(data[i], 'a'));
            ++n_inserted;
        }
        total_processed = insert_end;

        auto batch_insert_details = get_reads_writes(stats_begin_batch);

        std::chrono::duration<double, std::milli> insert_elapsed =
            std::chrono::high_resolution_clock::now() - t_start_batch;

        auto t_start_queries = std::chrono::high_resolution_clock::now();
        stxxl::stats_data stats_begin_batch_queries(*Stats);

        int actual_queries = std::min(k_queries_per_batch, total_processed);
        std::uniform_int_distribution<int> distribution(0, total_processed - 1);
        for (int q = 0; q < actual_queries; ++q)
        {
            KeyType const& key = data[distribution(gen)];
            auto value = my_map.find(key);
            if (value == my_map.end() || value->first != key)
            {
                std::cout << "Batch " << batch_no << ": query error for key " << key << "\n";
                out << "Batch " << batch_no << ": query error for key " << key << "\n";
                out.flush();
                out.close();
                return;
            }
        }

        auto batch_query_details = get_reads_writes(stats_begin_batch_queries);

        std::chrono::duration<double, std::milli> queries_elapsed =
            std::chrono::high_resolution_clock::now() - t_start_queries;

        std::chrono::duration<double, std::milli> batch_elapsed =
            std::chrono::high_resolution_clock::now() - t_start_batch;

        std::cout << "Batch " << batch_no
            << " | Insertions " << n_inserted
            << ", Insertions time " << insert_elapsed.count() << " ms"
            << ", Queries " << actual_queries
            << ", Queries time " << queries_elapsed.count() << " ms"
            << ", total time " << batch_elapsed.count() << " ms\n";

#ifndef IO_DETAILS
        out << "Batch " << batch_no
            << " | Insertions " << n_inserted
            << ", Queries " << actual_queries
            << ", total time " << batch_elapsed.count() << "\n";
#else
        out << "Batch " << batch_no
            << " | Insertions " << n_inserted
            << ", Insertions time " << insert_elapsed.count() << " ms"
            << ", Queries " << actual_queries
            << ", Queries time " << queries_elapsed.count() << " ms"
            << ", total time " << batch_elapsed.count() << ", insert details: ["
            << "reads " << std::get<0>(batch_insert_details) << ", "
            << "cached_reads " << std::get<1>(batch_insert_details) << ", "
            << "bytes read " << std::get<2>(batch_insert_details) << ", "
            << "writes " << std::get<3>(batch_insert_details) << ", "
            << "cached writes " << std::get<4>(batch_insert_details) << ", "
            << "bytes written " << std::get<5>(batch_insert_details) << ", "
            << "read wait time " << std::get<6>(batch_insert_details) << ", "
            << "write wait time " << std::get<7>(batch_insert_details) << "], query details: ["
            << "reads " << std::get<0>(batch_query_details) << ", "
            << "cached_reads " << std::get<1>(batch_query_details) << ", "
            << "bytes read " << std::get<2>(batch_query_details) << ", "
            << "writes " << std::get<3>(batch_query_details) << ", "
            << "cached writes " << std::get<4>(batch_query_details) << ", "
            << "bytes written " << std::get<5>(batch_query_details) << ", "
            << "read wait time " << std::get<6>(batch_query_details) << ", "
            << "write wait time " << std::get<7>(batch_query_details) << "]\n";
#endif
    }

    out << "Total\n"
        << (stxxl::stats_data(*Stats) - stats_begin)
        << std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - t_start).count()
        << "\n";

    std::cout << "Total\n"
        << (stxxl::stats_data(*Stats) - stats_begin)
        << std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - t_start).count()
        << "\n";

    ProcIOStats os_after = ProcIOStats::read_current();
    ProcIOStats os_delta = os_after - os_before;

    std::cout << "OS read_bytes:  " << os_delta.read_bytes << "\n";
    std::cout << "OS write_bytes: " << os_delta.write_bytes << "\n";
    std::cout << "OS read syscalls:  " << os_delta.syscr << "\n";
    std::cout << "OS write syscalls: " << os_delta.syscw << "\n";
    std::cout << "Max internal memory bytes: " << buffer_size << "\n";
    write_map_summary(std::cout, my_map);

    out << "OS read_bytes:  " << os_delta.read_bytes << "\n";
    out << "OS write_bytes: " << os_delta.write_bytes << "\n";
    out << "OS read syscalls:  " << os_delta.syscr << "\n";
    out << "OS write syscalls: " << os_delta.syscw << "\n";
    out << "Max internal memory bytes: " << buffer_size << "\n";
    write_map_summary(out, my_map);

    std::cout << "Output file [" << filename << "]\n";
    out << "End\n";
    out.flush();
    out.close();
}

int main()
{
    for (int i = 0; i < EXPERIMENTS_NO; ++i)
    {
        // insertions_then_queries_benchmark();

        int n_insertions_per_batch = 50000;
        int k_queries_per_batch = 2000;
        insertions_with_queries_benchmark(n_insertions_per_batch, k_queries_per_batch);
    }

    return 0;
}
//! [example]
