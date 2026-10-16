# 设计文档存档

本目录保存重写前确认的设计文档，作为实现依据与历史记录：

| 文件 | 内容 |
|---|---|
| `controversies.md` | 35 条设计争议与决策（挂载语义、写入规则、线程安全、IFileStream 适配等） |
| `vfs-format-draft.md` | 代码风格、目录结构、`.vfs` 格式**草案**与 CLI 设计 |
| `final-decisions.md` | 最终简化决策，**与草案冲突时以此为准** |
| `axmol-IFileStream.h` | axmol `IFileStream` 原文，`src/vfs/IFileStream.h` 与之逐字一致 |

实际实现的格式见 [`../format.md`](../format.md)，与设计的出入见 [`../deviations.md`](../deviations.md)。
