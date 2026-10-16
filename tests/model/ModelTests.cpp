// 参考模型模糊测试：单线程随机操作序列同时作用于 VFS 和一个极简的内存模型，
// 每一步比较返回的错误码，定期比较整个合并视图（以及写入层的真实内容）。
// 模型独立实现了 docs/mount-rules.md 中的规则：两层（只读底层 B + 可写上层 W），挂载点 /f。
#include "matrix/Envs.h"
#include <functional>
#include <map>
#include <random>

using namespace vfs;
using Mode = IFileStream::Mode;

namespace
{

// ---------------------------------------------------------------- 模型

struct Node
{
	bool dir = false;
	std::string content;
};

using Layer = std::map<std::string, Node>;   // 相对路径（"a/b"）→ 节点；根为 ""

std::string parentOf(const std::string& rel)
{
	size_t p = rel.rfind('/');
	return p == std::string::npos ? std::string() : rel.substr(0, p);
}

bool isUnder(const std::string& path, const std::string& dir)
{
	return path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 && path[dir.size()] == '/';
}

class Model
{
public:
	Layer w;   // 可写上层
	Layer b;   // 只读底层（单层配置时只有根）

	Model()
	{
		w[""].dir = true;
		b[""].dir = true;
	}

	// 解析：返回错误码；成功时 layer = 0（W）或 1（B）
	Error resolve(const std::string& rel, int& layer) const
	{
		if (rel.empty())
		{
			layer = 0;
			return Error::OK;
		}
		size_t pos = 0;
		while (true)
		{
			size_t end = rel.find('/', pos);
			const bool last = end == std::string::npos;
			std::string prefix = last ? rel : rel.substr(0, end);
			const Layer* top = w.count(prefix) ? &w : (b.count(prefix) ? &b : nullptr);
			if (!top)
				return Error::NOT_FOUND;
			if (last)
			{
				layer = top == &w ? 0 : 1;
				return Error::OK;
			}
			if (!top->at(prefix).dir)
				return Error::NOT_DIR;
			pos = end + 1;
		}
	}

	const Node& node(const std::string& rel, int layer) const { return layer == 0 ? w.at(rel) : b.at(rel); }

	// 在 W 中补齐父目录链
	void ensureParents(const std::string& rel)
	{
		std::string p = parentOf(rel);
		while (!p.empty())
		{
			w[p].dir = true;
			p = parentOf(p);
		}
	}

	Error checkParent(const std::string& rel) const
	{
		int pl = 0;
		Error pe = resolve(parentOf(rel), pl);
		if (pe != Error::OK)
			return pe == Error::NOT_DIR ? Error::NOT_DIR : Error::NOT_FOUND;
		if (!node(parentOf(rel), pl).dir)
			return Error::NOT_DIR;
		return Error::OK;
	}

	Error write(const std::string& rel, const std::string& data, bool append)
	{
		int layer = 0;
		Error e = resolve(rel, layer);
		if (e == Error::OK && node(rel, layer).dir)
			return Error::IS_DIR;
		if (e != Error::OK && e != Error::NOT_FOUND)
			return e;
		const bool found = e == Error::OK;
		if (found && layer == 0)
		{
			w[rel].content = append ? w[rel].content + data : data;
			return Error::OK;
		}
		if (!found)
		{
			Error pe = checkParent(rel);
			if (pe != Error::OK)
				return pe;
		}
		ensureParents(rel);
		Node n;
		n.content = (append && found) ? b.at(rel).content + data : data;
		w[rel] = n;
		return Error::OK;
	}

	Error removeFile(const std::string& rel)
	{
		int layer = 0;
		Error e = resolve(rel, layer);
		if (e != Error::OK)
			return e;
		if (node(rel, layer).dir)
			return Error::IS_DIR;
		if (layer == 1)
			return Error::READ_ONLY;
		w.erase(rel);
		return Error::OK;
	}

