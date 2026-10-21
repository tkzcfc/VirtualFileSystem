// vfstool extract: extract a specified file or directory
#include "Commands.h"
#include "vfs/Path.h"

NS_VFS_BEGIN

namespace tool
{

int runExtract(const ExtractOptions& options, std::ostream& out, std::ostream& err)
{
	if (options.outputDir.empty() || options.paths.empty())
	{
		err << "error: at least one path and -o <output dir> are required\n";
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
			err << "error: invalid path: " << raw << "\n";
			return static_cast<int>(ExitCode::USAGE);
		}
		// File: extract it directly; directory (including the root ""): extract every entry under it
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
			err << "error: not in archive: " << raw << "\n";
			return static_cast<int>(ExitCode::IO);
		}
		for (const ArchiveEntry* entry : targets)
		{
			e = writeEntryToDisk(*index, *entry, options.outputDir, options.force, true, stream.get());
			if (e != Error::OK)
			{
				err << "error: extract failed: " << entry->path << " (" << errorName(e) << ")\n";
				return e == Error::CORRUPTED ? static_cast<int>(ExitCode::INTEGRITY) : exitCodeFor(e);
			}
			out << "  " << entry->path << "\n";
			++count;
		}
	}
	out << "extracted " << count << " item(s) to " << options.outputDir << "\n";
	return 0;
}

} // namespace tool

NS_VFS_END
