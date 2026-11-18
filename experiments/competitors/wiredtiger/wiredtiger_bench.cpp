// WiredTiger competitor driver.
//
// Runs the same workloads as examples/containers/boa.cpp through the shared loops in kv_bench.h (same key/query
// generators and seeds, same options, same phases and checkpoints). I/O is measured from /proc/self/io. Every
// WiredTiger setting defaults to WiredTiger's own default (cache_size 100MB, 32 KiB leaf / 4 KiB internal pages,
// no compression, no logging) and is written to the results file. See experiments/competitors/wiredtiger/README.md.

#include <wiredtiger.h>

#include "kv_bench.h"

struct Args {
	WorkloadArgs w;
	std::string db_dir = "wiredtiger_bench_db";
	size_t cache_size = 100u << 20;   // connection cache_size: WiredTiger's default is 100 MB
	std::string leaf_page_max;        // table leaf_page_max, e.g. 32KB (empty: WiredTiger's default)
	std::string internal_page_max;    // table internal_page_max (empty: default)
	std::string allocation_size;      // table allocation_size (empty: default 4KB)
	bool direct_io = false;           // connection direct_io=[data]
	bool keep_db = false;
	std::string extra_connection;     // appended to the wiredtiger_open configuration
};

void print_usage(const char* prog) {
	const Args d;
	std::cout << "Usage: " << prog << " [options]\n";
	print_workload_usage(true);
	std::cout << "WiredTiger (defaults are WiredTiger's own):"
		<< "\n  --db-dir PATH             home directory, removed before each experiment (default " << d.db_dir << ")"
		<< "\n  --cache-size B            connection cache_size in bytes (default " << d.cache_size << " = 100 MB, WiredTiger's default)"
		<< "\n  --leaf-page-max B         table leaf_page_max in bytes (default: WiredTiger's, 32 KB)"
		<< "\n  --internal-page-max B     table internal_page_max in bytes (default: WiredTiger's, 4 KB)"
		<< "\n  --allocation-size B       table allocation_size in bytes (default: WiredTiger's, 4 KB)"
		<< "\n  --direct-io               connection direct_io=[data]: O_DIRECT for the data files (default off)"
		<< "\n  --connection-config STR   extra wiredtiger_open configuration, appended"
		<< "\n  --keep-db                 do not remove the database after the experiment"
		<< "\n  --help                    show this message\n";
}

Args parse_args(int argc, char* argv[]) {
	Args args;
	for (int i = 1; i < argc; ++i) {
		const std::string opt = argv[i];
		auto bytes = [&]() { return static_cast<size_t>(parse_positive(opt, option_value(argc, argv, i))); };
		if (opt == "--help" || opt == "-h") { print_usage(argv[0]); std::exit(0); }
		else if (parse_workload_option(argc, argv, i, args.w)) continue;
		else if (opt == "--db-dir") args.db_dir = option_value(argc, argv, i);
		else if (opt == "--cache-size") args.cache_size = bytes();
		else if (opt == "--leaf-page-max") args.leaf_page_max = std::to_string(bytes());
		else if (opt == "--internal-page-max") args.internal_page_max = std::to_string(bytes());
		else if (opt == "--allocation-size") args.allocation_size = std::to_string(bytes());
		else if (opt == "--direct-io") args.direct_io = true;
		else if (opt == "--connection-config") args.extra_connection = option_value(argc, argv, i);
		else if (opt == "--keep-db") args.keep_db = true;
		else { print_usage(argv[0]); fail("Unknown option: " + opt); }
	}
	return args;
}

static void check(int rc, const char* what) {
	if (rc != 0) fail(std::string(what) + " failed: " + wiredtiger_strerror(rc));
}

