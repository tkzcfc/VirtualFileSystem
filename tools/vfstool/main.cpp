// vfstool 命令行入口：用 CLI11 解析参数后调用 vfstool_core 中的命令实现
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

// 展开 @args.txt：文件中的参数按空白分隔，支持双引号。
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
			std::cerr << "error: cannot read argument file " << arg.substr(1) << "\n";
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
	cmd->add_option("--key-file", key.keyFile, "index key file (32 raw bytes or 64 hex characters)");
	cmd->add_option("--key-hex", key.keyHex, "index key (64 hex characters; prints a warning)");
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

	CLI::App app{"vfstool: .vfs archive tool (pack, unpack, inspect, verify)"};
	app.require_subcommand(1);
	// CLI11 在 Windows 上默认把 "/x" 当作 Windows 风格选项，会把以 '/' 开头的路径误当成选项。
	// 各平台统一只用 "-x/--xxx"（必须在添加子命令之前设置，子命令会继承）
	app.allow_windows_style_options(false);
	app.set_version_flag("--version", "vfstool 2.0.0 (.vfs format version 1)");

	// ---------------- pack
	PackOptions pack;
	auto* packCmd = app.add_subcommand("pack", "pack a directory into a .vfs archive");
	packCmd->add_option("dir", pack.inputDir, "source directory")->required();
	packCmd->add_option("-o,--output", pack.outputPath, "output .vfs file");
	packCmd->add_option("--prefix", pack.prefix, "path prefix inside the archive");
	auto* includeOpt = packCmd->add_option("--include", "include pattern (glob, repeatable, last match wins)")->allow_extra_args(false)->take_all();
	auto* excludeOpt = packCmd->add_option("--exclude", "exclude pattern (glob, repeatable)")->allow_extra_args(false)->take_all();
	auto* encryptOpt = packCmd->add_option("--encrypt", "XOR-encrypt pattern (glob, repeatable)")->allow_extra_args(false)->take_all();
	auto* noEncryptOpt = packCmd->add_option("--no-encrypt", "do-not-encrypt pattern (glob, repeatable)")->allow_extra_args(false)->take_all();
	auto* compressOpt = packCmd->add_option("--compress", "LZ4-compress pattern (glob, repeatable)")->allow_extra_args(false)->take_all();
	auto* noCompressOpt = packCmd->add_option("--no-compress", "do-not-compress pattern (glob, repeatable)")->allow_extra_args(false)->take_all();
	packCmd->add_flag("--encrypt-all", pack.encryptAll, "encrypt all files by default");
	packCmd->add_flag("--compress-all", pack.compressAll, "compress all files by default");
	packCmd->add_option("--align", pack.alignment, "data alignment (power of two, 1..65536, default 16)");
	addKeyOptions(packCmd, pack.key);
	packCmd->add_flag("--store-key", pack.storeKey, "also store the external index key in the archive");
	packCmd->add_flag("--no-index-encrypt", pack.noIndexEncrypt, "do not encrypt the index");
	packCmd->add_option("--xor-key-len", pack.xorKeyLength, "per-file XOR key length (16..32, default 32)");
	packCmd->add_option("--lz4-level", pack.compressionLevel, "LZ4 HC compression level (1..12, default 9)");
	packCmd->add_flag("--keep-mtime", pack.keepMtime, "store the real source mtime (default 0 for reproducible output)");
	packCmd->add_flag("--empty-dirs", pack.emptyDirs, "keep empty directories (dropped by default)");
	packCmd->add_flag("--no-dedup", pack.noDedup, "disable content deduplication");
	packCmd->add_flag("--no-default-excludes", pack.noDefaultExcludes, "do not exclude .git/.svn/.DS_Store/Thumbs.db/desktop.ini");
	packCmd->add_flag("--dry-run", pack.dryRun, "only list the files that would be packed");
	packCmd->add_flag("-v,--verbose", pack.verbose, "print every file");

	// ---------------- unpack
	UnpackOptions unpack;
	auto* unpackCmd = app.add_subcommand("unpack", "unpack the whole archive");
	unpackCmd->add_option("archive", unpack.archive.archive, "archive file")->required();
	unpackCmd->add_option("-o,--output", unpack.outputDir, "output directory")->required();
	auto* unIncludeOpt = unpackCmd->add_option("--include", "include pattern (glob)")->allow_extra_args(false)->take_all();
	auto* unExcludeOpt = unpackCmd->add_option("--exclude", "exclude pattern (glob)")->allow_extra_args(false)->take_all();
	addKeyOptions(unpackCmd, unpack.archive.key);
	unpackCmd->add_flag("--force", unpack.force, "overwrite existing files");
	unpackCmd->add_flag("--no-verify", unpack.noVerify, "do not verify content hashes");
	unpackCmd->add_flag("-v,--verbose", unpack.verbose, "print every file");

	// ---------------- info
	InfoOptions info;
	auto* infoCmd = app.add_subcommand("info", "show archive information");
	infoCmd->add_option("archive", info.archive.archive, "archive file")->required();
	addKeyOptions(infoCmd, info.archive.key);
	infoCmd->add_flag("--json", info.json, "JSON output");

	// ---------------- list
	ListOptions list;
	auto* listCmd = app.add_subcommand("list", "list entries");
	listCmd->alias("ls");
	listCmd->add_option("archive", list.archive.archive, "archive file")->required();
	listCmd->add_option("pattern", list.pattern, "glob filter");
	addKeyOptions(listCmd, list.archive.key);
	listCmd->add_flag("-l,--long", list.longFormat, "show size, stored size, flags and hash");
	listCmd->add_flag("--json", list.json, "JSON output");

	// ---------------- cat
	CatOptions cat;
	auto* catCmd = app.add_subcommand("cat", "write a single file to stdout");
	catCmd->add_option("archive", cat.archive.archive, "archive file")->required();
	catCmd->add_option("path", cat.path, "path inside the archive")->required();
	addKeyOptions(catCmd, cat.archive.key);

	// ---------------- extract
	ExtractOptions extract;
	auto* extractCmd = app.add_subcommand("extract", "extract the given files or directories");
	extractCmd->add_option("archive", extract.archive.archive, "archive file")->required();
	extractCmd->add_option("paths", extract.paths, "path inside the archive")->required();
	extractCmd->add_option("-o,--output", extract.outputDir, "output directory")->required();
	extractCmd->add_flag("--force", extract.force, "overwrite existing files");
	addKeyOptions(extractCmd, extract.archive.key);

	// ---------------- verify
	VerifyOptions verify;
	auto* verifyCmd = app.add_subcommand("verify", "verify the header, the index and every file's content hash");
	verifyCmd->add_option("archive", verify.archive.archive, "archive file")->required();
	addKeyOptions(verifyCmd, verify.archive.key);
	verifyCmd->add_flag("-v,--verbose", verify.verbose, "print the result for every file");

	// ---------------- keygen
	KeygenOptions keygen;
	auto* keygenCmd = app.add_subcommand("keygen", "generate a random 32-byte index key");
	keygenCmd->add_option("-o,--output", keygen.outputFile, "output file (default: print to stdout)");

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
