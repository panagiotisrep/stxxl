// Direct I/O for LevelDB's table files (.ldb), like BOA's direct=on and RocksDB's --direct-reads/--direct-writes.
// LevelDB has no such option; this leveldb::Env wrapper opens the table files with O_DIRECT, so reads and writes of
// the tables bypass the OS page cache (the log, MANIFEST and CURRENT files stay buffered: they are small, sequential appends).
//  - reads: every read is widened to 4 KiB alignment (offset, length, buffer) and the requested bytes are copied out
//  - writes: appended data is collected in an aligned buffer and written in aligned chunks; at Close the last partial
//    chunk is padded with zeros and the file is truncated to its real size
#pragma once

#include <leveldb/env.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <string>
#include <unistd.h>
#include <vector>

namespace direct_env {

constexpr size_t kAlign = 4096;
constexpr size_t kWriteChunk = 1 << 20; // 1 MiB aligned write buffer

inline leveldb::Status io_error(const std::string& ctx, int err) {
	return leveldb::Status::IOError(ctx, std::strerror(err));
}

struct AlignedBuf {
	char* p = nullptr;
	size_t cap = 0;
	~AlignedBuf() { std::free(p); }
	void reserve(size_t n) {
		if (n <= cap) return;
		std::free(p);
		n = (n + kAlign - 1) / kAlign * kAlign;
		if (posix_memalign(reinterpret_cast<void**>(&p), kAlign, n) != 0) { p = nullptr; cap = 0; throw std::bad_alloc(); }
		cap = n;
	}
};

class DirectRandomAccessFile final : public leveldb::RandomAccessFile {
public:
	DirectRandomAccessFile(std::string name, int fd) : name_(std::move(name)), fd_(fd) {}
	~DirectRandomAccessFile() override { ::close(fd_); }

	leveldb::Status Read(uint64_t offset, size_t n, leveldb::Slice* result, char* scratch) const override {
		thread_local AlignedBuf buf;
		const uint64_t start = offset / kAlign * kAlign;
		const size_t lead = static_cast<size_t>(offset - start);
		const size_t len = (lead + n + kAlign - 1) / kAlign * kAlign;
		buf.reserve(len);
		ssize_t got;
		do { got = ::pread(fd_, buf.p, len, static_cast<off_t>(start)); } while (got < 0 && errno == EINTR);
		if (got < 0) return io_error(name_, errno);
		const size_t avail = static_cast<size_t>(got) > lead ? static_cast<size_t>(got) - lead : 0;
		const size_t out = avail < n ? avail : n;
		std::memcpy(scratch, buf.p + lead, out);
		*result = leveldb::Slice(scratch, out);
		return leveldb::Status::OK();
	}

private:
	std::string name_;
	int fd_;
};

class DirectWritableFile final : public leveldb::WritableFile {
public:
	DirectWritableFile(std::string name, int fd) : name_(std::move(name)), fd_(fd) { buf_.reserve(kWriteChunk); }
	~DirectWritableFile() override { if (fd_ >= 0) ::close(fd_); }

	leveldb::Status Append(const leveldb::Slice& data) override {
		const char* p = data.data();
		size_t n = data.size();
		total_ += n;
		while (n > 0) {
			const size_t take = std::min(n, kWriteChunk - used_);
			std::memcpy(buf_.p + used_, p, take);
			used_ += take; p += take; n -= take;
			if (used_ == kWriteChunk) {
				leveldb::Status s = write_all(kWriteChunk);
				if (!s.ok()) return s;
				used_ = 0;
			}
		}
		return leveldb::Status::OK();
	}
	leveldb::Status Close() override {
		leveldb::Status s;
		if (used_ > 0) {
			const size_t padded = (used_ + kAlign - 1) / kAlign * kAlign;
			std::memset(buf_.p + used_, 0, padded - used_);
			s = write_all(padded);
			used_ = 0;
		}
		if (s.ok() && ::ftruncate(fd_, static_cast<off_t>(total_)) != 0) s = io_error(name_, errno);
		if (fd_ >= 0 && ::close(fd_) != 0 && s.ok()) s = io_error(name_, errno);
		fd_ = -1;
		return s;
	}
	leveldb::Status Flush() override { return leveldb::Status::OK(); } // data is written in aligned chunks / at Close
	leveldb::Status Sync() override { return leveldb::Status::OK(); }  // O_DIRECT data is not in the page cache; the table is finalised at Close

private:
	leveldb::Status write_all(size_t n) {
		size_t done = 0;
		while (done < n) {
			ssize_t w = ::pwrite(fd_, buf_.p + done, n - done, static_cast<off_t>(written_ + done));
			if (w < 0) { if (errno == EINTR) continue; return io_error(name_, errno); }
			done += static_cast<size_t>(w);
		}
		written_ += n;
		return leveldb::Status::OK();
	}
	std::string name_;
	int fd_;
	AlignedBuf buf_;
	size_t used_ = 0;
	uint64_t written_ = 0; // bytes written to the file (aligned)
	uint64_t total_ = 0;   // bytes appended (real file size)
};

inline bool is_table(const std::string& f) { return f.size() > 4 && f.compare(f.size() - 4, 4, ".ldb") == 0; }

class DirectEnv final : public leveldb::EnvWrapper {
public:
	DirectEnv() : leveldb::EnvWrapper(leveldb::Env::Default()) {}

	leveldb::Status NewRandomAccessFile(const std::string& f, leveldb::RandomAccessFile** r) override {
		if (!is_table(f)) return target()->NewRandomAccessFile(f, r);
		int fd = ::open(f.c_str(), O_RDONLY | O_DIRECT);
		if (fd < 0) return io_error(f, errno);
		*r = new DirectRandomAccessFile(f, fd);
		return leveldb::Status::OK();
	}
	leveldb::Status NewWritableFile(const std::string& f, leveldb::WritableFile** r) override {
		if (!is_table(f)) return target()->NewWritableFile(f, r);
		int fd = ::open(f.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);
		if (fd < 0) return io_error(f, errno);
		*r = new DirectWritableFile(f, fd);
		return leveldb::Status::OK();
	}
};

} // namespace direct_env
