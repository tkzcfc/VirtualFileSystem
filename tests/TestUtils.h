// This file: shared test helpers (temp directories, native file IO, archive building, deterministic random data).
#pragma once

#include "doctest.h"
#include "vfs/VirtualFileSystem.h"
#include "vfs/archive/ArchiveWriter.h"
#include <filesystem>
#include <map>
#include <mutex>
#include <ostream>
#include <string>
#include <vector>

// Emscripten without -pthread cannot start threads (std::thread throws); thread-based cases are skipped there.
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#    define VFS_TEST_HAS_THREADS 0
#else
#    define VFS_TEST_HAS_THREADS 1
#endif
// Decorator for TEST_CASE / TEST_SUITE that start threads
#define VFS_NEEDS_THREADS doctest::skip(!VFS_TEST_HAS_THREADS)

// Make doctest print the error-code name when an assertion fails.
// Note: doctest prefers to print enums as their underlying integer (it will not use a StringMaker specialization), so operator<< is required
namespace vfs
{
inline std::ostream& operator<<(std::ostream& os, Error e)
{
	return os << errorName(e);
}
} // namespace vfs

namespace doctest
{
template <>
struct StringMaker<vfs::Error>
{
	static String convert(vfs::Error e) { return vfs::errorName(e); }
};
} // namespace doctest

namespace test
{

/**
 * Temporary directory, recursively deleted on destruction.
 */
class TempDir
{
public:
	TempDir();

	~TempDir();

	TempDir(const TempDir&) = delete;

	TempDir& operator=(const TempDir&) = delete;

	// UTF-8 path of the directory itself ('/' separated)
	const std::string& path() const { return m_path; }

	// Child path
	std::string sub(std::string_view rel) const;

	// Write a file (parent directories are created automatically)
	void write(std::string_view rel, std::string_view content) const;

	void write(std::string_view rel, const std::vector<uint8_t>& content) const;

	// Create a directory
	void mkdir(std::string_view rel) const;

	bool exists(std::string_view rel) const;

	std::string read(std::string_view rel) const;

private:
	std::string m_path;
};

// Convert a UTF-8 string to std::filesystem::path
std::filesystem::path u8(std::string_view s);

// Read or write an entire native file
std::vector<uint8_t> readNative(const std::string& path);

void writeNative(const std::string& path, const std::vector<uint8_t>& data);

// Deterministic pseudo-random data (xorshift), so every run uses the same test data
std::vector<uint8_t> randomBytes(size_t size, uint64_t seed);

// String to bytes
std::vector<uint8_t> bytes(std::string_view s);

std::string str(const std::vector<uint8_t>& v);

// Read a whole file through the VFS as a string; on failure return "<ERR:NAME>"
std::string readAll(const vfs::VirtualFileSystem& fs, std::string_view path);

// Read all remaining bytes through a stream
std::vector<uint8_t> readStream(vfs::IFileStream& stream);

// List a directory as "name" or "name/" (directory) entries
std::vector<std::string> listNames(const vfs::VirtualFileSystem& fs, std::string_view path);

/**
 * Build an archive file: each item in files is (path, content), and options set per-file encryption/compression.
 */
struct ArchiveFile
{
	std::string path;
	std::vector<uint8_t> content;
	vfs::FileWriteOptions options;
};

/**
 * Slow-test switch: returns true when the environment variable VFS_TEST_SLOW=1.
 * Slow tests are written as TEST_CASE("..." * doctest::skip(!test::slowEnabled())) and are skipped by default.
 */
bool slowEnabled();

/**
 * Probe at runtime whether the filesystem of the local temp directory is case-sensitive (Linux usually is; Windows/macOS are not by default).
 * The result is cached; the probe runs once.
 */
bool nativeCaseSensitive();

/**
 * Probe at runtime for a filesystem whose lookup is case-sensitive, but creating a name that differs only in case still conflicts (EEXIST), for example the iOS simulator
 * (the underlying volume is the host's case-insensitive APFS). Real devices and common desktop filesystems are not like this.
 */
bool nativeCaseLookupOnly();

/**
 * Probe at runtime whether the native filesystem can replace (rename over) a file that is currently open.
 * POSIX always can; Windows needs FileRenameInfoEx with POSIX semantics (NTFS on Windows 10 1709+).
 * FAT/exFAT, some network drives, older systems, and Wine do not support it, and the replace returns PERMISSION_DENIED until other handles close.
 * The result is cached; the probe runs once.
 */
bool nativeReplaceOpenFile();

/**
 * Raw error code of the current thread's latest native operation, shaped like " (os=2)"; empty when there is none. Used in failure details
 */
std::string osErrorSuffix();

/**
 * Thread-safe error-code histogram: concurrent tests record how often each return code appears, and print the whole histogram on failure for cross-platform diagnosis.
 */
class ErrorHistogram
{
public:
	void add(vfs::Error e)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		++m_counts[e];
	}

	size_t count(vfs::Error e) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_counts.find(e);
		return it == m_counts.end() ? 0 : it->second;
	}

	// Shaped like "OK=120 NOT_FOUND=3 PERMISSION_DENIED=1"
	std::string str() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		std::string s;
		for (const auto& [e, n] : m_counts)
		{
			if (!s.empty())
				s.push_back(' ');
			s += vfs::errorName(e);
			s += '=';
			s += std::to_string(n);
		}
		return s.empty() ? "<empty>" : s;
	}

private:
	mutable std::mutex m_mutex;
	std::map<vfs::Error, size_t> m_counts;
};

/**
 * Thread-safe failure log: counts failures and keeps the first few descriptions with context (actual error code, path), printed when an assertion fails.
 * ++failures is still usable (without details).
 */
class FailureLog
{
public:
	static constexpr size_t MAX_DETAILS = 8;

	void add(const std::string& what)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		++m_count;
		if (m_details.size() < MAX_DETAILS)
			m_details.push_back(what);
	}

	FailureLog& operator++()
	{
		add("<no details>");
		return *this;
	}

	// When err is not OK, record "what -> error code name"
	void expectOk(vfs::Error err, const std::string& what)
	{
		if (err != vfs::Error::OK)
			add(what + " -> " + vfs::errorName(err) + osErrorSuffix());
	}

	// When contents differ, record the length and the beginning
	void expectEq(const std::string& got, const std::string& want, const std::string& what)
	{
		if (got != want)
			add(what + " -> got " + std::to_string(got.size()) + " bytes \"" + got.substr(0, 40) + "\", expected " +
				std::to_string(want.size()) + " bytes \"" + want.substr(0, 40) + "\"");
	}

	size_t load() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_count;
	}

	std::string str() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		std::string s = std::to_string(m_count) + " failure(s)";
		for (const auto& d : m_details)
			s += "\n  " + d;
		if (m_count > m_details.size())
			s += "\n  ...";
		return s;
	}

private:
	mutable std::mutex m_mutex;
	size_t m_count = 0;
	std::vector<std::string> m_details;
};

/**
 * Test scale factor: environment variable VFS_TEST_SCALE (default 1). Stress-test iteration counts are multiplied by it.
 */
int scale();

vfs::Error buildArchive(const std::string& outPath, const std::vector<ArchiveFile>& files,
						const vfs::ArchiveWriteOptions& options = {}, vfs::ArchiveWriteStats* stats = nullptr);

} // namespace test
