// This file: the read-only "asset pack" backend (AssetBackend) and its data-source interface IAssetProvider.
// On Android, AndroidAssetBackend supplies an AAssetManager implementation (assets/ inside the APK);
// the interface is platform-neutral, so a simulated IAssetProvider can exercise all of the logic on the desktop.
#pragma once

#include "../IBackend.h"
#include "../archive/ArchiveSource.h"
#include <unordered_map>
#include <unordered_set>

NS_VFS_BEGIN

/**
 * An open asset (same semantics as AAsset). A single object is used from one thread only and does not need to be thread-safe.
 */
class VFS_DLL IAssetFile
{
public:
	virtual ~IAssetFile() = default;

	/// Read from the current position. Returns the number of bytes read, 0 for EOF, or < 0 on error.
	virtual int read(void* buf, size_t size) = 0;

	/// Move the current position (whence is SEEK_SET/SEEK_CUR/SEEK_END). Returns the new position, or -1 on failure.
	virtual int64_t seek(int64_t offset, int whence) = 0;

	/// Total length of the asset.
	virtual int64_t length() const = 0;

	/**
	 * When the asset is stored uncompressed, return a file descriptor that can be read by offset (ownership passes to the caller, who must close it),
	 * and output the asset's start and length within that file; return -1 when it is stored compressed or the operation is unsupported.
	 * Used only on POSIX platforms (a Windows implementation should return -1).
	 */
	virtual int openFileDescriptor(int64_t& start, int64_t& length) = 0;
};

/**
 * Asset source. Every method may be called concurrently from multiple threads, so the implementation must be thread-safe (AAssetManager itself is thread-safe).
 * Paths are canonical relative to the asset root, with no leading '/', and the empty string is the root directory.
 */
class VFS_DLL IAssetProvider
{
public:
	virtual ~IAssetProvider() = default;

	/// Open a file; return nullptr if it does not exist (or is a directory).
	virtual std::unique_ptr<IAssetFile> open(std::string_view path) = 0;

	/**
	 * List the **file** names in a directory (same as AAssetDir: subdirectories are not included).
	 * @return true when the directory contains files; false when it does not exist, is empty, or contains only subdirectories
	 */
	virtual bool listFiles(std::string_view dir, std::vector<std::string>& names) = 0;
};

/**
 * Options for the asset backend.
 */
struct AssetOptions
{
	/**
	 * A subdirectory inside the assets that becomes the backend root, for example "res" mounts only assets/res/. The empty string means the whole asset tree.
	 */
	std::string root;

	/**
	 * Optional manifest file (an asset path relative to root, for example "assets.lst"): one file path per line (relative to root, '/' separated,
	 * at any depth outside "assets/"). When present, the directory structure comes entirely from the manifest and stat/list are exact,
	 * including directories that contain only subdirectories; when missing or empty, fall back to probe mode (see the AssetBackend notes).
	 */
	std::string manifest;
};

/**
 * Read-only asset backend.
 *
 * Two directory modes:
 * - Manifest mode (the manifest named by AssetOptions::manifest exists): the manifest is read at mount time to build the directory tree, and stat/list are exact;
 * - Probe mode: files are detected by open; directories are detected by "listing files in them is non-empty". AAssetManager cannot list subdirectories,
 *   so a directory that contains only subdirectories stats as NOT_FOUND and list does not show those subdirectories. Use a manifest when the full tree is required.
 *
 * Reads: each stream holds its own IAssetFile, so they do not interfere, and the same asset can be opened and read concurrently from multiple threads.
 * On POSIX, an uncompressed asset is read by offset (pread) through the descriptor from openFileDescriptor,
 * without using the AAsset cursor; compressed assets use IAssetFile::read/seek.
 * Thread safety: the backend itself holds only an immutable manifest index and can be used concurrently without limit.
 */
class VFS_DLL AssetBackend : public IBackend, public std::enable_shared_from_this<AssetBackend>
{
public:

	/**
	 * @param provider Asset source (must not be null)
	 * @param options  Options
	 * @param err      Optional error output: UNSUPPORTED if provider is null; INVALID_PATH if root is illegal; CORRUPTED if the manifest contains an illegal path
	 * @return nullptr on failure
	 */
	static std::shared_ptr<AssetBackend> create(std::shared_ptr<IAssetProvider> provider, const AssetOptions& options = {},
												Error* err = nullptr);

	AssetBackend(std::shared_ptr<IAssetProvider> provider, std::string root);

	virtual ~AssetBackend();

	virtual uint32_t capabilities() const override;

	virtual Error stat(std::string_view rel, Stat& out) const override;

	virtual std::unique_ptr<IFileStream> open(std::string_view rel, IFileStream::Mode mode, Error* err) override;

	virtual Error list(std::string_view relDir, const std::function<bool(const DirEntry&)>& cb) const override;

	virtual const char* typeName() const override { return "asset"; }

	/// Whether manifest mode is in use.
	bool hasManifest() const { return m_hasManifest; }

	/**
	 * Wrap a .vfs archive stored as an asset into an archive source, for ArchiveBackend::create (for example assets/game.vfs inside an APK).
	 * On POSIX, when the asset is stored uncompressed (Gradle: androidResources { noCompress += "vfs" }), the source holds one descriptor for positional reads,
	 * and opening a file inside the archive does not open the asset again; otherwise each stream opens the asset once (random reads of a compressed asset are slow and not recommended).
	 * @return nullptr when the asset is missing or is a directory, with err set to NOT_FOUND / IS_DIR and similar; UNSUPPORTED when the backend is not held by a shared_ptr
	 */
	std::shared_ptr<IArchiveSource> archiveSource(std::string_view rel, Error* err = nullptr);

private:
	// Backend-relative path -> asset path (with the root prefix).
	std::string assetPath(std::string_view rel) const;

	// When a path is missing, distinguish NOT_FOUND from NOT_DIR (some parent component is a file).
	Error classifyMissing(std::string_view rel) const;

	// Probe mode: whether a directory exists (the root always exists).
	bool probeDir(std::string_view rel) const;

	// Read and parse the manifest.
	Error loadManifest(std::string_view manifestPath);

	std::shared_ptr<IAssetProvider> m_provider;
	std::string m_root;   // Empty, or an asset subdirectory ending in '/'
	bool m_hasManifest = false;
	std::unordered_set<std::string> m_files;                                  // Manifest mode: every file
	std::unordered_map<std::string, std::vector<DirEntry>> m_dirs;            // Manifest mode: directory -> children
};

NS_VFS_END
