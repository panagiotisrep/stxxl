// STXXL competitor driver: stxxl::map (B+-tree) or stxxl::unordered_map (external hash map).
//
// One source, two binaries (stxxl/inject.cmake): STXXL_COMPETITOR_MAP → stxxl_map_bench,
// STXXL_COMPETITOR_UNORDERED_MAP → stxxl_unordered_map_bench. Built inside this repository's STXXL, with
// the same compiler flags and STXXL disk configuration as boa. Runs the same workloads as
// examples/containers/boa.cpp, with the same phases, checkpoints and output as the other competitor
// drivers. See experiments/competitors/stxxl/README.md.

#include "common.h" // first: uses std headers only

#include <stxxl/map>
#include <stxxl/stats>
#include <stxxl/unordered_map>

#include <limits>
#include <memory>

#if defined(STXXL_COMPETITOR_MAP) == defined(STXXL_COMPETITOR_UNORDERED_MAP)
#error "Define exactly one of STXXL_COMPETITOR_MAP / STXXL_COMPETITOR_UNORDERED_MAP (see stxxl/inject.cmake)"
#endif

/*********************************** structure ***********************************/

#ifdef STXXL_COMPETITOR_MAP
#define STRUCTURE_HEADER "STXXL map"
#define STRUCTURE_FILE "stxxl_map"

// stxxl::map needs a comparator with max_value(): a sentinel larger than every key (never inserted).
struct CompareLess {
	bool operator()(const KeyType& a, const KeyType& b) const { return a < b; }
	static KeyType max_value() { return std::numeric_limits<KeyType>::max(); }
};

using structure_type = stxxl::map<KeyType, char, CompareLess, STXXL_MAP_NODE_BLOCK_SIZE, STXXL_MAP_LEAF_BLOCK_SIZE>;

#else
#define STRUCTURE_HEADER "STXXL unordered_map"
#define STRUCTURE_FILE "stxxl_unordered_map"

// Same hash and comparator as examples/containers/unordered_map1.cpp.
struct HashFunctor {
	size_t operator()(uint32_t key) const { return static_cast<size_t>(key * 2654435761u); }
};

struct CompareLess {
	bool operator()(const uint32_t& a, const uint32_t& b) const { return a < b; }
	static uint32_t min_value() { return std::numeric_limits<uint32_t>::min(); }
	static uint32_t max_value() { return std::numeric_limits<uint32_t>::max(); }
};

using structure_type = stxxl::unordered_map<KeyType, char, HashFunctor, CompareLess,
                                            STXXL_UM_SUB_BLOCK_SIZE, STXXL_UM_SUB_BLOCKS_PER_BLOCK>;
#endif

/*********************************** arguments ***********************************/

struct Args {
	WorkloadArgs w;
#ifdef STXXL_COMPETITOR_MAP
	// Defaults as in examples/containers/map1.cpp: 1024 blocks of each kind.
	size_t node_cache = size_t(STXXL_MAP_NODE_BLOCK_SIZE) * 1024;
	size_t leaf_cache = size_t(STXXL_MAP_LEAF_BLOCK_SIZE) * 1024;
#else
	// Defaults as in examples/containers/unordered_map1.cpp.
	size_t initial_buckets = 10000;
	size_t buffer_size = size_t(1024) * 1024 * 1024;
	// stxxl::hash_map::tuning (0: keep STXXL's default, which depends on the number of disks)
	size_t block_cache_blocks = 0;   // blocks in the block cache (default 12 per disk)
	size_t prefetch_page_size = 0;   // blocks per prefetch page of the buffered reader (default 2 per disk)
	size_t prefetch_pages = 0;       // prefetch pages of the buffered reader (default 2)
#endif
};

void print_usage(const char* prog) {
	const Args d;
	std::cout << "Usage: " << prog << " [options]\n";
	print_workload_usage(true);
	std::cout << STRUCTURE_HEADER ":"
#ifdef STXXL_COMPETITOR_MAP
		<< "\n  --node-cache B            node cache in bytes (default " << d.node_cache << ")"
		<< "\n  --leaf-cache B            leaf cache in bytes (default " << d.leaf_cache << ")"
		<< "\n  Block sizes (compile time): node " << STXXL_MAP_NODE_BLOCK_SIZE << ", leaf " << STXXL_MAP_LEAF_BLOCK_SIZE
#else
		<< "\n  --initial-buckets N       initial number of buckets (default " << d.initial_buckets << ")"
		<< "\n  --buffer-size B           internal-memory buffer in bytes (default " << d.buffer_size << ")"
		<< "\n  --block-cache-blocks N    stxxl::hash_map::tuning::blockcache_size, blocks (default: STXXL's, 12 per disk)"
		<< "\n  --prefetch-page-size N    tuning::prefetch_page_size, blocks per prefetch page (default: STXXL's, 2 per disk)"
		<< "\n  --prefetch-pages N        tuning::prefetch_pages (default: STXXL's, 2)"
		<< "\n  Block sizes (compile time): sub-block " << STXXL_UM_SUB_BLOCK_SIZE << " bytes, "
		<< STXXL_UM_SUB_BLOCKS_PER_BLOCK << " sub-blocks per block"
#endif
		<< "\n  --help                    show this message\n";
}

