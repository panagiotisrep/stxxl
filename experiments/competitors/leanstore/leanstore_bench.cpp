// LeanStore competitor driver (LeanStore's B-tree).
//
// Runs the same workloads as examples/containers/boa.cpp, with the same phases, checkpoints and output as
// leveldb/leveldb_bench.cpp. LeanStore is configured through its own gflags (backend/leanstore/Config.cpp):
// every option this driver does not know is passed to gflags, and the value of every LeanStore flag is
// written to the results file. See experiments/competitors/leanstore/README.md.

#include "common.h" // first: uses std headers only

#include "leanstore/Config.hpp"
#include "leanstore/LeanStore.hpp"

#include <gflags/gflags.h>
#include <sys/stat.h>

#include <algorithm>
#include <filesystem>
#include <memory>

/*********************************** arguments ***********************************/

struct Args {
	WorkloadArgs w;
	bool keep_db = false;
};

// Defaults this driver changes from LeanStore's own (users can still override them on the command line).
const std::pair<const char*, const char*> kDriverFlagDefaults[] = {
	{"worker_threads", "1"},            // LeanStore default 4: one thread runs the benchmark, like boa
	{"ssd_path", "leanstore_bench_ssd"}, // LeanStore default ./leanstore
	{"trunc", "true"},                   // LeanStore default false: fresh file for every experiment
};

void print_usage(const char* prog) {
	std::cout << "Usage: " << prog << " [options] [LeanStore flags]\n";
	print_workload_usage(true);
	std::cout << "LeanStore:"
		<< "\n  --keep-db                 do not remove the LeanStore file after the experiment"
		<< "\n  --help                    show this message"
		<< "\n  Any other --name=value is a LeanStore flag (backend/leanstore/Config.cpp), e.g."
		<< "\n  --dram_gib=1 (buffer pool size in GiB), --isolation_level=si|rc|ru, --vi, --wal."
		<< "\n  LeanStore checks combinations: --vi needs --wal; --isolation_level=si needs --vi and --mv."
		<< "\n  Plain B-tree (BTreeLL): --vi=false --isolation_level=rc (or ru, then also --wal=false)."
		<< "\n  This driver changes these LeanStore defaults:";
	for (const auto& [name, value] : kDriverFlagDefaults) std::cout << " --" << name << "=" << value;
	std::cout << "\n  --helpfull lists all LeanStore flags.\n";
}

// Parses the driver's own options; everything else is left in argv for gflags.
Args parse_args(int& argc, char** argv) {
	Args args;
	int out = 1;
	for (int i = 1; i < argc; ++i) {
		const std::string opt = argv[i];
		if (opt == "--help" || opt == "-h") { print_usage(argv[0]); std::exit(0); }
		else if (opt == "--keep-db") args.keep_db = true;
		else if (parse_workload_option(argc, argv, i, args.w)) continue;
		else argv[out++] = argv[i]; // for gflags
	}
	argc = out;
	return args;
}

/*********************************** LeanStore ***********************************/

// 4-byte big-endian key: LeanStore's byte order equals numeric key order.
struct EncodedKey {
	u8 bytes[sizeof(KeyType)];
	explicit EncodedKey(KeyType k) {
		for (std::size_t i = 0; i < sizeof(KeyType); ++i)
			bytes[i] = static_cast<u8>((k >> (8 * (sizeof(KeyType) - 1 - i))) & 0xff);
	}
};

u8 kValue = 'a'; // 1-byte value, like boa.cpp's char 'a'

std::string store_name_params() {
	std::ostringstream ss;
	ss << FLAGS_dram_gib << "_" << (FLAGS_vi ? "vi" : "ll") << "_" << FLAGS_isolation_level;
	return ss.str();
}

class LeanStoreStore {
public:
	LeanStoreStore(leanstore::LeanStore& db, const Args& args)
		: args_(args), isolation_(leanstore::parseIsolationLevel(FLAGS_isolation_level)) {
		// Same tree choice as LeanStore's own LeanStoreAdapter (frontend/shared/LeanStoreAdapter.hpp).
		if (FLAGS_vi)
			tree_ = &db.registerBTreeVI("boa_bench", {.enable_wal = FLAGS_wal, .use_bulk_insert = false});
		else
			tree_ = &db.registerBTreeLL("boa_bench", {.enable_wal = FLAGS_wal, .use_bulk_insert = false});
	}

	void insert(KeyType k) {
		EncodedKey key(k);
		leanstore::cr::Worker::my().startTX(leanstore::TX_MODE::OLTP, isolation_);
		const leanstore::OP_RESULT r = tree_->insert(key.bytes, sizeof(key.bytes), &kValue, 1);
		if (r != leanstore::OP_RESULT::OK) {
			leanstore::cr::Worker::my().abortTX();
			fail("LeanStore insert failed for key " + std::to_string(k) + " (result " + std::to_string(static_cast<int>(r)) + ")");
		}
		leanstore::cr::Worker::my().commitTX();
	}

