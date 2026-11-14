// PaCHash competitor driver.
//
// PaCHash is a static object store: it is constructed once from all keys and then queried. It is compared
// with BOA's online-suspended mode (boa --suspended / --materialization), which also builds the structure
// in one step. The workload (keys, query distributions, seeds) is the same as in
// examples/containers/boa.cpp; see experiments/competitors/pachash/README.md for every PaCHash setting.

#include <cstdint> // PaCHash's StoreConfig.h uses uint64_t without including <cstdint>

#include <IoManager.h>
#include <ObjectStoreView.h>
#include <PaCHashObjectStore.h>
#include <QueryHandle.h>
#include <bytehamster/util/MurmurHash64.h>

#include "common.h"

#include <fcntl.h>

#include <filesystem>
#include <memory>

#ifndef VARIABLE_SIZE_STORE_BLOCK_LENGTH
#error "Build with experiments/competitors/pachash/build.sh (sets VARIABLE_SIZE_STORE_BLOCK_LENGTH)"
#endif

#if HAS_LIBURING
constexpr bool kWithUring = true;
#else
constexpr bool kWithUring = false;
#endif

/*********************************** arguments ***********************************/

struct Args {
	WorkloadArgs w;

	std::string store_file = "pachash_store.db";
	size_t a = 8;                   // PaCHash parameter a (bins per block)
	std::string io = "posix";       // posix | io_submit | uring
	size_t queue_depth = 1;         // queries in flight
	bool cached_io = false;         // false: O_DIRECT (as in PaCHash's own benchmark)
	bool keep_store = false;
};

const size_t kSupportedA[] = {1, 2, 4, 8, 16, 32, 64, 128};

void print_usage(const char* prog) {
	const Args d;
	std::cout << "Usage: " << prog << " [options]\n";
	print_workload_usage(false);
	std::cout << "PaCHash:"
		<< "\n  --store-file PATH         object store file, removed before and after each experiment (default " << d.store_file << ")"
		<< "\n  --a N                     PaCHash parameter a, bins per block: 1 2 4 8 16 32 64 128 (default " << d.a << ")"
		<< "\n  --io NAME                 query I/O: posix (pread), io_submit (Linux AIO syscalls)"
#if HAS_LIBURING
		<< ", uring"
#endif
		<< " (default " << d.io << ")"
		<< "\n  --queue-depth N           queries in flight (default " << d.queue_depth << ": one query at a time, like boa)"
		<< "\n  --cached-io               use the page cache instead of O_DIRECT (default: O_DIRECT)"
		<< "\n  --keep-store              do not remove the store file after the experiment"
		<< "\n  --help                    show this message"
		<< "\nBlock size (compile time): " << pachash::StoreConfig::BLOCK_LENGTH << " bytes\n";
}

Args parse_args(int argc, char* argv[]) {
	Args args;
	for (int i = 1; i < argc; ++i) {
		const std::string opt = argv[i];
		if (opt == "--help" || opt == "-h") { print_usage(argv[0]); std::exit(0); }
		else if (parse_workload_option(argc, argv, i, args.w)) continue;
		else if (opt == "--store-file") args.store_file = option_value(argc, argv, i);
		else if (opt == "--a") args.a = parse_positive(opt, option_value(argc, argv, i));
		else if (opt == "--io") args.io = option_value(argc, argv, i);
		else if (opt == "--queue-depth") args.queue_depth = parse_positive(opt, option_value(argc, argv, i));
		else if (opt == "--cached-io") args.cached_io = true;
		else if (opt == "--keep-store") args.keep_store = true;
		else { print_usage(argv[0]); fail("Unknown option: " + opt); }
	}
	if (args.w.interleaved)
		fail("PaCHash is a static object store: --interleaved is not supported "
		     "(compare with boa's online-suspended mode instead)");
	if (std::find(std::begin(kSupportedA), std::end(kSupportedA), args.a) == std::end(kSupportedA))
		fail("Invalid value for --a: " + std::to_string(args.a) + " (supported: 1 2 4 8 16 32 64 128)");
	if (args.io != "posix" && args.io != "io_submit" && args.io != "uring")
		fail("Invalid value for --io: " + args.io + " (expected posix, io_submit or uring)");
#if !HAS_LIBURING
	if (args.io == "uring")
		fail("--io uring: this binary was built without io_uring (pachash/build.sh with PACHASH_WITH_URING=ON)");
#endif
	if (args.io == "io_submit" && args.queue_depth >= 64)
		fail("--io io_submit supports a queue depth of at most 63 (PaCHash limitation)");
	return args;
}