Args parse_args(int argc, char* argv[]) {
	Args args;
	for (int i = 1; i < argc; ++i) {
		const std::string opt = argv[i];
		auto bytes = [&]() { return static_cast<size_t>(parse_positive(opt, option_value(argc, argv, i))); };
		if (opt == "--help" || opt == "-h") { print_usage(argv[0]); std::exit(0); }
		else if (parse_workload_option(argc, argv, i, args.w)) continue;
#ifdef STXXL_COMPETITOR_MAP
		else if (opt == "--node-cache") args.node_cache = bytes();
		else if (opt == "--leaf-cache") args.leaf_cache = bytes();
#else
		else if (opt == "--initial-buckets") args.initial_buckets = bytes();
		else if (opt == "--buffer-size") args.buffer_size = bytes();
		else if (opt == "--block-cache-blocks") args.block_cache_blocks = bytes();
		else if (opt == "--prefetch-page-size") args.prefetch_page_size = bytes();
		else if (opt == "--prefetch-pages") args.prefetch_pages = bytes();
#endif
		else { print_usage(argv[0]); fail("Unknown option: " + opt); }
	}
	return args;
}

// Results file name parameters (after "<structure>_").
std::string store_name_params(const Args& args) {
#ifdef STXXL_COMPETITOR_MAP
	return std::to_string(STXXL_MAP_NODE_BLOCK_SIZE) + "_" + std::to_string(args.node_cache)
		+ "_" + std::to_string(STXXL_MAP_LEAF_BLOCK_SIZE) + "_" + std::to_string(args.leaf_cache);
#else
	return std::to_string(STXXL_UM_SUB_BLOCK_SIZE) + "_" + std::to_string(STXXL_UM_SUB_BLOCKS_PER_BLOCK)
		+ "_" + std::to_string(args.initial_buckets) + "_" + std::to_string(args.buffer_size);
#endif
}

/*********************************** store ***********************************/

#ifndef STXXL_COMPETITOR_MAP
// Must run before the unordered_map is created (it reads the tuning in its constructor).
inline void apply_tuning(const Args& args) {
	auto* t = stxxl::hash_map::tuning::get_instance();
	if (args.block_cache_blocks) t->blockcache_size = args.block_cache_blocks;
	if (args.prefetch_page_size) t->prefetch_page_size = args.prefetch_page_size;
	if (args.prefetch_pages) t->prefetch_pages = args.prefetch_pages;
}
#endif

class StxxlStore {
public:
	explicit StxxlStore(const Args& args)
		: args_(args), stats_begin_(*stxxl::stats::get_instance()),
#ifdef STXXL_COMPETITOR_MAP
		  s_(std::make_unique<structure_type>(args.node_cache, args.leaf_cache))
#else
		  s_((apply_tuning(args), std::make_unique<structure_type>(args.initial_buckets, HashFunctor(), CompareLess(), args.buffer_size)))
#endif
	{ }

	void insert(KeyType k) { s_->insert(std::pair<KeyType, char>(k, 'a')); }

	bool find(KeyType k) {
		auto it = s_->find(k);
		return it != s_->end() && it->first == k && it->second == 'a';
	}

	// External memory currently allocated by STXXL (blocks of all STXXL containers in this process).
	uint64_t disk_bytes() const { return stxxl::block_manager::get_instance()->get_current_allocation(); }

