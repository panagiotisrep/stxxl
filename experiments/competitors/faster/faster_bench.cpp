// FASTER competitor driver (FASTER's C++ FasterKv with a hybrid log on disk).
//
// Runs the same workloads as examples/containers/boa.cpp through the shared loops in kv_bench.h (same key/query
// generators and seeds, same options, same phases and checkpoints). I/O is measured from /proc/self/io. FASTER has
// no defaults for its sizes: the hash index (--table-size buckets of 64 bytes, always in memory) and the in-memory
// part of the log (--log-size, a multiple of the 32 MiB log page) are set by the options below. See
// experiments/competitors/faster/README.md.

#include "core/auto_ptr.h"
#include "core/faster.h"
#include "device/file_system_disk.h"

#include "kv_bench.h"

#include <cstring>

using namespace FASTER::core;

class Key {
public:
	Key(uint32_t key) : key_{ key } {}
	inline static constexpr uint32_t size() { return static_cast<uint32_t>(sizeof(Key)); }
	inline KeyHash GetHash() const { return KeyHash{ Utility::GetHashCode(key_) }; }
	inline bool operator==(const Key& other) const { return key_ == other.key_; }
	inline bool operator!=(const Key& other) const { return key_ != other.key_; }
private:
	uint32_t key_;
};

class Value {
public:
	Value() : value_{ 0 } {}
	Value(uint8_t v) : value_{ v } {}
	inline static constexpr uint32_t size() { return static_cast<uint32_t>(sizeof(Value)); }
	uint8_t value_;
};

static Status g_read_status = Status::Ok; // result of the last pending read (set by its callback)
static uint8_t g_read_value = 0;

class ReadContext : public IAsyncContext {
public:
	typedef Key key_t;
	typedef Value value_t;
	explicit ReadContext(uint32_t key) : key_{ key } {}
	ReadContext(const ReadContext& other) : key_{ other.key_ } {}
	inline const Key& key() const { return key_; }
	inline void Get(const value_t& value) { g_read_value = value.value_; }
	inline void GetAtomic(const value_t& value) { g_read_value = value.value_; }
protected:
	Status DeepCopy_Internal(IAsyncContext*& context_copy) { return IAsyncContext::DeepCopy_Internal(*this, context_copy); }
private:
	Key key_;
};

class UpsertContext : public IAsyncContext {
public:
	typedef Key key_t;
	typedef Value value_t;
	explicit UpsertContext(uint32_t key) : key_{ key } {}
	UpsertContext(const UpsertContext& other) : key_{ other.key_ } {}
	inline const Key& key() const { return key_; }
	inline static constexpr uint32_t value_size() { return sizeof(Value); }
	inline void Put(Value& value) { value.value_ = 'a'; }
	inline bool PutAtomic(Value& value) { value.value_ = 'a'; return true; }
protected:
	Status DeepCopy_Internal(IAsyncContext*& context_copy) { return IAsyncContext::DeepCopy_Internal(*this, context_copy); }
private:
	Key key_;
};

typedef FASTER::environment::QueueIoHandler handler_t;
typedef FASTER::device::FileSystemDisk<handler_t, 1073741824ull> disk_t;
using store_t = FasterKv<Key, Value, disk_t>;

struct Args {
	WorkloadArgs w;
	std::string db_dir = "faster_bench_db";
	uint64_t table_size = uint64_t(1) << 20; // hash buckets (power of two), 64 bytes each: the benchmark's choice
	uint64_t log_size = uint64_t(512) << 20; // in-memory part of the log in bytes (multiple of the 32 MiB log page): the benchmark's choice
	double mutable_fraction = 0.9;           // FasterKv's default
	bool keep_db = false;
};

