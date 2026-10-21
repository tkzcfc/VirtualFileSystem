# vfs_bench full results - WebAssembly

- Host: the Linux box (Intel Xeon KVM virtual machine, 8 vCPU; Debian 13, Linux 6.12.94); the same machine as the Linux results
- Runtime: node v24.21.0 (V8), NODERAWFS (the native backend reaches the host filesystem through node's fs), working directory /tmp
- Compiler: Emscripten 6.0.13 (clang 24), Release (-O3), pthread build (`VFS_WASM_THREADS=ON`, `PROXY_TO_PTHREAD`), 3 GiB fixed heap
- How it was run: `node build-wasm/bench/vfs_bench.js --dir /tmp --csv wasm.csv --md wasm.md`. The 8-thread `write.large.seq` case first ran out of the then 1.5 GiB heap; after raising the heap to 3 GiB the three handle-based write scenarios (`write.large.seq`, `write.overwrite`, `write.append`) were rerun with `--filter`, all other scenarios come from the full run
- Note: every `base:*` and `vfs:native` call goes through Emscripten's syscall layer into node's synchronous fs API, so system calls are far more expensive than on native Linux, while in-memory paths (memory backend, archive index) run as compiled code. That makes the ratios against stdio much larger than on native Linux for metadata on 1 thread; with 8 threads, the proxied calls serialize
- Date: 2026-10-21 (UTC+8), default parameters (--scale 1, --iterations 3, threads 1,2,4,8,N), warm cache, Release

### read.small

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 14.6k | 57 | 64.0 | 95.7 | 1.00 |
| base:fstream | 1 | 12.7k | 50 | 75.5 | 117.8 | 0.87 |
| base:posix | 1 | 15.5k | 60 | 59.6 | 93.7 | 1.06 |
| vfs:native | 1 | 11.1k | 43 | 79.9 | 166.9 | 0.76 |
| vfs:memory | 1 | 803.0k | 3137 | 1.02 | 1.79 | 54.85 |
| vfs:archive | 1 | 56.6k | 221 | 17.2 | 22.3 | 3.87 |
| vfs:archive-xor | 1 | 52.6k | 205 | 17.9 | 24.6 | 3.59 |
| vfs:archive-lz4 | 1 | 29.7k | 116 | 32.8 | 42.0 | 2.03 |
| vfs:archive+native | 1 | 24.8k | 97 | 38.1 | 57.9 | 1.69 |
| vfs:memory+archive | 1 | 56.3k | 220 | 17.2 | 22.5 | 3.85 |
| vfs:3layer | 1 | 24.4k | 95 | 39.2 | 59.9 | 1.67 |
| vfs:nested8 | 1 | 9.7k | 38 | 98.8 | 159.2 | 0.66 |
| base:stdio | 2 | 32.5k | 127 | 59.1 | 91.1 | 1.00 |
| base:fstream | 2 | 24.0k | 94 | 80.1 | 125.4 | 0.74 |
| base:posix | 2 | 33.1k | 129 | 58.1 | 84.5 | 1.02 |
| vfs:native | 2 | 25.0k | 98 | 76.5 | 138.8 | 0.77 |
| vfs:memory | 2 | 282.4k | 1103 | 6.14 | 19.2 | 8.70 |
| vfs:archive | 2 | 101.3k | 396 | 17.7 | 29.2 | 3.12 |
| vfs:archive-xor | 2 | 95.2k | 372 | 20.0 | 31.2 | 2.93 |
| vfs:archive-lz4 | 2 | 50.7k | 198 | 37.4 | 52.2 | 1.56 |
| vfs:archive+native | 2 | 58.2k | 227 | 31.7 | 57.1 | 1.79 |
| vfs:memory+archive | 2 | 102.0k | 398 | 18.9 | 26.9 | 3.14 |
| vfs:3layer | 2 | 55.0k | 215 | 35.1 | 57.1 | 1.69 |
| vfs:nested8 | 2 | 21.2k | 83 | 90.4 | 185.1 | 0.65 |
| base:stdio | 4 | 55.5k | 217 | 68.4 | 157.2 | 1.00 |
| base:fstream | 4 | 42.8k | 167 | 88.3 | 163.6 | 0.77 |
| base:posix | 4 | 56.1k | 219 | 67.6 | 107.3 | 1.01 |
| vfs:native | 4 | 44.7k | 175 | 85.0 | 177.9 | 0.81 |
| vfs:memory | 4 | 169.8k | 663 | 21.2 | 58.6 | 3.06 |
| vfs:archive | 4 | 164.4k | 642 | 21.5 | 56.1 | 2.96 |
| vfs:archive-xor | 4 | 189.9k | 742 | 17.7 | 51.5 | 3.42 |
| vfs:archive-lz4 | 4 | 80.5k | 314 | 47.1 | 89.1 | 1.45 |
| vfs:archive+native | 4 | 88.5k | 346 | 39.7 | 82.2 | 1.59 |
| vfs:memory+archive | 4 | 162.8k | 636 | 21.8 | 57.9 | 2.94 |
| vfs:3layer | 4 | 91.1k | 356 | 41.5 | 74.8 | 1.64 |
| vfs:nested8 | 4 | 35.1k | 137 | 107.8 | 240.1 | 0.63 |
| base:stdio | 8 | 62.0k | 242 | 124.2 | 217.6 | 1.00 |
| base:fstream | 8 | 48.9k | 191 | 157.4 | 276.2 | 0.79 |
| base:posix | 8 | 64.1k | 250 | 118.0 | 239.6 | 1.03 |
| vfs:native | 8 | 49.5k | 193 | 153.3 | 253.7 | 0.80 |
| vfs:memory | 8 | 139.4k | 545 | 46.3 | 186.1 | 2.25 |
| vfs:archive | 8 | 141.6k | 553 | 49.4 | 141.3 | 2.29 |
| vfs:archive-xor | 8 | 131.5k | 514 | 54.5 | 147.7 | 2.12 |
| vfs:archive-lz4 | 8 | 77.5k | 303 | 95.7 | 199.7 | 1.25 |
| vfs:archive+native | 8 | 86.9k | 339 | 88.8 | 145.4 | 1.40 |
| vfs:memory+archive | 8 | 120.3k | 470 | 59.1 | 167.4 | 1.94 |
| vfs:3layer | 8 | 76.3k | 298 | 94.0 | 537.6 | 1.23 |
| vfs:nested8 | 8 | 35.6k | 139 | 212.2 | 376.6 | 0.57 |

### read.medium

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 6.5k | 1637 | 148.2 | 174.3 | 1.00 |
| base:fstream | 1 | 865 | 216 | 1130.5 | 1458.7 | 0.13 |
| base:posix | 1 | 7.6k | 1893 | 124.2 | 330.2 | 1.16 |
| vfs:native | 1 | 6.7k | 1679 | 142.8 | 194.6 | 1.03 |
| vfs:memory | 1 | 42.9k | 10713 | 22.8 | 34.8 | 6.54 |
| vfs:archive | 1 | 8.5k | 2119 | 113.2 | 204.5 | 1.29 |
| vfs:archive-xor | 1 | 4.2k | 1048 | 229.6 | 331.5 | 0.64 |
| vfs:archive-lz4 | 1 | 6.1k | 1513 | 157.7 | 208.4 | 0.92 |
| vfs:archive+native | 1 | 9.0k | 2254 | 107.5 | 157.2 | 1.38 |
| vfs:memory+archive | 1 | 12.4k | 3110 | 77.6 | 109.3 | 1.90 |
| vfs:3layer | 1 | 9.3k | 2321 | 103.4 | 150.3 | 1.42 |
| vfs:nested8 | 1 | 4.6k | 1145 | 198.9 | 778.8 | 0.70 |
| base:stdio | 2 | 15.5k | 3878 | 121.9 | 415.7 | 1.00 |
| base:fstream | 2 | 1.6k | 410 | 1180.7 | 2818.3 | 0.11 |
| base:posix | 2 | 19.0k | 4750 | 100.4 | 140.0 | 1.22 |
| vfs:native | 2 | 13.8k | 3459 | 135.2 | 351.7 | 0.89 |
| vfs:memory | 2 | 99.9k | 24974 | 14.6 | 35.8 | 6.44 |
| vfs:archive | 2 | 27.7k | 6935 | 65.8 | 300.5 | 1.79 |
| vfs:archive-xor | 2 | 8.5k | 2120 | 230.4 | 539.1 | 0.55 |
| vfs:archive-lz4 | 2 | 11.5k | 2875 | 160.0 | 613.6 | 0.74 |
| vfs:archive+native | 2 | 22.4k | 5588 | 87.3 | 123.4 | 1.44 |
| vfs:memory+archive | 2 | 29.2k | 7311 | 65.3 | 93.2 | 1.89 |
| vfs:3layer | 2 | 23.2k | 5804 | 80.9 | 126.2 | 1.50 |
| vfs:nested8 | 2 | 12.5k | 3136 | 158.7 | 189.4 | 0.81 |
| base:stdio | 4 | 19.5k | 4884 | 192.0 | 393.2 | 1.00 |
| base:fstream | 4 | 3.1k | 782 | 1239.0 | 1833.0 | 0.16 |
| base:posix | 4 | 21.0k | 5242 | 180.5 | 851.7 | 1.07 |
| vfs:native | 4 | 19.7k | 4936 | 190.7 | 364.8 | 1.01 |
| vfs:memory | 4 | 191.1k | 47771 | 18.2 | 44.8 | 9.78 |
| vfs:archive | 4 | 28.4k | 7110 | 137.0 | 173.3 | 1.46 |
| vfs:archive-xor | 4 | 18.7k | 4684 | 205.6 | 259.6 | 0.96 |
| vfs:archive-lz4 | 4 | 21.4k | 5345 | 187.6 | 222.5 | 1.09 |
| vfs:archive+native | 4 | 22.9k | 5722 | 161.0 | 382.0 | 1.17 |
| vfs:memory+archive | 4 | 30.2k | 7560 | 125.2 | 169.7 | 1.55 |
| vfs:3layer | 4 | 23.4k | 5861 | 161.5 | 313.3 | 1.20 |
| vfs:nested8 | 4 | 17.5k | 4366 | 222.5 | 322.0 | 0.89 |
| base:stdio | 8 | 21.3k | 5332 | 352.8 | 852.7 | 1.00 |
| base:fstream | 8 | 4.1k | 1034 | 1920.5 | 2204.7 | 0.19 |
| base:posix | 8 | 22.3k | 5579 | 347.9 | 442.9 | 1.05 |
| vfs:native | 8 | 19.2k | 4796 | 404.7 | 555.3 | 0.90 |
| vfs:memory | 8 | 139.3k | 34824 | 50.2 | 151.6 | 6.53 |
| vfs:archive | 8 | 29.1k | 7279 | 273.4 | 461.8 | 1.37 |
| vfs:archive-xor | 8 | 26.2k | 6556 | 307.2 | 347.6 | 1.23 |
| vfs:archive-lz4 | 8 | 35.9k | 8971 | 213.8 | 353.0 | 1.68 |
| vfs:archive+native | 8 | 21.8k | 5461 | 357.1 | 534.5 | 1.02 |
| vfs:memory+archive | 8 | 30.0k | 7499 | 263.4 | 407.8 | 1.41 |
| vfs:3layer | 8 | 23.0k | 5760 | 330.8 | 598.5 | 1.08 |
| vfs:nested8 | 8 | 18.8k | 4706 | 414.0 | 552.4 | 0.88 |

### read.large.seq

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 4.8k | 4784 | 214.8 | 449.0 | 1.00 |
| base:fstream | 1 | 239 | 239 | 4124.4 | 4854.5 | 0.05 |
| base:posix | 1 | 8.4k | 8398 | 111.9 | 207.9 | 1.76 |
| vfs:native | 1 | 8.6k | 8647 | 107.0 | 204.3 | 1.81 |
| vfs:memory | 1 | 11.2k | 11162 | 83.5 | 100.1 | 2.33 |
| vfs:archive | 1 | 6.6k | 6598 | 144.4 | 317.2 | 1.38 |
| vfs:archive-xor | 1 | 1.2k | 1245 | 767.5 | 1035.5 | 0.26 |
| vfs:archive-lz4 | 1 | 1.6k | 1621 | 537.3 | 2093.3 | 0.34 |
| vfs:archive+native | 1 | 6.9k | 6943 | 129.8 | 666.9 | 1.45 |
| vfs:memory+archive | 1 | 8.0k | 8018 | 120.8 | 147.7 | 1.68 |
| vfs:3layer | 1 | 8.1k | 8091 | 122.1 | 194.0 | 1.69 |
| vfs:nested8 | 1 | 6.2k | 6164 | 145.9 | 628.5 | 1.29 |
| base:stdio | 2 | 10.4k | 10371 | 178.4 | 352.0 | 1.00 |
| base:fstream | 2 | 434 | 434 | 4565.0 | 5647.4 | 0.04 |
| base:posix | 2 | 9.5k | 9460 | 197.6 | 427.8 | 0.91 |
| vfs:native | 2 | 10.2k | 10208 | 189.7 | 267.5 | 0.98 |
| vfs:memory | 2 | 44.1k | 44080 | 41.5 | 60.2 | 4.25 |
| vfs:archive | 2 | 8.5k | 8497 | 225.5 | 480.8 | 0.82 |
| vfs:archive-xor | 2 | 2.6k | 2572 | 779.5 | 1090.6 | 0.25 |
| vfs:archive-lz4 | 2 | 3.3k | 3262 | 585.2 | 1036.8 | 0.31 |
| vfs:archive+native | 2 | 8.5k | 8532 | 214.8 | 433.7 | 0.82 |
| vfs:memory+archive | 2 | 9.9k | 9931 | 196.4 | 334.6 | 0.96 |
| vfs:3layer | 2 | 9.2k | 9222 | 207.9 | 365.1 | 0.89 |
| vfs:nested8 | 2 | 8.7k | 8669 | 215.0 | 547.3 | 0.84 |
| base:stdio | 4 | 9.8k | 9839 | 390.9 | 745.2 | 1.00 |
| base:fstream | 4 | 850 | 850 | 4651.0 | 5266.4 | 0.09 |
| base:posix | 4 | 9.9k | 9893 | 374.8 | 946.7 | 1.01 |
| vfs:native | 4 | 10.7k | 10670 | 356.9 | 789.5 | 1.08 |
| vfs:memory | 4 | 94.1k | 94127 | 36.9 | 62.7 | 9.57 |
| vfs:archive | 4 | 9.1k | 9140 | 415.7 | 804.1 | 0.93 |
| vfs:archive-xor | 4 | 4.1k | 4098 | 967.2 | 1306.9 | 0.42 |
| vfs:archive-lz4 | 4 | 6.1k | 6077 | 605.2 | 953.3 | 0.62 |
| vfs:archive+native | 4 | 9.0k | 8997 | 427.3 | 907.0 | 0.91 |
| vfs:memory+archive | 4 | 9.6k | 9650 | 407.0 | 693.0 | 0.98 |
| vfs:3layer | 4 | 9.3k | 9263 | 408.1 | 898.0 | 0.94 |
| vfs:nested8 | 4 | 9.5k | 9506 | 402.2 | 860.9 | 0.97 |
| base:stdio | 8 | 9.6k | 9633 | 824.6 | 1165.6 | 1.00 |
| base:fstream | 8 | 1.1k | 1061 | 7469.1 | 8152.8 | 0.11 |
| base:posix | 8 | 10.2k | 10193 | 752.6 | 1482.8 | 1.06 |
| vfs:native | 8 | 10.4k | 10419 | 731.9 | 1231.9 | 1.08 |
| vfs:memory | 8 | 136.0k | 135999 | 36.4 | 60.2 | 14.12 |
| vfs:archive | 8 | 10.2k | 10236 | 763.6 | 1345.5 | 1.06 |
| vfs:archive-xor | 8 | 5.7k | 5728 | 1417.0 | 1971.2 | 0.59 |
| vfs:archive-lz4 | 8 | 9.9k | 9899 | 773.6 | 1106.7 | 1.03 |
| vfs:archive+native | 8 | 9.4k | 9424 | 822.8 | 1187.3 | 0.98 |
| vfs:memory+archive | 8 | 9.7k | 9670 | 825.1 | 1219.3 | 1.00 |
| vfs:3layer | 8 | 9.6k | 9587 | 823.8 | 1140.2 | 1.00 |
| vfs:nested8 | 8 | 10.3k | 10286 | 750.1 | 1140.7 | 1.07 |

### read.large.rand

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 32.8k | 128 | 30.2 | 35.6 | 1.00 |
| base:fstream | 1 | 22.1k | 86 | 41.5 | 64.5 | 0.67 |
| base:posix | 1 | 63.9k | 249 | 14.1 | 21.0 | 1.95 |
| vfs:native | 1 | 33.6k | 131 | 27.9 | 36.6 | 1.02 |
| vfs:memory | 1 | 1.39M | 5432 | 0.51 | 1.02 | 42.38 |
| vfs:archive | 1 | 61.4k | 240 | 15.6 | 20.5 | 1.87 |
| vfs:archive-xor | 1 | 49.3k | 192 | 20.0 | 25.3 | 1.50 |
| vfs:archive-lz4 | 1 | 27.0k | 106 | 33.0 | 75.3 | 0.82 |
| vfs:archive+native | 1 | 61.1k | 239 | 15.9 | 20.5 | 1.86 |
| vfs:memory+archive | 1 | 64.7k | 253 | 14.6 | 20.0 | 1.97 |
| vfs:3layer | 1 | 63.0k | 246 | 14.6 | 20.5 | 1.92 |
| vfs:nested8 | 1 | 32.0k | 125 | 28.9 | 42.0 | 0.98 |
| base:stdio | 2 | 61.4k | 240 | 33.5 | 46.3 | 1.00 |
| base:fstream | 2 | 39.6k | 155 | 50.4 | 66.6 | 0.64 |
| base:posix | 2 | 114.0k | 445 | 17.2 | 26.9 | 1.86 |
| vfs:native | 2 | 57.9k | 226 | 34.0 | 46.6 | 0.94 |
| vfs:memory | 2 | 2.10M | 8203 | 0.77 | 1.28 | 34.21 |
| vfs:archive | 2 | 116.8k | 456 | 16.9 | 25.1 | 1.90 |
| vfs:archive-xor | 2 | 97.6k | 381 | 19.5 | 28.4 | 1.59 |
| vfs:archive-lz4 | 2 | 52.1k | 203 | 34.8 | 80.1 | 0.85 |
| vfs:archive+native | 2 | 111.1k | 434 | 17.9 | 26.4 | 1.81 |
| vfs:memory+archive | 2 | 117.5k | 459 | 17.2 | 26.1 | 1.91 |
| vfs:3layer | 2 | 131.1k | 512 | 15.9 | 23.8 | 2.14 |
| vfs:nested8 | 2 | 61.2k | 239 | 31.2 | 44.8 | 1.00 |
| base:stdio | 4 | 104.8k | 409 | 36.9 | 64.8 | 1.00 |
| base:fstream | 4 | 70.3k | 274 | 54.5 | 91.9 | 0.67 |
| base:posix | 4 | 231.0k | 902 | 14.8 | 35.8 | 2.20 |
| vfs:native | 4 | 112.5k | 440 | 33.8 | 64.5 | 1.07 |
| vfs:memory | 4 | 1.78M | 6949 | 1.54 | 12.0 | 16.98 |
| vfs:archive | 4 | 219.0k | 856 | 15.9 | 37.1 | 2.09 |
| vfs:archive-xor | 4 | 186.8k | 730 | 20.5 | 37.1 | 1.78 |
| vfs:archive-lz4 | 4 | 96.5k | 377 | 37.4 | 87.3 | 0.92 |
| vfs:archive+native | 4 | 228.3k | 892 | 15.4 | 35.8 | 2.18 |
| vfs:memory+archive | 4 | 244.7k | 956 | 13.6 | 34.8 | 2.34 |
| vfs:3layer | 4 | 259.3k | 1013 | 12.8 | 33.3 | 2.47 |
| vfs:nested8 | 4 | 103.4k | 404 | 37.4 | 68.1 | 0.99 |
| base:stdio | 8 | 135.7k | 530 | 56.6 | 103.2 | 1.00 |
| base:fstream | 8 | 86.8k | 339 | 89.1 | 154.1 | 0.64 |
| base:posix | 8 | 292.8k | 1144 | 25.3 | 51.5 | 2.16 |
| vfs:native | 8 | 138.6k | 541 | 54.8 | 102.4 | 1.02 |
| vfs:memory | 8 | 1.34M | 5253 | 2.82 | 56.3 | 9.91 |
| vfs:archive | 8 | 283.4k | 1107 | 26.1 | 54.5 | 2.09 |
| vfs:archive-xor | 8 | 262.7k | 1026 | 29.2 | 53.2 | 1.94 |
| vfs:archive-lz4 | 8 | 172.1k | 672 | 41.0 | 106.2 | 1.27 |
| vfs:archive+native | 8 | 296.3k | 1157 | 25.9 | 49.4 | 2.18 |
| vfs:memory+archive | 8 | 296.5k | 1158 | 24.3 | 52.7 | 2.19 |
| vfs:3layer | 8 | 303.8k | 1187 | 24.3 | 49.9 | 2.24 |
| vfs:nested8 | 8 | 138.9k | 542 | 54.3 | 106.0 | 1.02 |

### meta.open

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 28.4k | - | 33.0 | 44.0 | 1.00 |
| base:fstream | 1 | 27.7k | - | 34.6 | 47.4 | 0.98 |
| base:posix | 1 | 27.9k | - | 34.3 | 46.8 | 0.98 |
| vfs:native | 1 | 18.7k | - | 50.7 | 68.1 | 0.66 |
| vfs:memory | 1 | 1.04M | - | 0.77 | 1.79 | 36.67 |
| vfs:archive | 1 | 1.27M | - | 0.51 | 1.02 | 44.85 |
| vfs:archive-xor | 1 | 1.29M | - | 0.51 | 1.28 | 45.59 |
| vfs:archive-lz4 | 1 | 1.33M | - | 0.51 | 1.02 | 46.83 |
| vfs:archive+native | 1 | 42.2k | - | 22.5 | 29.7 | 1.49 |
| vfs:memory+archive | 1 | 1.07M | - | 0.77 | 1.28 | 37.64 |
| vfs:3layer | 1 | 42.7k | - | 22.3 | 29.7 | 1.51 |
| vfs:nested8 | 1 | 13.6k | - | 70.4 | 95.0 | 0.48 |
| base:stdio | 2 | 60.8k | - | 31.5 | 51.2 | 1.00 |
| base:fstream | 2 | 63.6k | - | 30.5 | 46.6 | 1.05 |
| base:posix | 2 | 62.7k | - | 30.0 | 46.3 | 1.03 |
| vfs:native | 2 | 42.1k | - | 47.1 | 73.0 | 0.69 |
| vfs:memory | 2 | 285.6k | - | 6.40 | 20.2 | 4.70 |
| vfs:archive | 2 | 303.3k | - | 6.14 | 20.0 | 4.99 |
| vfs:archive-xor | 2 | 300.1k | - | 6.14 | 20.2 | 4.94 |
| vfs:archive-lz4 | 2 | 302.8k | - | 6.14 | 19.7 | 4.98 |
| vfs:archive+native | 2 | 112.9k | - | 15.9 | 30.7 | 1.86 |
| vfs:memory+archive | 2 | 269.4k | - | 6.91 | 20.2 | 4.43 |
| vfs:3layer | 2 | 112.3k | - | 16.1 | 26.9 | 1.85 |
| vfs:nested8 | 2 | 28.7k | - | 67.1 | 100.4 | 0.47 |
| base:stdio | 4 | 86.9k | - | 44.0 | 73.0 | 1.00 |
| base:fstream | 4 | 85.0k | - | 45.3 | 73.2 | 0.98 |
| base:posix | 4 | 90.9k | - | 41.2 | 69.4 | 1.05 |
| vfs:native | 4 | 58.7k | - | 64.3 | 108.5 | 0.68 |
| vfs:memory | 4 | 188.0k | - | 18.2 | 63.5 | 2.16 |
| vfs:archive | 4 | 192.9k | - | 18.2 | 59.9 | 2.22 |
| vfs:archive-xor | 4 | 198.1k | - | 17.2 | 60.7 | 2.28 |
| vfs:archive-lz4 | 4 | 190.0k | - | 18.4 | 60.7 | 2.19 |
| vfs:archive+native | 4 | 114.7k | - | 32.3 | 54.0 | 1.32 |
| vfs:memory+archive | 4 | 160.2k | - | 22.0 | 67.6 | 1.84 |
| vfs:3layer | 4 | 110.2k | - | 32.8 | 57.6 | 1.27 |
| vfs:nested8 | 4 | 41.0k | - | 89.3 | 154.1 | 0.47 |
| base:stdio | 8 | 114.9k | - | 67.3 | 104.7 | 1.00 |
| base:fstream | 8 | 109.6k | - | 71.2 | 108.3 | 0.95 |
| base:posix | 8 | 113.1k | - | 67.6 | 105.7 | 0.98 |
| vfs:native | 8 | 73.6k | - | 104.4 | 190.7 | 0.64 |
| vfs:memory | 8 | 143.5k | - | 47.4 | 178.2 | 1.25 |
| vfs:archive | 8 | 151.0k | - | 44.0 | 172.3 | 1.31 |
| vfs:archive-xor | 8 | 146.4k | - | 46.1 | 175.4 | 1.27 |
| vfs:archive-lz4 | 8 | 143.8k | - | 47.1 | 176.4 | 1.25 |
| vfs:archive+native | 8 | 110.7k | - | 67.3 | 152.8 | 0.96 |
| vfs:memory+archive | 8 | 127.2k | - | 53.5 | 193.8 | 1.11 |
| vfs:3layer | 8 | 109.4k | - | 66.6 | 164.1 | 0.95 |
| vfs:nested8 | 8 | 44.9k | - | 164.6 | 335.6 | 0.39 |

### meta.stat

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 25.8k | - | 34.0 | 77.1 | 1.00 |
| vfs:native | 1 | 51.2k | - | 18.2 | 26.6 | 1.98 |
| vfs:memory | 1 | 1.07M | - | 0.77 | 1.79 | 41.57 |
| vfs:archive | 1 | 1.50M | - | 0.51 | 0.77 | 58.27 |
| vfs:archive-xor | 1 | 1.56M | - | 0.51 | 0.77 | 60.53 |
| vfs:archive-lz4 | 1 | 1.54M | - | 0.51 | 1.02 | 59.55 |
| vfs:archive+native | 1 | 41.5k | - | 22.5 | 32.3 | 1.61 |
| vfs:memory+archive | 1 | 1.40M | - | 0.51 | 0.77 | 54.15 |
| vfs:3layer | 1 | 41.1k | - | 22.5 | 32.8 | 1.59 |
| vfs:nested8 | 1 | 48.5k | - | 19.5 | 27.4 | 1.88 |
| base:stdio | 2 | 82.8k | - | 21.0 | 53.2 | 1.00 |
| vfs:native | 2 | 130.1k | - | 14.1 | 30.0 | 1.57 |
| vfs:memory | 2 | 393.5k | - | 4.61 | 15.4 | 4.75 |
| vfs:archive | 2 | 424.1k | - | 4.10 | 15.9 | 5.12 |
| vfs:archive-xor | 2 | 408.5k | - | 4.35 | 16.9 | 4.93 |
| vfs:archive-lz4 | 2 | 409.3k | - | 4.10 | 17.2 | 4.94 |
| vfs:archive+native | 2 | 112.4k | - | 16.4 | 29.2 | 1.36 |
| vfs:memory+archive | 2 | 336.8k | - | 5.89 | 17.2 | 4.07 |
| vfs:3layer | 2 | 109.3k | - | 16.4 | 31.5 | 1.32 |
| vfs:nested8 | 2 | 100.9k | - | 19.2 | 33.0 | 1.22 |
| base:stdio | 4 | 94.2k | - | 39.2 | 98.6 | 1.00 |
| vfs:native | 4 | 180.1k | - | 20.0 | 48.1 | 1.91 |
| vfs:memory | 4 | 231.8k | - | 14.1 | 54.8 | 2.46 |
| vfs:archive | 4 | 248.9k | - | 12.8 | 53.8 | 2.64 |
| vfs:archive-xor | 4 | 238.2k | - | 13.8 | 54.5 | 2.53 |
| vfs:archive-lz4 | 4 | 240.6k | - | 13.3 | 56.3 | 2.55 |
| vfs:archive+native | 4 | 113.0k | - | 32.0 | 60.2 | 1.20 |
| vfs:memory+archive | 4 | 210.8k | - | 15.9 | 58.4 | 2.24 |
| vfs:3layer | 4 | 112.8k | - | 31.7 | 58.9 | 1.20 |
| vfs:nested8 | 4 | 121.8k | - | 26.9 | 82.9 | 1.29 |
| base:stdio | 8 | 96.8k | - | 77.1 | 167.2 | 1.00 |
| vfs:native | 8 | 151.9k | - | 42.0 | 119.0 | 1.57 |
| vfs:memory | 8 | 182.4k | - | 33.8 | 153.1 | 1.88 |
| vfs:archive | 8 | 196.9k | - | 28.7 | 146.7 | 2.03 |
| vfs:archive-xor | 8 | 188.8k | - | 32.8 | 150.8 | 1.95 |
| vfs:archive-lz4 | 8 | 195.6k | - | 29.7 | 151.8 | 2.02 |
| vfs:archive+native | 8 | 113.1k | - | 63.2 | 131.6 | 1.17 |
| vfs:memory+archive | 8 | 160.9k | - | 41.0 | 166.9 | 1.66 |
| vfs:3layer | 8 | 114.6k | - | 63.7 | 129.8 | 1.18 |
| vfs:nested8 | 8 | 77.9k | - | 94.5 | 231.2 | 0.80 |

### meta.miss

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 42.9k | - | 21.5 | 31.0 | 1.00 |
| vfs:native | 1 | 41.6k | - | 22.3 | 36.4 | 0.97 |
| vfs:memory | 1 | 1.15M | - | 0.77 | 1.28 | 26.88 |
| vfs:archive | 1 | 1.17M | - | 0.77 | 1.28 | 27.26 |
| vfs:archive-xor | 1 | 1.15M | - | 0.77 | 1.02 | 26.89 |
| vfs:archive-lz4 | 1 | 1.18M | - | 0.77 | 1.02 | 27.55 |
| vfs:archive+native | 1 | 42.9k | - | 21.8 | 31.7 | 1.00 |
| vfs:memory+archive | 1 | 1.17M | - | 0.77 | 1.02 | 27.27 |
| vfs:3layer | 1 | 42.5k | - | 22.0 | 31.0 | 0.99 |
| vfs:nested8 | 1 | 39.9k | - | 23.3 | 33.0 | 0.93 |
| base:stdio | 2 | 111.5k | - | 17.2 | 27.4 | 1.00 |
| vfs:native | 2 | 110.3k | - | 16.6 | 32.0 | 0.99 |
| vfs:memory | 2 | 355.8k | - | 5.12 | 16.1 | 3.19 |
| vfs:archive | 2 | 362.9k | - | 4.86 | 17.7 | 3.26 |
| vfs:archive-xor | 2 | 346.7k | - | 5.12 | 18.2 | 3.11 |
| vfs:archive-lz4 | 2 | 336.7k | - | 5.38 | 18.7 | 3.02 |
| vfs:archive+native | 2 | 112.2k | - | 16.4 | 27.9 | 1.01 |
| vfs:memory+archive | 2 | 307.2k | - | 6.14 | 17.9 | 2.76 |
| vfs:3layer | 2 | 110.3k | - | 16.4 | 32.3 | 0.99 |
| vfs:nested8 | 2 | 105.1k | - | 16.6 | 34.6 | 0.94 |
| base:stdio | 4 | 104.0k | - | 34.8 | 67.1 | 1.00 |
| vfs:native | 4 | 111.3k | - | 32.0 | 69.1 | 1.07 |
| vfs:memory | 4 | 207.7k | - | 16.1 | 56.1 | 2.00 |
| vfs:archive | 4 | 209.5k | - | 16.1 | 60.4 | 2.01 |
| vfs:archive-xor | 4 | 206.1k | - | 16.4 | 59.1 | 1.98 |
| vfs:archive-lz4 | 4 | 214.4k | - | 15.4 | 58.4 | 2.06 |
| vfs:archive+native | 4 | 113.2k | - | 31.5 | 60.2 | 1.09 |
| vfs:memory+archive | 4 | 186.6k | - | 18.2 | 64.3 | 1.79 |
| vfs:3layer | 4 | 113.2k | - | 31.2 | 62.7 | 1.09 |
| vfs:nested8 | 4 | 105.8k | - | 32.8 | 69.4 | 1.02 |
| base:stdio | 8 | 112.0k | - | 64.5 | 145.9 | 1.00 |
| vfs:native | 8 | 108.2k | - | 69.1 | 132.6 | 0.97 |
| vfs:memory | 8 | 156.0k | - | 43.3 | 163.8 | 1.39 |
| vfs:archive | 8 | 164.1k | - | 39.2 | 164.4 | 1.46 |
| vfs:archive-xor | 8 | 164.3k | - | 39.4 | 164.6 | 1.47 |
| vfs:archive-lz4 | 8 | 161.6k | - | 41.0 | 164.4 | 1.44 |
| vfs:archive+native | 8 | 111.9k | - | 67.3 | 134.7 | 1.00 |
| vfs:memory+archive | 8 | 140.8k | - | 47.1 | 184.3 | 1.26 |
| vfs:3layer | 8 | 111.0k | - | 64.3 | 136.4 | 0.99 |
| vfs:nested8 | 8 | 107.1k | - | 67.6 | 184.3 | 0.96 |

### meta.list

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 24 | - | 42939.9 | 45545.7 | 1.00 |
| vfs:native | 1 | 22 | - | 46066.9 | 49200.1 | 0.92 |
| vfs:memory | 1 | 397 | - | 2464.0 | 2786.0 | 16.79 |
| vfs:archive | 1 | 496 | - | 2121.5 | 2159.1 | 20.98 |
| vfs:archive-xor | 1 | 550 | - | 1771.8 | 1918.7 | 23.25 |
| vfs:archive-lz4 | 1 | 540 | - | 1786.9 | 2041.1 | 22.85 |
| vfs:archive+native | 1 | 464 | - | 2073.3 | 2692.1 | 19.61 |
| vfs:memory+archive | 1 | 547 | - | 1761.5 | 2001.2 | 23.11 |
| vfs:3layer | 1 | 493 | - | 1999.4 | 2178.0 | 20.85 |
| vfs:nested8 | 1 | 22 | - | 46273.3 | 49713.4 | 0.92 |
| base:stdio | 2 | 44 | - | 46246.7 | 48511.0 | 1.00 |
| vfs:native | 2 | 40 | - | 49246.5 | 56495.6 | 0.92 |
| vfs:memory | 2 | 85 | - | 23591.2 | 25261.6 | 1.94 |
| vfs:archive | 2 | 124 | - | 17029.6 | 17728.5 | 2.84 |
| vfs:archive-xor | 2 | 125 | - | 16120.6 | 17657.1 | 2.86 |
| vfs:archive-lz4 | 2 | 130 | - | 15738.6 | 17343.5 | 2.96 |
| vfs:archive+native | 2 | 125 | - | 16126.7 | 17068.0 | 2.86 |
| vfs:memory+archive | 2 | 128 | - | 15920.6 | 17539.1 | 2.93 |
| vfs:3layer | 2 | 128 | - | 15669.5 | 16292.9 | 2.92 |
| vfs:nested8 | 2 | 38 | - | 53287.4 | 59259.1 | 0.87 |
| base:stdio | 4 | 42 | - | 96577.5 | 104547.6 | 1.00 |
| vfs:native | 4 | 35 | - | 113378.3 | 126839.8 | 0.85 |
| vfs:memory | 4 | 49 | - | 82833.2 | 96534.3 | 1.18 |
| vfs:archive | 4 | 75 | - | 53851.4 | 61253.4 | 1.80 |
| vfs:archive-xor | 4 | 74 | - | 55241.0 | 67603.7 | 1.77 |
| vfs:archive-lz4 | 4 | 72 | - | 56395.8 | 67222.0 | 1.73 |
| vfs:archive+native | 4 | 73 | - | 55401.5 | 62967.3 | 1.75 |
| vfs:memory+archive | 4 | 70 | - | 56973.8 | 66841.3 | 1.69 |
| vfs:3layer | 4 | 78 | - | 57041.2 | 66619.6 | 1.86 |
| vfs:nested8 | 4 | 34 | - | 118048.3 | 125229.6 | 0.82 |
| base:stdio | 8 | 44 | - | 183359.0 | 192955.4 | 1.00 |
| vfs:native | 8 | 24 | - | 333258.2 | 396842.2 | 0.56 |
| vfs:memory | 8 | 45 | - | 175721.0 | 214208.5 | 1.04 |
| vfs:archive | 8 | 69 | - | 118391.0 | 173247.5 | 1.58 |
| vfs:archive-xor | 8 | 49 | - | 168900.9 | 310854.4 | 1.12 |
| vfs:archive-lz4 | 8 | 34 | - | 237489.9 | 277762.0 | 0.79 |
| vfs:archive+native | 8 | 43 | - | 195017.5 | 241641.0 | 0.99 |
| vfs:memory+archive | 8 | 48 | - | 165646.8 | 201342.2 | 1.11 |
| vfs:3layer | 8 | 48 | - | 166483.7 | 201525.2 | 1.09 |
| vfs:nested8 | 8 | 21 | - | 363501.1 | 499138.0 | 0.48 |

### write.create

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 6.9k | 431 | 134.7 | 292.4 | 1.00 |
| base:fstream | 1 | 7.8k | 488 | 110.8 | 390.9 | 1.13 |
| base:posix | 1 | 9.1k | 569 | 102.9 | 223.7 | 1.32 |
| vfs:native | 1 | 4.1k | 258 | 210.4 | 1154.6 | 0.60 |
| vfs:memory | 1 | 83.6k | 5224 | 10.8 | 18.2 | 12.12 |
| vfs:memory+archive | 1 | 131.0k | 8187 | 6.66 | 19.5 | 18.99 |
| base:stdio | 2 | 10.7k | 669 | 162.8 | 617.2 | 1.00 |
| base:fstream | 2 | 11.6k | 727 | 153.3 | 662.3 | 1.09 |
| base:posix | 2 | 15.6k | 977 | 113.9 | 542.7 | 1.46 |
| vfs:native | 2 | 8.5k | 529 | 214.0 | 422.7 | 0.79 |
| vfs:memory | 2 | 130.4k | 8149 | 13.6 | 28.4 | 12.18 |
| vfs:memory+archive | 2 | 113.7k | 7104 | 15.9 | 31.5 | 10.62 |
| base:stdio | 4 | 13.4k | 837 | 209.9 | 1015.0 | 1.00 |
| base:fstream | 4 | 13.3k | 831 | 239.1 | 999.4 | 0.99 |
| base:posix | 4 | 14.8k | 927 | 214.8 | 714.5 | 1.11 |
| vfs:native | 4 | 10.8k | 673 | 332.3 | 1045.2 | 0.80 |
| vfs:memory | 4 | 77.8k | 4860 | 44.8 | 119.6 | 5.80 |
| vfs:memory+archive | 4 | 82.4k | 5153 | 44.3 | 94.0 | 6.15 |
| base:stdio | 8 | 16.5k | 1033 | 414.2 | 1201.9 | 1.00 |
| base:fstream | 8 | 14.0k | 877 | 437.0 | 1496.1 | 0.85 |
| base:posix | 8 | 16.4k | 1026 | 380.7 | 2965.5 | 0.99 |
| vfs:native | 8 | 8.5k | 534 | 748.8 | 2475.3 | 0.52 |
| vfs:memory | 8 | 67.4k | 4212 | 101.4 | 321.8 | 4.08 |
| vfs:memory+archive | 8 | 68.8k | 4299 | 105.5 | 273.7 | 4.16 |

### write.churn

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 10.1k | - | 91.6 | 161.8 | 1.00 |
| base:fstream | 1 | 6.8k | - | 143.4 | 217.1 | 0.67 |
| base:posix | 1 | 12.7k | - | 76.8 | 118.8 | 1.26 |
| vfs:native | 1 | 2.8k | - | 336.4 | 502.5 | 0.28 |
| vfs:memory | 1 | 279.9k | - | 2.82 | 4.35 | 27.60 |
| vfs:memory+archive | 1 | 258.7k | - | 3.07 | 6.66 | 25.52 |
| base:stdio | 2 | 21.1k | - | 90.1 | 178.9 | 1.00 |
| base:fstream | 2 | 21.6k | - | 85.2 | 143.6 | 1.02 |
| base:posix | 2 | 20.8k | - | 81.9 | 221.7 | 0.99 |
| vfs:native | 2 | 7.7k | - | 194.8 | 874.2 | 0.37 |
| vfs:memory | 2 | 100.9k | - | 17.4 | 33.5 | 4.78 |
| vfs:memory+archive | 2 | 84.5k | - | 21.2 | 36.9 | 4.01 |
| base:stdio | 4 | 33.2k | - | 114.2 | 252.9 | 1.00 |
| base:fstream | 4 | 32.5k | - | 113.9 | 331.0 | 0.98 |
| base:posix | 4 | 37.4k | - | 98.0 | 285.4 | 1.13 |
| vfs:native | 4 | 14.0k | - | 260.6 | 837.6 | 0.42 |
| vfs:memory | 4 | 59.3k | - | 57.9 | 121.9 | 1.79 |
| vfs:memory+archive | 4 | 53.4k | - | 64.8 | 138.5 | 1.61 |
| base:stdio | 8 | 38.7k | - | 197.9 | 376.3 | 1.00 |
| base:fstream | 8 | 35.1k | - | 215.6 | 488.7 | 0.91 |
| base:posix | 8 | 40.5k | - | 187.6 | 438.3 | 1.05 |
| vfs:native | 8 | 15.6k | - | 495.6 | 747.5 | 0.40 |
| vfs:memory | 8 | 49.1k | - | 133.6 | 345.1 | 1.27 |
| vfs:memory+archive | 8 | 43.3k | - | 158.0 | 363.8 | 1.12 |

### write.large.seq

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 2.6k | 2647 | 337.7 | 639.2 | 1.00 |
| base:posix | 1 | 2.9k | 2911 | 321.8 | 585.2 | 1.10 |
| vfs:native | 1 | 2.1k | 2079 | 428.0 | 949.5 | 0.79 |
| vfs:memory | 1 | 4.5k | 4470 | 101.6 | 3500.0 | 1.69 |
| vfs:memory+archive | 1 | 6.3k | 6344 | 80.4 | 2851.3 | 2.40 |
| base:stdio | 2 | 2.5k | 2534 | 765.7 | 1213.2 | 1.00 |
| base:posix | 2 | 2.5k | 2542 | 762.6 | 1130.2 | 1.00 |
| vfs:native | 2 | 2.6k | 2569 | 764.7 | 1059.1 | 1.01 |
| vfs:memory | 2 | 9.0k | 8975 | 102.7 | 2922.0 | 3.54 |
| vfs:memory+archive | 2 | 10.1k | 10075 | 106.8 | 3064.8 | 3.98 |
| base:stdio | 4 | 2.2k | 2232 | 1746.4 | 2316.3 | 1.00 |
| base:posix | 4 | 2.4k | 2362 | 1637.4 | 3133.4 | 1.06 |
| vfs:native | 4 | 2.2k | 2186 | 1620.2 | 4026.1 | 0.98 |
| vfs:memory | 4 | 17.6k | 17648 | 108.3 | 3932.2 | 7.91 |
| vfs:memory+archive | 4 | 18.7k | 18653 | 103.7 | 4024.3 | 8.36 |
| base:stdio | 8 | 2.4k | 2369 | 3289.3 | 4082.9 | 1.00 |
| base:posix | 8 | 2.4k | 2406 | 3139.8 | 8173.3 | 1.02 |
| vfs:native | 8 | 2.6k | 2562 | 2969.6 | 3659.3 | 1.08 |
| vfs:memory | 8 | 14.4k | 14424 | 137.7 | 4808.7 | 6.09 |
| vfs:memory+archive | 8 | 28.7k | 28721 | 123.4 | 4730.9 | 12.12 |

### write.overwrite

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 31.5k | 123 | 30.5 | 44.5 | 1.00 |
| base:posix | 1 | 59.8k | 234 | 16.4 | 22.0 | 1.90 |
| vfs:native | 1 | 32.0k | 125 | 29.7 | 39.4 | 1.01 |
| vfs:memory | 1 | 1.49M | 5817 | 0.51 | 1.54 | 47.25 |
| vfs:memory+archive | 1 | 1.38M | 5380 | 0.51 | 1.02 | 43.70 |
| base:stdio | 2 | 59.8k | 234 | 31.7 | 48.4 | 1.00 |
| base:posix | 2 | 107.2k | 419 | 15.9 | 29.2 | 1.79 |
| vfs:native | 2 | 61.0k | 238 | 31.0 | 46.1 | 1.02 |
| vfs:memory | 2 | 2.21M | 8645 | 0.51 | 1.28 | 37.02 |
| vfs:memory+archive | 2 | 2.31M | 9017 | 0.51 | 1.28 | 38.61 |
| base:stdio | 4 | 90.4k | 353 | 37.4 | 133.9 | 1.00 |
| base:posix | 4 | 199.9k | 781 | 17.4 | 39.9 | 2.21 |
| vfs:native | 4 | 103.5k | 404 | 35.8 | 70.9 | 1.14 |
| vfs:memory | 4 | 3.72M | 14544 | 0.77 | 1.54 | 41.18 |
| vfs:memory+archive | 4 | 4.39M | 17151 | 0.51 | 1.02 | 48.57 |
| base:stdio | 8 | 122.2k | 477 | 60.9 | 139.5 | 1.00 |
| base:posix | 8 | 254.4k | 994 | 28.7 | 66.6 | 2.08 |
| vfs:native | 8 | 125.0k | 488 | 59.6 | 136.7 | 1.02 |
| vfs:memory | 8 | 4.60M | 17969 | 0.77 | 1.54 | 37.63 |
| vfs:memory+archive | 8 | 5.60M | 21856 | 0.77 | 1.79 | 45.77 |

### write.append

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 40.2k | 157 | 24.4 | 36.1 | 1.00 |
| base:posix | 1 | 49.2k | 192 | 20.0 | 32.3 | 1.23 |
| vfs:native | 1 | 44.9k | 175 | 22.2 | 32.6 | 1.12 |
| vfs:memory | 1 | 1.01M | 3937 | 0.69 | 9.36 | 25.09 |
| vfs:memory+archive | 1 | 1.05M | 4088 | 0.69 | 9.49 | 26.05 |
| base:stdio | 2 | 112.7k | 440 | 17.3 | 25.5 | 1.00 |
| base:posix | 2 | 140.2k | 548 | 12.9 | 37.1 | 1.24 |
| vfs:native | 2 | 117.3k | 458 | 15.5 | 55.5 | 1.04 |
| vfs:memory | 2 | 1.89M | 7389 | 0.62 | 8.08 | 16.79 |
| vfs:memory+archive | 2 | 1.89M | 7373 | 0.50 | 7.94 | 16.75 |
| base:stdio | 4 | 157.8k | 617 | 24.5 | 45.9 | 1.00 |
| base:posix | 4 | 158.2k | 618 | 24.0 | 52.0 | 1.00 |
| vfs:native | 4 | 140.7k | 550 | 27.6 | 44.8 | 0.89 |
| vfs:memory | 4 | 2.20M | 8603 | 1.28 | 9.60 | 13.95 |
| vfs:memory+archive | 4 | 2.16M | 8451 | 1.22 | 10.0 | 13.71 |
| base:stdio | 8 | 158.1k | 618 | 48.4 | 73.2 | 1.00 |
| base:posix | 8 | 172.5k | 674 | 42.3 | 76.3 | 1.09 |
| vfs:native | 8 | 157.9k | 617 | 49.6 | 74.7 | 1.00 |
| vfs:memory | 8 | 2.03M | 7940 | 2.37 | 21.5 | 12.86 |
| vfs:memory+archive | 8 | 1.75M | 6844 | 2.58 | 42.7 | 11.08 |
