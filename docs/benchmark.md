# Performance benchmarks (vfs_bench)

`bench/vfs_bench.cpp` is a benchmark with no third-party dependencies. It compares baselines that call the C / C++ / OS file APIs directly with VFS backends and layered combinations, measuring throughput and latency of reads, writes, and metadata operations.

## Build and run

It is not built by default; enable it with `VFS_BUILD_BENCH`. Always use a Release build. Numbers from Debug are not meaningful (the program prints the build type at startup).

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DVFS_BUILD_BENCH=ON -DVFS_BUILD_TESTS=OFF
cmake --build build-bench --target vfs_bench -j
./build-bench/bench/vfs_bench                                  # all scenarios, threads 1,2,4,8,N, warm cache
./build-bench/bench/vfs_bench --filter read.small,archive-lz4  # run only names containing any substring of "scenario/target"
./build-bench/bench/vfs_bench --scale 0.1 --threads 1,4 --iterations 1   # quick smoke
./build-bench/bench/vfs_bench --csv out.csv --md out.md        # also write CSV / Markdown
./build-bench/bench/vfs_bench --cold --threads 1,8 --filter read.   # cold cache (Linux only)
```

On Windows (MSVC) use `cmake --build build-bench --config Release`. The executable is `build-bench/bench/Release/vfs_bench.exe`.

| Option | Description |
|---|---|
| `--scale <x>` | Data-size multiplier, default 1: 10000 small files x 4 KiB (in one directory), 200 medium files x 256 KiB, one 64 MiB file; random-read count and written file count scale the same way |
| `--threads <list>` | Thread-count list. `N` is the hardware thread count. Default `1,2,4,8,N` (duplicates are dropped) |
| `--iterations <n>` | Repetitions per combination; the run with the median elapsed time is kept. Default 3 |
| `--filter <list>` | Comma-separated substrings matched against `scenario/target`; a hit on any one runs it |
| `--csv <file>` / `--md <file>` | Also write a CSV / Markdown table |
| `--dir <path>` | Working directory, default the system temp directory; the dataset is about 0.5 GB (scale=1: 154 MiB of raw files plus three archives) |
| `--keep` | Keep the dataset when finished |
| `--cold` | Before each iteration, drop data files from the page cache with `posix_fadvise(DONTNEED)` (Linux only, no root required); the default is one warmup run then timing (warm cache) |
| `--list` | List scenarios and targets |

### Scenarios

Each thread does the same amount of work (weak scaling). Aggregate throughput = operations across all threads / wall-clock time.

| Scenario | One operation |
|---|---|
| `read.small` | Open + read all 4 KiB + close; each thread reads every small file, with staggered start points |
| `read.medium` | Open + read all 256 KiB in 64 KiB chunks + close |
| `read.large.seq` | Sequentially read the 64 MiB file in 1 MiB chunks, once per thread; latency is per chunk |
| `read.large.rand` | Open the large file once, then read 4 KiB at random offsets (seek + read; the posix baseline uses `pread`); open counts toward total time but not per-operation latency |
| `meta.open` | Open and close a small file, nothing else |
| `meta.stat` | `stat` an existing small file |
| `meta.miss` | `exists` query for a name that is not in a large directory |
| `meta.list` | List a directory of 10000 files, 10 times per thread |
| `write.create` | Each thread creates and writes 64 KiB files in its own directory |
| `write.churn` | Create and write a 4 KiB file, then delete it (one operation = create + delete) |

### Targets

| Target | Description |
|---|---|
| `base:stdio` | `fopen` / `fread` / `fseeko`; **the baseline for ratios**. `meta.stat`/`miss`/`list` use `std::filesystem` (status / exists / directory_iterator) |
| `base:fstream` | `std::ifstream` / `std::ofstream` |
| `base:posix` / `base:win32` | POSIX `open`/`read`/`pread`; on Windows, `CreateFileW`/`ReadFile` (OVERLAPPED offsets) |
| `vfs:native` | `NativeBackend` mounted alone |
| `vfs:memory` | `MemoryBackend`, with all data preloaded |
| `vfs:archive` / `-xor` / `-lz4` | Three archives of the same dataset: plaintext, fully XOR-encrypted, and fully LZ4-compressed (42 MiB, about 3.7:1) |
| `vfs:archive+native` | Archive below + an empty native override directory above (the development "native files override the archive" layout), same mount point |
| `vfs:memory+archive` | Archive below + an empty writable memory layer above |
| `vfs:3layer` | Three layers: archive + native override + memory (WriteTarget) |
| `vfs:nested8` | The data directory is mounted 8 levels deep, with an empty memory mount on every ancestor (lookup crosses 9 layers), plus 32 unrelated mounts |

Write scenarios use the three baselines, `vfs:native`, `vfs:memory`, and `vfs:memory+archive` (writes land in the memory layer; the VFS creates parent directories in the write layer).

### Metrics

- `ops/s`: operations / wall-clock time; `MiB/s`: bytes read or written / wall-clock time (`-` for metadata scenarios).
- `p50` / `p99`: per-operation latency in microseconds after merging all threads.
- `vs stdio`: throughput relative to `base:stdio` for the same scenario and thread count. > 1 means faster than stdio.

### Warm cache and cold cache

- The default is a **warm cache**: one warmup run first, so the data is in the page cache. What is measured is the cost of the API and the VFS itself (system calls, lookup, copies, decryption, decompression). This is closer to a game repeatedly reading the same set of assets.
- `--cold` drops data files from the page cache before each iteration, measuring a first load. Results depend mainly on the storage device and readahead. Directory-entry / inode caches are not cleared (that needs root), so `meta.*` scenarios change little under a cold cache. `--cold` has no effect on macOS / Windows (a reboot, or `purge` on macOS, can stand in).
- VM disks usually have another host cache, so cold-cache numbers are more optimistic than a physical SSD/HDD.

## Results

- Date: 2026-10-21 (UTC+8); command: `vfs_bench --csv <platform>.csv --md <platform>.md` (defaults: --scale 1, threads 1/2/4/8/N, median of 3, warm cache), Release build.
- Throughput is in ops/s, higher is better; the number in parentheses is the multiple of `base:stdio` at the same thread count. > 1 means faster than calling the C file API directly.
- Numbers on the shared VM (Linux) fluctuate by about ±20%. Compare orders of magnitude and trends.
- Charts are generated from `--csv` output by [`bench/bench_charts.py`](../bench/bench_charts.py). Full tables for every scenario and thread count: [Linux](bench/linux.md), [macOS](bench/macos.md), [Windows](bench/windows.md), [iOS](bench/ios.md), [Android](bench/android.md).

### Linux

- CPU: Intel Xeon (KVM virtual machine), 8 vCPU; memory 15 GiB
- OS: Debian 13 (trixie), Linux 6.12.94
- Compiler: GCC 14.2.0, Release
- Disk: virtio block device, ext4 (working directory /tmp)

| Scenario | Threads | base:stdio | native | memory | archive | archive-lz4 | archive+native | 3layer |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| read.small | 1 | 336.6k | 385.8k (1.15) | 1.12M (3.33) | 841.6k (2.50) | 401.1k (1.19) | 654.5k (1.94) | 691.4k (2.05) |
| read.small | 8 | 987.4k | 1.27M (1.28) | 3.96M (4.01) | 3.10M (3.14) | 2.23M (2.26) | 3.99M (4.04) | 3.72M (3.76) |
| read.large.rand | 1 | 951.0k | 1.19M (1.25) | 1.79M (1.89) | 817.8k (0.86) | 70.9k (0.07) | 927.1k (0.97) | 1.30M (1.37) |
| read.large.rand | 8 | 6.11M | 7.63M (1.25) | 7.88M (1.29) | 6.03M (0.99) | 386.6k (0.06) | 7.22M (1.18) | 8.04M (1.32) |
| meta.open | 1 | 696.8k | 715.8k (1.03) | 1.94M (2.79) | 2.92M (4.20) | 476.8k (0.68) | 1.27M (1.83) | 1.11M (1.59) |
| meta.open | 8 | 1.02M | 1.18M (1.16) | 3.65M (3.58) | 3.21M (3.16) | 2.04M (2.01) | 3.89M (3.83) | 3.52M (3.46) |
| meta.stat | 1 | 668.0k | 1.23M (1.84) | 2.21M (3.30) | 3.87M (5.79) | 3.86M (5.77) | 1.41M (2.11) | 1.33M (2.00) |
| meta.stat | 8 | 5.00M | 6.11M (1.22) | 6.38M (1.28) | 20.31M (4.06) | 15.60M (3.12) | 9.51M (1.90) | 7.31M (1.46) |
| meta.miss | 1 | 1.40M | 1.32M (0.95) | 2.21M (1.58) | 3.00M (2.15) | 3.04M (2.18) | 1.38M (0.99) | 1.14M (0.82) |
| meta.miss | 8 | 6.06M | 5.03M (0.83) | 5.40M (0.89) | 11.30M (1.87) | 9.95M (1.64) | 4.80M (0.79) | 7.19M (1.19) |
| write.churn | 1 | 98.3k | 76.0k (0.77) | 525.6k (5.35) | - | - | - | - |
| write.churn | 8 | 298.5k | 299.7k (1.00) | 729.5k (2.44) | - | - | - | - |

![Linux: speed relative to stdio](images/bench/linux.svg)

### macOS

- CPU: Apple M4, 10 cores; memory 32 GiB
- OS: macOS 26.5.1
- Compiler: Apple clang 17.0.0, Release
- Disk: built-in SSD, APFS

| Scenario | Threads | base:stdio | native | memory | archive | archive-lz4 | archive+native | 3layer |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| read.small | 1 | 93.6k | 94.6k (1.01) | 2.99M (31.97) | 1.72M (18.43) | 666.8k (7.12) | 653.2k (6.98) | 640.9k (6.85) |
| read.small | 8 | 125.1k | 127.8k (1.02) | 1.24M (9.92) | 1.38M (11.05) | 1.37M (10.96) | 897.8k (7.18) | 776.7k (6.21) |
| read.large.rand | 1 | 840.6k | 1.53M (1.83) | 4.31M (5.13) | 1.91M (2.27) | 293.3k (0.35) | 2.01M (2.39) | 2.29M (2.73) |
| read.large.rand | 8 | 409.4k | 626.9k (1.53) | 1.88M (4.59) | 1.37M (3.35) | 1.17M (2.85) | 1.35M (3.31) | 1.35M (3.30) |
| meta.open | 1 | 103.1k | 98.4k (0.95) | 3.64M (35.26) | 4.81M (46.67) | 681.3k (6.61) | 867.6k (8.42) | 834.5k (8.10) |
| meta.open | 8 | 126.7k | 128.4k (1.01) | 1.40M (11.03) | 3.09M (24.43) | 1.40M (11.04) | 1.16M (9.19) | 913.5k (7.21) |
| meta.stat | 1 | 433.6k | 733.3k (1.69) | 4.13M (9.52) | 5.86M (13.52) | 5.98M (13.79) | 925.4k (2.13) | 849.7k (1.96) |
| meta.stat | 8 | 614.0k | 1.21M (1.97) | 1.66M (2.71) | 16.18M (26.35) | 16.65M (27.12) | 1.20M (1.95) | 970.4k (1.58) |
| meta.miss | 1 | 1.05M | 865.7k (0.82) | 3.20M (3.04) | 3.61M (3.44) | 3.45M (3.29) | 819.1k (0.78) | 790.8k (0.75) |
| meta.miss | 8 | 1.18M | 1.17M (0.99) | 1.55M (1.31) | 12.95M (10.95) | 11.20M (9.47) | 1.17M (0.99) | 977.0k (0.83) |
| write.churn | 1 | 12.8k | 12.5k (0.97) | 911.9k (71.04) | - | - | - | - |
| write.churn | 8 | 17.3k | 17.1k (0.99) | 288.3k (16.69) | - | - | - | - |

![macOS: speed relative to stdio](images/bench/macos.svg)

### Windows

- CPU: Intel Core i5-12400, 6 cores 12 threads; memory 32 GiB
- OS: Windows 11 Enterprise 10.0.26100
- Compiler: MSVC 19.51 (VS2026), Release
- Disk: Samsung SSD 980 1TB (NVMe), NTFS; Windows Defender real-time protection on

| Scenario | Threads | base:stdio | native | memory | archive | archive-lz4 | archive+native | 3layer |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| read.small | 1 | 40.7k | 40.5k (0.99) | 1.13M (27.79) | 306.2k (7.53) | 236.2k (5.81) | 60.2k (1.48) | 58.1k (1.43) |
| read.small | 8 | 171.6k | 185.1k (1.08) | 3.74M (21.80) | 875.1k (5.10) | 655.0k (3.82) | 256.6k (1.50) | 256.5k (1.50) |
| read.large.rand | 1 | 253.6k | 250.0k (0.99) | 3.03M (11.93) | 224.1k (0.88) | 106.7k (0.42) | 214.2k (0.84) | 195.9k (0.77) |
| read.large.rand | 8 | 303.2k | 308.0k (1.02) | 6.65M (21.94) | 301.8k (1.00) | 352.1k (1.16) | 280.0k (0.92) | 297.5k (0.98) |
| meta.open | 1 | 70.3k | 74.0k (1.05) | 1.17M (16.64) | 2.24M (31.91) | 243.4k (3.46) | 77.6k (1.10) | 75.9k (1.08) |
| meta.open | 8 | 298.0k | 291.0k (0.98) | 4.53M (15.20) | 5.68M (19.07) | 583.7k (1.96) | 324.4k (1.09) | 318.4k (1.07) |
| meta.stat | 1 | 42.3k | 81.2k (1.92) | 1.91M (45.05) | 2.85M (67.22) | 2.94M (69.53) | 81.6k (1.93) | 81.1k (1.92) |
| meta.stat | 8 | 185.0k | 348.3k (1.88) | 4.59M (24.82) | 11.23M (60.71) | 9.56M (51.68) | 313.4k (1.69) | 303.9k (1.64) |
| meta.miss | 1 | 108.3k | 104.9k (0.97) | 1.68M (15.51) | 2.20M (20.35) | 2.14M (19.72) | 83.0k (0.77) | 80.4k (0.74) |
| meta.miss | 8 | 512.4k | 489.1k (0.95) | 4.96M (9.69) | 9.19M (17.94) | 9.34M (18.22) | 329.0k (0.64) | 324.4k (0.63) |
| write.churn | 1 | 1.9k | 1.4k (0.70) | 439.6k (226.52) | - | - | - | - |
| write.churn | 8 | 6.5k | 5.7k (0.86) | 676.3k (103.30) | - | - | - | - |

![Windows: speed relative to stdio](images/bench/windows.svg)

### iOS

- Device: iPhone 14 Pro (Apple A16, 6 cores); memory 6 GiB
- OS: iOS 26.5
- Compiler: Apple clang 17.0.0, Release
- Disk: built-in flash, APFS (app sandbox)

| Scenario | Threads | base:stdio | native | memory | archive | archive-lz4 | archive+native | 3layer |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| read.small | 1 | 8.0k | 8.1k (1.01) | 2.00M (249.46) | 1.33M (165.32) | 530.0k (66.10) | 71.0k (8.85) | 70.7k (8.82) |
| read.small | 8 | 56.5k | 55.9k (0.99) | 894.8k (15.85) | 1.14M (20.17) | 887.5k (15.72) | 167.6k (2.97) | 157.9k (2.80) |
| read.large.rand | 1 | 359.7k | 760.9k (2.12) | 1.91M (5.31) | 928.3k (2.58) | 128.2k (0.36) | 885.1k (2.46) | 1.16M (3.22) |
| read.large.rand | 8 | 381.6k | 588.5k (1.54) | 1.12M (2.93) | 1.12M (2.93) | 530.2k (1.39) | 1.08M (2.83) | 1.06M (2.79) |
| meta.open | 1 | 27.2k | 27.3k (1.00) | 2.56M (94.20) | 3.61M (132.67) | 517.3k (19.00) | 66.1k (2.43) | 65.8k (2.42) |
| meta.open | 8 | 66.9k | 64.2k (0.96) | 694.8k (10.39) | 2.08M (31.05) | 800.6k (11.97) | 160.8k (2.40) | 142.0k (2.12) |
| meta.stat | 1 | 17.0k | 29.2k (1.72) | 3.20M (188.61) | 4.61M (271.22) | 4.71M (277.44) | 68.5k (4.04) | 68.7k (4.05) |
| meta.stat | 8 | 40.3k | 73.9k (1.83) | 752.9k (18.67) | 6.97M (172.94) | 6.99M (173.34) | 158.5k (3.93) | 144.9k (3.59) |
| meta.miss | 1 | 39.2k | 57.1k (1.46) | 2.33M (59.58) | 2.70M (68.82) | 2.70M (68.93) | 68.5k (1.75) | 68.8k (1.76) |
| meta.miss | 8 | 135.4k | 121.5k (0.90) | 735.4k (5.43) | 5.08M (37.56) | 5.53M (40.83) | 152.7k (1.13) | 145.8k (1.08) |
| write.churn | 1 | 1.5k | 1.7k (1.10) | 701.8k (454.15) | - | - | - | - |
| write.churn | 8 | 2.7k | 2.8k (1.02) | 181.5k (66.40) | - | - | - | - |

![iOS: speed relative to stdio](images/bench/ios.svg)

### Android

- Device: Xiaomi Redmi K80 (24117RK2CC), Qualcomm Snapdragon 8 Gen 3 (SM8650, 8 cores: 1x 3.3 GHz + 3x 3.15 GHz + 2x 2.96 GHz + 2x 2.27 GHz); memory 16 GB
- OS: Android 16 (API 36), HyperOS OS3.0, Linux 6.1
- Compiler: NDK r27c clang 18.0.3, arm64-v8a, Release
- Disk: built-in UFS flash, f2fs (working directory /data/local/tmp, run from `adb shell`)
- Note: a command-line process started from `adb shell` gets no foreground-app CPU boost, so the scheduler often keeps it on slower cores at lower clocks. In-memory paths (memory, archive index) are therefore far below what the same SoC reaches in a foreground app, and multi-thread results vary more than on desktop systems

| Scenario | Threads | base:stdio | native | memory | archive | archive-lz4 | archive+native | 3layer |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| read.small | 1 | 89.2k | 120.2k (1.35) | 195.2k (2.19) | 139.0k (1.56) | 125.8k (1.41) | 112.5k (1.26) | 107.1k (1.20) |
| read.small | 8 | 648.2k | 573.9k (0.89) | 767.7k (1.18) | 824.4k (1.27) | 541.1k (0.83) | 573.7k (0.89) | 486.7k (0.75) |
| read.large.rand | 1 | 105.7k | 110.0k (1.04) | 652.0k (6.17) | 157.3k (1.49) | 35.5k (0.34) | 138.7k (1.31) | 249.8k (2.36) |
| read.large.rand | 8 | 1.39M | 1.34M (0.97) | 1.82M (1.31) | 1.83M (1.32) | 180.8k (0.13) | 1.29M (0.93) | 1.53M (1.10) |
| meta.open | 1 | 133.9k | 91.9k (0.69) | 244.7k (1.83) | 221.2k (1.65) | 132.7k (0.99) | 143.4k (1.07) | 138.0k (1.03) |
| meta.open | 8 | 1.14M | 668.4k (0.58) | 782.5k (0.68) | 821.8k (0.72) | 543.7k (0.48) | 564.8k (0.49) | 478.5k (0.42) |
| meta.stat | 1 | 97.1k | 130.1k (1.34) | 235.7k (2.43) | 380.2k (3.91) | 387.8k (3.99) | 151.2k (1.56) | 152.9k (1.57) |
| meta.stat | 8 | 1.02M | 870.1k (0.85) | 1.24M (1.21) | 1.35M (1.32) | 1.25M (1.22) | 748.9k (0.73) | 608.7k (0.60) |
| meta.miss | 1 | 238.8k | 173.5k (0.73) | 448.0k (1.88) | 416.1k (1.74) | 273.7k (1.15) | 130.5k (0.55) | 128.7k (0.54) |
| meta.miss | 8 | 1.69M | 562.1k (0.33) | 682.7k (0.40) | 719.7k (0.43) | 724.6k (0.43) | 506.9k (0.30) | 452.0k (0.27) |
| write.churn | 1 | 12.3k | 15.3k (1.24) | 56.1k (4.55) | - | - | - | - |
| write.churn | 8 | 92.6k | 70.4k (0.76) | 268.7k (2.90) | - | - | - | - |

![Android: speed relative to stdio](images/bench/android.svg)

## Choosing backends and combinations

| Use | Recommendation | Basis (tables above) |
|---|---|---|
| Shipping read-only assets | Plaintext or XOR archive (`vfs:archive`) | Open, `stat`, and miss queries are served from the in-memory index. Versus stdio: small-file reads are 2.5-3.1x on Linux, 11-18x on macOS, 5-7.5x on Windows; `meta.open` is 3.2-4.2x, 24-47x, and 19-32x respectively; 8 threads scale the same way |
| Compression needed | Compress only small text such as scripts and config (`--compress "*.lua"`) | An LZ4 entry is decompressed as a whole on open: small files are barely affected, sequential reads of a 64 MiB file are only 0.06-0.3x stdio, and random reads must decompress the whole file first |
| Native override of the archive during development | `archive + native`, mounted only in development builds | Every lookup checks the native directory first: miss queries (`meta.miss`) are about 0.64-0.99x; files that hit the archive are still faster than stdio (`read.small` 1.5-7x). Do not mount an empty native directory in a shipping build |
| Runtime-generated / temporary data | Memory backend (`vfs:memory`), optionally stacked on an archive as a writable layer | No system calls: reads, `stat`, writes, and deletes are mostly several times to hundreds of times faster (`write.churn` 2.4-226x); an 8-thread miss query on Linux is about 0.9x |
| Data that must hit disk, such as saves and logs | Native directory (`vfs:native`), its own mount point | A single layer is roughly even with a direct call (reads and opens 0.95-1.3x, `stat` 1.2-1.9x); choosing the write layer and checking the parent before a write makes create + delete about 0.7-1.0x |
| Stacking layers | As few layers as possible; do not stack needless empty layers on the same path | Lookup walks layers top-down. `nested8` (9 layers) metadata operations are about 0.4-1.0x |

Other:
- Directory listing (`meta.list`): archive and memory backends are about 1.7-12.7x; the native backend must merge, deduplicate, sort, and convert names, about 0.62-1.04x.
- Threads: reads and metadata queries are concurrent on every backend, and mount-table lookup takes no global lock; each archive thread reads by offset on its own handle.

## Regenerating charts

```bash
./build-bench/bench/vfs_bench --csv linux.csv --md linux.md
python3 bench/bench_charts.py --platform linux --input linux.csv --out docs/images/bench --label "CPU, OS, compiler"   # writes docs/images/bench/linux.svg
```

`--input` accepts `--csv` output or the Markdown table produced by `--md`; only the Python standard library is used.
