# 与设计文档的出入

按“需要偏离时选更简单/更快的做法并记录”的要求，实现与 `design/` 中文档不一致之处如下。

## 格式（相对 `design/vfs-format-draft.md`，均遵循 `final-decisions.md`）

| # | 草案 | 实现 | 原因 |
|---|---|---|---|
| 1 | Header 256 B，major/minor 版本、特性位 | Header 128 B，单一版本号 | final-decisions #1 |
| 2 | Header 认证、Ed25519 签名、Monocypher | 只有 header/索引 XXH3-64 校验 | final-decisions #2 |
| 3 | Footer 32 B | Footer 16 B（magic + archiveSize） | 只需要定位起点 |
| 4 | 预留 `ENTRY_REMOVAL`（tombstone） | 未保留 | 版本号不兼容即拒绝，将来需要时升版本即可 |
| 5 | KDF / AEAD | 新增 `keyCheck`（明文索引 XXH3-64 低 32 位），区分“密钥错误”与“损坏” | 无 KDF 时最简单的判错方式 |
| 6 | 条目哈希 XXH3-64 | 存 XXH3-128 的低 64 位 | 同一次 128 位计算同时用于去重和 key 派生 |
| 7 | 去重时逐字节比较 | 只比较 XXH3-128 + 长度 + 存储方式 | 更快；128 位碰撞概率可忽略 |
| 8 | 条目数据范围不可重叠 | 允许重叠 | 去重需要多个条目共享同一段数据 |
| 9 | 空文件也可加密 | 空文件不加密（没有数据） | 无意义 |
| 10 | mtime=0 表示未知 | 运行时 stat 回退为归档文件自身的 mtime | 让上层缓存失效逻辑更好用 |

## 运行时库

| # | 设计 | 实现 | 原因 |
|---|---|---|---|
| 11 | 挂载表 `std::atomic<std::shared_ptr>` | mutex 只保护 `shared_ptr` 的读取与交换（决策 #17 的备选） | libc++ 尚不支持 `atomic<shared_ptr>`，保证可移植 |
| 12 | `ArchiveBackend::open(path)` 工厂 | 改名 `ArchiveBackend::create` | 与虚函数 `IBackend::open(rel, Mode, Error*)` 重载时 `{}` 会产生歧义 |
| 13 | 日志级别 `ERROR/DEBUG` | `VERBOSE/INFO/WARN/FAIL` | 避开 Windows 头文件的 `ERROR` 宏 |
| 14 | — | 归档流 `write()` 返回 -1，errno = `EROFS` | 最贴近 POSIX 语义 |
| 15 | — | 不带前导 `/` 的虚拟路径按根相对处理（`a/b` = `/a/b`） | 宽松输入，与 axmol 的相对路径习惯一致 |
| 16 | — | 非法 alignment 返回 `UNSUPPORTED`；目录跨后端 `moveFile` 返回 `CROSS_DEVICE` | 现有错误码中最贴切的 |
| 17 | 运行时可选校验单文件哈希 | 不提供 | final-decisions #3：只在 verify 时离线检查 |

## 工具与工程

| # | 设计 | 实现 | 原因 |
|---|---|---|---|
| 18 | 旧示例 `examples/test`、`test_gui`（glfw/imgui/glad/glm/md5） | 删除，换成无依赖的 `examples/basic`（默认不构建） | 旧示例依赖旧 pack 格式；第三方依赖限定为 4 个 |
| 19 | libFuzzer 模糊测试目标 | 用确定性随机变异的 doctest 用例代替 | 构建环境只有 GCC（无 libFuzzer）；用例同样覆盖“不崩溃、不越界”，并在 ASan/UBSan 下跑过 |
| 20 | glob | 不含 `/` 的模式匹配文件名；出现 `--include` 后未匹配的文件默认排除 | 与 .gitignore 的习惯一致 |
| 21 | — | 新增 `vfstool keygen` | 方便生成外部密钥 |
| 22 | CLI `--base-offset` | 未提供，读取时自动定位（magic 或 footer）；API 仍支持 `ArchiveOptions::baseOffset` | 自动定位已覆盖常见场景 |
| 23 | — | 未添加 LICENSE | 原仓库没有许可证，由作者决定 |
