# vfstool command-line reference

`vfstool` builds and inspects `.vfs` archives. Every subcommand accepts an `@args.txt` argument file (split on whitespace, double quotes supported, interpreted as UTF-8, a BOM is allowed). On Windows, arguments are taken as the original UTF-16 from `GetCommandLineW` and converted to UTF-8, so the ANSI code page does not affect them. Only `-x`/`--xxx` options are accepted (`/x` style is not), so a path that starts with `/` is not mistaken for an option.

## Exit codes

| Code | Meaning |
|---:|---|
| 0 | Success |
| 1 | Usage error (bad arguments) |
| 2 | IO error (file missing, already exists, or a read/write failed) |
| 3 | Format error (not an archive, corrupt, or version mismatch) |
| 4 | Integrity check failed (verify found a content-hash mismatch) |
| 5 | Missing key or wrong key |

## Key arguments (shared by info/list/cat/extract/unpack/verify/pack)

| Argument | Description |
|---|---|
| `--key-file <file>` | 32 raw bytes, or 64 hex characters (a trailing newline is allowed). Recommended |
| `--key-hex <hex>` | 64 hex characters. Prints a warning: the command line is visible to shell history and `ps` |
| Environment variable `VFS_KEY` | 64 hex characters, used when neither of the above is given |

Read commands locate the archive start automatically: the magic at offset 0, or the footer at the end of the file (when the archive is appended after an exe).

## pack

```
vfstool pack <dir> -o <out.vfs> [options]
```

| Option | Description |
|---|---|
| `-o, --output` | Output file, required (may be omitted with `--dry-run`); write `<out>.tmp` first, then rename |
| `--prefix <p>` | Path prefix inside the archive, for example `--prefix res` |
| `--include <glob>` / `--exclude <glob>` | Filter rules, repeatable; **the last match in command-line order wins**. As soon as any `--include` appears, unmatched files are excluded by default |
| `--encrypt <glob>` / `--no-encrypt <glob>` | XOR encryption rules, same as above |
| `--compress <glob>` / `--no-compress <glob>` | LZ4 compression rules, same as above |
| `--encrypt-all` / `--compress-all` | Encrypt/compress everything by default (later `--no-*` rules still apply) |
| `--align <n>` | Data alignment, a power of two, 1..65536, default 16 (1 = no alignment) |
| `--key-file` / `--key-hex` | External index key; by default it is not stored in the file and must be supplied when reading |
| `--store-key` | Also store the external key in the file |
| `--no-index-encrypt` | Do not encrypt the index (file names are visible in the clear) |
| `--xor-key-len <n>` | Per-file XOR key length, 16..32, default 32 |
| `--lz4-level <n>` | LZ4 HC level 1..12, default 9 |
| `--block-size <size>` / `--block-size <glob>=<size>` | LZ4 block size: a power of two from 4K to 4M (`4096`, `64K`, `1M`, ...). Without a glob it sets the default (64K); with a glob it overrides matching files. Repeatable; the last matching rule wins |
| `--keep-mtime` | Write the real mtime (default 0, so the archive is reproducible) |
| `--empty-dirs` | Keep empty directories (omitted by default) |
| `--no-dedup` | Disable content deduplication |
| `--no-default-excludes` | Do not exclude `.git/ .svn/ .DS_Store Thumbs.db desktop.ini` |
| `--dry-run` | Only list the files that would be packed and their encrypt/compress flags |
| `-v, --verbose` | Print each file |

**Glob syntax**: `*` matches any characters except `/`, `?` matches one character, `[abc]`, `[a-z]`, and `[!x]` are character classes, `**` crosses directories (`a/**/b` can match `a/b`), and `\` escapes. A pattern with no `/` matches the file name (`*.lua` matches lua in any directory); a pattern that starts with `/` is anchored at the root.

## unpack / extract

```
vfstool unpack  <archive> -o <dir> [--include/--exclude <glob>] [--force] [--no-verify] [-v]
vfstool extract <archive> <path>... -o <dir> [--force]
```

unpack checks each file's content hash by default; if the destination file already exists, it errors without `--force` (exit code 2). extract accepts files or directories.

## info / list / cat / verify / keygen

```
vfstool info   <archive> [--json]          # Version, entry count, size, alignment, flags, encrypt/compress/dedup statistics
vfstool list   <archive> [glob] [-l] [--json]   # -l shows size, stored size, flags (D directory / E encrypted / C compressed), LZ4 block size, and the hash
vfstool cat    <archive> <path>             # Raw bytes to stdout (switched to binary mode on Windows)
vfstool verify <archive> [-v]               # Header, index, and each file's content hash
vfstool keygen [-o key.txt]                 # Generate a 32-byte random key (64 hex characters)
```

## Examples

```bash
# Compress scripts and config, encrypt binary assets, exclude temporary files
vfstool pack assets -o game.vfs --compress "*.lua" --compress "*.json" --encrypt "**/*.png" --exclude "*.tmp"

# Compress everything with 64K blocks, but use 1M blocks for large sequentially-read audio
vfstool pack assets -o game.vfs --compress-all --block-size "*.ogg=1M"

# External key (not stored in the file)
vfstool keygen -o game.key
vfstool pack assets -o game.vfs --key-file game.key --encrypt-all
vfstool list game.vfs -l --key-file game.key

# Check that two packs match
vfstool pack assets -o a.vfs && vfstool pack assets -o b.vfs && sha256sum a.vfs b.vfs
```
