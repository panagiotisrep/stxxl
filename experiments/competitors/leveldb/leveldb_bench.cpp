// LevelDB competitor driver.
//
// Runs the same workloads as examples/containers/boa.cpp: same key/query generators and seeds
// (include/stxxl/bits/utils/workloads.h), same key type, same command-line options for the workload,
// same phase structure and checkpoints. I/O is measured from /proc/self/io (LevelDB has no STXXL
// counters). Every LevelDB setting defaults to LevelDB's own default and is written to the results file.
// See experiments/competitors/leveldb/README.md.

#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <leveldb/options.h>

#include "common.h"

#include <algorithm>
#include <filesystem>
#include <memory>

/*********************************** arguments ***********************************/

struct Args {
	WorkloadArgs w;

	// LevelDB. Defaults are taken from a default-constructed leveldb::Options (LevelDB's own defaults).
	std::string db_dir = "leveldb_bench_db";
	size_t write_buffer_size = leveldb::Options().write_buffer_size;
	size_t max_file_size = leveldb::Options().max_file_size;
	size_t block_size = leveldb::Options().block_size;
	int block_restart_interval = leveldb::Options().block_restart_interval;
	int max_open_files = leveldb::Options().max_open_files;
	size_t cache_size = 0; // 0: LevelDB's internal default block cache (8 MiB)
	int bloom_bits = 0;    // 0: no filter policy (LevelDB default)
	bool sync = false;     // WriteOptions::sync (LevelDB default: false)
	bool compact_before_queries = true;
	bool keep_db = false;
};

void print_usage(const char* prog) {
	const Args d;
	std::cout << "Usage: " << prog << " [options]\n";
	print_workload_usage(true);
	std::cout << "LevelDB (defaults are LevelDB's own):"
		<< "\n  --db-dir PATH             database directory, destroyed before each experiment (default " << d.db_dir << ")"
		<< "\n  --write-buffer-size B     Options::write_buffer_size in bytes (default " << d.write_buffer_size << ")"
		<< "\n  --max-file-size B         Options::max_file_size in bytes (default " << d.max_file_size << ")"
		<< "\n  --block-size B            Options::block_size in bytes (default " << d.block_size << ")"
		<< "\n  --block-restart-interval N Options::block_restart_interval (default " << d.block_restart_interval << ")"
		<< "\n  --max-open-files N        Options::max_open_files (default " << d.max_open_files << ")"
		<< "\n  --cache-size B            block cache in bytes (default: LevelDB's internal 8 MiB cache)"
		<< "\n  --bloom-bits N            Bloom filter bits per key (default: no filter)"
		<< "\n  --sync                    WriteOptions::sync = true (default false)"
		<< "\n  --no-compact-before-queries  skip the CompactRange over the whole key range that runs after the"
		<< "\n                              insertions (default: compact; timed separately as Compaction)"
		<< "\n  --keep-db                 do not destroy the database after the experiment"
		<< "\n  --help                    show this message\n";
}

Args parse_args(int argc, char* argv[]) {
	Args args;
	for (int i = 1; i < argc; ++i) {
		const std::string opt = argv[i];
		auto bytes = [&]() { return static_cast<size_t>(parse_positive(opt, option_value(argc, argv, i))); };
		auto positive_int = [&]() { return static_cast<int>(parse_positive(opt, option_value(argc, argv, i))); };

		if (opt == "--help" || opt == "-h") { print_usage(argv[0]); std::exit(0); }
		else if (parse_workload_option(argc, argv, i, args.w)) continue;
		else if (opt == "--sync") args.sync = true;
		else if (opt == "--compact-before-queries") args.compact_before_queries = true;
		else if (opt == "--no-compact-before-queries") args.compact_before_queries = false;
		else if (opt == "--keep-db") args.keep_db = true;
		else if (opt == "--db-dir") args.db_dir = option_value(argc, argv, i);
		else if (opt == "--write-buffer-size") args.write_buffer_size = bytes();
		else if (opt == "--max-file-size") args.max_file_size = bytes();
		else if (opt == "--block-size") args.block_size = bytes();
		else if (opt == "--block-restart-interval") args.block_restart_interval = positive_int();
		else if (opt == "--max-open-files") args.max_open_files = positive_int();
		else if (opt == "--cache-size") args.cache_size = bytes();
		else if (opt == "--bloom-bits") args.bloom_bits = positive_int();
		else { print_usage(argv[0]); fail("Unknown option: " + opt); }
	}
	return args;
}