class WiredTigerStore {
public:
	explicit WiredTigerStore(const Args& args) : args_(args) {
		std::filesystem::remove_all(args.db_dir);
		std::filesystem::create_directories(args.db_dir);
		conn_config_ = "create,cache_size=" + std::to_string(args.cache_size);
		if (args.direct_io) conn_config_ += ",direct_io=[data]";
		if (!args.extra_connection.empty()) conn_config_ += "," + args.extra_connection;
		check(wiredtiger_open(args.db_dir.c_str(), nullptr, conn_config_.c_str(), &conn_), "wiredtiger_open");
		check(conn_->open_session(conn_, nullptr, nullptr, &session_), "open_session");
		// Integer keys (4 bytes, "I") and one-byte values ("B"); WiredTiger packs integers in an order-preserving way.
		table_config_ = "key_format=I,value_format=B";
		if (!args.leaf_page_max.empty()) table_config_ += ",leaf_page_max=" + args.leaf_page_max;
		if (!args.internal_page_max.empty()) table_config_ += ",internal_page_max=" + args.internal_page_max;
		if (!args.allocation_size.empty()) table_config_ += ",allocation_size=" + args.allocation_size;
		check(session_->create(session_, "table:bench", table_config_.c_str()), "create table");
		check(session_->open_cursor(session_, "table:bench", nullptr, nullptr, &cursor_), "open_cursor");
	}

	~WiredTigerStore() {
		if (conn_) conn_->close(conn_, nullptr); // closes the session and cursor, checkpoints
		if (!args_.keep_db) std::filesystem::remove_all(args_.db_dir);
	}

	void insert(KeyType k) {
		cursor_->set_key(cursor_, static_cast<uint32_t>(k));
		cursor_->set_value(cursor_, static_cast<uint8_t>('a'));
		check(cursor_->insert(cursor_), "insert");
	}

	void flush() {}
	void begin_queries() {}
	void end_queries() {}

	bool find(KeyType k) {
		cursor_->set_key(cursor_, static_cast<uint32_t>(k));
		const int rc = cursor_->search(cursor_);
		if (rc == WT_NOTFOUND) return false;
		check(rc, "search");
		uint8_t v = 0;
		check(cursor_->get_value(cursor_, &v), "get_value");
		cursor_->reset(cursor_);
		return v == 'a';
	}

	bool compact() { return false; }

	uint64_t disk_bytes() const { return directory_bytes(args_.db_dir); }

	std::string settings() const {
		std::ostringstream ss;
		ss << "WiredTiger version " << wiredtiger_version(nullptr, nullptr, nullptr)
		   << "\nWiredTiger db dir " << args_.db_dir
		   << "\nWiredTiger connection config " << conn_config_
		   << "\nWiredTiger table config " << table_config_
		   << "\nWiredTiger cache_size " << args_.cache_size
		   << "\nWiredTiger leaf_page_max " << (args_.leaf_page_max.empty() ? "default (32KB)" : args_.leaf_page_max)
		   << "\nWiredTiger internal_page_max " << (args_.internal_page_max.empty() ? "default (4KB)" : args_.internal_page_max)
		   << "\nWiredTiger allocation_size " << (args_.allocation_size.empty() ? "default (4KB)" : args_.allocation_size)
		   << "\nWiredTiger direct_io " << (args_.direct_io ? "[data]" : "off")
		   << "\nWiredTiger compression none, logging off (WiredTiger defaults)"
		   << "\nKey encoding " << sizeof(KeyType) << "-byte integer (format I), value 1 byte (format B)\n";
		return ss.str();
	}

	std::string final_stats() { return ""; }

private:
	const Args& args_;
	std::string conn_config_, table_config_;
	WT_CONNECTION* conn_ = nullptr;
	WT_SESSION* session_ = nullptr;
	WT_CURSOR* cursor_ = nullptr;
};

int main(int argc, char* argv[]) {
	const Args args = parse_args(argc, argv);
	const std::string settings_tag = std::to_string(args.cache_size) + "_" + std::to_string(args.direct_io);
	run_benchmarks<WiredTigerStore>(args.w, "WiredTiger", "wiredtiger", settings_tag, [&]() { return std::make_unique<WiredTigerStore>(args); });
	return 0;
}
