// 本文件：本机目录后端。把一个本机目录挂到虚拟路径上（决策 #21）。
#pragma once

#include "../IBackend.h"

NS_VFS_BEGIN

/**
 * 本机后端选项。
 */
struct NativeOptions
{
	bool readOnly = false;        // 只读：所有写操作返回 READ_ONLY
	bool followSymlinks = true;   // 为 false 时，解析后逃出根目录的路径一律拒绝（决策 #20）
	bool strictCase = false;      // 为 true 时核对真实文件名的大小写，不一致打警告（决策 #12，建议只在 Debug 打开）
	bool createRoot = false;      // 根目录不存在时自动创建（mkdir -p）
};

/**
 * 本机目录后端。
 * - 不做任何缓存，每次都访问文件系统，外部修改立即可见（决策 #32）。
 * - 路径段额外拒绝 ':' 和 Windows 保留名，保证各平台行为一致（决策 #11）。
 * 线程安全：无共享可变状态，可以任意并发调用。
 */
class VFS_DLL NativeBackend : public IBackend
{
public:

	/**
	 * 创建本机后端。
	 * @param rootDir 本机目录（UTF-8，可以是相对路径，会转成绝对路径）
	 * @param options 选项
	 * @param err     可选的错误输出：目录不存在返回 NOT_FOUND，不是目录返回 NOT_DIR
	 * @return 成功返回后端，失败返回 nullptr
	 */
	static std::shared_ptr<NativeBackend> create(std::string_view rootDir, const NativeOptions& options = {},
												 Error* err = nullptr);

	NativeBackend(std::string root, const NativeOptions& options);

	virtual ~NativeBackend();

	virtual uint32_t capabilities() const override;

	virtual Error stat(std::string_view rel, Stat& out) const override;

	virtual std::unique_ptr<IFileStream> open(std::string_view rel, IFileStream::Mode mode, Error* err) override;

	virtual Error list(std::string_view relDir, const std::function<bool(const DirEntry&)>& cb) const override;

	virtual Error createDir(std::string_view rel) override;

	virtual Error removeDir(std::string_view rel) override;

	virtual Error removeFile(std::string_view rel) override;

	virtual Error rename(std::string_view from, std::string_view to) override;

	virtual std::string realPath(std::string_view rel) const override;

	virtual const char* typeName() const override { return "native"; }

	/**
	 * 本机根目录（绝对路径，'/' 分隔，UTF-8）。
	 */
	const std::string& root() const { return m_root; }

	const NativeOptions& options() const { return m_options; }

private:

	// 相对路径转成本机绝对路径，并做路径段检查和 symlink 逃逸检查
	Error toNative(std::string_view rel, std::string& out) const;

	// strictCase 模式下核对大小写，只打警告不影响结果
	void checkCase(std::string_view rel) const;

private:
	std::string m_root;            // 本机根目录
	std::string m_canonicalRoot;   // followSymlinks=false 时用于逃逸检查的规范根目录
	NativeOptions m_options;
};

NS_VFS_END