	Error createDir(const std::string& rel)
	{
		int layer = 0;
		Error e = resolve(rel, layer);
		if (e == Error::OK)
			return Error::EXISTS;
		if (e != Error::NOT_FOUND)
			return e;
		Error pe = checkParent(rel);
		if (pe != Error::OK)
			return pe;
		ensureParents(rel);
		w[rel].dir = true;
		return Error::OK;
	}

	Error createDirs(const std::string& rel)
	{
		size_t pos = 0;
		while (true)
		{
			size_t end = rel.find('/', pos);
			const bool last = end == std::string::npos;
			std::string prefix = last ? rel : rel.substr(0, end);
			int layer = 0;
			Error e = resolve(prefix, layer);
			if (e == Error::OK)
			{
				if (!node(prefix, layer).dir)
					return last ? Error::EXISTS : Error::NOT_DIR;
			}
			else if (e == Error::NOT_FOUND)
			{
				e = createDir(prefix);
				if (e != Error::OK && e != Error::EXISTS)
					return e;
			}
			else
				return e;
			if (last)
				return Error::OK;
			pos = end + 1;
		}
	}

	Error removeDir(const std::string& rel, bool recursive)
	{
		int layer = 0;
		Error e = resolve(rel, layer);
		if (e != Error::OK)
			return e;
		if (!node(rel, layer).dir)
			return Error::NOT_DIR;
		if (rel.empty())
			return Error::BUSY;
		const bool inW = w.count(rel) && w.at(rel).dir;
		const bool inB = b.count(rel) && b.at(rel).dir;
		if (inB)
			return Error::READ_ONLY;   // 只读层提供了该目录（无论 W 是否也有）
		if (!inW)
			return Error::NOT_FOUND;
		bool empty = true;
		for (const auto& kv : w)
			if (isUnder(kv.first, rel))
				empty = false;
		if (!recursive && !empty)
			return Error::NOT_EMPTY;
		for (auto it = w.begin(); it != w.end();)
			it = (it->first == rel || isUnder(it->first, rel)) ? w.erase(it) : std::next(it);
		return Error::OK;
	}

	Error rename(const std::string& from, const std::string& to)
	{
		int fl = 0;
		Error e = resolve(from, fl);
		if (e != Error::OK)
			return e;
		if (from == to)
			return Error::OK;
		if (from.empty() || to.empty())
			return Error::BUSY;
		if (fl == 1)
			return Error::READ_ONLY;
		const bool fromDir = w.at(from).dir;
		if (fromDir && b.count(from) && b.at(from).dir)
			return Error::READ_ONLY;
		if (fromDir && isUnder(to, from))
			return Error::INVALID_PATH;
		int tl = 0;
		e = resolve(to, tl);
		if (e != Error::OK && e != Error::NOT_FOUND)
			return e;
		const bool toFound = e == Error::OK;
		if (toFound)
		{
			const bool toDir = node(to, tl).dir;
			if (toDir && !fromDir)
				return Error::IS_DIR;
			if (!toDir && fromDir)
				return Error::NOT_DIR;
		}
		else
		{
			Error pe = checkParent(to);
			if (pe != Error::OK)
				return pe;
		}
		ensureParents(to);
		// 后端（POSIX）rename：只看 W 中的目标
		if (w.count(to))
		{
			const Node& t = w.at(to);
			if (t.dir && !fromDir)
				return Error::IS_DIR;
			if (!t.dir && fromDir)
				return Error::NOT_DIR;
			if (t.dir)
				for (const auto& kv : w)
					if (isUnder(kv.first, to))
						return Error::NOT_EMPTY;
			w.erase(to);
		}
		Layer moved;
		for (auto it = w.begin(); it != w.end();)
		{
			if (it->first == from || isUnder(it->first, from))
			{
				moved[to + it->first.substr(from.size())] = it->second;
				it = w.erase(it);
			}
			else
				++it;
		}
		for (auto& kv : moved)
			w[kv.first] = kv.second;
		return Error::OK;
	}

