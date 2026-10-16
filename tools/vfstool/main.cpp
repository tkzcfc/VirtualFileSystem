// vfstool 命令行入口：用 CLI11 解析参数后调用 vfstool_core 中的命令实现（vfs-format F19）
#include "core/Commands.h"
#include "vfs/native/NativeFileStream.h"
#include "CLI11.hpp"
#include <iostream>
#include <map>

#if defined(_WIN32)
#    include <fcntl.h>
#    include <io.h>
#    include <windows.h>
#    include <shellapi.h>
#endif

USING_NS_VFS;
using namespace vfs::tool;

namespace
{

#if defined(_WIN32)
// UTF-16 → UTF-8（只在 Windows 上用 Win32 API 转换，不依赖已废弃的 std::wstring_convert）
std::string wideToUtf8(const wchar_t* wide)
{
	int len = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
	if (len <= 1)
		return std::string();
	std::string out(static_cast<size_t>(len), '\0');
	::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), len, nullptr, nullptr);
	out.pop_back();   // 去掉结尾的 '\0'
	return out;
}

/**
 * 控制台输出代码页守卫：切到 UTF-8，退出时恢复。
 * 控制台代码页是父进程（cmd/PowerShell）共享的，不恢复会影响用户之后的命令。
 */
class ConsoleCodePageGuard
{
public:
	ConsoleCodePageGuard()
		: m_oldOutputCP(::GetConsoleOutputCP())
	{
		::SetConsoleOutputCP(CP_UTF8);
	}

	~ConsoleCodePageGuard()
	{
		if (m_oldOutputCP != 0)
			::SetConsoleOutputCP(m_oldOutputCP);
	}

	ConsoleCodePageGuard(const ConsoleCodePageGuard&) = delete;

	ConsoleCodePageGuard& operator=(const ConsoleCodePageGuard&) = delete;

private:
	UINT m_oldOutputCP;
};
#endif

/**
 * 取得 UTF-8 编码的命令行参数。
 * Windows 上 main 的 argv 是 ANSI 代码页（例如 GBK），中文路径会变成乱码，
 * 所以改用 GetCommandLineW + CommandLineToArgvW 取 UTF-16 原文再转成 UTF-8；
 * 其他平台的 argv 本身就是 UTF-8（按系统 locale，现代系统均为 UTF-8），原样使用。
 */
std::vector<std::string> utf8Arguments(int argc, char** argv)
{
	std::vector<std::string> args;
#if defined(_WIN32)
	int wargc = 0;
	LPWSTR* wargv = ::CommandLineToArgvW(::GetCommandLineW(), &wargc);
	if (wargv)
	{
		args.reserve(static_cast<size_t>(wargc));
		for (int i = 0; i < wargc; ++i)
			args.push_back(wideToUtf8(wargv[i]));
		::LocalFree(wargv);
		return args;
	}
	// 极少数情况下失败：退回 ANSI argv
#endif
	args.reserve(static_cast<size_t>(argc));
	for (int i = 0; i < argc; ++i)
		args.emplace_back(argv[i]);
	return args;
}

// 展开 @args.txt：文件中的参数按空白分隔，支持双引号（vfs-format F21）。
// 参数文件按 UTF-8 解释，允许带 UTF-8 BOM（Windows 记事本默认会加）
bool expandResponseFiles(const std::vector<std::string>& in, std::vector<std::string>& out)
{
	for (size_t i = 0; i < in.size(); ++i)
	{
		const std::string& arg = in[i];
		if (i == 0 || arg.size() < 2 || arg[0] != '@')
		{
			out.push_back(arg);
			continue;
		}
		NativeFileStream fs;
		if (!fs.open(arg.substr(1), IFileStream::Mode::READ))
		{
			std::cerr << "错误: 无法读取参数文件 " << arg.substr(1) << "\n";
			return false;
		}
		std::string content;
		char buf[4096];
		int n = 0;
		while ((n = fs.read(buf, sizeof(buf))) > 0)
			content.append(buf, static_cast<size_t>(n));
		if (content.size() >= 3 && content.compare(0, 3, "\xEF\xBB\xBF") == 0)
			content.erase(0, 3);
		std::string cur;
		bool inQuote = false;
		bool has = false;
		for (char c : content)
		{
			if (c == '"')
			{
				inQuote = !inQuote;
				has = true;
			}
			else if (!inQuote && (c == ' ' || c == '\t' || c == '\r' || c == '\n'))
			{
				if (has)
					out.push_back(cur);
				cur.clear();
				has = false;
			}
			else
			{
				cur.push_back(c);
				has = true;
			}
		}
		if (has)
			out.push_back(cur);
	}
	return true;
}

