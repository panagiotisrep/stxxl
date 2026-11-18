// SplinterDB competitor driver.
//
// Runs the same workloads as examples/containers/boa.cpp through the shared loops in kv_bench.h (same key/query
// generators and seeds, same options, same phases and checkpoints). I/O is measured from /proc/self/io. SplinterDB
// has no defaults for the cache and disk size: both are set by the options below. See
// experiments/competitors/splinterdb/README.md.

#include <splinterdb/default_data_config.h>
#include <splinterdb/splinterdb.h>

#include "kv_bench.h"

#include <sys/stat.h>

#include <cstring>

struct Args {
	WorkloadArgs w;
	std::string db_file = "splinterdb_bench.db";
	uint64_t cache_size = uint64_t(64) << 20; // the benchmark's choice: SplinterDB requires a value (its intro example uses 64 MiB)
	uint64_t disk_size = uint64_t(256) << 30; // fixed when the database is created; the file is sparse
	uint64_t memtable_capacity = 0;           // splinterdb_config::memtable_capacity (0: SplinterDB's default, 24 MiB; the cache reserves twice that)
	bool use_log = false;                     // splinterdb_config::use_log (SplinterDB's default: off)
	uint64_t memtable_bg_threads = 0;         // 0: SplinterDB's default
	uint64_t normal_bg_threads = 0;           // 0: SplinterDB's default
	bool keep_db = false;
};

void print_usage(const char* prog) {
	const Args d;
	std::cout << "Usage: " << prog << " [options]\n";
	print_workload_usage(true);
	std::cout << "SplinterDB (defaults are SplinterDB's own unless stated):"
		<< "\n  --db-file PATH            database file, removed before each experiment (default " << d.db_file << ")"
		<< "\n  --cache-size B            splinterdb_config::cache_size in bytes (default " << d.cache_size << "; required by SplinterDB, the benchmark's choice)"
		<< "\n  --disk-size B             splinterdb_config::disk_size, fixed at creation (default " << d.disk_size << ")"
		<< "\n  --memtable-capacity B     splinterdb_config::memtable_capacity (default 0 = 24 MiB; the cache must hold 2x this, so a small cache needs a small memtable)"
		<< "\n  --use-log                 splinterdb_config::use_log: write-ahead log (default off, SplinterDB's default)"
		<< "\n  --memtable-bg-threads N   num_memtable_bg_threads (default 0 = SplinterDB's default)"
		<< "\n  --normal-bg-threads N     num_normal_bg_threads (default 0 = SplinterDB's default)"
		<< "\n  --keep-db                 do not remove the database after the experiment"
		<< "\n  --help                    show this message\n";
}

Args parse_args(int argc, char* argv[]) {
	Args args;
	for (int i = 1; i < argc; ++i) {
		const std::string opt = argv[i];
		auto bytes = [&]() { return static_cast<uint64_t>(parse_positive(opt, option_value(argc, argv, i))); };
		if (opt == "--help" || opt == "-h") { print_usage(argv[0]); std::exit(0); }
		else if (parse_workload_option(argc, argv, i, args.w)) continue;
		else if (opt == "--db-file") args.db_file = option_value(argc, argv, i);
		else if (opt == "--cache-size") args.cache_size = bytes();
		else if (opt == "--disk-size") args.disk_size = bytes();
		else if (opt == "--memtable-capacity") args.memtable_capacity = bytes();
		else if (opt == "--use-log") args.use_log = true;
		else if (opt == "--memtable-bg-threads") args.memtable_bg_threads = bytes();
		else if (opt == "--normal-bg-threads") args.normal_bg_threads = bytes();
		else if (opt == "--keep-db") args.keep_db = true;
		else { print_usage(argv[0]); fail("Unknown option: " + opt); }
	}
	return args;
}

