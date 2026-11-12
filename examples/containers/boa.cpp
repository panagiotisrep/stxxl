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
// #define IN_MEMORY_SORT
// #define ONLINE_SUSPENDED_MODE        // default for --suspended
// #define SORT_BASED_MATERIALIZATION   // default for --materialization (sort)
// #define HYBRID_MATERIALIZATION       // default for --materialization (hybrid)



#include <iostream>
#include <stxxl/bits/utils/hash.h>
#include <stxxl/boa>
#include <stxxl/boa_pv>
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

// reads, cached reads, bytes read, writes, cached writes, bytes written, read wait time (ms), write wait time (ms)
std::tuple<size_t, size_t, size_t, size_t, size_t, size_t, double, double> get_reads_writes(
	const stxxl::stats_data &src) {
	auto delta = (stxxl::stats_data(*Stats) - src);
	return {
		delta.get_reads(),
		delta.get_cached_reads(),
		delta.get_read_volume(),
		delta.get_writes(),
		delta.get_cached_writes(),
		delta.get_written_volume(),
		delta.get_wait_read_time() * 1000,
		delta.get_wait_write_time() * 1000
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

// Online-suspended mode (lazy_insert + consolidate_structure); set from --suspended.
// The materialization strategy is stxxl::materialization (boa_params.h); set from --materialization.
#ifdef ONLINE_SUSPENDED_MODE
bool suspended_mode = true;
#else
bool suspended_mode = false;
#endif

// File-name and header tags for suspended mode, e.g. "static__sort_" and " STATIC SORT ".
std::string suspended_file_tag() {
	if (!suspended_mode)
		return "";
	if (stxxl::materialization == stxxl::materialization_strategy::sort)
		return "static__sort_";
	if (stxxl::materialization == stxxl::materialization_strategy::hybrid)
		return "static__hybrid_";
	return "static_";
}

std::string suspended_header_tag() {
	if (!suspended_mode)
		return "";
	if (stxxl::materialization == stxxl::materialization_strategy::sort)
		return " STATIC SORT ";
	if (stxxl::materialization == stxxl::materialization_strategy::hybrid)
		return " STATIC HYBRID ";
	return " STATIC";
}

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

// Selected at runtime with --boa-pv (not available with BOA_OPT)
typedef stxxl::boa_pv::boa<
	KeyType,
	char,
	uint64_t,
	HashFunction,
	HashCompare,
	lambda,
	pages,
	page_size,
	block_size
> boa_pv_type;

template <typename Boa>
constexpr bool is_boa_pv() {
	return std::is_same<Boa, boa_pv_type>::value;
}

template <typename Boa>
void insertions_then_queries_benchmark(const int pages, const int page_size, const int block_size, const int lambda,
                                       const int size, const int buffer_size, const int queries_no,
                                       const int output_every_inserts, const int output_every_queries,
                                       const KeyDistribution key_dist = KeyDistribution::UniformRandom,
                                       const std::vector<QueryDistribution>& query_dists = {QueryDistribution::Shuffled},
                                       const std::string& log_filename = "") {
	std::string query_dists_name;
	for (QueryDistribution qd : query_dists)
		query_dists_name += (query_dists_name.empty() ? "" : "-") + std::string(to_string(qd));

	Boa boa(buffer_size);

	/**************************** create data *****************************/
	std::vector<KeyType> data;
	generate_data(key_dist, data, size);

#ifdef BOA_OPT
	std::string tmp_filename = std::string("boa_opt_") + suspended_file_tag();
#else
	std::string tmp_filename = (is_boa_pv<Boa>() ? "boa_pv_" : "boa_") + suspended_file_tag();
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
	std::string start_msg = std::string("Start BOA OPT") + suspended_header_tag();
#else
	std::string start_msg = (is_boa_pv<Boa>() ? "Start BOA PV" : "Start BOA") + suspended_header_tag();
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

	if (suspended_mode)
		boa.reserve_lazy_space_for_elements(size);

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
						<< "read wait time " << std::get<6>(details) << " ms, "
						<< "write wait time " << std::get<7>(details) << " ms, "
						<< "boa size " << boa.get_size_bytes() << ", "
						<< "merges " << boa.get_merges_occurred() << "\n";
			}
			boa.reset_merges_occurred();
			start_iteration = std::chrono::high_resolution_clock::now();
		}
		if (suspended_mode)
			boa.lazy_insert(std::pair<unsigned long, char>(d, 'a'));
		else
			boa.insert(std::pair<unsigned long, char>(d, 'a'));
	}

	if (suspended_mode) {
		std::chrono::duration<double, std::milli> insertions_total = std::chrono::high_resolution_clock::now() -
		                                                             t_start_insertions;
		std::cout << "before consolidation, time " << insertions_total.count() << "\n";
		boa.consolidate_structure();
	}

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

