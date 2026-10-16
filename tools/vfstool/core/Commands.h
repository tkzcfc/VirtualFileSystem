// 本文件：vfstool 各子命令的参数结构与实现入口。
// 每个命令都是 vfstool_core 中的一个函数，CLI（main.cpp）只负责解析参数；测试直接调用这些函数。
#pragma once

#include "Glob.h"
#include "vfs/Error.h"
#include "vfs/archive/ArchiveReader.h"
#include <ostream>

NS_VFS_BEGIN

namespace tool
{

/**
 * 退出码。
 */
enum class ExitCode : int
{
	OK = 0,          // 成功
	USAGE = 1,       // 用法错误
	IO = 2,          // IO 错误
	FORMAT = 3,      // 格式或版本错误
	INTEGRITY = 4,   // 完整性校验失败
	KEY = 5,         // 缺少密钥或密钥错误
};

/**
 * 密钥来源。优先级：keyFile > keyHex > 环境变量 VFS_KEY。
 */
struct KeySource
{
	std::string keyFile;   // 32 字节原始数据，或 64 个 hex 字符（允许末尾换行）
	std::string keyHex;    // 64 个 hex 字符；会打印警告（会被 shell history 和 ps 看到）
	bool useEnv = true;    // 是否读取环境变量 VFS_KEY
};

/**
 * 打开已有归档时的公共参数。
 */
struct ArchiveArgs
{
	std::string archive;
	KeySource key;
	int64_t baseOffset = -1;
};

struct PackOptions
{
	std::string inputDir;                // 源目录
	std::string outputPath;              // 输出 .vfs
	std::string prefix;                  // 归档内路径前缀
	std::vector<Rule> filterRules;       // --include（positive）/ --exclude，按命令行顺序
	std::vector<Rule> encryptRules;      // --encrypt / --no-encrypt
	std::vector<Rule> compressRules;     // --compress / --no-compress
	bool encryptAll = false;             // 默认加密所有文件（之后的 --no-encrypt 仍然生效）
	bool compressAll = false;            // 默认压缩所有文件
	uint32_t alignment = 16;             // 数据对齐
	KeySource key;                       // 外部索引密钥（不给则派生并存入文件）
	bool storeKey = false;               // 外部密钥也存进文件
	bool noIndexEncrypt = false;         // 不加密索引
	uint32_t xorKeyLength = 32;          // 单文件 XOR key 长度（16..32）
	int compressionLevel = 9;            // LZ4 HC 级别
	bool keepMtime = false;              // 写入真实 mtime（默认 0，便于复现）
	bool emptyDirs = false;              // 保留空目录（默认不保留）
	bool noDedup = false;                // 关闭内容去重
	bool noDefaultExcludes = false;      // 关闭默认排除（.git/.svn/.DS_Store/Thumbs.db/desktop.ini）
	bool dryRun = false;                 // 只打印将要打包的文件
	bool verbose = false;
};

struct UnpackOptions
{
	ArchiveArgs archive;
	std::string outputDir;
	std::vector<Rule> filterRules;
	bool force = false;      // 覆盖已存在的文件
	bool noVerify = false;   // 不校验内容哈希
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
	std::string pattern;     // 可选 glob 过滤
	bool longFormat = false; // -l：显示大小、存储大小、标志、哈希
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
	std::vector<std::string> paths;   // 文件或目录（目录会提取其下所有文件）
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
	std::string outputFile;   // 为空时打印到 stdout
};

int runPack(const PackOptions& options, std::ostream& out, std::ostream& err);

int runUnpack(const UnpackOptions& options, std::ostream& out, std::ostream& err);

int runInfo(const InfoOptions& options, std::ostream& out, std::ostream& err);

int runList(const ListOptions& options, std::ostream& out, std::ostream& err);

int runCat(const CatOptions& options, std::ostream& out, std::ostream& err);

int runExtract(const ExtractOptions& options, std::ostream& out, std::ostream& err);

int runVerify(const VerifyOptions& options, std::ostream& out, std::ostream& err);

int runKeygen(const KeygenOptions& options, std::ostream& out, std::ostream& err);

// ---------------------------------------------------------------- 公共工具

/**
 * 按优先级解析密钥。没有任何来源时 key 为空并返回 OK。
 * @return OK；格式不对返回 KEY_INVALID；key 文件读不到返回 IO 错误
 */
Error resolveKey(const KeySource& source, std::vector<uint8_t>& key, std::ostream& err);

/**
 * 解析 64 个 hex 字符（允许首尾空白）。
 */
bool parseHexKey(std::string_view text, std::vector<uint8_t>& key);

std::string toHex(const uint8_t* data, size_t size);

/**
 * 打开归档并在失败时输出错误信息。
 * @return 退出码
 */
int openArchive(const ArchiveArgs& args, std::shared_ptr<const ArchiveIndex>& index, std::ostream& err);

/**
 * vfs::Error 映射到退出码。
 */
int exitCodeFor(Error e);

/**
 * 把条目内容写到本机目录 outDir/entry.path。
 */
Error writeEntryToDisk(const ArchiveIndex& index, const ArchiveEntry& entry, const std::string& outDir, bool force,
					   bool verify, IFileStream* stream);

} // namespace tool

NS_VFS_END
