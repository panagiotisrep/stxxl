// LMDB competitor driver.
//
// Runs the same workloads as examples/containers/boa.cpp through the shared loops in kv_bench.h (same key/query
// generators and seeds, same options, same phases and checkpoints). I/O is measured from /proc/self/io (LMDB has
// no counters of its own). Every LMDB setting defaults to LMDB's own default and is written to the results file.
// LMDB is a memory-mapped copy-on-write B+-tree: it has no cache of its own, the OS page cache is its cache, so
// its memory cannot be set (see experiments/competitors/lmdb/README.md).

#include <lmdb.h>

#include "kv_bench.h"

#include <cstring>

struct Args {
	WorkloadArgs w;
	std::string db_dir = "lmdb_bench_db";
	size_t map_size = size_t(256) << 30; // virtual address space reserved for the data file, not memory
	int commit_every = 100000;           // insertions per write transaction (LMDB commits, and by default syncs, per transaction)
	bool nosync = false;                 // MDB_NOSYNC
	bool writemap = false;               // MDB_WRITEMAP
	bool no_readahead = false;           // MDB_NORDAHEAD
	bool keep_db = false;
};

void print_usage(const char* prog) {
	const Args d;
	std::cout << "Usage: " << prog << " [options]\n";
	print_workload_usage(true);
	std::cout << "LMDB (defaults are LMDB's own unless stated):"
		<< "\n  --db-dir PATH             environment directory, removed before each experiment (default " << d.db_dir << ")"
		<< "\n  --map-size B              mdb_env_set_mapsize: size of the memory map = largest possible database (default " << d.map_size << ")"
		<< "\n  --commit-every N          insertions per write transaction (default " << d.commit_every << "; the benchmark's choice, LMDB has no default)"
		<< "\n  --nosync                  MDB_NOSYNC: no fsync at commit (default: sync at every commit)"
		<< "\n  --writemap                MDB_WRITEMAP: write through a writable memory map (default off)"
		<< "\n  --no-readahead            MDB_NORDAHEAD: turn off OS readahead for the map (default off = readahead on)"
		<< "\n  --keep-db                 do not remove the database after the experiment"
		<< "\n  --help                    show this message\n";
}

Args parse_args(int argc, char* argv[]) {
	Args args;
	for (int i = 1; i < argc; ++i) {
		const std::string opt = argv[i];
		if (opt == "--help" || opt == "-h") { print_usage(argv[0]); std::exit(0); }
		else if (parse_workload_option(argc, argv, i, args.w)) continue;
		else if (opt == "--db-dir") args.db_dir = option_value(argc, argv, i);
		else if (opt == "--map-size") args.map_size = static_cast<size_t>(parse_positive(opt, option_value(argc, argv, i)));
		else if (opt == "--commit-every") args.commit_every = static_cast<int>(parse_positive(opt, option_value(argc, argv, i)));
		else if (opt == "--nosync") args.nosync = true;
		else if (opt == "--writemap") args.writemap = true;
		else if (opt == "--no-readahead") args.no_readahead = true;
		else if (opt == "--keep-db") args.keep_db = true;
		else { print_usage(argv[0]); fail("Unknown option: " + opt); }
	}
	return args;
}

static void check(int rc, const char* what) {
	if (rc != MDB_SUCCESS) fail(std::string(what) + " failed: " + mdb_strerror(rc));
}

class LmdbStore {
public:
	explicit LmdbStore(const Args& args) : args_(args) {
		std::filesystem::remove_all(args.db_dir);
		std::filesystem::create_directories(args.db_dir);
		check(mdb_env_create(&env_), "mdb_env_create");
		check(mdb_env_set_mapsize(env_, args.map_size), "mdb_env_set_mapsize");
		unsigned flags = 0;
		if (args.nosync) flags |= MDB_NOSYNC;
		if (args.writemap) flags |= MDB_WRITEMAP;
		if (args.no_readahead) flags |= MDB_NORDAHEAD;
		check(mdb_env_open(env_, args.db_dir.c_str(), flags, 0664), "mdb_env_open");
		MDB_txn* txn;
		check(mdb_txn_begin(env_, nullptr, 0, &txn), "mdb_txn_begin");
		check(mdb_dbi_open(txn, nullptr, 0, &dbi_), "mdb_dbi_open");
		check(mdb_txn_commit(txn), "mdb_txn_commit");
	}

