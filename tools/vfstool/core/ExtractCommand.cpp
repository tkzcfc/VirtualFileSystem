// vfstool extract：提取指定的文件或目录
#include "Commands.h"
#include "vfs/Path.h"

NS_VFS_BEGIN

namespace tool
{

int runExtract(const ExtractOptions& options, std::ostream& out, std::ostream& err)
{
	if (options.outputDir.empty() || options.paths.empty())
	{
		err << "错误: 需要至少一个路径和 -o 输出目录\n";
		return static_cast<int>(ExitCode::USAGE);
	}
	std::shared_ptr<const ArchiveIndex> index;
	int code = openArchive(options.archive, index, err);
	if (code != 0)
		return code;

	Error e = Error::OK;
	auto stream = index->source->openStream(&e);
	if (!stream)
		return exitCodeFor(e);

	size_t count = 0;
	for (const auto& raw : options.paths)
	{
		std::string path;
		if (Path::normalizeRelative(raw, path) != Error::OK)
		{
			err << "错误: 非法路径: " << raw << "\n";
			return static_cast<int>(ExitCode::USAGE);
		}
		// 文件：直接提取；目录（含根目录 ""）：提取其下所有条目
		std::vector<const ArchiveEntry*> targets;
		if (const ArchiveEntry* entry = index->findFile(path))
			targets.push_back(entry);
		else if (index->isDir(path))
		{
			std::string prefix = path.empty() ? std::string() : path + "/";
			for (const auto& candidate : index->entries)
			{
				if (path.empty() || candidate.path.compare(0, prefix.size(), prefix) == 0 || candidate.path == path)
					targets.push_back(&candidate);
			}
		}
		else
		{
			err << "错误: 归档中没有: " << raw << "\n";
			return static_cast<int>(ExitCode::IO);
		}
		for (const ArchiveEntry* entry : targets)
		{
			e = writeEntryToDisk(*index, *entry, options.outputDir, options.force, true, stream.get());
			if (e != Error::OK)
			{
				err << "错误: 提取失败: " << entry->path << " (" << errorName(e) << ")\n";
				return e == Error::CORRUPTED ? static_cast<int>(ExitCode::INTEGRITY) : exitCodeFor(e);
			}
			out << "  " << entry->path << "\n";
			++count;
		}
	}
	out << "已提取 " << count << " 项到 " << options.outputDir << "\n";
	return 0;
}

} // namespace tool

NS_VFS_END