class SplinterDbStore {
public:
	explicit SplinterDbStore(const Args& args) : args_(args) {
		std::remove(args.db_file.c_str());
		default_data_config_init(&data_cfg_);
		std::memset(&cfg_, 0, sizeof(cfg_));
		cfg_.filename = args.db_file.c_str();
		cfg_.cache_size = args.cache_size / (uint64_t(64) << 12) * (uint64_t(64) << 12); // SplinterDB: multiple of 64 pages of 4 KiB, rounded down
		cfg_.disk_size = args.disk_size;
		cfg_.data_cfg = &data_cfg_;
		cfg_.use_log = args.use_log;
		cfg_.memtable_capacity = args.memtable_capacity;
		cfg_.num_memtable_bg_threads = args.memtable_bg_threads;
		cfg_.num_normal_bg_threads = args.normal_bg_threads;
		const int rc = splinterdb_create(&cfg_, &kvs_);
		if (rc != 0) fail("splinterdb_create failed: " + std::to_string(rc));
		splinterdb_lookup_result_init_options opts{};
		opts.struct_size = sizeof(opts);
		splinterdb_lookup_result_init_full(kvs_, &result_, &opts); // (the macro uses a C compound literal)
	}

	~SplinterDbStore() {
		splinterdb_lookup_result_deinit(&result_);
		splinterdb_close(&kvs_, false);
		if (!args_.keep_db) std::remove(args_.db_file.c_str());
	}

	void insert(KeyType k) {
		char kb[sizeof(KeyType)];
		encode_key(k, kb);
		const int rc = splinterdb_insert(kvs_, slice_create(sizeof(kb), kb), message_create(1, "a"), nullptr);
		if (rc != 0) fail("splinterdb_insert failed: " + std::to_string(rc));
	}

	void flush() {}
	void begin_queries() {}
	void end_queries() {}

	bool find(KeyType k) {
		char kb[sizeof(KeyType)];
		encode_key(k, kb);
		const int rc = splinterdb_lookup(kvs_, slice_create(sizeof(kb), kb), &result_);
		if (rc != 0) fail("splinterdb_lookup failed: " + std::to_string(rc));
		if (!splinterdb_lookup_found(&result_)) return false;
		slice v;
		if (splinterdb_lookup_result_value(&result_, &v) != 0) return false;
		return slice_length(v) == 1 && static_cast<const char*>(slice_data(v))[0] == 'a';
	}

	bool compact() { return false; }

	// Allocated bytes of the (sparse) database file.
	uint64_t disk_bytes() const {
		struct stat st;
		return stat(args_.db_file.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_blocks) * 512 : 0;
	}

	std::string settings() const {
		std::ostringstream ss;
		ss << "SplinterDB db file " << args_.db_file
		   << "\nSplinterDB cache_size " << cfg_.cache_size << " (requested " << args_.cache_size << ", rounded down to a multiple of 256 KiB)"
		   << "\nSplinterDB disk_size " << args_.disk_size << " (sparse file)"
		   << "\nSplinterDB memtable_capacity " << args_.memtable_capacity << " (0 = default 24 MiB)"
		   << "\nSplinterDB use_log " << args_.use_log
		   << "\nSplinterDB memtable bg threads " << (args_.memtable_bg_threads ? std::to_string(args_.memtable_bg_threads) : "default")
		   << "\nSplinterDB normal bg threads " << (args_.normal_bg_threads ? std::to_string(args_.normal_bg_threads) : "default")
		   << "\nSplinterDB I/O direct (SplinterDB opens its file with O_DIRECT), data config default_data_config (lexicographic)"
		   << "\nKey encoding " << sizeof(KeyType) << "-byte big-endian, value 1 byte\n";
		return ss.str();
	}

	std::string final_stats() { return ""; }

private:
	const Args& args_;
	data_config data_cfg_;
	splinterdb_config cfg_;
	splinterdb* kvs_ = nullptr;
	splinterdb_lookup_result result_;
};

int main(int argc, char* argv[]) {
	const Args args = parse_args(argc, argv);
	const std::string settings_tag = std::to_string(args.cache_size) + "_" + std::to_string(args.memtable_capacity) + "_" + std::to_string(args.use_log);
	run_benchmarks<SplinterDbStore>(args.w, "SplinterDB", "splinterdb", settings_tag, [&]() { return std::make_unique<SplinterDbStore>(args); });
	return 0;
}
