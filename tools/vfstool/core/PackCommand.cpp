// vfstool pack：把目录打包成 .vfs
#include "Commands.h"
#include "vfs/Path.h"
#include "vfs/archive/ArchiveWriter.h"
#include "vfs/native/NativeUtils.h"
#include <filesystem>
#include <iomanip>

namespace stdfs = std::filesystem;

NS_VFS_BEGIN

namespace tool
{

namespace
{

struct PackItem
{
	std::string rel;          // 相对源目录的 '/' 路径
	std::string nativePath;   // 本机路径
	bool isDir = false;
	FileWriteOptions options;
};

// 默认排除（vfs-format F21），作为最前面的规则，用户的 --include 可以覆盖
const char* const s_defaultExcludes[] = {".git", ".svn", ".DS_Store", "Thumbs.db", "desktop.ini"};

class Collector
{
public:
	Collector(const PackOptions& options, std::string outputCanonical, std::ostream& err)
		: m_options(options)
		, m_outputCanonical(std::move(outputCanonical))
		, m_err(err)
	{
		if (!options.noDefaultExcludes)
		{
			for (const char* p : s_defaultExcludes)
				m_filters.push_back(Rule{p, false});
		}
		for (const auto& r : options.filterRules)
		{
			m_filters.push_back(r);
			if (r.positive)
				m_hasInclude = true;
		}
	}

