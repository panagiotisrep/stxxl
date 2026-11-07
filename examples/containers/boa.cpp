//! [example]

#ifndef BOA_PAGES
#define BOA_PAGES 2
#endif

#ifndef BOA_BLOCK_SIZE
#define BOA_BLOCK_SIZE (32*1024)
#endif

#ifndef BOA_LAMBDA
#define BOA_LAMBDA 16
#endif

// #define BOA_OPT
#define BOA_SEARCH_VIA_BUCKETS
// #define IN_MEMORY_SORT
// #define ONLINE_SUSPENDED_MODE
// #define SORT_BASED_MATERIALIZATION
// #define HYBRID_MATERIALIZATION



#include <iostream>
#include <stxxl/bits/utils/hash.h>
#include <stxxl/boa>
#include <stxxl/boa_opt>
#include <type_traits>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <stdexcept>
#include <tuple>
#include <bytehamster/util/MurmurHash64.h>
#include <stxxl/bits/utils/workloads.h>

//! [comparator]
struct HashCompare {
	bool operator ()(const unsigned long long &a, const unsigned long long &b) const {
		return a < b;
	}

	static unsigned long long max_value() {
		return std::numeric_limits<unsigned long long>::max();
	}
};

//! [comparator]
KWiseHash hash(30); // Example: aim for N ≈ 1e9 items → k ≈ ceil(log2 N) ≈ 30


uint64_t reverse_bits(uint64_t const &x) noexcept {
	using U = typename std::make_unsigned<uint64_t>::type;

	constexpr unsigned W = sizeof(U) * CHAR_BIT;

	U v = static_cast<U>(x);
	U r = 0;

	for (unsigned i = 0; i < W; ++i) {
		r <<= 1;
		r |= (v & 1);
		v >>= 1;
	}

	return static_cast<uint64_t>(r);
}

uint64_t HashFunction(uint32_t const &k) {
	// auto h = hash.hash_uint64(k);
	// auto a = reverse_bits(h);
	// return a;
	return bytehamster::util::MurmurHash64(&k, sizeof(k));
}

stxxl::stats *Stats;

