// 分层挂载 + 别名：本机目录（模拟开发资源）叠在内存层（模拟发布资源）之上，
// 再挂一个可写的存档目录，并用 "res://"、"save://" 别名访问。
#include "vfs/VirtualFileSystem.h"
#include <cstdio>
#include <filesystem>

USING_NS_VFS;
namespace fs_ = std::filesystem;

int main()
{
	// 准备一个临时本机目录作为“开发覆盖层”
	const fs_::path work = fs_::temp_directory_path() / "vfs_example_layers";
	fs_::remove_all(work);
	fs_::create_directories(work / "dev" / "ui");

	{
	VirtualFileSystem fs;

	// 1. 底层：内存后端，预填“发布版”资源（优先级 0）
	std::shared_ptr<MemoryBackend> shipped;
	fs.mountMemory("/res", MountOptions{0, MountFlags::ReadOnly}, &shipped);
	shipped->addFile("config.json", {'{', '"', 'v', '"', ':', '1', '}'});
	shipped->addFile("ui/title.txt", {'S', 'h', 'i', 'p', 'p', 'e', 'd'});

	// 2. 上层：本机开发目录（优先级 10），同名文件以它为准
	fs.mountNative("/res", (work / "dev").string(), MountOptions{10, MountFlags::ReadOnly});

	// 3. 可写存档目录单独挂在 /save；createRoot 让不存在的根目录自动创建
	NativeOptions saveOptions;
	saveOptions.createRoot = true;
	fs.mountNative("/save", (work / "save").string(), MountOptions{0, MountFlags::WriteTarget}, saveOptions);

	// 4. 别名：res://xxx 等价于 /res/xxx
	fs.addAlias("res://", "/res");
	fs.addAlias("save://", "/save");

	std::string text;
	fs.readFile("res://ui/title.txt", text);
	std::printf("title before override: %s\n", text.c_str());

	// 在开发目录放一个同名文件（直接写磁盘，模拟美术更新资源），它会遮蔽内存层里的版本
	{
		std::FILE* f = std::fopen((work / "dev" / "ui" / "title.txt").string().c_str(), "wb");
		std::fputs("Dev build", f);
		std::fclose(f);
	}
	fs.readFile("res://ui/title.txt", text);
	std::printf("title after override:  %s\n", text.c_str());

	// /res 的两层都是只读，写入会失败；存档写到 /save
	Error e = fs.writeFile("res://config.json", "{}");
	std::printf("write to res:// -> %s\n", errorName(e));
	fs.writeFile("save://slot1.dat", "level=3");
	fs.readFile("save://slot1.dat", text);
	std::printf("save://slot1.dat = %s\n", text.c_str());

	// 合并视图：两层的 ui/ 目录内容合并
	for (const auto& entry : fs.list("res://"))
		std::printf("res://%s%s\n", entry.name.c_str(), entry.type == FileType::DIR ? "/" : "");

	// 查看可见文件来自哪个挂载
	Stat st;
	fs.stat("res://ui/title.txt", st);
	for (const auto& m : fs.mounts())
		if (m.id == st.mount)
			std::printf("ui/title.txt comes from the %s layer\n", m.backend->typeName());

	}   // fs 析构：卸载全部挂载、关闭本机句柄（Windows 上之后才能删除目录）
	fs_::remove_all(work);
	return 0;
}
