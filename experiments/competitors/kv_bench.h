// The two benchmarks of the competitor drivers for any key-value store that offers the small interface
// below (LMDB, SplinterDB, WiredTiger, FASTER). The loops are those of leveldb/leveldb_bench.cpp
// (mirrors of insertions_then_queries_benchmark and insertions_with_queries_benchmark in
// examples/containers/boa.cpp): same key/query generators and seeds, same phase structure, same results-file
// lines; I/O comes from /proc/self/io.
//
// A store class provides (key: KeyType, value: one byte 'a'):
//   void insert(KeyType);            // one insertion (the store may buffer, e.g. in a transaction)
//   void flush();                    // end of the insertions of a phase / of a batch: everything is visible to find()
//   void begin_queries();            // before the queries of a phase / of a batch
//   bool find(KeyType);              // true if the key is present
//   void end_queries();              // after them
//   bool compact();                  // work between insertions and queries (true if it did something, it is timed separately)
//   std::string settings() const;    // every setting in effect, one per line
//   std::string final_stats();       // store-specific lines for the end of the results file
//   uint64_t disk_bytes() const;     // size of the store on disk
#pragma once

#include "common.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>

// Bytes of all regular files below a directory.
inline uint64_t directory_bytes(const std::string& dir) {
	uint64_t total = 0;
	std::error_code ec;
	for (const auto& e : std::filesystem::recursive_directory_iterator(dir, ec))
		if (e.is_regular_file(ec)) total += e.file_size(ec);
	return total;
}

// Writes the 4-byte big-endian encoding of a key: bytewise order equals numeric order.
inline void encode_key(KeyType k, char* out) {
	for (std::size_t i = 0; i < sizeof(KeyType); ++i)
		out[i] = static_cast<char>((k >> (8 * (sizeof(KeyType) - 1 - i))) & 0xff);
}

template <typename Store>
using StoreFactory = std::function<std::unique_ptr<Store>()>;

template <typename Store>
void final_report(Log& log, const std::string& name, Store& store, const ProcIOStats& os_before, Clock::time_point t_start) {
	report_totals(log, os_before, t_start);
	log << name << " disk bytes: " << store.disk_bytes() << "\n" << store.final_stats() << "End\n";
}

