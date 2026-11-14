// Shared by the competitor drivers: workload options (same names, meanings and defaults as in
// examples/containers/boa.cpp), results-file helpers and I/O measurement.
#pragma once

#include <stxxl/bits/utils/workloads.h>

#include <sys/resource.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using KeyType = uint32_t; // must match KeyType in examples/containers/boa.cpp

// Copied from examples/containers/boa.cpp.
struct ProcIOStats {
	uint64_t rchar = 0;
	uint64_t wchar = 0;
	uint64_t syscr = 0;
	uint64_t syscw = 0;
	uint64_t read_bytes = 0;
	uint64_t write_bytes = 0;
	uint64_t cancelled_write_bytes = 0;

	static ProcIOStats read_current() {
		ProcIOStats stats;
		std::ifstream file("/proc/self/io");
		std::string key;
		uint64_t value;

		while (file >> key >> value) {
			if (key == "rchar:") stats.rchar = value;
			else if (key == "wchar:") stats.wchar = value;
			else if (key == "syscr:") stats.syscr = value;
			else if (key == "syscw:") stats.syscw = value;
			else if (key == "read_bytes:") stats.read_bytes = value;
			else if (key == "write_bytes:") stats.write_bytes = value;
			else if (key == "cancelled_write_bytes:") stats.cancelled_write_bytes = value;
		}

		return stats;
	}

	ProcIOStats operator-(const ProcIOStats &other) const {
		ProcIOStats delta;
		delta.rchar = rchar - other.rchar;
		delta.wchar = wchar - other.wchar;
		delta.syscr = syscr - other.syscr;
		delta.syscw = syscw - other.syscw;
		delta.read_bytes = read_bytes - other.read_bytes;
		delta.write_bytes = write_bytes - other.write_bytes;
		delta.cancelled_write_bytes =
				cancelled_write_bytes - other.cancelled_write_bytes;
		return delta;
	}
};

// Copied from examples/containers/boa.cpp.
inline bool file_exists(const std::string &filename) {
	std::ifstream f(filename.c_str());
	return f.good();
}

// Copied from examples/containers/boa.cpp: "<stem>_exp_<N><ext>" with the first free N.
inline std::string get_available_filename(const std::string &base_name) {
	std::string stem = base_name;
	std::string ext;

	std::size_t dot = base_name.find_last_of('.');
	if (dot != std::string::npos) {
		stem = base_name.substr(0, dot);
		ext = base_name.substr(dot);
	}

	int counter = 1;
	while (true) {
		std::ostringstream candidate;
		candidate << stem << "_exp_" << counter << ext;

		if (!file_exists(candidate.str()))
			return candidate.str();

		++counter;
	}
}

/*********************************** workload options ***********************************/

struct WorkloadArgs {
	int size = 1000000;
	int queries_no = 20000;
	int output_every_inserts = 100000;
	int output_every_queries = 1000;
	int experiments_no = 1;
	KeyDistribution key_dist = KeyDistribution::UniformRandom;
	std::vector<QueryDistribution> query_dists = {QueryDistribution::Shuffled};
	bool io_details = true;
	std::string log_filename; // empty: generated from the parameters
	bool interleaved = false;
	int insertions_per_batch = 50000;
	int queries_per_batch = 2000;
	std::string baseline_file; // --baseline-file: see memory_baseline()

	std::string query_dists_name() const {
		std::string name;
		for (QueryDistribution qd : query_dists)
			name += (name.empty() ? "" : "-") + std::string(to_string(qd));
		return name;
	}
};