	std::string settings() const {
		std::ostringstream ss;
		ss << "STXXL structure " << STRUCTURE_HEADER << "\n";
#ifdef STXXL_COMPETITOR_MAP
		ss << "STXXL map node block size " << STXXL_MAP_NODE_BLOCK_SIZE
		   << "\nSTXXL map leaf block size " << STXXL_MAP_LEAF_BLOCK_SIZE
		   << "\nSTXXL map node cache " << args_.node_cache
		   << "\nSTXXL map leaf cache " << args_.leaf_cache << "\n";
#else
		ss << "STXXL unordered_map sub-block size " << STXXL_UM_SUB_BLOCK_SIZE
		   << "\nSTXXL unordered_map sub-blocks per block " << STXXL_UM_SUB_BLOCKS_PER_BLOCK
		   << "\nSTXXL unordered_map initial buckets " << args_.initial_buckets
		   << "\nSTXXL unordered_map buffer size " << args_.buffer_size
		   << "\nSTXXL unordered_map block cache blocks " << stxxl::hash_map::tuning::get_instance()->blockcache_size
		   << "\nSTXXL unordered_map prefetch page size " << stxxl::hash_map::tuning::get_instance()->prefetch_page_size
		   << "\nSTXXL unordered_map prefetch pages " << stxxl::hash_map::tuning::get_instance()->prefetch_pages << "\n";
#endif
		ss << "Key encoding uint32_t key, char value 'a'\n";
		return ss.str();
	}

	uint64_t internal_memory_bytes() const {
#ifdef STXXL_COMPETITOR_MAP
		return args_.node_cache + args_.leaf_cache;
#else
		return args_.buffer_size;
#endif
	}

	const stxxl::stats_data& stats_begin() const { return stats_begin_; }

private:
	const Args& args_;
	stxxl::stats_data stats_begin_;
	std::unique_ptr<structure_type> s_;
};

void final_report(Log& log, StxxlStore& store, const ProcIOStats& os_before, Clock::time_point t_start) {
	report_totals(log, os_before, t_start);
	log << "Max internal memory bytes: " << store.internal_memory_bytes() << "\n"
	    << "STXXL allocated bytes: " << store.disk_bytes() << "\n"
	    << "STXXL I/O statistics (whole experiment):\n"
	    << (stxxl::stats_data(*stxxl::stats::get_instance()) - store.stats_begin())
	    << "End\n";
}

/*********************************** benchmarks ***********************************/

// Mirrors insertions_then_queries_benchmark in examples/containers/boa.cpp.
void insertions_then_queries(const Args& args, StxxlStore& store, const std::vector<KeyType>& data) {

	const std::string query_dists_name = args.w.query_dists_name();

	const std::string tmp_filename = STRUCTURE_FILE "_"
		+ store_name_params(args)
		+ "_" + to_string(args.w.key_dist) + "_" + query_dists_name + ".txt";
	const std::string filename = args.w.log_filename.empty() ? get_available_filename(tmp_filename) : args.w.log_filename;

	Log log(filename);

	const ProcIOStats os_before = ProcIOStats::read_current();
	const auto t_start = Clock::now();

	log << "Start " STRUCTURE_HEADER
	    << "\nElements: " << data.size()
	    << "\nKey distribution " << to_string(args.w.key_dist)
	    << "\nQuery distributions " << query_dists_name
	    << "\nKeys checksum " << checksum(data)
	    << "\n" << store.settings();

	/******************************** Insertions ********************************/
	log << "Start Insertions\n";
	ProcIOStats io_phase = ProcIOStats::read_current();
	auto t_start_insertions = Clock::now();
	auto start_iteration = Clock::now();
	int counter = 0;
	for (KeyType d : data) {
		++counter;
		if (counter % args.w.output_every_inserts == 0) {
			const Ms elapsed = Clock::now() - start_iteration;
			if (!args.w.io_details) {
				log << "Insertions " << counter << ", time " << elapsed.count() << "\n";
			} else {
				log << "Insertions " << counter << ", time " << elapsed.count() << ", details: "
				    << io_details(ProcIOStats::read_current() - io_phase)
				    << ", disk bytes " << store.disk_bytes() << "\n";
			}
			start_iteration = Clock::now();
		}
		store.insert(d);
	}
	const Ms insertions_total = Clock::now() - t_start_insertions;
	log << "End Insertions, time " << insertions_total.count() << "\n"
	    << "Phase I/O: " << io_details(ProcIOStats::read_current() - io_phase) << "\n";

	/******************************** Queries ********************************/
	for (QueryDistribution query_dist : args.w.query_dists) {
		std::vector<KeyType> queries;
		generate_queries(query_dist, data, queries, args.w.queries_no);

		log << "Start Queries " << to_string(query_dist) << "\n"
		    << "Queries checksum " << to_string(query_dist) << " " << checksum(queries) << "\n";
		io_phase = ProcIOStats::read_current();
		const auto t_start_queries = Clock::now();
		start_iteration = Clock::now();
		counter = 0;
		for (KeyType d : queries) {
			if (counter == args.w.queries_no) break;
			++counter;
			if (counter % args.w.output_every_queries == 0) {
				const Ms elapsed = Clock::now() - start_iteration;
				if (!args.w.io_details) {
					log << "Queries " << counter << ", time " << elapsed.count() << "\n";
				} else {
					log << "Queries " << counter << ", time " << elapsed.count() << ", details: "
					    << io_details(ProcIOStats::read_current() - io_phase) << "\n";
				}
				start_iteration = Clock::now();
			}
			if (!store.find(d)) {
				log << "did not find " << d << "\n";
				return;
			}
		}
		const Ms queries_total = Clock::now() - t_start_queries;
		log << "End Queries " << to_string(query_dist) << ", time " << queries_total.count() << "\n"
		    << "Phase I/O: " << io_details(ProcIOStats::read_current() - io_phase) << "\n";
	}

	final_report(log, store, os_before, t_start);
	std::cout << "Output file [" << filename << "]\n";
}

