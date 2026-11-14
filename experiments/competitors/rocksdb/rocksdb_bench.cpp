// RocksDB competitor driver.
//
// Runs the same workloads as examples/containers/boa.cpp, with the same phases, checkpoints and output
// as leveldb/leveldb_bench.cpp. Every RocksDB setting defaults to RocksDB's own default and is written to
// the results file. See experiments/competitors/rocksdb/README.md.

#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/version.h>

#include "common.h"

#include <algorithm>
#include <filesystem>
#include <memory>

/*********************************** arguments ***********************************/

struct Args {
	WorkloadArgs w;

	// RocksDB. Defaults are taken from default-constructed rocksdb::Options / BlockBasedTableOptions
	// (RocksDB's own defaults).
	std::string db_dir = "rocksdb_bench_db";
	size_t write_buffer_size = rocksdb::Options().write_buffer_size;
	uint64_t target_file_size_base = rocksdb::Options().target_file_size_base;
	int max_background_jobs = rocksdb::Options().max_background_jobs;
	uint64_t block_size = rocksdb::BlockBasedTableOptions().block_size;
	size_t cache_size = 0;      // 0: RocksDB's internal default block cache (32 MiB)
	int bloom_bits = 0;         // 0: no filter policy (RocksDB default)
	bool cache_index_and_filter_blocks = rocksdb::BlockBasedTableOptions().cache_index_and_filter_blocks;
	bool direct_reads = rocksdb::Options().use_direct_reads;
	bool direct_writes = rocksdb::Options().use_direct_io_for_flush_and_compaction;
	size_t compaction_readahead_size = rocksdb::Options().compaction_readahead_size;
	size_t writable_file_max_buffer_size = rocksdb::Options().writable_file_max_buffer_size;
	bool sync = false;          // WriteOptions::sync (RocksDB default: false)
	bool compact_before_queries = true;
	bool bulk_load = false;     // auto compactions off; CompactRange consolidates (see --bulk-load)
	bool keep_db = false;
};