	bool find(KeyType k) {
		EncodedKey key(k);
		bool ok = false;
		leanstore::cr::Worker::my().startTX(leanstore::TX_MODE::OLTP, isolation_);
		const leanstore::OP_RESULT r = tree_->lookup(key.bytes, sizeof(key.bytes), [&](const u8* payload, u16 length) {
			ok = length == 1 && payload[0] == kValue;
		});
		if (r == leanstore::OP_RESULT::ABORT_TX) {
			leanstore::cr::Worker::my().abortTX();
			return false;
		}
		leanstore::cr::Worker::my().commitTX();
		return r == leanstore::OP_RESULT::OK && ok;
	}

	// Space actually allocated for the LeanStore file (it is sparse: the WAL starts at --wal_offset_gib).
	uint64_t disk_bytes() const {
		struct stat st{};
		return stat(FLAGS_ssd_path.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_blocks) * 512 : 0;
	}

	uint64_t tree_height() { return tree_->getHeight(); }

	// Every setting in effect, one per line: all LeanStore flags with their values.
	std::string settings() const {
		std::ostringstream ss;
		ss << "LeanStore commit fcb64bf\n"
		   << "LeanStore tree " << (FLAGS_vi ? "BTreeVI" : "BTreeLL") << "\n";
		std::vector<gflags::CommandLineFlagInfo> flags;
		gflags::GetAllFlags(&flags);
		for (const auto& f : flags)
			if (f.filename.find("leanstore/Config.cpp") != std::string::npos)
				ss << "LeanStore flag " << f.name << " " << f.current_value << (f.is_default ? "" : " (set)") << "\n";
		ss << "Key encoding " << sizeof(KeyType) << "-byte big-endian, value 1 byte, one transaction per operation\n";
		return ss.str();
	}

private:
	const Args& args_;
	leanstore::TX_ISOLATION_LEVEL isolation_;
	leanstore::KVInterface* tree_ = nullptr;
};

void final_report(Log& log, LeanStoreStore& store, const ProcIOStats& os_before, Clock::time_point t_start) {
	report_totals(log, os_before, t_start);
	log << "LeanStore buffer pool bytes: " << static_cast<uint64_t>(FLAGS_dram_gib * 1024 * 1024 * 1024) << "\n"
	    << "LeanStore tree height: " << store.tree_height() << "\n"
	    << "LeanStore disk bytes: " << store.disk_bytes() << "\n"
	    << "End\n";
}

/*********************************** benchmarks ***********************************/

// Mirrors insertions_then_queries_benchmark in examples/containers/boa.cpp.
void insertions_then_queries(const Args& args, LeanStoreStore& store) {
	std::vector<KeyType> data;
	generate_data(args.w.key_dist, data, args.w.size);

	const std::string query_dists_name = args.w.query_dists_name();

	const std::string tmp_filename = "leanstore_"
		+ store_name_params()
		+ "_" + to_string(args.w.key_dist) + "_" + query_dists_name + ".txt";
	const std::string filename = args.w.log_filename.empty() ? get_available_filename(tmp_filename) : args.w.log_filename;

	Log log(filename);

	const ProcIOStats os_before = ProcIOStats::read_current();
	const auto t_start = Clock::now();

	log << "Start LeanStore"
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

// Mirrors insertions_with_queries_benchmark in examples/containers/boa.cpp: uniform keys, and query keys
// picked with rand() exactly as boa does (same glibc sequence, never reseeded).
void insertions_with_queries(const Args& args, LeanStoreStore& store) {
	const int size = args.w.size;
	std::vector<KeyType> data;
	uniform_random_data(data, size);

	const std::string tmp_filename = "leanstore_interleaved_"
		+ store_name_params()
		+ "_" + std::to_string(args.w.insertions_per_batch) + "_" + std::to_string(args.w.queries_per_batch) + ".txt";
	const std::string filename = args.w.log_filename.empty() ? get_available_filename(tmp_filename) : args.w.log_filename;

	Log log(filename);

	const ProcIOStats os_before = ProcIOStats::read_current();
	const auto t_start = Clock::now();

	log << "Interleaved Start LeanStore"
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


void run_experiment(const Args& args) {
	std::filesystem::remove(FLAGS_ssd_path);
	{
		leanstore::LeanStore db; // opens FLAGS_ssd_path with O_DIRECT, starts FLAGS_worker_threads workers
		std::unique_ptr<LeanStoreStore> store;
		auto& crm = db.getCRManager();
		crm.scheduleJobSync(0, [&]() { store = std::make_unique<LeanStoreStore>(db, args); });
		// The whole benchmark runs as one job on worker thread 0 (transactions must run on a worker).
		crm.scheduleJobSync(0, [&]() {
			if (args.w.interleaved)
				insertions_with_queries(args, *store);
			else
				insertions_then_queries(args, *store);
		});
	}
	if (!args.keep_db) std::filesystem::remove(FLAGS_ssd_path);
}

int main(int argc, char* argv[]) {
	const Args args = parse_args(argc, argv);
	for (const auto& [name, value] : kDriverFlagDefaults)
		gflags::SetCommandLineOptionWithMode(name, value, gflags::SET_FLAGS_DEFAULT);
	gflags::SetUsageMessage("LeanStore competitor driver; see --help");
	gflags::ParseCommandLineFlags(&argc, &argv, true);
	if (argc > 1) fail(std::string("Unknown option: ") + argv[1]);

	for (int i = 0; i < args.w.experiments_no; ++i) run_experiment(args);
	return 0;
}
