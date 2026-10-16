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
#endif

USING_NS_VFS;
using namespace vfs::tool;

namespace
{

// 展开 @args.txt：文件中的参数按空白分隔，支持双引号（vfs-format F21）
bool expandResponseFiles(int argc, char** argv, std::vector<std::string>& out)
{
	for (int i = 0; i < argc; ++i)
	{
		std::string arg = argv[i];
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
	// 中文输出与 cat 的二进制输出
	SetConsoleOutputCP(CP_UTF8);
#endif

	std::vector<std::string> args;
	if (!expandResponseFiles(argc, argv, args))
		return static_cast<int>(ExitCode::USAGE);
	std::vector<char*> argvExpanded;
	for (auto& a : args)
		argvExpanded.push_back(a.data());

	CLI::App app{"vfstool：.vfs 归档工具（打包、解包、查看、校验）"};
	app.require_subcommand(1);
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
