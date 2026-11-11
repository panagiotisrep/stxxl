// SILT competitor driver (extern/silt: ByteHamster/silt build-fix branch, commit 55ee50b).
//
// Two modes (--mode):
//   combi   SILT's combined store (log store → hash stores → sorted store, background conversion and
//           merges): online inserts, queries, interleaved. Compare with boa's online mode.
//   sorted  SILT's sorted store alone, bulk-built (SILT's sorter, then the sorted store): static, no
//           interleaved mode. Compare with boa's online-suspended mode (--suspended / --materialization).
// Runs the same workloads as examples/containers/boa.cpp, with the same phases, checkpoints and output as
// the other competitor drivers. The final SILT XML configuration is written into the results file.
// See experiments/competitors/silt/README.md.

#include "common.h" // first: uses std headers only

#include <configuration.h>
#include <fawnds.h>
#include <fawnds_factory.h>
#include <value.h>

#include <bytehamster/util/MurmurHash64.h>

#include <filesystem>
#include <memory>

#ifndef SILT_CONFIG_DIR
#error "Build with experiments/competitors/silt/build.sh (sets SILT_CONFIG_DIR)"
#endif

/*********************************** arguments ***********************************/

struct Args {
	WorkloadArgs w;
	std::string mode = "combi";            // combi | sorted
	std::string db_dir = "silt_bench_db";  // SILT's files; removed before and after each experiment
	bool buffered_io = false;              // false: SILT's default (use-buffered-io-only = 0, direct I/O)
	bool keep_db = false;
};

void print_usage(const char* prog) {
	const Args d;
	std::cout << "Usage: " << prog << " [options]\n";
	print_workload_usage(true);
	std::cout << "SILT:"
		<< "\n  --mode NAME               combi (combined store; online, interleaved) or sorted (sorted store,"
		<< "\n                            bulk-built; static, no --interleaved) (default " << d.mode << ")"
		<< "\n  --db-dir PATH             directory for SILT's files, removed before and after each experiment (default " << d.db_dir << ")"
		<< "\n  --buffered-io             use-buffered-io-only = 1 for every store (default 0: SILT's default, direct I/O)"
		<< "\n  --keep-db                 do not remove the files after the experiment"
		<< "\n  --help                    show this message"
		<< "\n  Everything else is in silt_combi.xml / silt_sorted.xml (SILT's own test configuration values).\n";
}

Args parse_args(int argc, char* argv[]) {
	Args args;
	for (int i = 1; i < argc; ++i) {
		const std::string opt = argv[i];
		if (opt == "--help" || opt == "-h") { print_usage(argv[0]); std::exit(0); }
		else if (parse_workload_option(argc, argv, i, args.w)) continue;
		else if (opt == "--mode") args.mode = option_value(argc, argv, i);
		else if (opt == "--db-dir") args.db_dir = option_value(argc, argv, i);
		else if (opt == "--buffered-io") args.buffered_io = true;
		else if (opt == "--keep-db") args.keep_db = true;
		else { print_usage(argv[0]); fail("Unknown option: " + opt); }
	}
	if (args.mode != "combi" && args.mode != "sorted")
		fail("Invalid value for --mode: " + args.mode + " (expected combi or sorted)");
	if (args.mode == "sorted" && args.w.interleaved)
		fail("--mode sorted is static: --interleaved is not supported (use --mode combi)");
	return args;
}

std::string store_name_params(const Args& args) {
	return args.mode + "_" + (args.buffered_io ? "buffered" : "direct");
}

/*********************************** SILT ***********************************/

// SILT stores fixed-length keys and expects them to be uniformly distributed (its cuckoo hash tables and
// the sorted store's trie index use key bits directly; the SILT paper uses SHA-1 hashes as keys). Keys are
// therefore stored as the 8-byte MurmurHash64 of the uint32_t key: the same hash as BOA's HashFunction.
inline uint64_t key_hash(KeyType k) { return bytehamster::util::MurmurHash64(&k, sizeof(k)); }

const char kValue = 'a'; // 1-byte value, like boa.cpp's char 'a'
constexpr size_t kKeyLen = sizeof(uint64_t);
constexpr size_t kDataLen = 1;