void print_usage(const char* prog) {
	const Args d;
	std::cout << "Usage: " << prog << " [options]\n";
	print_workload_usage(true);
	std::cout << "RocksDB (defaults are RocksDB's own):"
		<< "\n  --db-dir PATH             database directory, destroyed before each experiment (default " << d.db_dir << ")"
		<< "\n  --write-buffer-size B     Options::write_buffer_size in bytes (default " << d.write_buffer_size << ")"
		<< "\n  --target-file-size B      Options::target_file_size_base in bytes (default " << d.target_file_size_base << ")"
		<< "\n  --max-background-jobs N   Options::max_background_jobs (default " << d.max_background_jobs << ")"
		<< "\n  --block-size B            BlockBasedTableOptions::block_size in bytes (default " << d.block_size << ")"
		<< "\n  --cache-size B            block cache in bytes (default: RocksDB's internal 32 MiB cache)"
		<< "\n  --bloom-bits N            Bloom filter bits per key (default: no filter)"
		<< "\n  --cache-index-and-filter-blocks  BlockBasedTableOptions::cache_index_and_filter_blocks = true:"
		<< "\n                            index/filter blocks count against --cache-size (default false)"
		<< "\n  --direct-reads            Options::use_direct_reads = true (default false)"
		<< "\n  --direct-writes           Options::use_direct_io_for_flush_and_compaction = true (default false)"
		<< "\n  --compaction-readahead-size B  Options::compaction_readahead_size: read-ahead buffer per compaction"
		<< "\n                            input file, in bytes (default " << d.compaction_readahead_size << ")"
		<< "\n  --writable-file-max-buffer-size B  Options::writable_file_max_buffer_size: write buffer per file"
		<< "\n                            being written (WAL, flush and compaction output), in bytes (default "
		<< d.writable_file_max_buffer_size << ")"
		<< "\n  --sync                    WriteOptions::sync = true (default false)"
		<< "\n  --no-compact-before-queries  skip the CompactRange over the whole key range that runs after the"
		<< "\n                              insertions (default: compact; timed separately as Compaction)"
		<< "\n  --bulk-load               bulk-load mode, RocksDB's counterpart of BOA's online-suspended mode:"
		<< "\n                            Options::disable_auto_compactions = true, so insertions only fill"
		<< "\n                            memtables and flush them to L0 files; a full CompactRange then builds"
		<< "\n                            the levels: after all insertions (insert-then-query; always, even with"
		<< "\n                            --no-compact-before-queries) or after every batch (--interleaved, counted"
		<< "\n                            in the batch's insertion time)"
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
		else if (opt == "--direct-reads") args.direct_reads = true;
		else if (opt == "--cache-index-and-filter-blocks") args.cache_index_and_filter_blocks = true;
		else if (opt == "--direct-writes") args.direct_writes = true;
		else if (opt == "--compact-before-queries") args.compact_before_queries = true;
		else if (opt == "--no-compact-before-queries") args.compact_before_queries = false;
		else if (opt == "--keep-db") args.keep_db = true;
		else if (opt == "--bulk-load") args.bulk_load = true;
		else if (opt == "--db-dir") args.db_dir = option_value(argc, argv, i);
		else if (opt == "--write-buffer-size") args.write_buffer_size = bytes();
		else if (opt == "--target-file-size") args.target_file_size_base = bytes();
		else if (opt == "--max-background-jobs") args.max_background_jobs = positive_int();
		else if (opt == "--block-size") args.block_size = bytes();
		else if (opt == "--cache-size") args.cache_size = bytes();
		else if (opt == "--bloom-bits") args.bloom_bits = positive_int();
		else if (opt == "--compaction-readahead-size") args.compaction_readahead_size = bytes();
		else if (opt == "--writable-file-max-buffer-size") args.writable_file_max_buffer_size = bytes();
		else { print_usage(argv[0]); fail("Unknown option: " + opt); }
	}
	if (args.bulk_load) args.compact_before_queries = true; // the compaction is the consolidation step
	return args;
}

/*********************************** RocksDB ***********************************/

// 4-byte big-endian key: RocksDB's default bytewise order equals numeric key order.
struct EncodedKey {
	char bytes[sizeof(KeyType)];
	explicit EncodedKey(KeyType k) {
		for (std::size_t i = 0; i < sizeof(KeyType); ++i)
			bytes[i] = static_cast<char>((k >> (8 * (sizeof(KeyType) - 1 - i))) & 0xff);
	}
	rocksdb::Slice slice() const { return rocksdb::Slice(bytes, sizeof(bytes)); }
};

const rocksdb::Slice kValue("a", 1); // 1-byte value, like boa.cpp's char 'a'