	// 递归收集；目录被显式排除时整棵子树都跳过
	Error collect(const stdfs::path& dir, const std::string& rel, std::vector<PackItem>& out)
	{
		std::error_code ec;
		std::vector<stdfs::directory_entry> children;
		for (stdfs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
			children.push_back(*it);
		if (ec)
		{
			m_err << "错误: 无法读取目录 " << native::fromFsPath(dir) << ": " << ec.message() << "\n";
			return native::errorFromCode(ec);
		}

		for (const auto& child : children)
		{
			std::string name = native::fromFsPath(child.path().filename());
			std::string childRel = rel.empty() ? name : rel + "/" + name;
			std::error_code ec2;
			if (child.is_directory(ec2))
			{
				bool matched = false;
				bool keep = Glob::evaluate(m_filters, childRel, true, &matched);
				if (matched && !keep)
					continue;
				if (m_options.emptyDirs)
					out.push_back(PackItem{childRel, native::fromFsPath(child.path()), true, {}});
				Error e = collect(child.path(), childRel, out);
				if (e != Error::OK)
					return e;
			}
			else if (child.is_regular_file(ec2))
			{
				// 有 --include 时默认不包含，否则默认包含；最后匹配的规则生效
				if (!Glob::evaluate(m_filters, childRel, !m_hasInclude))
					continue;
				std::string nativePath = native::fromFsPath(child.path());
				// 输出文件在源目录里时不要把自己打进去
				if (!m_outputCanonical.empty())
				{
					std::error_code ec3;
					auto canon = stdfs::weakly_canonical(child.path(), ec3);
					if (!ec3 && native::fromFsPath(canon) == m_outputCanonical)
						continue;
				}
				PackItem item;
				item.rel = childRel;
				item.nativePath = std::move(nativePath);
				item.options.encrypt = Glob::evaluate(m_options.encryptRules, childRel, m_options.encryptAll);
				item.options.compress = Glob::evaluate(m_options.compressRules, childRel, m_options.compressAll);
				if (m_options.keepMtime)
				{
					Stat st;
					if (native::statPath(item.nativePath, st) == Error::OK)
						item.options.mtime = st.mtime;
				}
				out.push_back(std::move(item));
			}
		}
		return Error::OK;
	}

private:
	const PackOptions& m_options;
	std::string m_outputCanonical;
	std::ostream& m_err;
	std::vector<Rule> m_filters;
	bool m_hasInclude = false;
};

} // namespace

int runPack(const PackOptions& options, std::ostream& out, std::ostream& err)
{
	if (options.inputDir.empty() || (options.outputPath.empty() && !options.dryRun))
	{
		err << "错误: 需要源目录和 -o 输出文件\n";
		return static_cast<int>(ExitCode::USAGE);
	}
	Stat st;
	if (native::statPath(options.inputDir, st) != Error::OK || st.type != FileType::DIR)
	{
		err << "错误: 源目录不存在或不是目录: " << options.inputDir << "\n";
		return static_cast<int>(ExitCode::IO);
	}
	if (options.xorKeyLength < 16 || options.xorKeyLength > 32)
	{
		err << "错误: --xor-key-len 必须在 16..32 之间\n";
		return static_cast<int>(ExitCode::USAGE);
	}
	std::string prefix;
	if (Path::normalizeRelative(options.prefix, prefix) != Error::OK)
	{
		err << "错误: 非法的 --prefix: " << options.prefix << "\n";
		return static_cast<int>(ExitCode::USAGE);
	}

	ArchiveWriteOptions writeOptions;
	writeOptions.alignment = options.alignment;
	writeOptions.fileKeyLength = options.xorKeyLength;
	writeOptions.dedup = !options.noDedup;
	writeOptions.compressionLevel = options.compressionLevel;
	writeOptions.encryptIndex = !options.noIndexEncrypt;
	Error e = resolveKey(options.key, writeOptions.indexKey, err);
	if (e != Error::OK)
		return exitCodeFor(e);
	writeOptions.storeKey = options.storeKey;
	if (options.noIndexEncrypt && !writeOptions.indexKey.empty())
		err << "警告: 指定了 --no-index-encrypt，忽略索引密钥\n";

	std::string outputCanonical;
	if (!options.outputPath.empty())
	{
		std::error_code ec;
		auto canon = stdfs::weakly_canonical(native::toFsPath(options.outputPath), ec);
		if (!ec)
			outputCanonical = native::fromFsPath(canon);
	}

	std::vector<PackItem> items;
	Collector collector(options, outputCanonical, err);
	e = collector.collect(native::toFsPath(options.inputDir), std::string(), items);
	if (e != Error::OK)
		return exitCodeFor(e);

	ArchiveWriter writer(writeOptions);
	for (const auto& item : items)
	{
		std::string archivePath = prefix.empty() ? item.rel : prefix + "/" + item.rel;
		if (options.dryRun || options.verbose)
		{
			if (item.isDir)
				out << "  D  " << archivePath << "/\n";
			else
				out << "  " << (item.options.encrypt ? 'E' : '-') << (item.options.compress ? 'C' : '-') << " "
					<< archivePath << "\n";
		}
		e = item.isDir ? writer.addDir(archivePath) : writer.addFile(archivePath, item.nativePath, item.options);
		if (e != Error::OK)
		{
			err << "错误: " << writer.lastError() << "\n";
			return exitCodeFor(e);
		}
	}
	if (options.dryRun)
	{
		out << "dry-run: " << items.size() << " 项，未写入文件\n";
		return static_cast<int>(ExitCode::OK);
	}

	ArchiveWriteStats stats;
	e = writer.write(options.outputPath, &stats);
	if (e != Error::OK)
	{
		err << "错误: 打包失败: " << writer.lastError() << " (" << errorName(e) << ")\n";
		return e == Error::EXISTS || e == Error::NOT_DIR ? static_cast<int>(ExitCode::USAGE) : exitCodeFor(e);
	}

	out << "已打包 " << options.outputPath << "\n"
		<< "  文件: " << stats.fileCount << "，空目录: " << stats.dirCount << "，去重: " << stats.dedupCount
		<< "，XOR 加密: " << stats.encryptedCount << "，LZ4 压缩: " << stats.compressedCount << "\n"
		<< "  原始大小: " << stats.totalSize << " 字节，存储数据（不含对齐填充）: " << stats.storedSize
		<< " 字节，归档: " << stats.archiveSize << " 字节\n";
	return static_cast<int>(ExitCode::OK);
}

} // namespace tool

NS_VFS_END
