// 本文件：单文件后端。把一个本机文件挂到虚拟路径上，相当于 Linux 的文件 bind mount（决策 #16）。
#pragma once

#include "../IBackend.h"

NS_VFS_BEGIN

/**
 * 单文件后端：只含一个条目（相对路径为空串）。
 * 挂载点路径下的子路径一律返回 NOT_DIR。用途：覆盖单个配置或调试文件。
 * 线程安全：无共享可变状态。
 */
class VFS_DLL SingleFileBackend : public IBackend
{
public:

	/**
	 * 创建单文件后端。
	 * @param nativeFile 本机文件路径（UTF-8），必须是已存在的普通文件
	 * @param readOnly   为 true 时禁止写入
	 * @param err        可选的错误输出
	 */
	static std::shared_ptr<SingleFileBackend> create(std::string_view nativeFile, bool readOnly = false,
													 Error* err = nullptr);

	SingleFileBackend(std::string nativeFile, bool readOnly);

	virtual ~SingleFileBackend();

	virtual uint32_t capabilities() const override;

	virtual Error stat(std::string_view rel, Stat& out) const override;

	virtual std::unique_ptr<IFileStream> open(std::string_view rel, IFileStream::Mode mode, Error* err) override;

	virtual Error list(std::string_view relDir, const std::function<bool(const DirEntry&)>& cb) const override;

	virtual Error createDir(std::string_view rel) override;

	virtual Error removeFile(std::string_view rel) override;

	virtual std::string realPath(std::string_view rel) const override;

	virtual const char* typeName() const override { return "file"; }

	const std::string& nativeFile() const { return m_nativeFile; }

private:
	std::string m_nativeFile;
	bool m_readOnly;
};

NS_VFS_END
