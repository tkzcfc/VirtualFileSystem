// This file: argument structs and implementation entry points for each vfstool subcommand.
// Each command is a function in vfstool_core. The CLI (main.cpp) only parses arguments; tests call these functions directly.
#pragma once

#include "Glob.h"
#include "vfs/Error.h"
#include "vfs/archive/ArchiveReader.h"
#include <ostream>

NS_VFS_BEGIN

namespace tool
{

/**
 * Exit codes.
 */
enum class ExitCode : int
{
	OK = 0,          // Success
	USAGE = 1,       // Usage error
	IO = 2,          // IO error
	FORMAT = 3,      // Format or version error
	INTEGRITY = 4,   // Integrity check failed
	KEY = 5,         // Missing key or wrong key
};

/**
 * Key source. Priority: keyFile > keyHex > environment variable VFS_KEY.
 */
struct KeySource
{
	std::string keyFile;   // 32 bytes of raw data, or 64 hex characters (a trailing newline is allowed)
	std::string keyHex;    // 64 hex characters; a warning is printed (visible in shell history and ps)
	bool useEnv = true;    // Whether to read the environment variable VFS_KEY
};

/**
 * Shared arguments for opening an existing archive.
 */
struct ArchiveArgs
{
	std::string archive;
	KeySource key;
	int64_t baseOffset = -1;
};

struct PackOptions
{
	std::string inputDir;                // Source directory
	std::string outputPath;              // Output .vfs
	std::string prefix;                  // Path prefix inside the archive
	std::vector<Rule> filterRules;       // --include (positive) / --exclude, in command-line order
	std::vector<Rule> encryptRules;      // --encrypt / --no-encrypt
	std::vector<Rule> compressRules;     // --compress / --no-compress
	bool encryptAll = false;             // Encrypt every file by default (a later --no-encrypt still applies)
	bool compressAll = false;            // Compress every file by default
	uint32_t alignment = 16;             // Data alignment
	KeySource key;                       // External index key (if omitted, one is derived and stored in the file)
	bool storeKey = false;               // Also store an external key in the file
	bool noIndexEncrypt = false;         // Do not encrypt the index
	uint32_t xorKeyLength = 32;          // Per-file XOR key length (16..32)
	int compressionLevel = 9;            // LZ4 HC level
	std::vector<std::string> blockSizes; // --block-size: "<size>" sets the default, "<glob>=<size>" overrides matching files (last match wins)
	bool keepMtime = false;              // Write the real mtime (default 0, for reproducibility)
	bool emptyDirs = false;              // Keep empty directories (omitted by default)
	bool noDedup = false;                // Disable content dedup
	bool noDefaultExcludes = false;      // Disable the default excludes (.git/.svn/.DS_Store/Thumbs.db/desktop.ini)
	bool dryRun = false;                 // Only print the files that would be packed
	bool verbose = false;
};

struct UnpackOptions
{
	ArchiveArgs archive;
	std::string outputDir;
	std::vector<Rule> filterRules;
	bool force = false;      // Overwrite existing files
	bool noVerify = false;   // Do not check content hashes
	bool verbose = false;
};

struct InfoOptions
{
	ArchiveArgs archive;
	bool json = false;
};

struct ListOptions
{
	ArchiveArgs archive;
	std::string pattern;     // Optional glob filter
	bool longFormat = false; // -l: show size, stored size, flags, hash
	bool json = false;
};

struct CatOptions
{
	ArchiveArgs archive;
	std::string path;
};

struct ExtractOptions
{
	ArchiveArgs archive;
	std::vector<std::string> paths;   // Files or directories (a directory extracts every file under it)
	std::string outputDir;
	bool force = false;
};

struct VerifyOptions
{
	ArchiveArgs archive;
	bool verbose = false;
};

struct KeygenOptions
{
	std::string outputFile;   // Print to stdout when empty
};

/**
 * Parse an LZ4 block size such as "65536", "64K", "64KiB", "1M" (power of two, 4 KiB..4 MiB) into its log2.
 * @return false if the text is not a valid block size
 */
bool parseBlockSize(std::string_view text, uint8_t& log2);

/**
 * Format a block size for display, e.g. 65536 -> "64K", 1048576 -> "1M".
 */
std::string formatBlockSize(uint64_t size);

int runPack(const PackOptions& options, std::ostream& out, std::ostream& err);

int runUnpack(const UnpackOptions& options, std::ostream& out, std::ostream& err);

int runInfo(const InfoOptions& options, std::ostream& out, std::ostream& err);

int runList(const ListOptions& options, std::ostream& out, std::ostream& err);

int runCat(const CatOptions& options, std::ostream& out, std::ostream& err);

int runExtract(const ExtractOptions& options, std::ostream& out, std::ostream& err);

int runVerify(const VerifyOptions& options, std::ostream& out, std::ostream& err);

int runKeygen(const KeygenOptions& options, std::ostream& out, std::ostream& err);

// ---------------------------------------------------------------- shared helpers

/**
 * Resolve the key by priority. When there is no source, key is empty and OK is returned.
 * @return OK; KEY_INVALID if the format is wrong; an IO error if the key file cannot be read
 */
Error resolveKey(const KeySource& source, std::vector<uint8_t>& key, std::ostream& err);

/**
 * Parse 64 hex characters (leading and trailing whitespace is allowed).
 */
bool parseHexKey(std::string_view text, std::vector<uint8_t>& key);

std::string toHex(const uint8_t* data, size_t size);

/**
 * Open an archive and print an error message on failure.
 * @return exit code
 */
int openArchive(const ArchiveArgs& args, std::shared_ptr<const ArchiveIndex>& index, std::ostream& err);

/**
 * Map vfs::Error to an exit code.
 */
int exitCodeFor(Error e);

/**
 * Write the entry contents to the native path outDir/entry.path.
 */
Error writeEntryToDisk(const ArchiveIndex& index, const ArchiveEntry& entry, const std::string& outDir, bool force,
					   bool verify, IFileStream* stream);

} // namespace tool

NS_VFS_END
