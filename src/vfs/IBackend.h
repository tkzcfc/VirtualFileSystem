// 本文件：后端（挂载类型）接口。
// 后端只负责自己的相对路径，完全不知道分层的存在；union、copy-up、递归删除等逻辑全部在 VirtualFileSystem 中。
#pragma once

#include "Common.h"
#include "Error.h"
#include "IFileStream.h"
#include "Stat.h"
#include "DirEntry.h"

NS_VFS_BEGIN

/**
 * 后端能力位。使用时必须带类型名限定，例如 BackendCaps::Write。
 */
enum BackendCaps : uint32_t
{
	Read = 1u << 0,    // 可以读
	Write = 1u << 1,   // 可以写文件、创建/删除文件和目录
	Rename = 1u << 2,  // 支持后端内原子 rename
	Resize = 1u << 3,  // 流支持 resize
};

/**
 * 后端接口。
 *
 * 路径约定：rel 是相对后端根目录的路径，已经规范化，不带前导 '/'，不含 `.` 和 `..`；
 * 空串表示后端根目录。
 *
 * 线程安全：所有方法都可能被多个线程并发调用，实现必须自己保证线程安全。
 * 所有权：后端由 std::shared_ptr 持有；open 返回的流应当自己保活所需的数据
 * （例如归档索引、内存缓冲区），这样卸载后流仍然可用。
 */
class VFS_DLL IBackend
{
public:
	virtual ~IBackend() = default;

	/**
	 * @return BackendCaps 位组合
	 */
	virtual uint32_t capabilities() const = 0;

	/**
	 * 查询元数据。
	 * @param rel 相对路径
	 * @param out 输出元数据（mount、writable 字段由 VFS 填写，后端不用管）
	 * @return OK；不存在返回 NOT_FOUND；中间某一级是文件返回 NOT_DIR
	 */
	virtual Error stat(std::string_view rel, Stat& out) const = 0;

	/**
	 * 打开文件流。
	 * @param rel  相对路径
	 * @param mode 打开模式，语义见 IFileStream.h
	 * @param err  可选的错误输出
	 * @return 成功返回流对象（所有权交给调用方），失败返回 nullptr。
	 *         rel 是目录时必须失败并返回 IS_DIR（VFS 在只有一层时直接 open、不先 stat，依赖这一点）
	 */
	virtual std::unique_ptr<IFileStream> open(std::string_view rel, IFileStream::Mode mode, Error* err) = 0;

	/**
	 * 列出目录。顺序不要求，VFS 会排序。
	 * @param relDir 相对目录
	 * @param cb     回调，返回 false 时停止遍历
	 */
	virtual Error list(std::string_view relDir, const std::function<bool(const DirEntry&)>& cb) const = 0;

	/**
	 * 创建单级目录。父目录必须存在。
	 */
	virtual Error createDir(std::string_view /*rel*/) { return Error::READ_ONLY; }

	/**
	 * 删除空目录。
	 */
	virtual Error removeDir(std::string_view /*rel*/) { return Error::READ_ONLY; }

	/**
	 * 删除文件。
	 */
	virtual Error removeFile(std::string_view /*rel*/) { return Error::READ_ONLY; }

	/**
	 * 后端内原子改名。默认不支持（与 BackendCaps 不含 Rename 一致，返回 UNSUPPORTED）。
	 */
	virtual Error rename(std::string_view /*from*/, std::string_view /*to*/) { return Error::UNSUPPORTED; }

	/**
	 * 返回本机路径（只有本机后端返回非空串），用于必须使用真实路径的模块。
	 */
	virtual std::string realPath(std::string_view /*rel*/) const { return std::string(); }

	/**
	 * 后端类型名，例如 "native"、"memory"、"archive"，用于日志和调试。
	 */
	virtual const char* typeName() const = 0;
};

NS_VFS_END