// Mirrors insertions_then_queries_benchmark in examples/containers/boa.cpp.
template <typename Store>
void insertions_then_queries(const WorkloadArgs& w, const std::string& name, const std::string& tag, const std::string& settings_tag,
                             const StoreFactory<Store>& make) {
	std::vector<KeyType> data;
	generate_data(w.key_dist, data, w.size);

	const std::string query_dists_name = w.query_dists_name();
	const std::string tmp_filename = tag + "_" + settings_tag + "_" + to_string(w.key_dist) + "_" + query_dists_name + ".txt";
	const std::string filename = w.log_filename.empty() ? get_available_filename(tmp_filename) : w.log_filename;

	Log log(filename);

	memory_baseline(w); // keys generated, structure not yet created
	std::unique_ptr<Store> store_ptr = make();
	Store& store = *store_ptr;
	const ProcIOStats os_before = ProcIOStats::read_current();
	const auto t_start = Clock::now();

	log << "Start " << name
	    << "\nElements: " << data.size()
	    << "\nKey distribution " << to_string(w.key_dist)
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
		if (counter % w.output_every_inserts == 0) {
			const Ms elapsed = Clock::now() - start_iteration;
			if (!w.io_details) {
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
	store.flush();
	const Ms insertions_total = Clock::now() - t_start_insertions;
	log << "End Insertions, time " << insertions_total.count() << "\n"
	    << "Phase I/O: " << io_details(ProcIOStats::read_current() - io_phase) << "\n";

	io_phase = ProcIOStats::read_current();
	const auto t_compaction = Clock::now();
	if (store.compact()) {
		const Ms compaction = Clock::now() - t_compaction;
		log << "Compaction, time " << compaction.count() << "\n"
		    << "Phase I/O: " << io_details(ProcIOStats::read_current() - io_phase) << "\n"
		    << "Insertions + compaction, time " << (insertions_total + compaction).count() << "\n";
	}

	/******************************** Queries ********************************/
	for (QueryDistribution query_dist : w.query_dists) {
		std::vector<KeyType> queries;
		generate_queries(query_dist, data, queries, w.queries_no);

		log << "Start Queries " << to_string(query_dist) << "\n"
		    << "Queries checksum " << to_string(query_dist) << " " << checksum(queries) << "\n";
		io_phase = ProcIOStats::read_current();
		const auto t_start_queries = Clock::now();
		start_iteration = Clock::now();
		counter = 0;
		store.begin_queries();
		for (KeyType d : queries) {
			if (counter == w.queries_no) break;
			++counter;
			if (counter % w.output_every_queries == 0) {
				const Ms elapsed = Clock::now() - start_iteration;
				if (!w.io_details) {
					log << "Queries " << counter << ", time " << elapsed.count() << "\n";
				} else {
					log << "Queries " << counter << ", time " << elapsed.count() << ", details: "
					    << io_details(ProcIOStats::read_current() - io_phase) << "\n";
				}
				start_iteration = Clock::now();
			}
			if (!store.find(d)) {
				log << "did not find " << d << "\n";
				store.end_queries();
				return;
			}
		}
		store.end_queries();
		const Ms queries_total = Clock::now() - t_start_queries;
		log << "End Queries " << to_string(query_dist) << ", time " << queries_total.count() << "\n"
		    << "Phase I/O: " << io_details(ProcIOStats::read_current() - io_phase) << "\n";
	}

	final_report(log, name, store, os_before, t_start);
	std::cout << "Output file [" << filename << "]\n";
}

// Mirrors insertions_with_queries_benchmark in examples/containers/boa.cpp: keys from --key-dist, and
// query keys picked with rand() exactly as boa does (same glibc sequence, never reseeded).
template <typename Store>
void insertions_with_queries(const WorkloadArgs& w, const std::string& name, const std::string& tag, const std::string& settings_tag,
                             const StoreFactory<Store>& make) {
	const int size = w.size;
	std::vector<KeyType> data;
	generate_data(w.key_dist, data, size);

	const std::string tmp_filename = tag + "_interleaved_" + settings_tag
		+ "_" + std::to_string(w.insertions_per_batch) + "_" + std::to_string(w.queries_per_batch) + ".txt";
	const std::string filename = w.log_filename.empty() ? get_available_filename(tmp_filename) : w.log_filename;

	Log log(filename);

	memory_baseline(w); // keys generated, structure not yet created
	std::unique_ptr<Store> store_ptr = make();
	Store& store = *store_ptr;
	const ProcIOStats os_before = ProcIOStats::read_current();
	const auto t_start = Clock::now();

	log << "Interleaved Start " << name
	    << "\nElements: " << data.size()
	    << "\nKey distribution " << to_string(w.key_dist)
	    << "\nInsertions per batch: " << w.insertions_per_batch
	    << "\nQueries per batch: " << w.queries_per_batch
	    << "\nKeys checksum " << checksum(data)
	    << "\n" << store.settings();

	int total_processed = 0;
	int batch_no = 0;
	while (total_processed < size) {
		++batch_no;
		const ProcIOStats io_batch = ProcIOStats::read_current();
		const auto t_start_batch = Clock::now();

		const int insert_end = std::min(total_processed + w.insertions_per_batch, size);
		int n_inserted = 0;
		for (int i = total_processed; i < insert_end; ++i) {
			store.insert(data[i]);
			++n_inserted;
		}
		store.flush();
		total_processed = insert_end;
		const ProcIOStats insert_io = ProcIOStats::read_current() - io_batch;
		const Ms insert_elapsed = Clock::now() - t_start_batch;

		const ProcIOStats io_queries = ProcIOStats::read_current();
		const auto t_start_queries = Clock::now();
		const int actual_queries = std::min(w.queries_per_batch, total_processed);
		store.begin_queries();
		for (int q = 0; q < actual_queries; ++q) {
			const KeyType key = data[rand() % total_processed];
			if (!store.find(key)) {
				log << "Batch " << batch_no << ": query error for key " << key << "\n";
				store.end_queries();
				return;
			}
		}
		store.end_queries();
		const ProcIOStats query_io = ProcIOStats::read_current() - io_queries;
		const Ms queries_elapsed = Clock::now() - t_start_queries;
		const Ms batch_elapsed = Clock::now() - t_start_batch;

		log << "Batch " << batch_no
		    << " | Insertions " << n_inserted
		    << ", Insertions time " << insert_elapsed.count() << " ms"
		    << ", Queries " << actual_queries
		    << ", Queries time " << queries_elapsed.count() << " ms"
		    << ", total time " << batch_elapsed.count() << " ms";
		if (w.io_details) {
			log << ", insert details: [" << io_details(insert_io) << "]"
			    << ", query details: [" << io_details(query_io) << "]"
			    << ", disk bytes " << store.disk_bytes();
		}
		log << "\n";
	}

	final_report(log, name, store, os_before, t_start);
	std::cout << "Output file [" << filename << "]\n";
}

template <typename Store>
void run_benchmarks(const WorkloadArgs& w, const std::string& name, const std::string& tag, const std::string& settings_tag,
                    const StoreFactory<Store>& make) {
	for (int i = 0; i < w.experiments_no; ++i) {
		if (w.interleaved) insertions_with_queries<Store>(w, name, tag, settings_tag, make);
		else insertions_then_queries<Store>(w, name, tag, settings_tag, make);
	}
}
