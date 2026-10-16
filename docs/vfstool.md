# vfstool 命令行参考

`vfstool` 用于生成和检查 `.vfs` 归档。所有子命令都支持 `@args.txt` 参数文件（按空白分隔，支持双引号）。

## 退出码

| 码 | 含义 |
|---:|---|
| 0 | 成功 |
| 1 | 用法错误（参数不对） |
| 2 | IO 错误（文件不存在、已存在、读写失败） |
| 3 | 格式错误（不是归档、损坏、版本不匹配） |
| 4 | 完整性校验失败（verify 发现内容哈希不匹配） |
| 5 | 缺少密钥或密钥错误 |

## 密钥参数（info/list/cat/extract/unpack/verify/pack 通用）

| 参数 | 说明 |
|---|---|
| `--key-file <file>` | 32 字节原始数据，或 64 个 hex 字符（允许末尾换行）。推荐 |
| `--key-hex <hex>` | 64 个 hex 字符。会打印警告：命令行会被 shell history 和 `ps` 看到 |
| 环境变量 `VFS_KEY` | 64 个 hex 字符，前两者都没给时使用 |

读取类命令会自动定位归档起点：偏移 0 处的 magic，或文件末尾的 footer（归档附加在 exe 后面时）。

## pack

```
vfstool pack <dir> -o <out.vfs> [选项]
```

| 选项 | 说明 |
|---|---|
| `-o, --output` | 输出文件，必填（`--dry-run` 时可省略）；先写 `<out>.tmp` 再 rename |
| `--prefix <p>` | 归档内路径前缀，例如 `--prefix res` |
| `--include <glob>` / `--exclude <glob>` | 过滤规则，可重复，**按命令行顺序最后匹配者生效**；只要出现 `--include`，未匹配的文件默认排除 |
| `--encrypt <glob>` / `--no-encrypt <glob>` | XOR 加密规则，同上 |
| `--compress <glob>` / `--no-compress <glob>` | LZ4 压缩规则，同上 |
| `--encrypt-all` / `--compress-all` | 默认全部加密/压缩（之后的 `--no-*` 规则仍生效） |
| `--align <n>` | 数据对齐，2 的幂，1..65536，默认 16（1 = 不对齐） |
| `--key-file` / `--key-hex` | 外部索引密钥；默认不存入文件，读取时必须提供 |
| `--store-key` | 把外部密钥也存进文件 |
| `--no-index-encrypt` | 索引不加密（文件名明文可见） |
| `--xor-key-len <n>` | 每文件 XOR key 长度，16..32，默认 32 |
| `--lz4-level <n>` | LZ4 HC 级别 1..12，默认 9 |
| `--keep-mtime` | 写入真实 mtime（默认 0，便于复现） |
| `--empty-dirs` | 保留空目录（默认不保留） |
| `--no-dedup` | 关闭内容去重 |
| `--no-default-excludes` | 不排除 `.git/ .svn/ .DS_Store Thumbs.db desktop.ini` |
| `--dry-run` | 只列出将要打包的文件及其加密/压缩标志 |
| `-v, --verbose` | 输出每个文件 |

**glob 语法**：`*` 匹配除 `/` 外任意字符，`?` 匹配单个字符，`[abc]`、`[a-z]`、`[!x]` 字符集，`**` 跨目录（`a/**/b` 可匹配 `a/b`），`\` 转义。不含 `/` 的模式匹配文件名（`*.lua` 匹配任意目录下的 lua）；以 `/` 开头的模式锚定到根。

## unpack / extract

```
vfstool unpack  <archive> -o <dir> [--include/--exclude <glob>] [--force] [--no-verify] [-v]
vfstool extract <archive> <path>... -o <dir> [--force]
```

unpack 默认校验每个文件的内容哈希；目标文件已存在时不加 `--force` 报错（退出码 2）。extract 可以给文件或目录。

## info / list / cat / verify / keygen

```
vfstool info   <archive> [--json]          # 版本、条目数、大小、对齐、flags、加密/压缩/去重统计
vfstool list   <archive> [glob] [-l] [--json]   # -l 显示大小、存储大小、标志（D 目录 / E 加密 / C 压缩）、哈希
vfstool cat    <archive> <path>             # 原始字节输出到 stdout（Windows 下切换为二进制模式）
vfstool verify <archive> [-v]               # header、索引、每个文件的内容哈希
vfstool keygen [-o key.txt]                 # 生成 32 字节随机密钥（64 个 hex 字符）
```

## 示例

```bash
# 脚本和配置压缩，二进制资源加密，排除临时文件
vfstool pack assets -o game.vfs --compress "*.lua" --compress "*.json" --encrypt "**/*.png" --exclude "*.tmp"

# 外部密钥（不存入文件）
vfstool keygen -o game.key
vfstool pack assets -o game.vfs --key-file game.key --encrypt-all
vfstool list game.vfs -l --key-file game.key

# 检查两次打包是否一致
vfstool pack assets -o a.vfs && vfstool pack assets -o b.vfs && sha256sum a.vfs b.vfs
```