#ifndef BOA_OPT
		boa.stats.reset(); // search stats per query distribution
#endif
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
							<< "bytes written " << std::get<5>(details) << ", "
							<< "read wait time " << std::get<6>(details) << " ms, "
							<< "write wait time " << std::get<7>(details) << " ms\n";
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
#ifndef BOA_OPT
		out << "Search stats " << to_string(query_dist) << ": " << boa.stats.info();
		std::cout << "Search stats " << to_string(query_dist) << ": " << boa.stats.info();
#endif
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

	if constexpr (is_boa_pv<Boa>()) {
		std::cout << "Directory bytes: " << boa.get_directory_size_bytes() << "\n";
		std::cout << "Pointer escapes: " << boa.m_stats.pointer_escapes << "\n";
		std::cout << "Pointer max |delta|: " << boa.m_stats.pointer_max_abs_delta << "\n";
		out << "Directory bytes: " << boa.get_directory_size_bytes() << "\n";
		out << "Pointer escapes: " << boa.m_stats.pointer_escapes << "\n";
		out << "Pointer max |delta|: " << boa.m_stats.pointer_max_abs_delta << "\n";
	}

	std::cout << "Output file [" << filename << "]\n";
	out << "End\n";

	for (auto e: boa.m_stats.tier_to_collisions) {
		std::cout << "Tier: " << e.first << " collisions: " << e.second << ", runs: " << boa.active_runs(e.first) <<
				"\n";
	}
	out.flush();
	out.close();
}

// FNV-1a over the key bytes, the same as experiments/competitors/common.h checksum(): equal values prove
// that BOA and the competitors inserted the same keys in the same order.
inline uint64_t keys_checksum(const std::vector<KeyType>& v) {
	uint64_t h = 14695981039346656037ull;
	for (KeyType k : v)
		for (std::size_t i = 0; i < sizeof(KeyType); ++i) {
			h ^= (k >> (8 * i)) & 0xff;
			h *= 1099511628211ull;
		}
	return h;
}