class RocksDbStore {
public:
	explicit RocksDbStore(const Args& args) : args_(args) {
		table_options_.block_size = args.block_size;
		table_options_.cache_index_and_filter_blocks = args.cache_index_and_filter_blocks;
		if (args.cache_size > 0) table_options_.block_cache = rocksdb::NewLRUCache(args.cache_size);
		if (args.bloom_bits > 0) table_options_.filter_policy.reset(rocksdb::NewBloomFilterPolicy(args.bloom_bits));

		options_.create_if_missing = true;
		options_.error_if_exists = true;
		options_.write_buffer_size = args.write_buffer_size;
		options_.target_file_size_base = args.target_file_size_base;
		options_.max_background_jobs = args.max_background_jobs;
		options_.use_direct_reads = args.direct_reads;
		options_.use_direct_io_for_flush_and_compaction = args.direct_writes;
		// Bulk load: no automatic compaction. Without it RocksDB also applies no L0 file-count or
		// pending-compaction write stalls, so L0 files accumulate until compact() is called.
		options_.disable_auto_compactions = args.bulk_load;
		options_.compaction_readahead_size = args.compaction_readahead_size;
		options_.writable_file_max_buffer_size = args.writable_file_max_buffer_size;
		options_.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options_));
		// compression: left at RocksDB's default, which is Snappy only if RocksDB was built with it.
		// rocksdb/build.sh builds without any compression library, so the default is kNoCompression.
		write_options_.sync = args.sync;

		rocksdb::DestroyDB(args.db_dir, rocksdb::Options());
		const rocksdb::Status s = rocksdb::DB::Open(options_, args.db_dir, &db_);
		if (!s.ok()) fail("Cannot open RocksDB at " + args.db_dir + ": " + s.ToString());
	}

	~RocksDbStore() {
		db_.reset(); // close before destroying the files
		if (!args_.keep_db) rocksdb::DestroyDB(args_.db_dir, rocksdb::Options());
	}

	void insert(KeyType k) {
		const EncodedKey key(k);
		const rocksdb::Status s = db_->Put(write_options_, key.slice(), kValue);
		if (!s.ok()) fail("Put failed: " + s.ToString());
	}

	bool find(KeyType k) {
		const EncodedKey key(k);
		const rocksdb::Status s = db_->Get(read_options_, key.slice(), &value_);
		return s.ok() && value_ == kValue.ToString();
	}

	void compact() {
		const rocksdb::Status s = db_->CompactRange(rocksdb::CompactRangeOptions(), nullptr, nullptr);
		if (!s.ok()) fail("CompactRange failed: " + s.ToString());
	}

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

	int num_levels() const { return options_.num_levels; }

	// Every setting in effect, one per line.
	std::string settings() const {
		std::ostringstream ss;
		ss << "RocksDB version " << ROCKSDB_MAJOR << "." << ROCKSDB_MINOR << "." << ROCKSDB_PATCH
		   << "\nRocksDB db dir " << args_.db_dir
		   << "\nRocksDB write_buffer_size " << options_.write_buffer_size
		   << "\nRocksDB max_write_buffer_number " << options_.max_write_buffer_number
		   << "\nRocksDB target_file_size_base " << options_.target_file_size_base
		   << "\nRocksDB max_bytes_for_level_base " << options_.max_bytes_for_level_base
		   << "\nRocksDB level_compaction_dynamic_level_bytes " << options_.level_compaction_dynamic_level_bytes
		   << "\nRocksDB compaction_style " << (options_.compaction_style == rocksdb::kCompactionStyleLevel ? "level" : "other")
		   << "\nRocksDB num_levels " << options_.num_levels
		   << "\nRocksDB max_background_jobs " << options_.max_background_jobs
		   << "\nRocksDB max_open_files " << options_.max_open_files
		   << "\nRocksDB block_size " << table_options_.block_size
		   << "\nRocksDB block cache " << (args_.cache_size > 0 ? std::to_string(args_.cache_size) : "default (33554432)")
		   << "\nRocksDB cache_index_and_filter_blocks " << table_options_.cache_index_and_filter_blocks
		   << "\nRocksDB filter " << (args_.bloom_bits > 0 ? "bloom " + std::to_string(args_.bloom_bits) + " bits/key" : "none")
		   << "\nRocksDB compression " << (options_.compression == rocksdb::kNoCompression ? "none" : "compressed")
		   << "\nRocksDB use_direct_reads " << options_.use_direct_reads
		   << "\nRocksDB use_direct_io_for_flush_and_compaction " << options_.use_direct_io_for_flush_and_compaction
		   << "\nRocksDB compaction_readahead_size " << options_.compaction_readahead_size
		   << "\nRocksDB writable_file_max_buffer_size " << options_.writable_file_max_buffer_size
		   << "\nRocksDB arena_block_size " << options_.arena_block_size << " (0: write_buffer_size / 8)"
		   << "\nRocksDB sync writes " << write_options_.sync
		   << "\nRocksDB WAL " << (write_options_.disableWAL ? "off" : "on")
		   << "\nRocksDB verify_checksums " << read_options_.verify_checksums
		   << "\nRocksDB fill_cache " << read_options_.fill_cache
		   << "\nRocksDB compact before queries " << args_.compact_before_queries
		   << "\nRocksDB bulk load " << args_.bulk_load << " (disable_auto_compactions " << options_.disable_auto_compactions
		   << (args_.bulk_load ? "; CompactRange after the insertions, or after every interleaved batch)" : ")")
		   << "\nKey encoding " << sizeof(KeyType) << "-byte big-endian, value 1 byte\n";
		return ss.str();
	}

