#include "ArchiveBackend.h"
#include "ArchiveFileStream.h"
#include "../memory/MemoryFileStream.h"

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

	if (entry->compression != CompressionType::NONE)
	{
		// 压缩文件不能按偏移流式读：整体解压到内存（适合配置、脚本等小文件）
		auto buffer = std::make_shared<MemoryBuffer>();
		Error e = ArchiveReader::readEntry(*m_index, *entry, buffer->data);
		if (e != Error::OK)
		{
			setError(err, e);
			return nullptr;
		}
		buffer->mtime.store(entry->mtime != 0 ? entry->mtime : m_index->sourceMtime);
		setError(err, Error::OK);
		return std::make_unique<MemoryFileStream>(std::move(buffer), IFileStream::Mode::READ);
	}

	// 未压缩：来源支持位置读时直接按偏移读取（打开文件没有系统调用）；否则每次打开一个独立的来源流。读取时都不加锁
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