[[noreturn]] void silt_fail(const std::string& what, fawn::FawnDS_Return r) {
	fail("SILT " + what + " failed (FawnDS_Return " + std::to_string(static_cast<int>(r)) + ")");
}

void set(fawn::Configuration* c, const std::string& path, const std::string& value) {
	if (c->SetStringValue(path, value) != 0) fail("SILT configuration: cannot set " + path);
}

class SiltStore {
public:
	explicit SiltStore(const Args& args) : args_(args), dir_(std::filesystem::absolute(args.db_dir).string()) {
		std::filesystem::remove_all(dir_);
		std::filesystem::create_directories(dir_);
		const std::string io = args.buffered_io ? "1" : "0";
		const std::string n = std::to_string(args.w.size);

		if (args.mode == "combi") {
			config_ = new fawn::Configuration(SILT_CONFIG_DIR "/silt_combi.xml");
			set(config_, "key-len", std::to_string(kKeyLen));
			set(config_, "data-len", std::to_string(kDataLen));
			set(config_, "size", n);
			set(config_, "temp-file", dir_ + "/");
			const char* names[] = {"front", "middle"};
			for (int s = 0; s < 2; ++s) {
				const std::string store = "/fawnds/store" + std::to_string(s);
				set(config_, store + "/file", dir_ + "/" + names[s] + "_file_header");
				set(config_, store + "/hashtable/file", dir_ + "/" + names[s] + "_hashtable");
				set(config_, store + "/hashtable/use-buffered-io-only", io);
				set(config_, store + "/datastore/file", dir_ + "/" + names[s] + "_datastore");
				set(config_, store + "/datastore/use-buffered-io-only", io);
			}
			set(config_, "/fawnds/store2/datastore/file", dir_ + "/back_datastore");
			set(config_, "/fawnds/store2/datastore/use-buffered-io-only", io);
		} else {
			config_ = new fawn::Configuration(SILT_CONFIG_DIR "/silt_sorted.xml");
			set(config_, "key-len", std::to_string(kKeyLen));
			set(config_, "data-len", std::to_string(kDataLen));
			set(config_, "size", n);
			set(config_, "/fawnds/datastore/file", dir_ + "/store");
			set(config_, "/fawnds/datastore/use-buffered-io-only", io);
		}
		const std::string used = dir_ + "/silt_config_used.xml";
		config_->WriteConfigFile(used);
		std::ifstream in(used);
		config_text_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());

		store_ = fawn::FawnDS_Factory::New(config_);
		if (store_ == nullptr) fail("SILT: cannot create the store from the configuration");
		if (const auto r = store_->Create(); r != fawn::OK) silt_fail("Create", r);

