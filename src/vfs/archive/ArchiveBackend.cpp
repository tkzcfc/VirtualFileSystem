#include "ArchiveBackend.h"
#include "ArchiveFileStream.h"

NS_VFS_BEGIN

std::shared_ptr<ArchiveBackend> ArchiveBackend::create(std::string_view nativePath, const ArchiveOptions& options,
													   Error* err)
{
	return create(std::make_shared<FileArchiveSource>(std::string(nativePath)), options, err);
}

std::shared_ptr<ArchiveBackend> ArchiveBackend::create(std::shared_ptr<IArchiveSource> source,
													   const ArchiveOptions& options, Error* err)
{
	auto index = ArchiveReader::open(std::move(source), options, err);
	if (!index)
		return nullptr;
	return std::make_shared<ArchiveBackend>(std::move(index));
}

ArchiveBackend::ArchiveBackend(std::shared_ptr<const ArchiveIndex> index)
	: m_index(std::move(index))
{
}

ArchiveBackend::~ArchiveBackend()
{
}

uint32_t ArchiveBackend::capabilities() const
{
	return BackendCaps::Read;
}

Error ArchiveBackend::stat(std::string_view rel, Stat& out) const
{
	return m_index->stat(rel, out);
}

std::unique_ptr<IFileStream> ArchiveBackend::open(std::string_view rel, IFileStream::Mode mode, Error* err)
{
	if (mode != IFileStream::Mode::READ)
	{
		setError(err, Error::READ_ONLY);
		return nullptr;
	}
	const ArchiveEntry* entry = m_index->findFile(rel);
	if (!entry)
	{
		Stat st;
		Error e = m_index->stat(rel, st);
		setError(err, e == Error::OK ? Error::IS_DIR : e);
		return nullptr;
	}

	// Uncompressed and block-LZ4 entries alike: when the source supports positional reads, read directly by offset (opening a file makes no system call); otherwise open an independent source stream each time. Neither path takes a lock while reading
	if (m_index->source->supportsReadAt())
	{
		setError(err, Error::OK);
		return std::make_unique<ArchiveFileStream>(m_index, entry, nullptr);
	}
	Error e = Error::OK;
	std::unique_ptr<IFileStream> file = m_index->source->openStream(&e);
	if (!file)
	{
		setError(err, e);
		return nullptr;
	}
	setError(err, Error::OK);
	return std::make_unique<ArchiveFileStream>(m_index, entry, std::move(file));
}

Error ArchiveBackend::list(std::string_view relDir, const std::function<bool(const DirEntry&)>& cb) const
{
	const std::vector<DirEntry>* children = m_index->children(relDir);
	if (!children)
	{
		Stat st;
		Error e = m_index->stat(relDir, st);
		return e == Error::OK ? Error::NOT_DIR : e;
	}
	for (const auto& entry : *children)
	{
		if (!cb(entry))
			break;
	}
	return Error::OK;
}

Error ArchiveBackend::verifyAll() const
{
	return ArchiveReader::verify(*m_index);
}

NS_VFS_END
