// vfstool command-line entry: parse arguments with CLI11, then call the command implementations in vfstool_core
#include "core/Commands.h"
#include "vfs/native/NativeFileStream.h"
#include "CLI11.hpp"
#include <iostream>
#include <new>
#include <map>

#if VFS_PLATFORM_WINDOWS
#    include <fcntl.h>
#    include <io.h>
#    include <windows.h>
#    include <shellapi.h>
#endif

USING_NS_VFS;
using namespace vfs::tool;

namespace
{

#if VFS_PLATFORM_WINDOWS
// UTF-16 to UTF-8 (converted with the Win32 API on Windows only; does not depend on the deprecated std::wstring_convert)
std::string wideToUtf8(const wchar_t* wide)
{
	int len = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
	if (len <= 1)
		return std::string();
	std::string out(static_cast<size_t>(len), '\0');
	::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), len, nullptr, nullptr);
	out.pop_back();   // drop the trailing '\0'
	return out;
}

/**
 * Guard for the console output code page: switch to UTF-8 and restore it on exit.
 * The console code page is shared with the parent process (cmd/PowerShell); leaving it changed would affect the user's later commands.
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
 * Obtain the command-line arguments as UTF-8.
 * On Windows, main's argv is in the ANSI code page (for example GBK), so a Chinese path would be garbled,
 * so use GetCommandLineW + CommandLineToArgvW to take the original UTF-16 and convert it to UTF-8;
 * on other platforms argv is already UTF-8 (per the system locale; modern systems are UTF-8) and is used as-is.
 */
std::vector<std::string> utf8Arguments(int argc, char** argv)
{
	std::vector<std::string> args;
#if VFS_PLATFORM_WINDOWS
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
	// Fails in rare cases: fall back to the ANSI argv
#endif
	args.reserve(static_cast<size_t>(argc));
	for (int i = 0; i < argc; ++i)
		args.emplace_back(argv[i]);
	return args;
}

// Expand @args.txt: arguments in the file are split on whitespace, and double quotes are supported.
// The argument file is interpreted as UTF-8 and may carry a UTF-8 BOM (Windows Notepad adds one by default)
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

// From the parse order recorded by CLI11, turn paired positive/negative rule options back into a rule list in command-line order
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
#if VFS_PLATFORM_WINDOWS
	// Show output on the console as UTF-8 (the original code page is restored on exit)
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
	// On Windows, CLI11 treats "/x" as a Windows-style option by default, so a path that starts with '/' would be mistaken for an option.
	// Every platform uses only "-x/--xxx" (must be set before adding subcommands; subcommands inherit it)
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
	packCmd->add_option("--block-size", pack.blockSizes,
						"LZ4 block size: <size> sets the default (power of two, 4K..4M, default 64K); "
						"<glob>=<size> overrides matching files (repeatable, last match wins)")
		->allow_extra_args(false);
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

	// If a large allocation inside a command (reading a whole file, decompression) fails, or std::filesystem throws, report an error and an exit code instead of terminating
	try
	{
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
	#if VFS_PLATFORM_WINDOWS
			// Switch stdout to binary mode so "\n" is not converted to "\r\n"
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
	}
	catch (const std::bad_alloc&)
	{
		std::cerr << "vfstool: out of memory\n";
		return static_cast<int>(ExitCode::IO);
	}
	catch (const std::exception& e)
	{
		std::cerr << "vfstool: " << e.what() << "\n";
		return static_cast<int>(ExitCode::IO);
	}
	return static_cast<int>(ExitCode::USAGE);
}
