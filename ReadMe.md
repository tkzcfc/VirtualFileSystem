## 虚拟文件系统，用于游戏资源打包和管理

#### 1. 支持挂载本机目录

#### 2. 支持挂载内存

#### 3.支持挂载pack文件 https://github.com/tkzcfc/paktool



```c++

	VirtualFileSystem virtualFileSystem;
	virtualFileSystem.mount(new NativeFileSystem("./test-data/dlc1", "/root"));
	virtualFileSystem.mount(new MemoryFileSystem("", "/root"));
	virtualFileSystem.mount(new MemoryFileSystem("", "/"));
	virtualFileSystem.mount(new NativeFileSystem("./test-data/dlc2", "/root"));
	virtualFileSystem.mount(new NativeFileSystem("./test-data/dlc2", "/root/vfs/vvv"));
	virtualFileSystem.mount(new PackFileSystem("./test-data/test.pak", "/root"));

	std::vector<uint8_t> bin = { 'H', 'E', 'L', 'L', 'O', '^', 'v', '^'};

	assert(virtualFileSystem.isFile("/root/packroot/packfile1.txt") == true);
	assert(virtualFileSystem.isDir("/root/packroot/packdir1///") == true);

	assert(virtualFileSystem.createDir("/mem"));
	assert(writeFile(virtualFileSystem, "/aaaaaaaaaaaaaaaa.txt", bin));
	assert(writeFile(virtualFileSystem, "/bbbbbbbbbbbbbb.txt", bin));
	assert(writeFile(virtualFileSystem, "/ccccccccccccccc.txt", bin));
	assert(writeFile(virtualFileSystem, "/xzcxz/ddddddddd.txt", bin) == false);

	enumerateFiles(virtualFileSystem, "/");
	enumerateFiles(virtualFileSystem, "/root/vfs");
	assert(writeFile(virtualFileSystem, "/root/vfs/ddddddddd.txt", bin) == false);
	enumerateFiles(virtualFileSystem, "/root/vfs/vvv");


	virtualFileSystem.createDir("/root/mem/");
	assert(writeFile(virtualFileSystem, "/root/mem/aaaaaaaaaaaaaaaa.txt", bin));
	assert(writeFile(virtualFileSystem, "/root/mem_data.txt", bin));

	enumerateFiles(virtualFileSystem, "/root/");

	assert(readFile(virtualFileSystem, "/check.txt", true) == false);
	assert(readFile(virtualFileSystem, "/root/../check.txt", true) == false);
	assert(readFile(virtualFileSystem, "/root/file.txt", true) == true);
	assert(readFile(virtualFileSystem, "/root/mem/aaaaaaaaaaaaaaaa.txt", true) == true);
	assert(readFile(virtualFileSystem, "/root/mem_data.txt", true) == true);
	assert(readFile(virtualFileSystem, "/root/check_png.py", false) == true);

```