template <typename Boa>
void insertions_with_queries_benchmark(const int pages, const int page_size, const int block_size, const int lambda,
                                       const int size, const int buffer_size, const int n_insertions_per_batch,
                                       const int k_queries_per_batch,
                                       const KeyDistribution key_dist = KeyDistribution::UniformRandom,
                                       const std::string& log_filename = "") {
	Boa boa(buffer_size);

	/**************************** create data *****************************/
	std::vector<KeyType> data;
	generate_data(key_dist, data, size);

#ifdef BOA_OPT
	std::string tmp_filename = std::string("boa_interleaved_opt_") + suspended_file_tag();
#else
	std::string tmp_filename = (is_boa_pv<Boa>() ? "boa_interleaved_pv_" : "boa_interleaved_") + suspended_file_tag();
#endif

	tmp_filename = tmp_filename
	               + std::to_string(lambda)
	               + "_" + std::to_string(stxxl::routing_filter_mult).erase(std::to_string(stxxl::routing_filter_mult).find('.'), 1)
	               + "_" + std::to_string(pages)
	               + "_" + std::to_string(block_size)
	               + "_" + std::to_string(n_insertions_per_batch)
	               + "_" + std::to_string(k_queries_per_batch) + ".txt";

	const std::string filename = log_filename.empty() ? get_available_filename(tmp_filename) : log_filename;

	std::ofstream out(filename, std::ios::app);
	if (!out)
		throw std::runtime_error("Cannot open results file");

	ProcIOStats os_before = ProcIOStats::read_current();

	/*************************** start experiment *************************/
	auto t_start = std::chrono::system_clock::now();

#ifdef BOA_OPT
	std::string start_msg = std::string("Interleaved Start BOA OPT") + suspended_header_tag();
#else
	std::string start_msg = (is_boa_pv<Boa>() ? "Interleaved Start BOA PV" : "Interleaved Start BOA") + suspended_header_tag();
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
			<< "\nKey distribution " << to_string(key_dist)
			<< "\nKeys checksum " << keys_checksum(data)
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

	if (suspended_mode)
		boa.reserve_lazy_space_for_elements(size);

	while (total_processed < size) {
		++batch_no;
		stxxl::stats_data stats_begin_batch(*Stats);

		/************************* Combined Insertions and Queries ***************************/
		auto t_start_batch = std::chrono::high_resolution_clock::now();

		int insert_end = std::min(total_processed + n_insertions_per_batch, size);
		int n_inserted = 0;

		for (int i = total_processed; i < insert_end; ++i) {
			if (suspended_mode)
				boa.lazy_insert(std::pair<KeyType, char>(data[i], 'a'));
			else
				boa.insert(std::pair<KeyType, char>(data[i], 'a'));

			++n_inserted;
		}
		total_processed = insert_end;

		if (suspended_mode)
			boa.consolidate_structure();

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
					<< "read wait time " << std::get<6>(batch_insert_details) << " ms, "
					<< "write wait time " << std::get<7>(batch_insert_details) << " ms], query details: ["
					<< "reads " << std::get<0>(batch_query_details) << ", "
					<< "cached_reads " << std::get<1>(batch_query_details) << ", "
					<< "bytes read " << std::get<2>(batch_query_details) << ", "
					<< "writes " << std::get<3>(batch_query_details) << ", "
					<< "cached writes " << std::get<4>(batch_query_details) << ", "
					<< "bytes written " << std::get<5>(batch_query_details) << ", "
					<< "read wait time " << std::get<6>(batch_query_details) << " ms, "
					<< "write wait time " << std::get<7>(batch_query_details) << " ms], "
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

	if constexpr (is_boa_pv<Boa>()) {
		std::cout << "Directory bytes: " << boa.get_directory_size_bytes() << "\n";
		std::cout << "Pointer escapes: " << boa.m_stats.pointer_escapes << "\n";
		std::cout << "Pointer max |delta|: " << boa.m_stats.pointer_max_abs_delta << "\n";
		out << "Directory bytes: " << boa.get_directory_size_bytes() << "\n";
		out << "Pointer escapes: " << boa.m_stats.pointer_escapes << "\n";
		out << "Pointer max |delta|: " << boa.m_stats.pointer_max_abs_delta << "\n";
	}

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
	int buffer_size = 0; // 0: BOA_BLOCK_SIZE / run element footprint of the chosen implementation
	int experiments_no = 1;
	bool boa_pv = false;
	KeyDistribution key_dist = KeyDistribution::UniformRandom;
	std::vector<QueryDistribution> query_dists = {QueryDistribution::Shuffled};
	double routing_filter_mult = 1;
	bool io_details = true;
	std::string log_filename; // empty: generated from the parameters
	bool interleaved = false; // run insertions_with_queries_benchmark instead of insertions_then_queries_benchmark
	int insertions_per_batch = 50000;
	int queries_per_batch = 2000;
	bool suspended = suspended_mode; // online-suspended mode (default from ONLINE_SUSPENDED_MODE)
	stxxl::materialization_strategy materialization = stxxl::materialization; // default from the strategy macros
};

std::string to_string(stxxl::materialization_strategy m) {
	switch (m) {
		case stxxl::materialization_strategy::sort: return "sort";
		case stxxl::materialization_strategy::hybrid: return "hybrid";
		default: return "merge";
	}
}

void print_usage(const char* prog) {
	const BenchmarkArgs defaults;
	std::cout << "Usage: " << prog << " [options]\n"
		<< "  --size N                  number of elements to insert (default " << defaults.size << ")\n"
		<< "  --queries N               number of queries (default " << defaults.queries_no << ")\n"
		<< "  --output-every-inserts N  report interval for insertions (default " << defaults.output_every_inserts << ")\n"
		<< "  --output-every-queries N  report interval for queries (default " << defaults.output_every_queries << ")\n"
		<< "  --buffer-size N           in-memory buffer size in elements (default "
		<< BOA_BLOCK_SIZE / boa_type::get_run_element_footprint() << ", with --boa-pv "
		<< BOA_BLOCK_SIZE / boa_pv_type::get_run_element_footprint() << ")\n"
		<< "  --repeat N                number of times to run the experiment (default " << defaults.experiments_no << ")\n"
		<< "  --key-dist NAME           key distribution (default " << to_string(defaults.key_dist) << "):";
	for (KeyDistribution d : all_key_distributions) std::cout << " " << to_string(d);
	std::cout << "\n  --query-dist LIST         comma-separated query distributions, run in order on the same structure,"
		<< "\n                            or 'all' (default " << to_string(defaults.query_dists.front()) << "):";
	for (QueryDistribution d : all_query_distributions) std::cout << " " << to_string(d);
	std::cout << "\n  --routing-filter-mult X   routing filter multiplier (default " << defaults.routing_filter_mult << ")"
		<< "\n  --log-file PATH           results file (default: generated from the parameters; appended to if it exists)"
		<< "\n  --no-io-details           omit per-checkpoint I/O details from the results file"
		<< "\n  --boa-pv                  use the boa_pv.h implementation (default " << (defaults.boa_pv ? "on" : "off") << ")"
		<< "\n  --interleaved             run batches of insertions followed by queries (default " << (defaults.interleaved ? "on" : "off") << ")"
		<< "\n  --insertions-per-batch N  with --interleaved: insertions per batch (default " << defaults.insertions_per_batch << ")"
		<< "\n  --queries-per-batch N     with --interleaved: queries per batch (default " << defaults.queries_per_batch << ")"
		<< "\n  --suspended               online-suspended mode: lazy_insert + consolidate_structure (default "
		<< (defaults.suspended ? "on" : "off") << ")"
		<< "\n  --materialization NAME    suspended-mode materialization: merge, sort, hybrid (default "
		<< to_string(defaults.materialization) << "); implies --suspended"
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

		if (opt == "--interleaved") {
			args.interleaved = true;
			continue;
		}

		if (opt == "--suspended") {
			args.suspended = true;
			continue;
		}

		if (opt == "--materialization") {
			if (i + 1 >= argc) {
				std::cerr << "Missing value for " << opt << "\n";
				std::exit(1);
			}
			const std::string value = argv[++i];
			if (value == "merge")
				args.materialization = stxxl::materialization_strategy::merge;
			else if (value == "sort")
				args.materialization = stxxl::materialization_strategy::sort;
			else if (value == "hybrid")
				args.materialization = stxxl::materialization_strategy::hybrid;
			else {
				std::cerr << "Invalid value for " << opt << ": " << value << " (expected merge, sort or hybrid)\n";
				std::exit(1);
			}
			args.suspended = true;
			continue;
		}

		if (opt == "--no-io-details") {
			args.io_details = false;
			continue;
		}

		if (opt == "--boa-pv") {
#ifdef BOA_OPT
			std::cerr << "--boa-pv is not available when compiled with BOA_OPT\n";
			std::exit(1);
#endif
			args.boa_pv = true;
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
		else if (opt == "--insertions-per-batch") target = &args.insertions_per_batch;
		else if (opt == "--queries-per-batch") target = &args.queries_per_batch;
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

template <typename Boa>
void run_experiments(const BenchmarkArgs& args) {
	const int size = args.size;
	const int buffer_size = args.buffer_size > 0
		                        ? args.buffer_size
		                        : BOA_BLOCK_SIZE / Boa::get_run_element_footprint();

	for (int i = 0; i < args.experiments_no; ++i) {
		if (args.interleaved) {
			insertions_with_queries_benchmark<Boa>(pages, page_size, block_size, lambda, size, buffer_size,
			                                       args.insertions_per_batch, args.queries_per_batch,
			                                       args.key_dist, args.log_filename);
		} else {
			insertions_then_queries_benchmark<Boa>(pages, page_size, block_size, lambda, size, buffer_size,
			                                       args.queries_no, args.output_every_inserts, args.output_every_queries,
			                                       args.key_dist, args.query_dists, args.log_filename);
		}
	}
}

int main(int argc, char* argv[]) {
	const BenchmarkArgs args = parse_args(argc, argv);

	io_details = args.io_details;
	stxxl::routing_filter_mult = args.routing_filter_mult;
	suspended_mode = args.suspended;
	stxxl::materialization = args.materialization;

	if (args.boa_pv)
		run_experiments<boa_pv_type>(args);
	else
		run_experiments<boa_type>(args);

	return 0;
}

//! [example]
