// 多线程读取：8 个线程同时从同一个归档读文件，同时主线程热更新（remount）归档。
// VirtualFileSystem 的所有接口都是线程安全的；已打开的流在卸载/替换后仍然可以读完。
#include "vfs/VirtualFileSystem.h"
#include "vfs/archive/ArchiveSource.h"
#include "vfs/archive/ArchiveWriter.h"
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

USING_NS_VFS;

namespace
{

// 在内存中打一个归档：100 个文件，内容是 "<version>:<i>"
std::shared_ptr<IBackend> makeArchive(int version)
{
	ArchiveWriter writer;
	for (int i = 0; i < 100; ++i)
	{
		std::string s = std::to_string(version) + ":" + std::to_string(i);
		FileWriteOptions fo;
		fo.compress = (i % 2) == 0;   // 一半压缩，一半明文
		writer.addData("data/" + std::to_string(i) + ".txt", std::vector<uint8_t>(s.begin(), s.end()), fo);
	}
	auto memory = MemoryBackend::create();
	{
		auto out = memory->open("a.vfs", IFileStream::Mode::WRITE, nullptr);
		writer.write(*out);
	}
	std::vector<uint8_t> bytes;
	auto in = memory->open("a.vfs", IFileStream::Mode::READ, nullptr);
	bytes.resize(static_cast<size_t>(in->size()));
	in->read(bytes.data(), static_cast<unsigned int>(bytes.size()));
	return ArchiveBackend::create(std::make_shared<MemoryArchiveSource>(std::move(bytes)));
}

}   // namespace

int main()
{
	VirtualFileSystem fs;
	MountId id = fs.mount("/res", makeArchive(1));

	std::atomic<bool> stop{false};
	std::atomic<long> reads{0}, errors{0};
	std::vector<std::thread> workers;
	for (int t = 0; t < 8; ++t)
	{
		workers.emplace_back([&, t]
		{
			std::string text;
			for (int i = t; !stop.load(std::memory_order_relaxed); i = (i + 7) % 100)
			{
				// 每次读到的内容要么是旧版本，要么是新版本，不会是混合的
				if (fs.readFile("/res/data/" + std::to_string(i) + ".txt", text) != Error::OK ||
					text.substr(text.find(':') + 1) != std::to_string(i))
					errors.fetch_add(1);
				reads.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}

	// 主线程：读线程运行期间把归档热更新 5 次
	for (int v = 2; v <= 6; ++v)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		fs.remount(id, makeArchive(v));
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	stop = true;
	for (auto& w : workers)
		w.join();

	std::string text;
	fs.readFile("/res/data/42.txt", text);
	std::printf("reads ok: %s, errors: %ld, final content of data/42.txt: %s\n", reads.load() > 0 ? "yes" : "no",
				errors.load(), text.c_str());
	return errors.load() == 0 ? 0 : 1;
}