		if (args.mode == "sorted") {
			// SILT's sorter, configured as in SILT's own conversion code / PaCHash-Experiments.
			auto* sc = new fawn::Configuration();
			for (const auto& [name, value] : std::vector<std::pair<std::string, std::string>>{
				     {"type", "sorter"}, {"key-len", std::to_string(kKeyLen)}, {"data-len", std::to_string(kDataLen)},
				     {"size", n}, {"temp-file", dir_ + "/"}}) {
				if (sc->CreateNodeAndAppend(name, ".") != 0) fail("SILT sorter configuration: " + name);
				set(sc, name, value);
			}
			sorter_ = fawn::FawnDS_Factory::New(sc);
			if (sorter_ == nullptr) fail("SILT: cannot create the sorter");
			if (const auto r = sorter_->Create(); r != fawn::OK) silt_fail("sorter Create", r);
		}
	}

	~SiltStore() {
		if (sorter_) { sorter_->Destroy(); delete sorter_; }
		store_->Close();
		store_->Destroy();
		delete store_;
		if (!args_.keep_db) std::filesystem::remove_all(dir_);
	}

	void insert(KeyType k) {
		const uint64_t h = key_hash(k);
		fawn::FawnDS* target = sorter_ ? sorter_ : store_;
		if (const auto r = target->Put(fawn::ConstRefValue(&h), fawn::ConstRefValue(&kValue, kDataLen)); r != fawn::OK)
			silt_fail("Put", r);
	}

	// combi: Flush. sorted: sort all keys (SILT's sorter), then build the sorted store from them.
	void finish_insertions() {
		if (!sorter_) {
			if (const auto r = store_->Flush(); r != fawn::OK) silt_fail("Flush", r);
			return;
		}
		if (const auto r = sorter_->Flush(); r != fawn::OK) silt_fail("sorter Flush", r);
		for (fawn::FawnDS_ConstIterator it = sorter_->Enumerate(); !it.IsEnd(); ++it)
			if (const auto r = store_->Put(it->key, it->data); r != fawn::OK) silt_fail("sorted store Put", r);
		if (const auto r = store_->Flush(); r != fawn::OK) silt_fail("sorted store Flush", r);
		sorter_->Destroy();
		delete sorter_;
		sorter_ = nullptr;
	}

	bool find(KeyType k) {
		const uint64_t h = key_hash(k);
		fawn::Value v;
		const auto r = store_->Get(fawn::ConstRefValue(&h), v);
		return r == fawn::OK && v.size() == kDataLen && v.data()[0] == kValue;
	}

	uint64_t disk_bytes() const {
		uint64_t total = 0;
		std::error_code ec;
		for (const auto& e : std::filesystem::recursive_directory_iterator(dir_, ec))
			if (e.is_regular_file(ec) && e.path().filename() != "silt_config_used.xml") total += e.file_size(ec);
		return total;
	}

	std::string settings() const {
		std::ostringstream ss;
		ss << "SILT commit 55ee50b (ByteHamster/silt build-fix)"
		   << "\nSILT mode " << args_.mode << (args_.mode == "combi" ? " (combined store)" : " (sorted store, bulk-built)")
		   << "\nSILT I/O " << (args_.buffered_io ? "buffered (use-buffered-io-only 1)" : "direct (use-buffered-io-only 0, SILT default)")
		   << "\nSILT db dir " << dir_
		   << "\nKey encoding 8-byte MurmurHash64 of the " << sizeof(KeyType) << "-byte key, value 1 byte"
		   << "\nSILT configuration begin\n" << config_text_ << "\nSILT configuration end\n";
		return ss.str();
	}

private:
	const Args& args_;
	const std::string dir_;
	fawn::Configuration* config_ = nullptr; // owned by the store (SILT keeps using it)
	fawn::FawnDS* store_ = nullptr;
	fawn::FawnDS* sorter_ = nullptr;
	std::string config_text_;
};

void final_report(Log& log, SiltStore& store, const ProcIOStats& os_before, Clock::time_point t_start) {
	report_totals(log, os_before, t_start);
	log << "SILT disk bytes: " << store.disk_bytes() << "\n"
	    << "End\n";
}

/*********************************** benchmarks ***********************************/

// Mirrors insertions_then_queries_benchmark in examples/containers/boa.cpp.
void insertions_then_queries(const Args& args, SiltStore& store) {
	std::vector<KeyType> data;
	generate_data(args.w.key_dist, data, args.w.size);

	const std::string query_dists_name = args.w.query_dists_name();

	const std::string tmp_filename = "silt_"
		+ store_name_params(args)
		+ "_" + to_string(args.w.key_dist) + "_" + query_dists_name + ".txt";
	const std::string filename = args.w.log_filename.empty() ? get_available_filename(tmp_filename) : args.w.log_filename;

	Log log(filename);

	const ProcIOStats os_before = ProcIOStats::read_current();
	const auto t_start = Clock::now();

	log << "Start SILT"
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
	store.finish_insertions(); // combined store: Flush; sorted store: sort + build (part of the insertion time)
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
void insertions_with_queries(const Args& args, SiltStore& store) {
	const int size = args.w.size;
	std::vector<KeyType> data;
	uniform_random_data(data, size);

	const std::string tmp_filename = "silt_interleaved_"
		+ store_name_params(args)
		+ "_" + std::to_string(args.w.insertions_per_batch) + "_" + std::to_string(args.w.queries_per_batch) + ".txt";
	const std::string filename = args.w.log_filename.empty() ? get_available_filename(tmp_filename) : args.w.log_filename;

	Log log(filename);

	const ProcIOStats os_before = ProcIOStats::read_current();
	const auto t_start = Clock::now();

	log << "Interleaved Start SILT"
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
		SiltStore store(args); // fresh store per experiment
		if (args.w.interleaved)
			insertions_with_queries(args, store);
		else
			insertions_then_queries(args, store);
	}
	return 0;
}