	// 合并视图："path" → "D" 或 "F:内容"
	std::map<std::string, std::string> view() const
	{
		std::map<std::string, std::string> out;
		auto add = [&](const Layer& l)
		{
			for (const auto& kv : l)
			{
				if (kv.first.empty())
					continue;
				int layer = 0;
				if (resolve(kv.first, layer) != Error::OK)
					continue;
				const Node& n = node(kv.first, layer);
				out[kv.first] = n.dir ? "D" : "F:" + n.content;
			}
		};
		add(w);
		add(b);
		return out;
	}
};

// ---------------------------------------------------------------- 被测配置

std::map<std::string, std::string> vfsView(const VirtualFileSystem& fs)
{
	std::map<std::string, std::string> out;
	fs.walk("/f", [&](const std::string& full, const DirEntry& d)
	{
		const std::string rel = full.substr(3);
		out[rel] = d.type == FileType::DIR ? "D" : "F:" + test::readAll(fs, full);
		return true;
	});
	return out;
}

// 直接读后端（绕过 VFS），用于核对写入层里“看不见”的父目录链
std::map<std::string, std::string> backendView(IBackend& backend)
{
	std::map<std::string, std::string> out;
	std::function<void(const std::string&)> rec = [&](const std::string& dir)
	{
		backend.list(dir, [&](const DirEntry& d)
		{
			const std::string rel = dir.empty() ? d.name : dir + "/" + d.name;
			if (d.type == FileType::DIR)
			{
				out[rel] = "D";
				rec(rel);
			}
			else
			{
				auto s = backend.open(rel, Mode::READ, nullptr);
				out[rel] = "F:" + (s ? test::str(test::readStream(*s)) : std::string("<open failed>"));
			}
			return true;
		});
	};
	rec("");
	return out;
}

std::map<std::string, std::string> layerView(const Layer& l)
{
	std::map<std::string, std::string> out;
	for (const auto& kv : l)
		if (!kv.first.empty())
			out[kv.first] = kv.second.dir ? "D" : "F:" + kv.second.content;
	return out;
}

// 随机生成底层内容（保证父目录链完整）
Layer randomBase(std::mt19937& rng)
{
	Layer b;
	b[""].dir = true;
	const char* names[] = {"a", "b", "c"};
	for (int i = 0; i < 8; ++i)
	{
		std::string rel;
		int depth = 1 + static_cast<int>(rng() % 3);
		bool ok = true;
		for (int d = 0; d < depth && ok; ++d)
		{
			std::string next = rel.empty() ? names[rng() % 3] : rel + "/" + names[rng() % 3];
			if (b.count(next) && !b[next].dir)
				ok = false;
			else
				rel = next;
		}
		if (!ok || b.count(rel))
			continue;
		std::string p = parentOf(rel);
		while (!p.empty())
		{
			b[p].dir = true;
			p = parentOf(p);
		}
		if (rng() % 4 == 0)
			b[rel].dir = true;
		else
			b[rel].content = "base:" + rel;
	}
	return b;
}

struct Config
{
	test::TempDir dir;
	VirtualFileSystem fs;
	std::shared_ptr<IBackend> upper;
	Model model;
};

enum class Kind
{
	Memory,
	Native,
	MemoryOverMemory,
	NativeOverArchive,
};

void setup(Config& c, Kind kind, std::mt19937& rng)
{
	if (kind == Kind::Memory)
	{
		c.upper = MemoryBackend::create();
		REQUIRE(c.fs.mount("/f", c.upper) != 0);
		return;
	}
	if (kind == Kind::Native)
	{
		c.dir.mkdir("root");
		c.upper = NativeBackend::create(c.dir.sub("root"));
		REQUIRE(c.fs.mount("/f", c.upper) != 0);
		return;
	}
	c.model.b = randomBase(rng);
	if (kind == Kind::MemoryOverMemory)
	{
		auto base = MemoryBackend::create();
		for (const auto& kv : c.model.b)
		{
			if (kv.first.empty())
				continue;
			if (kv.second.dir)
				base->createDir(kv.first);
			else
				base->addFile(kv.first, kv.second.content);
		}
		base->setReadOnly(true);
		REQUIRE(c.fs.mount("/f", base, env::prio(0)) != 0);
		c.upper = MemoryBackend::create();
	}
	else
	{
		ArchiveWriter writer;
		for (const auto& kv : c.model.b)
		{
			if (kv.first.empty())
				continue;
			if (kv.second.dir)
				writer.addDir(kv.first);
			else
				writer.addData(kv.first, test::bytes(kv.second.content));
		}
		REQUIRE(writer.write(c.dir.sub("base.vfs")) == Error::OK);
		REQUIRE(c.fs.mountArchive("/f", c.dir.sub("base.vfs"), {}, env::prio(0)) != 0);
		c.dir.mkdir("upper");
		c.upper = NativeBackend::create(c.dir.sub("upper"));
	}
	REQUIRE(c.upper);
	REQUIRE(c.fs.mount("/f", c.upper, env::prio(1)) != 0);
}

std::string randomPath(std::mt19937& rng)
{
	const char* names[] = {"a", "b", "c"};
	std::string rel = names[rng() % 3];
	int depth = static_cast<int>(rng() % 3);
	for (int d = 0; d < depth; ++d)
		rel += std::string("/") + names[rng() % 3];
	return rel;
}

// 统计每种结果出现的次数，确保随机序列确实覆盖了成功与各种失败分支
std::map<std::string, int> g_outcomes;

void runFuzz(Kind kind, uint32_t seed, int ops)
{
	std::mt19937 rng(seed);
	Config c;
	setup(c, kind, rng);
	CAPTURE(seed);
	REQUIRE(vfsView(c.fs) == c.model.view());

	std::string log;
	for (int i = 0; i < ops; ++i)
	{
		const std::string p = randomPath(rng);
		const std::string q = randomPath(rng);
		const std::string data = "d" + std::to_string(i);
		Error got = Error::OK;
		Error want = Error::OK;
		const unsigned op = rng() % 100;
		std::string desc;
		if (op < 22)
		{
			desc = "write " + p;
			got = c.fs.writeFile("/f/" + p, data);
			want = c.model.write(p, data, false);
		}
		else if (op < 32)
		{
			desc = "append " + p;
			Error e = Error::OK;
			auto s = c.fs.openFileStream("/f/" + p, Mode::APPEND, &e);
			if (s && s->write(data.data(), static_cast<unsigned>(data.size())) != static_cast<int>(data.size()))
				e = Error::IO_ERROR;
			got = e;
			want = c.model.write(p, data, true);
		}
		else if (op < 45)
		{
			desc = "removeFile " + p;
			got = c.fs.removeFile("/f/" + p);
			want = c.model.removeFile(p);
		}
		else if (op < 55)
		{
			desc = "createDir " + p;
			got = c.fs.createDir("/f/" + p);
			want = c.model.createDir(p);
		}
		else if (op < 63)
		{
			desc = "createDirs " + p;
			got = c.fs.createDirs("/f/" + p);
			want = c.model.createDirs(p);
		}
		else if (op < 75)
		{
			const bool rec = rng() % 2 == 0;
			desc = std::string(rec ? "removeDir -r " : "removeDir ") + p;
			got = c.fs.removeDir("/f/" + p, rec);
			want = c.model.removeDir(p, rec);
		}
		else if (op < 97)
		{
			desc = "rename " + p + " -> " + q;
			got = c.fs.rename("/f/" + p, "/f/" + q);
			want = c.model.rename(p, q);
		}
		else
		{
			desc = "removeDir -r <root>";
			got = c.fs.removeDir("/f", true);
			want = Error::BUSY;
		}
		log += desc + " = " + errorName(want) + "\n";
		++g_outcomes[desc.substr(0, desc.find(' ')) + ":" + errorName(want)];
		if (got != want)
		{
			CAPTURE(log);
			CAPTURE(desc);
			CHECK(got == want);
			return;
		}
		if (i % 20 == 19 || i == ops - 1)
		{
			const auto actual = vfsView(c.fs);
			const auto expected = c.model.view();
			if (actual != expected)
			{
				CAPTURE(log);
				CHECK(actual == expected);
				return;
			}
			const auto upperActual = backendView(*c.upper);
			const auto upperExpected = layerView(c.model.w);
			if (upperActual != upperExpected)
			{
				CAPTURE(log);
				CHECK(upperActual == upperExpected);
				return;
			}
		}
	}
}

} // namespace

