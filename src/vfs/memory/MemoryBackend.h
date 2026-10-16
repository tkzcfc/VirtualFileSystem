// 本文件：内存后端。内存中的可读写目录树（决策 #26）。
#pragma once

#include "../IBackend.h"
#include "MemoryNode.h"
#include <atomic>
#include <shared_mutex>

NS_VFS_BEGIN

struct MemoryOptions
{
	bool readOnly = false;   // 只读：只能通过 addFile 预填数据
};

/**
 * 内存后端。
 * - 节点是目录或带 shared_ptr<MemoryBuffer> 的文件；支持 mkdir/rmdir/rename/resize。
 * - 删除打开中的文件采用 unlink 语义：名字立即移除，数据由流继续持有（决策 #18）。
 * 线程安全：树结构由一把读写锁保护；文件数据由各自缓冲区的锁保护。
 */
class VFS_DLL MemoryBackend : public IBackend
{
public:

	static std::shared_ptr<MemoryBackend> create(const MemoryOptions& options = {});

	explicit MemoryBackend(const MemoryOptions& options = {});

	virtual ~MemoryBackend();

	/**
	 * 预填文件：自动创建父目录（mkdir -p），已存在的文件被覆盖。只读模式下也可以调用。
	 * @param rel  相对路径（会被规范化）
	 * @param data 文件内容
	 * @return OK；路径不合法返回 INVALID_PATH；路径与已有目录冲突返回 IS_DIR/NOT_DIR
	 */
	Error addFile(std::string_view rel, std::vector<uint8_t> data);

	Error addFile(std::string_view rel, std::string_view data);

	void setReadOnly(bool value) { m_readOnly.store(value); }

	bool isReadOnly() const { return m_readOnly.load(); }

	virtual uint32_t capabilities() const override;

	virtual Error stat(std::string_view rel, Stat& out) const override;

	virtual std::unique_ptr<IFileStream> open(std::string_view rel, IFileStream::Mode mode, Error* err) override;

	virtual Error list(std::string_view relDir, const std::function<bool(const DirEntry&)>& cb) const override;

	virtual Error createDir(std::string_view rel) override;

	virtual Error removeDir(std::string_view rel) override;

	virtual Error removeFile(std::string_view rel) override;

	virtual Error rename(std::string_view from, std::string_view to) override;

	virtual const char* typeName() const override { return "memory"; }

private:

	// 查找节点（调用方必须持有锁）。中间某一级是文件返回 NOT_DIR
	Error findNode(std::string_view rel, std::shared_ptr<MemoryNode>& out) const;

	// 查找父目录节点和最后一段名字（调用方必须持有锁）
	Error findParent(std::string_view rel, std::shared_ptr<MemoryNode>& parent, std::string_view& name) const;

private:
	mutable std::shared_mutex m_mutex;     // 保护整棵树的结构
	std::shared_ptr<MemoryNode> m_root;    // 根目录节点
	std::atomic<bool> m_readOnly;
};

NS_VFS_END