inline void print_workload_usage(bool with_interleaved) {
	const WorkloadArgs d;
	std::cout << "Workload (same meaning as in boa):\n"
		<< "  --size N                  number of keys to insert (default " << d.size << ")\n"
		<< "  --queries N               queries per query distribution (default " << d.queries_no << ")\n"
		<< "  --output-every-inserts N  insertion checkpoint interval (default " << d.output_every_inserts << ")\n"
		<< "  --output-every-queries N  query checkpoint interval (default " << d.output_every_queries << ")\n"
		<< "  --repeat N                number of experiments (default " << d.experiments_no << ")\n"
		<< "  --key-dist NAME           key distribution (default " << to_string(d.key_dist) << "):";
	for (KeyDistribution k : all_key_distributions) std::cout << " " << to_string(k);
	std::cout << "\n  --query-dist LIST         comma-separated query distributions or 'all' (default "
		<< to_string(d.query_dists.front()) << "):";
	for (QueryDistribution q : all_query_distributions) std::cout << " " << to_string(q);
	std::cout << "\n  --log-file PATH           results file (default: generated; appended to if it exists)"
		<< "\n  --no-io-details           omit per-checkpoint I/O details"
		<< "\n  --baseline-file PATH      after generating the keys and before creating the structure, write"
		<< "\n                            \"<pid> <RSS bytes>\" to PATH and stop (SIGSTOP) until SIGCONT, so that"
		<< "\n                            experiments/run_all.sh can set the memory limit to baseline + M\n";
	if (with_interleaved)
		std::cout << "  --interleaved             batches of insertions followed by queries\n"
			<< "  --insertions-per-batch N  with --interleaved (default " << d.insertions_per_batch << ")\n"
			<< "  --queries-per-batch N     with --interleaved (default " << d.queries_per_batch << ")\n";
}

[[noreturn]] inline void fail(const std::string& msg) {
	std::cerr << msg << "\n";
	std::exit(1);
}

inline std::string option_value(int argc, char* argv[], int& i) {
	if (i + 1 >= argc) fail(std::string("Missing value for ") + argv[i]);
	++i;
	return argv[i];
}

inline long long parse_positive(const std::string& opt, const std::string& v) {
	try {
		std::size_t pos = 0;
		const long long x = std::stoll(v, &pos);
		if (pos != v.size() || x <= 0) throw std::invalid_argument(v);
		return x;
	} catch (const std::exception&) {
		fail("Invalid value for " + opt + ": " + v + " (expected a positive integer)");
	}
}

// Parses the workload option at argv[i] (advancing i past its value). Returns false if argv[i] is not
// a workload option.
inline bool parse_workload_option(int argc, char* argv[], int& i, WorkloadArgs& w) {
	const std::string opt = argv[i];
	auto positive_int = [&]() { return static_cast<int>(parse_positive(opt, option_value(argc, argv, i))); };

	if (opt == "--interleaved") w.interleaved = true;
	else if (opt == "--no-io-details") w.io_details = false;
	else if (opt == "--log-file") w.log_filename = option_value(argc, argv, i);
	else if (opt == "--baseline-file") w.baseline_file = option_value(argc, argv, i);
	else if (opt == "--key-dist") {
		const std::string v = option_value(argc, argv, i);
		if (!from_string(v, w.key_dist)) fail("Invalid value for " + opt + ": " + v);
	}
	else if (opt == "--query-dist") {
		const std::string v = option_value(argc, argv, i);
		if (v == "all") {
			w.query_dists.assign(std::begin(all_query_distributions), std::end(all_query_distributions));
		} else {
			w.query_dists.clear();
			std::stringstream names(v);
			std::string name;
			while (std::getline(names, name, ',')) {
				QueryDistribution qd;
				if (!from_string(name, qd)) fail("Invalid value for " + opt + ": " + v);
				w.query_dists.push_back(qd);
			}
			if (w.query_dists.empty()) fail("Invalid value for " + opt + ": " + v);
		}
	}
	else if (opt == "--size") w.size = positive_int();
	else if (opt == "--queries") w.queries_no = positive_int();
	else if (opt == "--output-every-inserts") w.output_every_inserts = positive_int();
	else if (opt == "--output-every-queries") w.output_every_queries = positive_int();
	else if (opt == "--repeat") w.experiments_no = positive_int();
	else if (opt == "--insertions-per-batch") w.insertions_per_batch = positive_int();
	else if (opt == "--queries-per-batch") w.queries_per_batch = positive_int();
	else return false;
	return true;
}

