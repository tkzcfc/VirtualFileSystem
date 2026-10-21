// vfstool verify: check the header, the index, and each file content hash
#include "Commands.h"

NS_VFS_BEGIN

namespace tool
{

int runVerify(const VerifyOptions& options, std::ostream& out, std::ostream& err)
{
	// Opening already checked the header checksum, section bounds, index XXH3, key, and entry validity
	std::shared_ptr<const ArchiveIndex> index;
	int code = openArchive(options.archive, index, err);
	if (code != 0)
		return code;

	size_t ok = 0, bad = 0;
	Error e = ArchiveReader::verify(*index, [&](const ArchiveEntry& entry, Error result)
	{
		if (result == Error::OK)
		{
			++ok;
			if (options.verbose)
				out << "  OK   " << entry.path << "\n";
		}
		else
		{
			++bad;
			err << "  FAIL " << entry.path << " (" << errorName(result) << ")\n";
		}
	});
	if (e != Error::OK && bad == 0)
	{
		err << "error: " << errorMessage(e) << "\n";
		return exitCodeFor(e);
	}
	out << "header: OK, index: OK, files: " << ok << " passed, " << bad << " failed\n";
	return bad == 0 ? 0 : static_cast<int>(ExitCode::INTEGRITY);
}

} // namespace tool

NS_VFS_END
