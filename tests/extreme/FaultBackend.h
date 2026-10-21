// This file: fault-injection backend. Wraps any IBackend and fails stat / list / open / read / write / rename and similar operations as configured,
// to verify that the VFS propagates I/O errors and that a failure does not leave a partial result.
#pragma once

#include "vfs/IBackend.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <functional>
#include <string>

namespace test
{

struct FaultConfig
{
	std::string match;                        // Inject faults only for paths containing this substring (empty means all)
	vfs::Error statError = vfs::Error::OK;
	vfs::Error listError = vfs::Error::OK;
	vfs::Error openError = vfs::Error::OK;
	vfs::Error renameError = vfs::Error::OK;
	vfs::Error removeError = vfs::Error::OK;
	vfs::Error mkdirError = vfs::Error::OK;
	int64_t readFailAt = -1;                  // After a read reaches this position, read returns -1 (EIO)
	int64_t writeFailAt = -1;                 // After a write reaches this position, write returns -1 (ENOSPC)
	int shortWrite = 0;                       // When > 0, each write writes at most this many bytes
	int closeError = 0;                       // When non-zero, close returns -1 and sets this errno
	std::atomic<int> injected{0};             // Number of faults already injected
};

class FaultStream : public vfs::IFileStream
{
public:
	FaultStream(std::unique_ptr<vfs::IFileStream> inner, FaultConfig& cfg, bool active)
		: m_inner(std::move(inner))
		, m_cfg(cfg)
		, m_active(active)
	{
	}

	virtual bool open(std::string_view, Mode) override
	{
		errno = ENOTSUP;
		return false;
	}

	virtual int close() override
	{
		int r = m_inner->close();
		if (m_active && m_cfg.closeError != 0)
		{
			++m_cfg.injected;
			errno = m_cfg.closeError;
			return -1;
		}
		return r;
	}

	virtual int64_t seek(int64_t offset, int origin) const override { return m_inner->seek(offset, origin); }

	virtual int read(void* buf, unsigned int size) const override
	{
		if (m_active && m_cfg.readFailAt >= 0)
		{
			int64_t pos = m_inner->tell();
			if (pos >= m_cfg.readFailAt)
			{
				++m_cfg.injected;
				errno = EIO;
				return -1;
			}
			size = static_cast<unsigned int>(std::min<int64_t>(size, m_cfg.readFailAt - pos));
		}
		return m_inner->read(buf, size);
	}

	virtual int write(const void* buf, unsigned int size) const override
	{
		if (m_active && m_cfg.shortWrite > 0 && size > static_cast<unsigned int>(m_cfg.shortWrite))
			size = static_cast<unsigned int>(m_cfg.shortWrite);
		if (m_active && m_cfg.writeFailAt >= 0)
		{
			int64_t pos = m_inner->tell();
			if (pos >= m_cfg.writeFailAt)
			{
				++m_cfg.injected;
				errno = ENOSPC;
				return -1;
			}
			size = static_cast<unsigned int>(std::min<int64_t>(size, m_cfg.writeFailAt - pos));
		}
		return m_inner->write(buf, size);
	}

	virtual int64_t tell() const override { return m_inner->tell(); }

	virtual int64_t size() const override { return m_inner->size(); }

	virtual bool resize(int64_t size) const override { return m_inner->resize(size); }

	virtual bool isOpen() const override { return m_inner->isOpen(); }

private:
	std::unique_ptr<vfs::IFileStream> m_inner;
	FaultConfig& m_cfg;
	bool m_active;
};

class FaultBackend : public vfs::IBackend
{
public:
	FaultBackend(std::shared_ptr<vfs::IBackend> inner, FaultConfig& cfg)
		: m_inner(std::move(inner))
		, m_cfg(cfg)
	{
	}

	FaultConfig& config() { return m_cfg; }

	virtual uint32_t capabilities() const override { return m_inner->capabilities(); }

	virtual vfs::Error stat(std::string_view rel, vfs::Stat& out) const override
	{
		if (hit(rel, m_cfg.statError))
			return m_cfg.statError;
		return m_inner->stat(rel, out);
	}

	virtual std::unique_ptr<vfs::IFileStream> open(std::string_view rel, vfs::IFileStream::Mode mode, vfs::Error* err) override
	{
		if (hit(rel, m_cfg.openError))
		{
			if (err)
				*err = m_cfg.openError;
			return nullptr;
		}
		auto s = m_inner->open(rel, mode, err);
		if (!s)
			return nullptr;
		return std::make_unique<FaultStream>(std::move(s), m_cfg, matches(rel));
	}

	virtual vfs::Error list(std::string_view relDir, const std::function<bool(const vfs::DirEntry&)>& cb) const override
	{
		if (hit(relDir, m_cfg.listError))
			return m_cfg.listError;
		return m_inner->list(relDir, cb);
	}

	virtual vfs::Error createDir(std::string_view rel) override
	{
		return hit(rel, m_cfg.mkdirError) ? m_cfg.mkdirError : m_inner->createDir(rel);
	}

	virtual vfs::Error removeDir(std::string_view rel) override
	{
		return hit(rel, m_cfg.removeError) ? m_cfg.removeError : m_inner->removeDir(rel);
	}

	virtual vfs::Error removeFile(std::string_view rel) override
	{
		return hit(rel, m_cfg.removeError) ? m_cfg.removeError : m_inner->removeFile(rel);
	}

	virtual vfs::Error rename(std::string_view from, std::string_view to) override
	{
		if (hit(from, m_cfg.renameError) || hit(to, m_cfg.renameError))
			return m_cfg.renameError;
		return m_inner->rename(from, to);
	}

	virtual const char* typeName() const override { return "fault"; }

private:
	bool matches(std::string_view rel) const
	{
		return m_cfg.match.empty() || rel.find(m_cfg.match) != std::string_view::npos;
	}

	bool hit(std::string_view rel, vfs::Error e) const
	{
		if (e == vfs::Error::OK || !matches(rel))
			return false;
		++m_cfg.injected;
		return true;
	}

	std::shared_ptr<vfs::IBackend> m_inner;
	FaultConfig& m_cfg;
};

} // namespace test
