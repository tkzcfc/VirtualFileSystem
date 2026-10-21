# Third-party dependencies (vendored)

Every third-party library is vendored here at a pinned version as source. The build does not download anything from the network.

| Library | Version | Source | License | Purpose |
|---|---|---|---|---|
| doctest | v2.5.3 | https://github.com/doctest/doctest | MIT | Unit tests (tests only) |
| xxHash | v0.8.4 | https://github.com/Cyan4973/xxHash | BSD-2-Clause | XXH3 hashes (header check, index check, content hash, dedup) |
| LZ4 | v1.10.0 | https://github.com/lz4/lz4 (`lib/lz4.c lz4.h lz4hc.c lz4hc.h`) | BSD-2-Clause | Optional per-file compression |
| CLI11 | v2.7.2 | https://github.com/CLIUtils/CLI11 (release single header `CLI11.hpp`) | BSD-3-Clause | vfstool command-line parsing |

To upgrade: replace the files in the corresponding directory and update this table.
