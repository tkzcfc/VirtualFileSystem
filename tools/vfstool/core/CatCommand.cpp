// vfstool cat: write one file's contents to stdout as binary
#include "Commands.h"
#include "vfs/Path.h"
#include "vfs/archive/ArchiveBackend.h"

NS_VFS_BEGIN

namespace tool
{

int runCat(const CatOptions& options, std::ostream& out, std::ostream& err)
{
	std::shared_ptr<const ArchiveIndex> index;
	int code = openArchive(options.archive, index, err);
	if (code != 0)
		return code;

	std::string path;
	if (Path::normalizeRelative(options.path, path) != Error::OK)
	{
		err << "error: invalid path: " << options.path << "\n";
		return static_cast<int>(ExitCode::USAGE);
	}
	// Read through the runtime backend, the same path the game uses (large files are streamed too)
	ArchiveBackend backend(index);
	Error e = Error::OK;
	auto stream = backend.open(path, IFileStream::Mode::READ, &e);
	if (!stream)
	{
		err << "error: cannot open " << options.path << ": " << errorMessage(e) << "\n";
		return e == Error::NOT_FOUND || e == Error::IS_DIR ? static_cast<int>(ExitCode::IO) : exitCodeFor(e);
	}
	std::vector<char> buf(256 * 1024);
	while (true)
	{
		int n = stream->read(buf.data(), static_cast<unsigned int>(buf.size()));
		if (n < 0)
		{
			err << "error: read failed\n";
			return static_cast<int>(ExitCode::IO);
		}
		if (n == 0)
			break;
		out.write(buf.data(), n);
	}
	out.flush();
	return 0;
}

} // namespace tool

NS_VFS_END