/*********************************** LevelDB ***********************************/

// 4-byte big-endian key: LevelDB's bytewise order equals numeric key order.
struct EncodedKey {
	char bytes[sizeof(KeyType)];
	explicit EncodedKey(KeyType k) {
		for (std::size_t i = 0; i < sizeof(KeyType); ++i)
			bytes[i] = static_cast<char>((k >> (8 * (sizeof(KeyType) - 1 - i))) & 0xff);
	}
	leveldb::Slice slice() const { return leveldb::Slice(bytes, sizeof(bytes)); }
};

const leveldb::Slice kValue("a", 1); // 1-byte value, like boa.cpp's char 'a'

class LevelDbStore {
public:
	explicit LevelDbStore(const Args& args) : args_(args) {
		if (args.cache_size > 0) cache_.reset(leveldb::NewLRUCache(args.cache_size));
		if (args.bloom_bits > 0) filter_.reset(leveldb::NewBloomFilterPolicy(args.bloom_bits));

		options_.create_if_missing = true;
		options_.error_if_exists = true;
		options_.write_buffer_size = args.write_buffer_size;
		options_.max_file_size = args.max_file_size;
		options_.block_size = args.block_size;
		options_.block_restart_interval = args.block_restart_interval;
		options_.max_open_files = args.max_open_files;
		options_.block_cache = cache_.get();
		options_.filter_policy = filter_.get();
		// compression: left at LevelDB's default (kSnappyCompression). leveldb/build.sh builds LevelDB
		// without Snappy, so blocks are stored uncompressed (see README).
		write_options_.sync = args.sync;

		leveldb::DestroyDB(args.db_dir, leveldb::Options());
		leveldb::DB* db = nullptr;
		const leveldb::Status s = leveldb::DB::Open(options_, args.db_dir, &db);
		if (!s.ok()) fail("Cannot open LevelDB at " + args.db_dir + ": " + s.ToString());
		db_.reset(db);
	}

	~LevelDbStore() {
		db_.reset(); // close before destroying the files
		if (!args_.keep_db) leveldb::DestroyDB(args_.db_dir, leveldb::Options());
	}

	void insert(KeyType k) {
		const EncodedKey key(k);
		const leveldb::Status s = db_->Put(write_options_, key.slice(), kValue);
		if (!s.ok()) fail("Put failed: " + s.ToString());
	}

	bool find(KeyType k) {
		const EncodedKey key(k);
		const leveldb::Status s = db_->Get(read_options_, key.slice(), &value_);
		return s.ok() && value_ == kValue.ToString();
	}

	void compact() { db_->CompactRange(nullptr, nullptr); }

	std::string property(const std::string& name) {
		std::string v;
		return db_->GetProperty(name, &v) ? v : "n/a";
	}

	uint64_t disk_bytes() const {
		uint64_t total = 0;
		std::error_code ec;
		for (const auto& e : std::filesystem::recursive_directory_iterator(args_.db_dir, ec))
			if (e.is_regular_file(ec)) total += e.file_size(ec);
		return total;
	}

	// Every setting in effect, one per line.
	std::string settings() const {
		std::ostringstream ss;
		ss << "LevelDB version " << leveldb::kMajorVersion << "." << leveldb::kMinorVersion
		   << "\nLevelDB db dir " << args_.db_dir
		   << "\nLevelDB write_buffer_size " << options_.write_buffer_size
		   << "\nLevelDB max_file_size " << options_.max_file_size
		   << "\nLevelDB block_size " << options_.block_size
		   << "\nLevelDB block_restart_interval " << options_.block_restart_interval
		   << "\nLevelDB max_open_files " << options_.max_open_files
		   << "\nLevelDB block cache " << (args_.cache_size > 0 ? std::to_string(args_.cache_size) : "default (8388608)")
		   << "\nLevelDB filter " << (args_.bloom_bits > 0 ? "bloom " + std::to_string(args_.bloom_bits) + " bits/key" : "none")
		   << "\nLevelDB compression option " << (options_.compression == leveldb::kSnappyCompression ? "snappy" : "none")
		   << " (built without Snappy: stored uncompressed)"
		   << "\nLevelDB paranoid_checks " << options_.paranoid_checks
		   << "\nLevelDB reuse_logs " << options_.reuse_logs
		   << "\nLevelDB sync writes " << write_options_.sync
		   << "\nLevelDB verify_checksums " << read_options_.verify_checksums
		   << "\nLevelDB fill_cache " << read_options_.fill_cache
		   << "\nLevelDB compact before queries " << args_.compact_before_queries
		   << "\nKey encoding " << sizeof(KeyType) << "-byte big-endian, value 1 byte\n";
		return ss.str();
	}

private:
	const Args& args_;
	std::unique_ptr<leveldb::Cache> cache_;
	std::unique_ptr<const leveldb::FilterPolicy> filter_;
	leveldb::Options options_;
	leveldb::WriteOptions write_options_;
	leveldb::ReadOptions read_options_;
	std::unique_ptr<leveldb::DB> db_;
	std::string value_;
};