/*********************************** benchmark ***********************************/

// Same hash as HashFunction in examples/containers/boa.cpp. PaCHash stores objects by this 64-bit hash.
inline uint64_t key_hash(const KeyType& k) {
	return bytehamster::util::MurmurHash64(&k, sizeof(k));
}

const char kValue = 'a'; // 1-byte value, like boa.cpp's char 'a'

template <typename Store, typename Io>
void run(const Args& args) {
	const WorkloadArgs& w = args.w;

	std::vector<KeyType> data;
	generate_data(w.key_dist, data, w.size);

	const std::string query_dists_name = w.query_dists_name();
	const std::string tmp_filename = "pachash_" + std::to_string(args.a)
		+ "_" + std::to_string(pachash::StoreConfig::BLOCK_LENGTH)
		+ "_" + args.io + "_" + std::to_string(args.queue_depth) + (args.cached_io ? "_cached" : "")
		+ "_" + to_string(w.key_dist) + "_" + query_dists_name + ".txt";
	const std::string filename = w.log_filename.empty() ? get_available_filename(tmp_filename) : w.log_filename;
	Log log(filename);

	for (KeyType k : data)
		if (key_hash(k) == 0) fail("Key " + std::to_string(k) + " hashes to 0, which PaCHash reserves for metadata");

	memory_baseline(w); // keys generated, structure not yet created
	std::filesystem::remove(args.store_file);
	const int open_flags = args.cached_io ? 0 : O_DIRECT;
	Store store(1.0f, args.store_file.c_str(), open_flags);

	const ProcIOStats os_before = ProcIOStats::read_current();
	const auto t_start = Clock::now();

	log << "Start PaCHash"
	    << "\nElements: " << data.size()
	    << "\nKey distribution " << to_string(w.key_dist)
	    << "\nQuery distributions " << query_dists_name
	    << "\nKeys checksum " << checksum(data)
	    << "\nPaCHash commit f7f18fd"
	    << "\nPaCHash method " << Store::name()
	    << "\nPaCHash a " << args.a
	    << "\nPaCHash block length " << pachash::StoreConfig::BLOCK_LENGTH
	    << "\nPaCHash query io " << args.io
	    << "\nPaCHash queue depth " << args.queue_depth
	    << "\nPaCHash open flags " << (args.cached_io ? "none (page cache)" : "O_DIRECT")
	    << "\nPaCHash built with io_uring " << (kWithUring ? "yes" : "no")
	    << "\nPaCHash store file " << args.store_file
	    << "\nKey hash MurmurHash64 of the " << sizeof(KeyType) << "-byte key, value 1 byte\n";

	/******************************** Construction ********************************/
	// PaCHash's writeToFile sorts its input range, so it gets a copy: `data` stays in insertion order
	// for the query generators (e.g. latest). The copy is made before the timer starts.
	std::vector<KeyType> input(data);
	auto length = [](const KeyType&) -> size_t { return 1; };
	auto value = [](const KeyType&) -> const char* { return &kValue; };

	log << "Start Insertions\n";
	ProcIOStats io_phase = ProcIOStats::read_current();
	const auto t_construction = Clock::now();

	store.writeToFile(input.begin(), input.end(), key_hash, length, value); // sort by hash + write blocks
	const Ms write_time = Clock::now() - t_construction;
	log << "Construction write, time " << write_time.count() << ", details: "
	    << io_details(ProcIOStats::read_current() - io_phase) << "\n";

	const auto t_index = Clock::now();
	store.buildIndex(); // read the file back, build the in-memory index
	const Ms index_time = Clock::now() - t_index;
	const Ms construction_total = Clock::now() - t_construction;
	log << "Construction index, time " << index_time.count() << "\n"
	    << "End Insertions, time " << construction_total.count() << "\n"
	    << "Phase I/O: " << io_details(ProcIOStats::read_current() - io_phase) << "\n";
	std::vector<KeyType>().swap(input);

	/******************************** Queries ********************************/
	for (QueryDistribution query_dist : w.query_dists) {
		std::vector<KeyType> queries;
		generate_queries(query_dist, data, queries, w.queries_no);
		const size_t n = std::min(queries.size(), static_cast<size_t>(w.queries_no));

		std::vector<std::unique_ptr<pachash::QueryHandle>> handles;
		for (size_t i = 0; i < args.queue_depth; ++i)
			handles.push_back(std::make_unique<pachash::QueryHandle>(store));
		pachash::ObjectStoreView<Store, Io> view(store, open_flags, args.queue_depth);

		log << "Start Queries " << to_string(query_dist) << "\n"
		    << "Queries checksum " << to_string(query_dist) << " " << checksum(queries) << "\n";
		io_phase = ProcIOStats::read_current();
		const auto t_start_queries = Clock::now();
		auto start_iteration = Clock::now();

		size_t next = 0, done = 0;
		auto enqueue = [&](pachash::QueryHandle* h) {
			h->key = key_hash(queries[next]);
			h->name = next;
			view.enqueueQuery(h);
			++next;
		};
		for (size_t i = 0; i < args.queue_depth && next < n; ++i) enqueue(handles[i].get());
		view.submit();

		while (done < n) {
			pachash::QueryHandle* h = view.awaitAny();
			while (h != nullptr) {
				if (h->resultPtr == nullptr || h->length != 1 || *h->resultPtr != kValue) {
					log << "did not find " << queries[h->name] << "\n";
					return;
				}
				++done;
				if (done % w.output_every_queries == 0) {
					const Ms elapsed = Clock::now() - start_iteration;
					log << "Queries " << done << ", time " << elapsed.count();
					if (w.io_details) log << ", details: " << io_details(ProcIOStats::read_current() - io_phase);
					log << "\n";
					start_iteration = Clock::now();
				}
				if (next < n) enqueue(h);
				h = next > done ? view.peekAny() : nullptr; // more completed requests, if any are in flight
			}
			view.submit();
		}

		const Ms queries_total = Clock::now() - t_start_queries;
		log << "End Queries " << to_string(query_dist) << ", time " << queries_total.count() << "\n"
		    << "Phase I/O: " << io_details(ProcIOStats::read_current() - io_phase) << "\n";
	}

	report_totals(log, os_before, t_start);
	const uintmax_t file_bytes = std::filesystem::file_size(args.store_file);
	log << "PaCHash blocks " << file_bytes / pachash::StoreConfig::BLOCK_LENGTH << "\n"
	    << "PaCHash file bytes " << file_bytes << "\n"
	    << "PaCHash index bytes " << store.index->space() << "\n"
	    << "PaCHash index bits per block " << store.internalSpaceUsage() << "\n"
	    << "PaCHash construction timer" << store.constructionTimer << "\n"
	    << "End\n";

	if (!args.keep_store) std::filesystem::remove(args.store_file);
	std::cout << "Output file [" << filename << "]\n";
}

template <typename Store>
void dispatch_io(const Args& args) {
	if (args.io == "posix") run<Store, pachash::PosixIO>(args);
	else if (args.io == "io_submit") run<Store, pachash::LinuxIoSubmit>(args);
#if HAS_LIBURING
	else if (args.io == "uring") run<Store, pachash::UringIO>(args);
#endif
}

template <size_t A>
bool dispatch_a(const Args& args) {
	if (args.a != A) return false;
	dispatch_io<pachash::PaCHashObjectStore<A>>(args);
	return true;
}

int main(int argc, char* argv[]) {
	const Args args = parse_args(argc, argv);
	for (int i = 0; i < args.w.experiments_no; ++i) {
		dispatch_a<1>(args) || dispatch_a<2>(args) || dispatch_a<4>(args) || dispatch_a<8>(args)
			|| dispatch_a<16>(args) || dispatch_a<32>(args) || dispatch_a<64>(args) || dispatch_a<128>(args);
	}
	return 0;
}
