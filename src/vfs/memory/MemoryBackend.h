// 本文件：内存后端。内存中的可读写目录树。
#pragma once

#include "../IBackend.h"
#include "MemoryNode.h"
#include <atomic>
#include <mutex>

NS_VFS_BEGIN

struct MemoryOptions
{
	bool readOnly = false;   // 只读：只能通过 addFile 预填数据
};

/**
 * 内存后端。
 * - 节点是目录或带 shared_ptr<MemoryBuffer> 的文件；支持 mkdir/rmdir/rename/resize。
 * - 删除打开中的文件采用 unlink 语义：名字立即移除，数据由流继续持有。
 * 线程安全：每个目录一把读写锁（只保护它的 children），查找时从根开始逐级“先锁子目录、再放父目录”下行，
 * 不同目录下的创建、删除互不阻塞；rename 额外持有一把全局改名锁（同一时刻只有一个 rename 改变树的形状）。
 * 文件数据由各自缓冲区的锁保护。
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

	class DirLock;

	/**
	 * 逐级加锁下行到目录 dirRel，返回时 out 持有该目录的锁（exclusive 为 true 时是独占锁，否则共享锁）。
	 * 中间各级只短暂持有共享锁（先锁子目录再放父目录）。
	 * @param from 非空时从这个已被调用方锁住的目录开始（dirRel 相对于它，且不能为空），不会释放 from 的锁
	 * @return OK；某一级不存在返回 NOT_FOUND；某一级（包括目标本身）是文件返回 NOT_DIR
	 */
	Error lockDir(std::string_view dirRel, bool exclusive, DirLock& out, MemoryNode* from = nullptr) const;

private:
	std::mutex m_renameMutex;              // 串行化 rename：改名期间树的形状不会被其他 rename 改变
	std::shared_ptr<MemoryNode> m_root;    // 根目录节点
	std::atomic<bool> m_readOnly;
};

NS_VFS_END
