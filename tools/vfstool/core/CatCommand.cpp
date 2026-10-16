// vfstool cat：把单个文件的内容以二进制输出到 stdout
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
		err << "错误: 非法路径: " << options.path << "\n";
		return static_cast<int>(ExitCode::USAGE);
	}
	// 通过运行时后端读取，与游戏里的读取路径完全一致（大文件也是流式输出）
	ArchiveBackend backend(index);
	Error e = Error::OK;
	auto stream = backend.open(path, IFileStream::Mode::READ, &e);
	if (!stream)
	{
		err << "错误: 无法打开 " << options.path << ": " << errorMessage(e) << "\n";
		return e == Error::NOT_FOUND || e == Error::IS_DIR ? static_cast<int>(ExitCode::IO) : exitCodeFor(e);
	}
	std::vector<char> buf(256 * 1024);
	while (true)
	{
		int n = stream->read(buf.data(), static_cast<unsigned int>(buf.size()));
		if (n < 0)
		{
			err << "错误: 读取失败\n";
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