// Mirrors insertions_with_queries_benchmark in examples/containers/boa.cpp: keys from --key-dist, and
// query keys picked with rand() exactly as boa does (same glibc sequence, never reseeded).
void insertions_with_queries(const Args& args, StxxlStore& store, const std::vector<KeyType>& data) {
	const int size = args.w.size;

	const std::string tmp_filename = STRUCTURE_FILE "_interleaved_"
		+ store_name_params(args)
		+ "_" + std::to_string(args.w.insertions_per_batch) + "_" + std::to_string(args.w.queries_per_batch) + ".txt";
	const std::string filename = args.w.log_filename.empty() ? get_available_filename(tmp_filename) : args.w.log_filename;

	Log log(filename);

	const ProcIOStats os_before = ProcIOStats::read_current();
	const auto t_start = Clock::now();

	log << "Interleaved Start " STRUCTURE_HEADER
	    << "\nElements: " << data.size()
	    << "\nKey distribution " << to_string(args.w.key_dist)
	    << "\nInsertions per batch: " << args.w.insertions_per_batch
	    << "\nQueries per batch: " << args.w.queries_per_batch
	    << "\nKeys checksum " << checksum(data)
	    << "\n" << store.settings();

	int total_processed = 0;
	int batch_no = 0;
	while (total_processed < size) {
		++batch_no;
		const ProcIOStats io_batch = ProcIOStats::read_current();
		const auto t_start_batch = Clock::now();

		const int insert_end = std::min(total_processed + args.w.insertions_per_batch, size);
		int n_inserted = 0;
		for (int i = total_processed; i < insert_end; ++i) {
			store.insert(data[i]);
			++n_inserted;
		}
		total_processed = insert_end;
		const ProcIOStats insert_io = ProcIOStats::read_current() - io_batch;
		const Ms insert_elapsed = Clock::now() - t_start_batch;

		const ProcIOStats io_queries = ProcIOStats::read_current();
		const auto t_start_queries = Clock::now();
		const int actual_queries = std::min(args.w.queries_per_batch, total_processed);
		for (int q = 0; q < actual_queries; ++q) {
			const KeyType key = data[rand() % total_processed];
			if (!store.find(key)) {
				log << "Batch " << batch_no << ": query error for key " << key << "\n";
				return;
			}
		}
		const ProcIOStats query_io = ProcIOStats::read_current() - io_queries;
		const Ms queries_elapsed = Clock::now() - t_start_queries;
		const Ms batch_elapsed = Clock::now() - t_start_batch;

		log << "Batch " << batch_no
		    << " | Insertions " << n_inserted
		    << ", Insertions time " << insert_elapsed.count() << " ms"
		    << ", Queries " << actual_queries
		    << ", Queries time " << queries_elapsed.count() << " ms"
		    << ", total time " << batch_elapsed.count() << " ms";
		if (args.w.io_details) {
			log << ", insert details: [" << io_details(insert_io) << "]"
			    << ", query details: [" << io_details(query_io) << "]"
			    << ", disk bytes " << store.disk_bytes();
		}
		log << "\n";
	}

	final_report(log, store, os_before, t_start);
	std::cout << "Output file [" << filename << "]\n";
}


int main(int argc, char* argv[]) {
	const Args args = parse_args(argc, argv);
	for (int i = 0; i < args.w.experiments_no; ++i) {
		// Keys first, then the memory baseline, then the structure (see memory_baseline in common.h).
		std::vector<KeyType> data;
		generate_data(args.w.key_dist, data, args.w.size);
		memory_baseline(args.w);
		StxxlStore store(args); // fresh structure per experiment
		if (args.w.interleaved)
			insertions_with_queries(args, store, data);
		else
			insertions_then_queries(args, store, data);
	}
	return 0;
}