/*********************************** measurement and output ***********************************/

// steady_clock: never goes backwards (high_resolution_clock is the wall clock in libstdc++).
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::duration<double, std::milli>;

// FNV-1a 64 over the key values (little-endian bytes): identifies the exact workload in the results file.
inline uint64_t checksum(const std::vector<KeyType>& v) {
	uint64_t h = 14695981039346656037ull;
	for (KeyType k : v)
		for (std::size_t i = 0; i < sizeof(KeyType); ++i) {
			h ^= (k >> (8 * i)) & 0xff;
			h *= 1099511628211ull;
		}
	return h;
}

inline uint64_t peak_rss_bytes() {
	rusage ru{};
	getrusage(RUSAGE_SELF, &ru);
	return static_cast<uint64_t>(ru.ru_maxrss) * 1024; // Linux: KiB
}

// Current resident set size (VmRSS), bytes.
inline uint64_t current_rss_bytes() {
	std::ifstream status("/proc/self/status");
	std::string line;
	while (std::getline(status, line))
		if (line.rfind("VmRSS:", 0) == 0) return std::strtoull(line.c_str() + 6, nullptr, 10) * 1024;
	return 0;
}

// Memory baseline: RSS once the benchmark's own data (the keys) exists and before the structure is
// created. Everything above it is the structure (plus the library's fixed overhead); the key set itself is
// harness memory, the same in every driver and in boa. Logged as "Baseline RSS bytes" by report_totals().
// With --baseline-file the process writes "<pid> <baseline>" to that file and stops itself; run_all.sh
// then sets the cgroup memory limit to baseline + M + overhead and continues it (SIGCONT). Only the first
// call stops (--repeat).
inline uint64_t g_baseline_rss = 0;
inline void memory_baseline(const WorkloadArgs& w) {
	g_baseline_rss = current_rss_bytes();
	static bool stopped = false;
	if (w.baseline_file.empty() || stopped) return;
	stopped = true;
	{
		std::ofstream f(w.baseline_file + ".tmp");
		f << getpid() << " " << g_baseline_rss << "\n";
	}
	std::rename((w.baseline_file + ".tmp").c_str(), w.baseline_file.c_str()); // appears complete
	std::raise(SIGSTOP);
}

inline std::string io_details(const ProcIOStats& d) {
	std::ostringstream ss;
	ss << "OS bytes read " << d.read_bytes << ", "
	   << "OS bytes written " << d.write_bytes << ", "
	   << "read syscalls " << d.syscr << ", "
	   << "write syscalls " << d.syscw << ", "
	   << "rchar " << d.rchar << ", "
	   << "wchar " << d.wchar;
	return ss.str();
}

// Writes to the results file and stdout.
struct Log {
	std::ofstream out;

	explicit Log(const std::string& filename) : out(filename, std::ios::app) {
		if (!out) fail("Cannot open results file " + filename);
	}

	template <typename T>
	Log& operator<<(const T& x) { out << x; std::cout << x; return *this; }
};

// Same lines as at the end of boa.cpp's benchmarks.
inline void report_totals(Log& log, const ProcIOStats& os_before, Clock::time_point t_start) {
	const ProcIOStats d = ProcIOStats::read_current() - os_before;
	log << "Total\n"
	    << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t_start).count() << "\n"
	    << "OS read_bytes:  " << d.read_bytes << "\n"
	    << "OS write_bytes: " << d.write_bytes << "\n"
	    << "OS read syscalls:  " << d.syscr << "\n"
	    << "OS write syscalls: " << d.syscw << "\n"
	    << "Peak RSS bytes: " << peak_rss_bytes() << "\n"
	    << "Baseline RSS bytes: " << g_baseline_rss << " (keys generated, structure not yet created)\n"
	    << "Peak RSS above baseline bytes: " << (peak_rss_bytes() > g_baseline_rss ? peak_rss_bytes() - g_baseline_rss : 0) << "\n";
}