	~LmdbStore() {
		if (wtxn_) mdb_txn_abort(wtxn_);
		if (rtxn_) mdb_txn_abort(rtxn_);
		mdb_env_close(env_);
		if (!args_.keep_db) std::filesystem::remove_all(args_.db_dir);
	}

	void insert(KeyType k) {
		if (!wtxn_) check(mdb_txn_begin(env_, nullptr, 0, &wtxn_), "mdb_txn_begin");
		char kb[sizeof(KeyType)];
		encode_key(k, kb);
		MDB_val key{sizeof(kb), kb}, val{1, const_cast<char*>("a")};
		check(mdb_put(wtxn_, dbi_, &key, &val, 0), "mdb_put");
		if (++pending_ == args_.commit_every) flush();
	}

	void flush() { // commits the pending write transaction
		if (!wtxn_) return;
		const int rc = mdb_txn_commit(wtxn_);
		wtxn_ = nullptr;
		pending_ = 0;
		check(rc, "mdb_txn_commit");
	}

	void begin_queries() { check(mdb_txn_begin(env_, nullptr, MDB_RDONLY, &rtxn_), "mdb_txn_begin (read)"); }
	void end_queries() { if (rtxn_) { mdb_txn_abort(rtxn_); rtxn_ = nullptr; } }

	bool find(KeyType k) {
		char kb[sizeof(KeyType)];
		encode_key(k, kb);
		MDB_val key{sizeof(kb), kb}, val;
		const int rc = mdb_get(rtxn_, dbi_, &key, &val);
		if (rc == MDB_NOTFOUND) return false;
		check(rc, "mdb_get");
		return val.mv_size == 1 && static_cast<const char*>(val.mv_data)[0] == 'a';
	}

	bool compact() { return false; } // a copy-on-write B+-tree has no background work

	uint64_t disk_bytes() const { return directory_bytes(args_.db_dir); }

	std::string settings() const {
		std::ostringstream ss;
		ss << "LMDB version " << mdb_version(nullptr, nullptr, nullptr)
		   << "\nLMDB db dir " << args_.db_dir
		   << "\nLMDB map size " << args_.map_size
		   << "\nLMDB commit every " << args_.commit_every << " insertions"
		   << "\nLMDB MDB_NOSYNC " << args_.nosync
		   << "\nLMDB MDB_WRITEMAP " << args_.writemap
		   << "\nLMDB MDB_NORDAHEAD " << args_.no_readahead
		   << "\nLMDB cache the OS page cache (LMDB has no cache of its own; not limited)"
		   << "\nKey encoding " << sizeof(KeyType) << "-byte big-endian, value 1 byte\n";
		return ss.str();
	}

	std::string final_stats() {
		MDB_stat st;
		MDB_envinfo info;
		mdb_env_stat(env_, &st);
		mdb_env_info(env_, &info);
		std::ostringstream ss;
		ss << "LMDB page size " << st.ms_psize << "\nLMDB tree depth " << st.ms_depth
		   << "\nLMDB branch pages " << st.ms_branch_pages << "\nLMDB leaf pages " << st.ms_leaf_pages
		   << "\nLMDB overflow pages " << st.ms_overflow_pages << "\nLMDB entries " << st.ms_entries
		   << "\nLMDB last used page " << info.me_last_pgno << "\n";
		return ss.str();
	}

private:
	const Args& args_;
	MDB_env* env_ = nullptr;
	MDB_dbi dbi_ = 0;
	MDB_txn* wtxn_ = nullptr;
	MDB_txn* rtxn_ = nullptr;
	int pending_ = 0;
};

int main(int argc, char* argv[]) {
	const Args args = parse_args(argc, argv);
	const std::string settings_tag = std::to_string(args.commit_every) + "_" + std::to_string(args.nosync);
	run_benchmarks<LmdbStore>(args.w, "LMDB", "lmdb", settings_tag, [&]() { return std::make_unique<LmdbStore>(args); });
	return 0;
}
