// Layered mounts plus aliases: a native directory (standing in for development assets) is stacked on a memory layer (standing in for shipped assets),
// then a writable save directory is mounted and reached through the "res://" and "save://" aliases.
#include "vfs/VirtualFileSystem.h"
#include <cstdio>
#include <filesystem>

USING_NS_VFS;
namespace fs_ = std::filesystem;

int main()
{
	// Prepare a temporary native directory to act as the "development overlay"
	const fs_::path work = fs_::temp_directory_path() / "vfs_example_layers";
	fs_::remove_all(work);
	fs_::create_directories(work / "dev" / "ui");

	{
	VirtualFileSystem fs;

	// 1. Bottom layer: memory backend, prefilled with "shipped" assets (priority 0)
	std::shared_ptr<MemoryBackend> shipped;
	fs.mountMemory("/res", MountOptions{0, MountFlags::ReadOnly}, &shipped);
	shipped->addFile("config.json", {'{', '"', 'v', '"', ':', '1', '}'});
	shipped->addFile("ui/title.txt", {'S', 'h', 'i', 'p', 'p', 'e', 'd'});

	// 2. Upper layer: native development directory (priority 10); a file with the same name wins here
	fs.mountNative("/res", (work / "dev").string(), MountOptions{10, MountFlags::ReadOnly});

	// 3. A writable save directory is mounted on its own at /save; createRoot creates a missing root automatically
	NativeOptions saveOptions;
	saveOptions.createRoot = true;
	fs.mountNative("/save", (work / "save").string(), MountOptions{0, MountFlags::WriteTarget}, saveOptions);

	// 4. Aliases: res://xxx is equivalent to /res/xxx
	fs.addAlias("res://", "/res");
	fs.addAlias("save://", "/save");

	std::string text;
	fs.readFile("res://ui/title.txt", text);
	std::printf("title before override: %s\n", text.c_str());

	// Place a same-named file in the development directory (written straight to disk, as if art updated the asset); it hides the version in the memory layer
	{
		std::FILE* f = std::fopen((work / "dev" / "ui" / "title.txt").string().c_str(), "wb");
		std::fputs("Dev build", f);
		std::fclose(f);
	}
	fs.readFile("res://ui/title.txt", text);
	std::printf("title after override:  %s\n", text.c_str());

	// Both layers of /res are read-only, so a write fails; saves go to /save
	Error e = fs.writeFile("res://config.json", "{}");
	std::printf("write to res:// -> %s\n", errorName(e));
	fs.writeFile("save://slot1.dat", "level=3");
	fs.readFile("save://slot1.dat", text);
	std::printf("save://slot1.dat = %s\n", text.c_str());

	// Merged view: the ui/ directory contents of both layers are combined
	for (const auto& entry : fs.list("res://"))
		std::printf("res://%s%s\n", entry.name.c_str(), entry.type == FileType::DIR ? "/" : "");

	// See which mount the visible file comes from
	Stat st;
	fs.stat("res://ui/title.txt", st);
	for (const auto& m : fs.mounts())
		if (m.id == st.mount)
			std::printf("ui/title.txt comes from the %s layer\n", m.backend->typeName());

	}   // fs destructor: unmount everything and close native handles (on Windows the directory can be deleted only after that)
	fs_::remove_all(work);
	return 0;
}
