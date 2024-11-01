#pragma once

#include "NativeFileStream.h"
#include <filesystem>

namespace fs = std::filesystem;

NS_VFS_BEGIN

bool NativeFileStream::open(std::string path, FileStream::Mode mode)
{
	std::ios_base::openmode open_mode = std::fstream::binary;
	if (mode == Mode::READ)
	{
		open_mode |= std::fstream::in;
	}
	else if (mode == Mode::WRITE)
	{
		open_mode |= std::fstream::in;
		open_mode |= std::fstream::out;
		if (!fs::exists(path) || !fs::is_regular_file(path))
		{
			open_mode |= std::fstream::trunc;
		}
	}
	else if (mode == Mode::APPEND)
	{
		open_mode |= std::fstream::in;
		open_mode |= std::fstream::out;
		open_mode |= std::fstream::app;
	}
	m_fs.open(path.c_str(), open_mode);
	return m_fs.is_open();
}

void NativeFileStream::close()
{
	m_fs.close();
}

uint64_t NativeFileStream::seek(uint64_t offset, SeekOrigin origin)
{
	if (origin == SeekOrigin::CUR)
		m_fs.seekg(offset, std::ios_base::cur);
	else if (origin == SeekOrigin::END)
		m_fs.seekg(offset, std::ios_base::end);
	else if (origin == SeekOrigin::SET)
		m_fs.seekg(offset, std::ios_base::beg);

	if (m_fs.fail()) 
	{
		return 0;
	}

	return (uint64_t)m_fs.tellg();
}

uint64_t NativeFileStream::read(void* buf, uint64_t size)
{
	m_fs.read(reinterpret_cast<char*>(buf), (std::streamsize)size);
	return static_cast<uint64_t>(m_fs.gcount());
}

uint64_t NativeFileStream::write(const void* buf, uint64_t size)
{
#if 0
	auto prep = m_fs.tellp();
	m_fs.write(reinterpret_cast<const char*>(buf), (std::streamsize)size);
	return static_cast<uint64_t>(m_fs.tellp() - prep);
#else
	m_fs.write(reinterpret_cast<const char*>(buf), (std::streamsize)size);
	return size;
#endif
}

uint64_t NativeFileStream::tell()
{
	return static_cast<uint64_t>(m_fs.tellg());
}

uint64_t NativeFileStream::size()
{
	uint64_t curPos = tell();
	uint64_t size = seek(0, SeekOrigin::END);
	seek(curPos, SeekOrigin::SET);
	return size;
}

bool NativeFileStream::isOpen() const
{
	return m_fs.is_open();
}

NS_VFS_END