std::tuple<size_t, size_t, size_t, size_t, size_t, size_t, size_t, size_t> get_reads_writes(
	const stxxl::stats_data &src) {
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

bool file_exists(const std::string &filename) {
	std::ifstream f(filename.c_str());
	return f.good();
}

std::string get_available_filename(const std::string &base_name) {
	// // if original name is free
	// if (!file_exists(base_name))
	// 	return base_name;

	// split "boa.txt" → "boa" + ".txt"
	std::string stem = base_name;
	std::string ext;

	std::size_t dot = base_name.find_last_of('.');
	if (dot != std::string::npos) {
		stem = base_name.substr(0, dot);
		ext = base_name.substr(dot);
	}

	int counter = 1;
	while (true) {
		std::ostringstream candidate;
		candidate << stem << "_exp_" << counter << ext;

		if (!file_exists(candidate.str()))
			return candidate.str();

		++counter;
	}
}

struct ProcIOStats {
	uint64_t rchar = 0;
	uint64_t wchar = 0;
	uint64_t syscr = 0;
	uint64_t syscw = 0;
	uint64_t read_bytes = 0;
	uint64_t write_bytes = 0;
	uint64_t cancelled_write_bytes = 0;

	static ProcIOStats read_current() {
		ProcIOStats stats;
		std::ifstream file("/proc/self/io");
		std::string key;
		uint64_t value;

		while (file >> key >> value) {
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

	ProcIOStats operator-(const ProcIOStats &other) const {
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

using KeyType = uint32_t;
const int pages = BOA_PAGES;
const int page_size = 1;
const int block_size = BOA_BLOCK_SIZE;
const int lambda = BOA_LAMBDA;

// Per-checkpoint I/O details in the results file; set from BenchmarkArgs in main
bool io_details = true;

#ifdef BOA_OPT
typedef stxxl::boa_opt::boa<
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
#else
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
#endif



void insertions_then_queries_benchmark(const int pages, const int page_size, const int block_size, const int lambda,
                                       const int size, const int buffer_size, const int queries_no,
                                       const int output_every_inserts, const int output_every_queries,
                                       const KeyDistribution key_dist = KeyDistribution::UniformRandom,
                                       const std::vector<QueryDistribution>& query_dists = {QueryDistribution::Shuffled},
                                       const std::string& log_filename = "") {
	std::string query_dists_name;
	for (QueryDistribution qd : query_dists)
		query_dists_name += (query_dists_name.empty() ? "" : "-") + std::string(to_string(qd));

	boa_type boa(buffer_size);

	/**************************** create data *****************************/
	std::vector<KeyType> data;
	generate_data(key_dist, data, size);

#ifdef BOA_OPT

#ifdef ONLINE_SUSPENDED_MODE
	std::string tmp_filename = "boa_opt_static_";
#ifdef SORT_BASED_MATERIALIZATION
	tmp_filename += "_sort_";
#elif defined(HYBRID_MATERIALIZATION)
	tmp_filename += "_hybrid_";
#endif

#else
	std::string tmp_filename = "boa_opt_";
#endif

#else
#ifdef BOA_SEARCH_VIA_BUCKETS
#ifdef ONLINE_SUSPENDED_MODE
	std::string tmp_filename = "boa_buckets_static_";
#ifdef SORT_BASED_MATERIALIZATION
	tmp_filename += "_sort_";
#elif defined(HYBRID_MATERIALIZATION)
	tmp_filename += "_hybrid_";
#endif
#else
	std::string tmp_filename = "boa_buckets_";
#endif
#else
#ifdef ONLINE_SUSPENDED_MODE
	std::cout << "STATIC MODE NOT SUPPORTED FOR BOA_SEARCH_VIA_NO_BUCKETS" << std::endl;
	exit(-1);
#endif
	std::string tmp_filename = "boa_no_buckets_";
#endif
#endif

	tmp_filename = tmp_filename
	               + std::to_string(lambda)
	               + "_" + std::to_string(stxxl::routing_filter_mult).erase(std::to_string(stxxl::routing_filter_mult).find('.'), 1)
	               + "_" + std::to_string(pages)
	               + "_" + std::to_string(block_size)
	               + "_" + to_string(key_dist)
	               + "_" + query_dists_name + ".txt";

	const std::string filename = log_filename.empty() ? get_available_filename(tmp_filename) : log_filename;

	std::ofstream out(filename, std::ios::app);
	if (!out)
		throw std::runtime_error("Cannot open results file");

	ProcIOStats os_before = ProcIOStats::read_current();

	/*************************** start experiment *************************/
	auto t_start = std::chrono::system_clock::now();

#ifdef BOA_OPT

#ifdef ONLINE_SUSPENDED_MODE
	std::string start_msg = "Start BOA OPT STATIC";
#ifdef SORT_BASED_MATERIALIZATION
	start_msg += " SORT ";
#elif defined(HYBRID_MATERIALIZATION)
	start_msg += " HYBRID ";
#endif
#else
	std::string start_msg = "Start BOA OPT";
#endif

#else
#ifdef BOA_SEARCH_VIA_BUCKETS
#ifdef ONLINE_SUSPENDED_MODE
	std::string start_msg = "Start BOA BUCKETS STATIC";
#ifdef SORT_BASED_MATERIALIZATION
	start_msg += " SORT ";
#elif defined(HYBRID_MATERIALIZATION)
	start_msg += " HYBRID ";
#endif

#else
	std::string start_msg = "Start BOA BUCKETS";
#endif

#else
	std::string start_msg = "Start BOA NO BUCKETS";
#endif
#endif

	start_msg += " " + std::to_string(stxxl::routing_filter_mult).erase(std::to_string(stxxl::routing_filter_mult).find('.'), 1);

	out << start_msg
			<< "\nElements: " << data.size()
			<< "\nBuffer: " << buffer_size
			<< "\nLambda " << lambda
			<< "\nPages " << pages
			<< "\nPage size " << page_size
			<< "\nBlock size " << block_size
			<< "\nKey distribution " << to_string(key_dist)
			<< "\nQuery distributions " << query_dists_name
			<< "\n";

	std::cout << "Start"
			<< "\nElements: " << data.size()
			<< "\nBuffer: " << buffer_size
			<< "\nLambda " << lambda
			<< "\nPages " << pages
			<< "\nPage size " << page_size
			<< "\nBlock size " << block_size
			<< "\nKey distribution " << to_string(key_dist)
			<< "\nQuery distributions " << query_dists_name
			<< "\n";


	Stats = stxxl::stats::get_instance();
	stxxl::stats_data stats_begin(*Stats);


	/************************* Insertions ***********************************/

	stxxl::stats_data stats_insert_begin(*Stats);
	auto t_start_insertions = std::chrono::high_resolution_clock::now();

	std::cout << "Start Insertions\n";
	out << "Start Insertions\n";

	auto start_iteration = std::chrono::high_resolution_clock::now();

#ifdef ONLINE_SUSPENDED_MODE
	boa.reserve_lazy_space_for_elements(size);
#endif

	int counter{0};
	for (auto d: data) {
		++counter;
		if (counter % output_every_inserts == 0) {
			auto end = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double, std::milli> elapsed = end - start_iteration;
			std::cout << counter << " " << elapsed.count() << std::endl;
			if (!io_details) {
				out << "Insertions " << counter << ", time " << elapsed.count() << "\n";
			} else {
				auto details = get_reads_writes(stats_insert_begin);
				out << "Insertions " << counter
						<< ", time " << elapsed.count() << ", details: "
						<< "reads " << std::get<0>(details) << ", "
						<< "cached_reads " << std::get<1>(details) << ", "
						<< "bytes read " << std::get<2>(details) << ", "
						<< "writes " << std::get<3>(details) << ", "
						<< "cached writes " << std::get<4>(details) << ", "
						<< "bytes written " << std::get<5>(details) << ", "
						<< "boa size " << boa.get_size_bytes() << ", "
						<< "merges " << boa.get_merges_occurred() << "\n";
			}
			boa.reset_merges_occurred();
			start_iteration = std::chrono::high_resolution_clock::now();
		}
#ifdef ONLINE_SUSPENDED_MODE
		boa.lazy_insert(std::pair<unsigned long, char>(d, 'a'));
#else
		boa.insert(std::pair<unsigned long, char>(d, 'a'));
#endif
	}

#ifdef ONLINE_SUSPENDED_MODE
	{
		std::chrono::duration<double, std::milli> insertions_total = std::chrono::high_resolution_clock::now() -
		                                                             t_start_insertions;
		std::cout << "before consolidation, time " << insertions_total.count() << "\n";
	}
	// return;
	boa.consolidate_structure();
#endif

	std::chrono::duration<double, std::milli> insertions_total = std::chrono::high_resolution_clock::now() -
	                                                             t_start_insertions;
	out << "End Insertions, time " << insertions_total.count() << "\n";
	std::cout << "End Insertions, time " << insertions_total.count() << "\n";

	out << (stxxl::stats_data(*Stats) - stats_insert_begin);

	/******************************** Queries ***********************************/
	// The structure is built once; each distribution queries the same structure.
	for (QueryDistribution query_dist : query_dists) {
		std::vector<KeyType> queries;
		generate_queries(query_dist, data, queries, queries_no);

		stxxl::stats_data stats_queries_begin(*Stats);
		auto t_start_queries = std::chrono::high_resolution_clock::now();

		std::cout << "Start Queries " << to_string(query_dist) << "\n";
		out << "Start Queries " << to_string(query_dist) << "\n";

		counter = 0;
		start_iteration = std::chrono::high_resolution_clock::now();
		for (const KeyType &d: queries) {
			if (counter == queries_no) {
				break;
			}

			++counter;
			if (counter % output_every_queries == 0) {
				auto end = std::chrono::high_resolution_clock::now();
				std::chrono::duration<double, std::milli> elapsed = end - start_iteration;
				std::cout << counter << " " << elapsed.count() << std::endl;
				if (!io_details) {
					out << "Queries " << counter << ", time " << elapsed.count() << "\n";
				} else {
					auto details = get_reads_writes(stats_queries_begin);
					out << "Queries " << counter
							<< ", time " << elapsed.count() << ", details: "
							<< "reads " << std::get<0>(details) << ", "
							<< "cached_reads " << std::get<1>(details) << ", "
							<< "bytes read " << std::get<2>(details) << ", "
							<< "writes " << std::get<3>(details) << ", "
							<< "cached writes " << std::get<4>(details) << ", "
							<< "bytes written " << std::get<5>(details) << "\n";
				}
				start_iteration = std::chrono::high_resolution_clock::now();
				// break;
			}

			auto value = boa.find(d);
			if (!value) {
				std::cout << "did not find " << d << std::endl;
				out << "did not find " << d << std::endl;
				out.flush();
				out.close();
				return;
			}
			if (value->first != d) {
				std::cout << "wrong result for key" << d << std::endl;
				out << "wrong result for key " << d << std::endl;
				out.flush();
				out.close();
				return;
			}
		}

		std::chrono::duration<double, std::milli> queries_total = std::chrono::high_resolution_clock::now() -
		                                                          t_start_queries;
		out << "End Queries " << to_string(query_dist) << ", time " << queries_total.count() << "\n";
		std::cout << "End Queries " << to_string(query_dist) << ", time " << queries_total.count() << "\n";

		out << (stxxl::stats_data(*Stats) - stats_queries_begin);
	}

	out << "Total\n"
			<< (stxxl::stats_data(*Stats) - stats_begin)
			<< std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - t_start).count()
			<< "\n";

	std::cout << "Total\n"
			<< (stxxl::stats_data(*Stats) - stats_begin)
			<< std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - t_start).count()
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

	for (auto e: boa.m_stats.tier_to_collisions) {
		std::cout << "Tier: " << e.first << " collisions: " << e.second << ", runs: " << boa.active_runs(e.first) <<
				"\n";
	}
	out.flush();
	out.close();
}

void insertions_with_queries_benchmark(const int pages, const int page_size, const int block_size, const int lambda,
                                       const int size, const int buffer_size, const int n_insertions_per_batch,
                                       const int k_queries_per_batch) {
	boa_type boa(buffer_size);

	/**************************** create data *****************************/
	std::vector<KeyType> data;
	uniform_random_data(data, size);

#ifdef BOA_OPT
#ifdef ONLINE_SUSPENDED_MODE
	std::string tmp_filename = "boa_interleaved_opt_static_";
#ifdef SORT_BASED_MATERIALIZATION
	tmp_filename += "_sort_";
#elif defined(HYBRID_MATERIALIZATION)
	tmp_filename += "_hybrid_";
#endif
#else
	std::string tmp_filename = "boa_interleaved_opt_";
#endif

#else
#ifdef BOA_SEARCH_VIA_BUCKETS

#ifdef ONLINE_SUSPENDED_MODE
	std::string tmp_filename = "boa_interleaved_buckets_static_";
#ifdef SORT_BASED_MATERIALIZATION
	tmp_filename += "_sort_";
#elif defined(HYBRID_MATERIALIZATION)
	tmp_filename += "_hybrid_";
#endif
#else
	std::string tmp_filename = "boa_interleaved_buckets_";
#endif

#else
	std::string tmp_filename = "boa_interleaved_no_buckets_";
#endif
#endif

	tmp_filename = tmp_filename
	               + std::to_string(lambda)
	               + "_" + std::to_string(stxxl::routing_filter_mult).erase(std::to_string(stxxl::routing_filter_mult).find('.'), 1)
	               + "_" + std::to_string(pages)
	               + "_" + std::to_string(block_size)
	               + "_" + std::to_string(n_insertions_per_batch)
	               + "_" + std::to_string(k_queries_per_batch) + ".txt";

	const std::string filename = get_available_filename(tmp_filename);

	std::ofstream out(filename, std::ios::app);
	if (!out)
		throw std::runtime_error("Cannot open results file");

	ProcIOStats os_before = ProcIOStats::read_current();

	/*************************** start experiment *************************/
	auto t_start = std::chrono::system_clock::now();

#ifdef BOA_OPT
#ifdef ONLINE_SUSPENDED_MODE
	std::string start_msg = "Interleaved Start BOA OPT STATIC";
#ifdef SORT_BASED_MATERIALIZATION
	start_msg += " SORT ";
#elif defined(HYBRID_MATERIALIZATION)
	start_msg += " HYBRID ";
#endif
#else
	std::string start_msg = "Interleaved Start BOA OPT";
#endif
#else
#ifdef BOA_SEARCH_VIA_BUCKETS
#ifdef ONLINE_SUSPENDED_MODE
	std::string start_msg = "Interleaved Start BOA BUCKETS STATIC";
#ifdef SORT_BASED_MATERIALIZATION
	start_msg += " SORT ";
#elif defined(HYBRID_MATERIALIZATION)
	start_msg += " HYBRID ";
#endif
#else
	std::string start_msg = "Interleaved Start BOA BUCKETS";
#endif
#else
	std::string start_msg = "Interleaved Start BOA NO BUCKETS";
#endif
#endif

	start_msg += " " + std::to_string(stxxl::routing_filter_mult).erase(std::to_string(stxxl::routing_filter_mult).find('.'), 1);

	out << start_msg
			<< "\nElements: " << data.size()
			<< "\nBuffer: " << buffer_size
			<< "\nLambda " << lambda
			<< "\nPages " << pages
			<< "\nPage size " << page_size
			<< "\nBlock size " << block_size
			<< "\nInsertions per batch: " << n_insertions_per_batch
			<< "\nQueries per batch: " << k_queries_per_batch
			<< "\n";

	std::cout << start_msg
			<< "\nElements: " << data.size()
			<< "\nBuffer: " << buffer_size
			<< "\nLambda " << lambda
			<< "\nPages " << pages
			<< "\nPage size " << page_size
			<< "\nBlock size " << block_size
			<< "\nInsertions per batch: " << n_insertions_per_batch
			<< "\nQueries per batch: " << k_queries_per_batch
			<< "\n";

	Stats = stxxl::stats::get_instance();
	stxxl::stats_data stats_begin(*Stats);

	int total_processed = 0;
	int batch_no = 0;

#ifdef ONLINE_SUSPENDED_MODE
	boa.reserve_lazy_space_for_elements(size);
#endif

	while (total_processed < size) {
		++batch_no;
		stxxl::stats_data stats_begin_batch(*Stats);

		/************************* Combined Insertions and Queries ***************************/
		auto t_start_batch = std::chrono::high_resolution_clock::now();

		int insert_end = std::min(total_processed + n_insertions_per_batch, size);
		int n_inserted = 0;

		for (int i = total_processed; i < insert_end; ++i) {
#ifdef ONLINE_SUSPENDED_MODE
			boa.lazy_insert(std::pair<KeyType, char>(data[i], 'a'));
#else
			boa.insert(std::pair<KeyType, char>(data[i], 'a'));
#endif

			++n_inserted;
		}
		total_processed = insert_end;

#ifdef ONLINE_SUSPENDED_MODE
		boa.consolidate_structure();
#endif

		auto batch_insert_details = get_reads_writes(stats_begin_batch);

		std::chrono::duration<double, std::milli> insert_elapsed =
				std::chrono::high_resolution_clock::now() - t_start_batch;

		auto t_start_queries = std::chrono::high_resolution_clock::now();
		stxxl::stats_data stats_begin_batch_queries(*Stats);

		int actual_queries = std::min(k_queries_per_batch, total_processed);
		for (int q = 0; q < actual_queries; ++q) {
			KeyType const &key = data[rand() % total_processed];
			auto value = boa.find(key);
			if (!value || value->first != key) {
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

		if (!io_details) {
			out << "Batch " << batch_no
					<< " | Insertions " << n_inserted
					<< ", Queries " << actual_queries
					<< ", total time " << batch_elapsed.count() << "\n";
		} else {

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
					<< "write wait time " << std::get<7>(batch_query_details) << "], "
					<< "boa size " << boa.get_size_bytes() << ", "
					<< "merges " << boa.get_merges_occurred() << "\n";

#ifndef BOA_OPT
			out << boa.get_structure_info();
			out << boa.stats.info();
			boa.stats.reset();
#endif
		}
	}

	out << "Total\n"
			<< (stxxl::stats_data(*Stats) - stats_begin)
			<< std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - t_start).count()
			<< "\n";

	std::cout << "Total\n"
			<< (stxxl::stats_data(*Stats) - stats_begin)
			<< std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - t_start).count()
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

	for (auto e: boa.m_stats.tier_to_collisions) {
		std::cout << "Tier: " << e.first << " collisions: " << e.second << ", runs: " << boa.active_runs(e.first) <<
				"\n";
	}
	out.flush();
	out.close();
}

// Defaults; can be overridden from the command line (see parse_args)
struct BenchmarkArgs {
	int size = 1000000;
	int queries_no = 20000;
	int output_every_inserts = 100000;
	int output_every_queries = 1000;
	int buffer_size = BOA_BLOCK_SIZE / boa_type::get_run_element_footprint();
	int experiments_no = 1;
	KeyDistribution key_dist = KeyDistribution::UniformRandom;
	std::vector<QueryDistribution> query_dists = {QueryDistribution::Shuffled};
	double routing_filter_mult = 1;
	bool io_details = true;
	std::string log_filename; // empty: generated from the parameters
};

void print_usage(const char* prog) {
	const BenchmarkArgs defaults;
	std::cout << "Usage: " << prog << " [options]\n"
		<< "  --size N                  number of elements to insert (default " << defaults.size << ")\n"
		<< "  --queries N               number of queries (default " << defaults.queries_no << ")\n"
		<< "  --output-every-inserts N  report interval for insertions (default " << defaults.output_every_inserts << ")\n"
		<< "  --output-every-queries N  report interval for queries (default " << defaults.output_every_queries << ")\n"
		<< "  --buffer-size N           in-memory buffer size in elements (default " << defaults.buffer_size << ")\n"
		<< "  --repeat N                number of times to run the experiment (default " << defaults.experiments_no << ")\n"
		<< "  --key-dist NAME           key distribution (default " << to_string(defaults.key_dist) << "):";
	for (KeyDistribution d : all_key_distributions) std::cout << " " << to_string(d);
	std::cout << "\n  --query-dist LIST         comma-separated query distributions, run in order on the same structure,"
		<< "\n                            or 'all' (default " << to_string(defaults.query_dists.front()) << "):";
	for (QueryDistribution d : all_query_distributions) std::cout << " " << to_string(d);
	std::cout << "\n  --routing-filter-mult X   routing filter multiplier (default " << defaults.routing_filter_mult << ")"
		<< "\n  --log-file PATH           results file (default: generated from the parameters; appended to if it exists)"
		<< "\n  --no-io-details           omit per-checkpoint I/O details from the results file"
		<< "\n  --help                    show this message\n";
}

BenchmarkArgs parse_args(int argc, char* argv[]) {
	BenchmarkArgs args;
	for (int i = 1; i < argc; ++i) {
		const std::string opt = argv[i];
		if (opt == "--help" || opt == "-h") {
			print_usage(argv[0]);
			std::exit(0);
		}

		if (opt == "--no-io-details") {
			args.io_details = false;
			continue;
		}

		if (opt == "--routing-filter-mult") {
			if (i + 1 >= argc) {
				std::cerr << "Missing value for " << opt << "\n";
				std::exit(1);
			}
			const std::string value = argv[++i];
			try {
				std::size_t pos = 0;
				const double mult = std::stod(value, &pos);
				if (pos != value.size() || mult <= 0)
					throw std::invalid_argument(value);
				args.routing_filter_mult = mult;
			}
			catch (const std::exception&) {
				std::cerr << "Invalid value for " << opt << ": " << value << " (expected a positive number)\n";
				std::exit(1);
			}
			continue;
		}

		if (opt == "--log-file") {
			if (i + 1 >= argc) {
				std::cerr << "Missing value for " << opt << "\n";
				std::exit(1);
			}
			args.log_filename = argv[++i];
			continue;
		}

		if (opt == "--key-dist" || opt == "--query-dist") {
			if (i + 1 >= argc) {
				std::cerr << "Missing value for " << opt << "\n";
				std::exit(1);
			}
			const std::string value = argv[++i];
			bool ok = true;
			if (opt == "--key-dist") {
				ok = from_string(value, args.key_dist);
			}
			else if (value == "all") {
				args.query_dists.assign(std::begin(all_query_distributions), std::end(all_query_distributions));
			}
			else {
				args.query_dists.clear();
				std::stringstream names(value);
				std::string name;
				while (ok && std::getline(names, name, ',')) {
					QueryDistribution qd;
					ok = from_string(name, qd);
					args.query_dists.push_back(qd);
				}
				ok = ok && !args.query_dists.empty();
			}
			if (!ok) {
				std::cerr << "Invalid value for " << opt << ": " << value << "\n";
				print_usage(argv[0]);
				std::exit(1);
			}
			continue;
		}

		int* target = nullptr;
		if (opt == "--size") target = &args.size;
		else if (opt == "--queries") target = &args.queries_no;
		else if (opt == "--output-every-inserts") target = &args.output_every_inserts;
		else if (opt == "--output-every-queries") target = &args.output_every_queries;
		else if (opt == "--buffer-size") target = &args.buffer_size;
		else if (opt == "--repeat") target = &args.experiments_no;
		else {
			std::cerr << "Unknown option: " << opt << "\n";
			print_usage(argv[0]);
			std::exit(1);
		}

		if (i + 1 >= argc) {
			std::cerr << "Missing value for " << opt << "\n";
			std::exit(1);
		}
		const std::string value = argv[++i];
		try {
			std::size_t pos = 0;
			*target = std::stoi(value, &pos);
			if (pos != value.size() || *target <= 0)
				throw std::invalid_argument(value);
		}
		catch (const std::exception&) {
			std::cerr << "Invalid value for " << opt << ": " << value << " (expected a positive integer)\n";
			std::exit(1);
		}
	}
	return args;
}

int main(int argc, char* argv[]) {
	const BenchmarkArgs args = parse_args(argc, argv);
	const int size = args.size;

	const int buffer_size = args.buffer_size;
	io_details = args.io_details;
	stxxl::routing_filter_mult = args.routing_filter_mult;

	for (int i = 0; i < args.experiments_no; ++i) {
		insertions_then_queries_benchmark(pages, page_size, block_size, lambda, size, buffer_size,
		                                  args.queries_no, args.output_every_inserts, args.output_every_queries,
		                                  args.key_dist, args.query_dists, args.log_filename);

		// int n_insertions_per_batch = 50000;
		// int k_queries_per_batch = 2000;
		// insertions_with_queries_benchmark(pages, page_size, block_size, lambda, size, buffer_size, n_insertions_per_batch,
		// k_queries_per_batch);
	}

	// n_insertions_per_batch = 50000;
	// k_queries_per_batch = 50000;
	// insertions_with_queries_benchmark(pages, page_size, block_size, lambda, size, buffer_size, n_insertions_per_batch,
	// k_queries_per_batch);

	// n_insertions_per_batch = 500;
	// k_queries_per_batch = 50000;
	// insertions_with_queries_benchmark(pages, page_size, block_size, lambda, size, buffer_size, n_insertions_per_batch,
	// k_queries_per_batch);

	return 0;
}

//! [example]