private:
	const Args& args_;
	rocksdb::BlockBasedTableOptions table_options_;
	rocksdb::Options options_;
	rocksdb::WriteOptions write_options_;
	rocksdb::ReadOptions read_options_;
	std::unique_ptr<rocksdb::DB> db_;
	std::string value_;
};

void final_report(Log& log, RocksDbStore& store, const ProcIOStats& os_before,
                  Clock::time_point t_start) {
	report_totals(log, os_before, t_start);
	log << "RocksDB memtables bytes: " << store.property("rocksdb.cur-size-all-mem-tables") << "\n"
	    << "RocksDB table readers memory bytes: " << store.property("rocksdb.estimate-table-readers-mem") << "\n"
	    << "RocksDB block cache usage bytes: " << store.property("rocksdb.block-cache-usage") << "\n"
	    << "RocksDB disk bytes: " << store.disk_bytes() << "\n"
	    << "RocksDB files per level:";
	for (int level = 0; level < store.num_levels(); ++level)
		log << " L" << level << " " << store.property("rocksdb.num-files-at-level" + std::to_string(level));
	log << "\nRocksDB stats:\n" << store.property("rocksdb.stats") << "End\n";
}

/*********************************** benchmarks ***********************************/

// Mirrors insertions_then_queries_benchmark in examples/containers/boa.cpp.
void insertions_then_queries(const Args& args) {
	std::vector<KeyType> data;
	generate_data(args.w.key_dist, data, args.w.size);

	const std::string query_dists_name = args.w.query_dists_name();

	const std::string tmp_filename = "rocksdb_"
		+ std::to_string(args.write_buffer_size) + "_" + std::to_string(args.block_size)
		+ "_" + std::to_string(args.cache_size) + "_" + std::to_string(args.bloom_bits)
		+ "_" + to_string(args.w.key_dist) + "_" + query_dists_name + ".txt";
	const std::string filename = args.w.log_filename.empty() ? get_available_filename(tmp_filename) : args.w.log_filename;

	Log log(filename);

	memory_baseline(args.w); // keys generated, structure not yet created
	RocksDbStore store(args);
	const ProcIOStats os_before = ProcIOStats::read_current();
	const auto t_start = Clock::now();

	log << "Start RocksDB"
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

// Mirrors insertions_with_queries_benchmark in examples/containers/boa.cpp: keys from --key-dist, and
// query keys picked with rand() exactly as boa does (same glibc sequence, never reseeded).
void insertions_with_queries(const Args& args) {
	const int size = args.w.size;
	std::vector<KeyType> data;
	generate_data(args.w.key_dist, data, size);

	const std::string tmp_filename = "rocksdb_interleaved_"
		+ std::to_string(args.write_buffer_size) + "_" + std::to_string(args.block_size)
		+ "_" + std::to_string(args.cache_size) + "_" + std::to_string(args.bloom_bits)
		+ "_" + std::to_string(args.w.insertions_per_batch) + "_" + std::to_string(args.w.queries_per_batch) + ".txt";
	const std::string filename = args.w.log_filename.empty() ? get_available_filename(tmp_filename) : args.w.log_filename;

	Log log(filename);

	memory_baseline(args.w); // keys generated, structure not yet created
	RocksDbStore store(args);
	const ProcIOStats os_before = ProcIOStats::read_current();
	const auto t_start = Clock::now();

	log << "Interleaved Start RocksDB"
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
		if (args.bulk_load) store.compact(); // consolidation, counted as insertion time (as BOA's)
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
