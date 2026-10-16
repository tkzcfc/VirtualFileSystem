# 第三方依赖（vendored）

所有第三方库都以源码形式固定版本放在这里，构建时不联网下载。

| 库 | 版本 | 来源 | 许可证 | 用途 |
|---|---|---|---|---|
| doctest | v2.5.3 | https://github.com/doctest/doctest | MIT | 单元测试（只给 tests 用） |
| xxHash | v0.8.4 | https://github.com/Cyan4973/xxHash | BSD-2-Clause | XXH3 哈希（header 校验、索引校验、内容哈希、去重） |
| LZ4 | v1.10.0 | https://github.com/lz4/lz4 （`lib/lz4.c lz4.h lz4hc.c lz4hc.h`） | BSD-2-Clause | 单文件可选压缩 |
| CLI11 | v2.7.2 | https://github.com/CLIUtils/CLI11 （release 单头文件 `CLI11.hpp`） | BSD-3-Clause | vfstool 命令行解析 |

升级方法：替换对应目录中的文件，并更新本表。