void print_usage(const char* prog) {
	const Args d;
	std::cout << "Usage: " << prog << " [options]\n";
	print_workload_usage(true);
	std::cout << "FASTER (FasterKv<Key, Value, FileSystemDisk>; defaults are FASTER's own unless stated):"
		<< "\n  --db-dir PATH             directory of the log file, removed before each experiment (default " << d.db_dir << ")"
		<< "\n  --table-size N            hash index size in buckets, a power of two, 64 bytes each (default " << d.table_size << "; the benchmark's choice)"
		<< "\n  --log-size B              in-memory part of the hybrid log, a multiple of 32 MiB (default " << d.log_size << "; the benchmark's choice)"
		<< "\n  --mutable-fraction F      FasterKv log_mutable_fraction (default " << d.mutable_fraction << ")"
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
		else if (opt == "--table-size") args.table_size = static_cast<uint64_t>(parse_positive(opt, option_value(argc, argv, i)));
		else if (opt == "--log-size") args.log_size = static_cast<uint64_t>(parse_positive(opt, option_value(argc, argv, i)));
		else if (opt == "--mutable-fraction") args.mutable_fraction = std::stod(option_value(argc, argv, i));
		else if (opt == "--keep-db") args.keep_db = true;
		else { print_usage(argv[0]); fail("Unknown option: " + opt); }
	}
	return args;
}

static void upsert_callback(IAsyncContext* ctxt, Status result) { (void)ctxt; (void)result; }
static void read_callback(IAsyncContext* ctxt, Status result) { (void)ctxt; g_read_status = result; }

class FasterStore {
public:
	explicit FasterStore(const Args& args) : args_(args) {
		std::filesystem::remove_all(args.db_dir);
		std::filesystem::create_directories(args.db_dir);
		store_ = std::make_unique<store_t>(args.table_size, args.log_size, args.db_dir + "/", args.mutable_fraction);
		store_->StartSession();
	}

	~FasterStore() {
		store_->CompletePending(true);
		store_->StopSession();
		store_.reset();
		if (!args_.keep_db) std::filesystem::remove_all(args_.db_dir);
	}

	void insert(KeyType k) {
		UpsertContext context{ k };
		store_->Upsert(context, upsert_callback, ++serial_);
		if (serial_ % 1600 == 0) store_->CompletePending(false); // as in FASTER's benchmark
	}

	void flush() { store_->CompletePending(true); }
	void begin_queries() {}
	void end_queries() {}

	bool find(KeyType k) {
		ReadContext context{ k };
		g_read_value = 0;
		const Status s = store_->Read(context, read_callback, ++serial_);
		if (s == Status::Pending) { // the record is on disk: wait for it
			g_read_status = Status::Pending;
			store_->CompletePending(true);
			return g_read_status == Status::Ok && g_read_value == 'a';
		}
		return s == Status::Ok && g_read_value == 'a';
	}

	bool compact() { return false; }

	uint64_t disk_bytes() const { return directory_bytes(args_.db_dir); }

	std::string settings() const {
		std::ostringstream ss;
		ss << "FASTER db dir " << args_.db_dir
		   << "\nFASTER hash table size " << args_.table_size << " buckets (" << args_.table_size * 64 << " bytes, always in memory)"
		   << "\nFASTER in-memory log size " << args_.log_size
		   << "\nFASTER log mutable fraction " << args_.mutable_fraction
		   << "\nFASTER disk FileSystemDisk, QueueIoHandler (libaio), 1 GiB log segments"
		   << "\nKey encoding " << sizeof(KeyType) << "-byte integer, value 1 byte (stored with 8-byte record alignment)\n";
		return ss.str();
	}

	std::string final_stats() { return ""; }

private:
	const Args& args_;
	std::unique_ptr<store_t> store_;
	uint64_t serial_ = 0;
};

int main(int argc, char* argv[]) {
	const Args args = parse_args(argc, argv);
	const std::string settings_tag = std::to_string(args.table_size) + "_" + std::to_string(args.log_size);
	run_benchmarks<FasterStore>(args.w, "FASTER", "faster", settings_tag, [&]() { return std::make_unique<FasterStore>(args); });
	return 0;
}