void final_report(Log& log, LevelDbStore& store, const ProcIOStats& os_before,
                  Clock::time_point t_start) {
	report_totals(log, os_before, t_start);
	log << "LevelDB approximate memory usage: " << store.property("leveldb.approximate-memory-usage") << "\n"
	    << "LevelDB disk bytes: " << store.disk_bytes() << "\n"
	    << "LevelDB files per level:";
	for (int level = 0; level < 7; ++level)
		log << " L" << level << " " << store.property("leveldb.num-files-at-level" + std::to_string(level));
	log << "\nLevelDB stats:\n" << store.property("leveldb.stats") << "End\n";
}

/*********************************** benchmarks ***********************************/

// Mirrors insertions_then_queries_benchmark in examples/containers/boa.cpp.
void insertions_then_queries(const Args& args) {
	std::vector<KeyType> data;
	generate_data(args.w.key_dist, data, args.w.size);

	const std::string query_dists_name = args.w.query_dists_name();

	const std::string tmp_filename = "leveldb_"
		+ std::to_string(args.write_buffer_size) + "_" + std::to_string(args.block_size)
		+ "_" + std::to_string(args.cache_size) + "_" + std::to_string(args.bloom_bits)
		+ "_" + to_string(args.w.key_dist) + "_" + query_dists_name + ".txt";
	const std::string filename = args.w.log_filename.empty() ? get_available_filename(tmp_filename) : args.w.log_filename;

	Log log(filename);

	LevelDbStore store(args);
	const ProcIOStats os_before = ProcIOStats::read_current();
	const auto t_start = Clock::now();

	log << "Start LevelDB"
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

	if (args.compact_before_queries) {
		io_phase = ProcIOStats::read_current();
		const auto t0 = Clock::now();
		store.compact();
		const Ms compaction = Clock::now() - t0;
		log << "Compaction, time " << compaction.count() << "\n"
		    << "Phase I/O: " << io_details(ProcIOStats::read_current() - io_phase) << "\n"
		    << "Insertions + compaction, time " << (insertions_total + compaction).count() << "\n";
	}

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

// Mirrors insertions_with_queries_benchmark in examples/containers/boa.cpp: uniform keys, and query keys
// picked with rand() exactly as boa does (same glibc sequence, never reseeded).
void insertions_with_queries(const Args& args) {
	const int size = args.w.size;
	std::vector<KeyType> data;
	uniform_random_data(data, size);

	const std::string tmp_filename = "leveldb_interleaved_"
		+ std::to_string(args.write_buffer_size) + "_" + std::to_string(args.block_size)
		+ "_" + std::to_string(args.cache_size) + "_" + std::to_string(args.bloom_bits)
		+ "_" + std::to_string(args.w.insertions_per_batch) + "_" + std::to_string(args.w.queries_per_batch) + ".txt";
	const std::string filename = args.w.log_filename.empty() ? get_available_filename(tmp_filename) : args.w.log_filename;

	Log log(filename);

	LevelDbStore store(args);
	const ProcIOStats os_before = ProcIOStats::read_current();
	const auto t_start = Clock::now();

	log << "Interleaved Start LevelDB"
	    << "\nElements: " << data.size()
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
		if (args.w.interleaved)
			insertions_with_queries(args);
		else
			insertions_then_queries(args);
	}
	return 0;
}