namespace doctest
{
template <>
struct StringMaker<std::map<std::string, std::string>>
{
	static String convert(const std::map<std::string, std::string>& m)
	{
		std::string s = "{";
		for (const auto& kv : m)
			s += kv.first + "=" + kv.second + "; ";
		return String((s + "}").c_str());
	}
};
} // namespace doctest

// 每个用例结束后检查覆盖：至少出现了这些（操作:结果）组合
void checkCoverage(std::initializer_list<const char*> required)
{
	for (const char* r : required)
	{
		CAPTURE(r);
		CHECK(g_outcomes[r] > 0);
	}
	g_outcomes.clear();
}

TEST_SUITE("model")
{
	TEST_CASE("参考模型：单个内存后端，随机操作序列")
	{
		for (uint32_t seed = 1; seed <= static_cast<uint32_t>(40 * test::scale()); ++seed)
			runFuzz(Kind::Memory, seed, 300);
		checkCoverage({"write:OK", "write:IS_DIR", "write:NOT_DIR", "write:NOT_FOUND", "append:OK", "removeFile:OK",
					   "removeFile:IS_DIR", "createDir:OK", "createDir:EXISTS", "createDirs:OK", "removeDir:OK",
					   "removeDir:NOT_EMPTY", "rename:OK", "rename:INVALID_PATH", "rename:IS_DIR", "rename:NOT_DIR",
					   "rename:NOT_EMPTY"});
	}

	TEST_CASE("参考模型：单个本机目录，随机操作序列")
	{
		for (uint32_t seed = 1; seed <= static_cast<uint32_t>(15 * test::scale()); ++seed)
			runFuzz(Kind::Native, seed, 300);
		checkCoverage({"write:OK", "rename:OK", "removeDir:OK", "createDirs:OK"});
	}

	TEST_CASE("参考模型：只读内存底层 + 可写内存上层，随机操作序列")
	{
		for (uint32_t seed = 1; seed <= static_cast<uint32_t>(40 * test::scale()); ++seed)
			runFuzz(Kind::MemoryOverMemory, seed, 300);
		checkCoverage({"write:OK", "append:OK", "removeFile:OK", "removeFile:READ_ONLY", "removeDir:READ_ONLY",
					   "removeDir:OK", "rename:OK", "rename:READ_ONLY", "createDir:OK"});
	}

	TEST_CASE("参考模型：归档底层 + 本机目录上层，随机操作序列")
	{
		for (uint32_t seed = 1; seed <= static_cast<uint32_t>(15 * test::scale()); ++seed)
			runFuzz(Kind::NativeOverArchive, seed, 300);
		checkCoverage({"write:OK", "append:OK", "removeFile:READ_ONLY", "rename:READ_ONLY", "rename:OK"});
	}

	TEST_CASE("参考模型（长序列）：每种配置 200 个种子 × 2000 步" * doctest::skip(!test::slowEnabled()))
	{
		for (Kind k : {Kind::Memory, Kind::Native, Kind::MemoryOverMemory, Kind::NativeOverArchive})
			for (uint32_t seed = 1000; seed < 1200; ++seed)
				runFuzz(k, seed, 2000);
	}
}