// 根据 CLI11 记录的解析顺序，把成对的正/反规则选项还原成按命令行顺序排列的规则列表
std::vector<Rule> orderedRules(CLI::App* app, CLI::Option* positive, CLI::Option* negative)
{
	std::vector<Rule> rules;
	std::map<CLI::Option*, size_t> used;
	for (CLI::Option* opt : app->parse_order())
	{
		if (opt != positive && opt != negative)
			continue;
		const auto& results = opt->results();
		size_t& idx = used[opt];
		if (idx < results.size())
			rules.push_back(Rule{results[idx++], opt == positive});
	}
	return rules;
}

void addKeyOptions(CLI::App* cmd, KeySource& key)
{
	cmd->add_option("--key-file", key.keyFile, "索引密钥文件（32 字节原始数据或 64 个 hex 字符）");
	cmd->add_option("--key-hex", key.keyHex, "索引密钥（64 个 hex 字符，会打印警告）");
}

} // namespace

int main(int argc, char** argv)
{
#if defined(_WIN32)
	// 控制台按 UTF-8 显示中文输出（退出时恢复原代码页）
	ConsoleCodePageGuard codePageGuard;
#endif

	std::vector<std::string> args;
	if (!expandResponseFiles(utf8Arguments(argc, argv), args))
		return static_cast<int>(ExitCode::USAGE);
	std::vector<char*> argvExpanded;
	for (auto& a : args)
		argvExpanded.push_back(a.data());

	CLI::App app{"vfstool：.vfs 归档工具（打包、解包、查看、校验）"};
	app.require_subcommand(1);
	// CLI11 在 Windows 上默认把 "/x" 当作 Windows 风格选项，会把以 '/' 开头的路径误当成选项。
	// 各平台统一只用 "-x/--xxx"（必须在添加子命令之前设置，子命令会继承）
	app.allow_windows_style_options(false);
	app.set_version_flag("--version", "vfstool 2.0.0（.vfs 格式版本 1）");

	// ---------------- pack
	PackOptions pack;
	auto* packCmd = app.add_subcommand("pack", "把目录打包成 .vfs");
	packCmd->add_option("dir", pack.inputDir, "源目录")->required();
	packCmd->add_option("-o,--output", pack.outputPath, "输出 .vfs 文件");
	packCmd->add_option("--prefix", pack.prefix, "归档内路径前缀");
	auto* includeOpt = packCmd->add_option("--include", "包含规则（glob，可重复，最后匹配生效）")->allow_extra_args(false)->take_all();
	auto* excludeOpt = packCmd->add_option("--exclude", "排除规则（glob，可重复）")->allow_extra_args(false)->take_all();
	auto* encryptOpt = packCmd->add_option("--encrypt", "XOR 加密规则（glob，可重复）")->allow_extra_args(false)->take_all();
	auto* noEncryptOpt = packCmd->add_option("--no-encrypt", "不加密规则（glob，可重复）")->allow_extra_args(false)->take_all();
	auto* compressOpt = packCmd->add_option("--compress", "LZ4 压缩规则（glob，可重复）")->allow_extra_args(false)->take_all();
	auto* noCompressOpt = packCmd->add_option("--no-compress", "不压缩规则（glob，可重复）")->allow_extra_args(false)->take_all();
	packCmd->add_flag("--encrypt-all", pack.encryptAll, "默认加密所有文件");
	packCmd->add_flag("--compress-all", pack.compressAll, "默认压缩所有文件");
	packCmd->add_option("--align", pack.alignment, "数据对齐（2 的幂，1..65536，默认 16）");
	addKeyOptions(packCmd, pack.key);
	packCmd->add_flag("--store-key", pack.storeKey, "把外部索引密钥也存进文件");
	packCmd->add_flag("--no-index-encrypt", pack.noIndexEncrypt, "索引不加密");
	packCmd->add_option("--xor-key-len", pack.xorKeyLength, "单文件 XOR key 长度（16..32，默认 32）");
	packCmd->add_option("--lz4-level", pack.compressionLevel, "LZ4 HC 压缩级别（1..12，默认 9）");
	packCmd->add_flag("--keep-mtime", pack.keepMtime, "写入源文件的真实 mtime（默认 0，便于复现）");
	packCmd->add_flag("--empty-dirs", pack.emptyDirs, "保留空目录（默认不保留）");
	packCmd->add_flag("--no-dedup", pack.noDedup, "关闭内容去重");
	packCmd->add_flag("--no-default-excludes", pack.noDefaultExcludes, "不排除 .git/.svn/.DS_Store/Thumbs.db/desktop.ini");
	packCmd->add_flag("--dry-run", pack.dryRun, "只列出将要打包的文件");
	packCmd->add_flag("-v,--verbose", pack.verbose, "输出每个文件");

	// ---------------- unpack
	UnpackOptions unpack;
	auto* unpackCmd = app.add_subcommand("unpack", "解包整个归档");
	unpackCmd->add_option("archive", unpack.archive.archive, "归档文件")->required();
	unpackCmd->add_option("-o,--output", unpack.outputDir, "输出目录")->required();
	auto* unIncludeOpt = unpackCmd->add_option("--include", "包含规则（glob）")->allow_extra_args(false)->take_all();
	auto* unExcludeOpt = unpackCmd->add_option("--exclude", "排除规则（glob）")->allow_extra_args(false)->take_all();
	addKeyOptions(unpackCmd, unpack.archive.key);
	unpackCmd->add_flag("--force", unpack.force, "覆盖已存在的文件");
	unpackCmd->add_flag("--no-verify", unpack.noVerify, "不校验内容哈希");
	unpackCmd->add_flag("-v,--verbose", unpack.verbose, "输出每个文件");

	// ---------------- info
	InfoOptions info;
	auto* infoCmd = app.add_subcommand("info", "显示归档信息");
	infoCmd->add_option("archive", info.archive.archive, "归档文件")->required();
	addKeyOptions(infoCmd, info.archive.key);
	infoCmd->add_flag("--json", info.json, "JSON 输出");

	// ---------------- list
	ListOptions list;
	auto* listCmd = app.add_subcommand("list", "列出条目");
	listCmd->alias("ls");
	listCmd->add_option("archive", list.archive.archive, "归档文件")->required();
	listCmd->add_option("pattern", list.pattern, "glob 过滤");
	addKeyOptions(listCmd, list.archive.key);
	listCmd->add_flag("-l,--long", list.longFormat, "显示大小、存储大小、标志和哈希");
	listCmd->add_flag("--json", list.json, "JSON 输出");

	// ---------------- cat
	CatOptions cat;
	auto* catCmd = app.add_subcommand("cat", "把单个文件输出到 stdout");
	catCmd->add_option("archive", cat.archive.archive, "归档文件")->required();
	catCmd->add_option("path", cat.path, "归档内路径")->required();
	addKeyOptions(catCmd, cat.archive.key);

	// ---------------- extract
	ExtractOptions extract;
	auto* extractCmd = app.add_subcommand("extract", "提取指定文件或目录");
	extractCmd->add_option("archive", extract.archive.archive, "归档文件")->required();
	extractCmd->add_option("paths", extract.paths, "归档内路径")->required();
	extractCmd->add_option("-o,--output", extract.outputDir, "输出目录")->required();
	extractCmd->add_flag("--force", extract.force, "覆盖已存在的文件");
	addKeyOptions(extractCmd, extract.archive.key);

	// ---------------- verify
	VerifyOptions verify;
	auto* verifyCmd = app.add_subcommand("verify", "校验 header、索引和每个文件的内容哈希");
	verifyCmd->add_option("archive", verify.archive.archive, "归档文件")->required();
	addKeyOptions(verifyCmd, verify.archive.key);
	verifyCmd->add_flag("-v,--verbose", verify.verbose, "输出每个文件的结果");

	// ---------------- keygen
	KeygenOptions keygen;
	auto* keygenCmd = app.add_subcommand("keygen", "生成 32 字节随机索引密钥");
	keygenCmd->add_option("-o,--output", keygen.outputFile, "输出文件（默认打印到 stdout）");

	try
	{
		app.parse(static_cast<int>(argvExpanded.size()), argvExpanded.data());
	}
	catch (const CLI::ParseError& e)
	{
		int code = app.exit(e);
		return code == 0 ? 0 : static_cast<int>(ExitCode::USAGE);
	}

	if (packCmd->parsed())
	{
		pack.filterRules = orderedRules(packCmd, includeOpt, excludeOpt);
		pack.encryptRules = orderedRules(packCmd, encryptOpt, noEncryptOpt);
		pack.compressRules = orderedRules(packCmd, compressOpt, noCompressOpt);
		return runPack(pack, std::cout, std::cerr);
	}
	if (unpackCmd->parsed())
	{
		unpack.filterRules = orderedRules(unpackCmd, unIncludeOpt, unExcludeOpt);
		return runUnpack(unpack, std::cout, std::cerr);
	}
	if (infoCmd->parsed())
		return runInfo(info, std::cout, std::cerr);
	if (listCmd->parsed())
		return runList(list, std::cout, std::cerr);
	if (catCmd->parsed())
	{
#if defined(_WIN32)
		// stdout 切到二进制模式，避免 "\n" 被转换成 "\r\n"
		_setmode(_fileno(stdout), _O_BINARY);
#endif
		return runCat(cat, std::cout, std::cerr);
	}
	if (extractCmd->parsed())
		return runExtract(extract, std::cout, std::cerr);
	if (verifyCmd->parsed())
		return runVerify(verify, std::cout, std::cerr);
	if (keygenCmd->parsed())
		return runKeygen(keygen, std::cout, std::cerr);
	return static_cast<int>(ExitCode::USAGE);
}
