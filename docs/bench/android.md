# vfs_bench full results - Android

- Device: Xiaomi Redmi K80 (24117RK2CC), Qualcomm Snapdragon 8 Gen 3 (SM8650, 8 cores); memory 16 GB
- OS: Android 16 (API 36), HyperOS OS3.0, Linux 6.1.138
- Compiler: NDK r27c clang 18.0.3, arm64-v8a, Release
- Disk: built-in UFS flash, f2fs (working directory /data/local/tmp)
- How it was run: `vfs_bench` pushed with `adb push`, run as `adb shell "cd /data/local/tmp/vfs && ./vfs_bench --dir /data/local/tmp/vfs --csv android.csv --md android.md"`, results pulled with `adb pull`. The write scenarios come from a separate run with `--threads 1,2,4,8 --filter write`.
- Date: 2026-10-21 (UTC+8), default parameters (--scale 1, --iterations 3, threads 1,2,4,8,N), warm cache, Release

### read.small

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 96.0k | 375 | 6.41 | 69.3 | 1.00 |
| base:fstream | 1 | 91.4k | 357 | 7.29 | 46.0 | 0.95 |
| base:posix | 1 | 110.8k | 433 | 5.68 | 26.7 | 1.15 |
| vfs:native | 1 | 110.6k | 432 | 6.51 | 34.1 | 1.15 |
| vfs:memory | 1 | 326.3k | 1275 | 2.76 | 4.12 | 3.40 |
| vfs:archive | 1 | 144.7k | 565 | 3.39 | 25.9 | 1.51 |
| vfs:archive-xor | 1 | 135.3k | 528 | 6.61 | 16.6 | 1.41 |
| vfs:archive-lz4 | 1 | 105.1k | 411 | 5.10 | 51.8 | 1.09 |
| vfs:archive+native | 1 | 109.6k | 428 | 8.18 | 19.8 | 1.14 |
| vfs:memory+archive | 1 | 133.6k | 522 | 3.65 | 24.3 | 1.39 |
| vfs:3layer | 1 | 121.3k | 474 | 4.22 | 39.0 | 1.26 |
| vfs:nested8 | 1 | 71.5k | 279 | 9.95 | 108.8 | 0.75 |
| base:stdio | 2 | 217.1k | 848 | 7.08 | 28.4 | 1.00 |
| base:fstream | 2 | 181.1k | 707 | 7.66 | 35.4 | 0.83 |
| base:posix | 2 | 193.6k | 756 | 6.20 | 37.0 | 0.89 |
| vfs:native | 2 | 206.1k | 805 | 6.82 | 37.3 | 0.95 |
| vfs:memory | 2 | 423.3k | 1653 | 3.44 | 11.7 | 1.95 |
| vfs:archive | 2 | 363.8k | 1421 | 4.22 | 13.0 | 1.68 |
| vfs:archive-xor | 2 | 266.7k | 1042 | 5.00 | 25.3 | 1.23 |
| vfs:archive-lz4 | 2 | 188.9k | 738 | 5.42 | 43.4 | 0.87 |
| vfs:archive+native | 2 | 224.8k | 878 | 6.15 | 34.8 | 1.04 |
| vfs:memory+archive | 2 | 288.2k | 1126 | 5.42 | 22.5 | 1.33 |
| vfs:3layer | 2 | 220.7k | 862 | 4.79 | 39.1 | 1.02 |
| vfs:nested8 | 2 | 128.7k | 503 | 10.2 | 59.5 | 0.59 |
| base:stdio | 4 | 338.8k | 1324 | 7.81 | 52.7 | 1.00 |
| base:fstream | 4 | 337.3k | 1318 | 8.65 | 51.9 | 1.00 |
| base:posix | 4 | 503.6k | 1967 | 5.99 | 22.1 | 1.49 |
| vfs:native | 4 | 403.7k | 1577 | 8.12 | 35.6 | 1.19 |
| vfs:memory | 4 | 835.5k | 3264 | 3.75 | 10.5 | 2.47 |
| vfs:archive | 4 | 809.0k | 3160 | 3.02 | 11.6 | 2.39 |
| vfs:archive-xor | 4 | 716.9k | 2800 | 3.02 | 21.2 | 2.12 |
| vfs:archive-lz4 | 4 | 405.2k | 1583 | 6.61 | 31.2 | 1.20 |
| vfs:archive+native | 4 | 459.6k | 1795 | 5.73 | 34.0 | 1.36 |
| vfs:memory+archive | 4 | 540.7k | 2112 | 5.16 | 19.9 | 1.60 |
| vfs:3layer | 4 | 397.4k | 1552 | 6.41 | 34.6 | 1.17 |
| vfs:nested8 | 4 | 243.2k | 950 | 13.0 | 48.5 | 0.72 |
| base:stdio | 8 | 679.4k | 2654 | 8.02 | 22.9 | 1.00 |
| base:fstream | 8 | 545.6k | 2131 | 9.43 | 37.3 | 0.80 |
| base:posix | 8 | 754.6k | 2948 | 6.46 | 19.4 | 1.11 |
| vfs:native | 8 | 594.7k | 2323 | 8.91 | 28.8 | 0.88 |
| vfs:memory | 8 | 753.0k | 2942 | 9.17 | 16.5 | 1.11 |
| vfs:archive | 8 | 799.4k | 3123 | 7.61 | 15.2 | 1.18 |
| vfs:archive-xor | 8 | 832.5k | 3252 | 7.76 | 15.1 | 1.23 |
| vfs:archive-lz4 | 8 | 462.1k | 1805 | 13.9 | 29.1 | 0.68 |
| vfs:archive+native | 8 | 550.5k | 2150 | 4.53 | 15.6 | 0.81 |
| vfs:memory+archive | 8 | 799.8k | 3124 | 5.26 | 15.1 | 1.18 |
| vfs:3layer | 8 | 532.7k | 2081 | 9.69 | 87.2 | 0.78 |
| vfs:nested8 | 8 | 287.4k | 1122 | 19.3 | 143.1 | 0.42 |

### read.medium

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 8.6k | 2159 | 107.0 | 400.7 | 1.00 |
| base:fstream | 1 | 8.0k | 1990 | 110.5 | 220.4 | 0.92 |
| base:posix | 1 | 13.8k | 3447 | 59.8 | 238.1 | 1.60 |
| vfs:native | 1 | 14.1k | 3535 | 63.9 | 176.3 | 1.64 |
| vfs:memory | 1 | 53.1k | 13272 | 14.5 | 123.4 | 6.15 |
| vfs:archive | 1 | 9.1k | 2285 | 110.1 | 248.9 | 1.06 |
| vfs:archive-xor | 1 | 8.2k | 2042 | 78.9 | 393.6 | 0.95 |
| vfs:archive-lz4 | 1 | 9.2k | 2310 | 106.6 | 171.1 | 1.07 |
| vfs:archive+native | 1 | 11.1k | 2764 | 76.5 | 416.9 | 1.28 |
| vfs:memory+archive | 1 | 18.9k | 4714 | 50.9 | 93.1 | 2.18 |
| vfs:3layer | 1 | 15.8k | 3944 | 52.9 | 277.9 | 1.83 |
| vfs:nested8 | 1 | 13.6k | 3406 | 70.0 | 122.0 | 1.58 |
| base:stdio | 2 | 23.5k | 5865 | 61.4 | 257.7 | 1.00 |
| base:fstream | 2 | 14.4k | 3603 | 112.8 | 425.1 | 0.61 |
| base:posix | 2 | 26.4k | 6607 | 56.0 | 165.2 | 1.13 |
| vfs:native | 2 | 30.1k | 7528 | 52.6 | 149.2 | 1.28 |
| vfs:memory | 2 | 64.5k | 16114 | 21.2 | 114.8 | 2.75 |
| vfs:archive | 2 | 45.0k | 11253 | 41.4 | 114.3 | 1.92 |
| vfs:archive-xor | 2 | 25.1k | 6266 | 63.5 | 285.0 | 1.07 |
| vfs:archive-lz4 | 2 | 14.5k | 3618 | 112.7 | 374.9 | 0.62 |
| vfs:archive+native | 2 | 27.5k | 6873 | 54.8 | 114.4 | 1.17 |
| vfs:memory+archive | 2 | 39.5k | 9877 | 47.8 | 105.2 | 1.68 |
| vfs:3layer | 2 | 37.0k | 9246 | 51.3 | 107.3 | 1.58 |
| vfs:nested8 | 2 | 25.0k | 6239 | 73.3 | 178.2 | 1.06 |
| base:stdio | 4 | 44.3k | 11087 | 66.1 | 183.7 | 1.00 |
| base:fstream | 4 | 32.3k | 8068 | 115.3 | 194.2 | 0.73 |
| base:posix | 4 | 35.7k | 8921 | 68.8 | 184.9 | 0.80 |
| vfs:native | 4 | 57.1k | 14280 | 65.4 | 122.4 | 1.29 |
| vfs:memory | 4 | 122.1k | 30526 | 17.3 | 36.1 | 2.75 |
| vfs:archive | 4 | 39.1k | 9767 | 52.1 | 265.8 | 0.88 |
| vfs:archive-xor | 4 | 44.2k | 11049 | 75.1 | 188.2 | 1.00 |
| vfs:archive-lz4 | 4 | 22.8k | 5699 | 103.9 | 294.7 | 0.51 |
| vfs:archive+native | 4 | 42.4k | 10592 | 54.1 | 141.8 | 0.96 |
| vfs:memory+archive | 4 | 38.8k | 9712 | 54.9 | 128.3 | 0.88 |
| vfs:3layer | 4 | 38.5k | 9633 | 50.9 | 484.2 | 0.87 |
| vfs:nested8 | 4 | 29.0k | 7260 | 73.5 | 228.7 | 0.65 |
| base:stdio | 8 | 56.8k | 14200 | 72.2 | 187.0 | 1.00 |
| base:fstream | 8 | 35.7k | 8923 | 121.1 | 333.6 | 0.63 |
| base:posix | 8 | 54.4k | 13606 | 64.1 | 187.8 | 0.96 |
| vfs:native | 8 | 59.6k | 14909 | 76.0 | 158.2 | 1.05 |
| vfs:memory | 8 | 135.3k | 33817 | 30.7 | 103.0 | 2.38 |
| vfs:archive | 8 | 77.7k | 19424 | 52.9 | 163.3 | 1.37 |
| vfs:archive-xor | 8 | 52.8k | 13197 | 76.5 | 191.3 | 0.93 |
| vfs:archive-lz4 | 8 | 36.4k | 9100 | 103.4 | 279.2 | 0.64 |
| vfs:archive+native | 8 | 71.0k | 17756 | 58.8 | 172.3 | 1.25 |
| vfs:memory+archive | 8 | 75.4k | 18852 | 60.1 | 163.6 | 1.33 |
| vfs:3layer | 8 | 66.4k | 16603 | 55.4 | 182.1 | 1.17 |
| vfs:nested8 | 8 | 67.8k | 16944 | 77.6 | 185.6 | 1.19 |

### read.large.seq

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 2.8k | 2790 | 300.5 | 1723.8 | 1.00 |
| base:fstream | 1 | 1.3k | 1265 | 769.2 | 1108.0 | 0.45 |
| base:posix | 1 | 3.1k | 3142 | 284.4 | 710.9 | 1.13 |
| vfs:native | 1 | 3.1k | 3052 | 317.4 | 524.2 | 1.09 |
| vfs:memory | 1 | 11.6k | 11591 | 75.1 | 251.9 | 4.15 |
| vfs:archive | 1 | 4.7k | 4720 | 194.6 | 353.3 | 1.69 |
| vfs:archive-xor | 1 | 1.7k | 1718 | 537.7 | 946.4 | 0.62 |
| vfs:archive-lz4 | 1 | 1.0k | 1033 | 931.6 | 1523.6 | 0.37 |
| vfs:archive+native | 1 | 4.5k | 4547 | 198.0 | 495.9 | 1.63 |
| vfs:memory+archive | 1 | 5.0k | 4951 | 184.1 | 373.1 | 1.77 |
| vfs:3layer | 1 | 4.6k | 4562 | 202.7 | 402.3 | 1.63 |
| vfs:nested8 | 1 | 2.9k | 2932 | 338.0 | 590.4 | 1.05 |
| base:stdio | 2 | 6.6k | 6649 | 274.4 | 541.2 | 1.00 |
| base:fstream | 2 | 2.5k | 2458 | 747.5 | 1562.9 | 0.37 |
| base:posix | 2 | 6.3k | 6297 | 296.2 | 494.5 | 0.95 |
| vfs:native | 2 | 6.7k | 6698 | 276.4 | 467.5 | 1.01 |
| vfs:memory | 2 | 22.4k | 22413 | 70.9 | 447.0 | 3.37 |
| vfs:archive | 2 | 7.5k | 7524 | 233.3 | 510.9 | 1.13 |
| vfs:archive-xor | 2 | 4.6k | 4597 | 416.7 | 756.8 | 0.69 |
| vfs:archive-lz4 | 2 | 1.8k | 1829 | 1028.6 | 2175.0 | 0.28 |
| vfs:archive+native | 2 | 7.9k | 7944 | 236.8 | 450.4 | 1.19 |
| vfs:memory+archive | 2 | 8.5k | 8517 | 207.6 | 502.9 | 1.28 |
| vfs:3layer | 2 | 8.2k | 8161 | 226.5 | 462.9 | 1.23 |
| vfs:nested8 | 2 | 6.5k | 6503 | 272.8 | 526.5 | 0.98 |
| base:stdio | 4 | 10.7k | 10685 | 275.7 | 715.5 | 1.00 |
| base:fstream | 4 | 7.9k | 7853 | 415.6 | 1209.5 | 0.73 |
| base:posix | 4 | 12.1k | 12125 | 203.1 | 483.5 | 1.13 |
| vfs:native | 4 | 13.3k | 13308 | 193.3 | 1043.8 | 1.25 |
| vfs:memory | 4 | 23.9k | 23933 | 87.4 | 151.2 | 2.24 |
| vfs:archive | 4 | 12.2k | 12230 | 165.9 | 669.4 | 1.14 |
| vfs:archive-xor | 4 | 8.6k | 8567 | 274.0 | 925.1 | 0.80 |
| vfs:archive-lz4 | 4 | 6.5k | 6518 | 399.4 | 1412.4 | 0.61 |
| vfs:archive+native | 4 | 17.2k | 17162 | 151.5 | 564.6 | 1.61 |
| vfs:memory+archive | 4 | 14.4k | 14356 | 170.1 | 648.6 | 1.34 |
| vfs:3layer | 4 | 14.7k | 14729 | 164.8 | 1422.0 | 1.38 |
| vfs:nested8 | 4 | 10.6k | 10645 | 218.2 | 653.6 | 1.00 |
| base:stdio | 8 | 17.3k | 17253 | 255.4 | 757.2 | 1.00 |
| base:fstream | 8 | 10.2k | 10176 | 396.9 | 1198.5 | 0.59 |
| base:posix | 8 | 13.2k | 13212 | 258.3 | 1980.6 | 0.77 |
| vfs:native | 8 | 13.2k | 13169 | 239.2 | 2681.1 | 0.76 |
| vfs:memory | 8 | 37.4k | 37419 | 90.9 | 1635.4 | 2.17 |
| vfs:archive | 8 | 20.1k | 20092 | 185.5 | 1527.6 | 1.16 |
| vfs:archive-xor | 8 | 14.4k | 14382 | 272.4 | 1346.1 | 0.83 |
| vfs:archive-lz4 | 8 | 10.0k | 9957 | 409.6 | 1220.2 | 0.58 |
| vfs:archive+native | 8 | 23.5k | 23537 | 190.2 | 816.0 | 1.36 |
| vfs:memory+archive | 8 | 21.5k | 21548 | 213.1 | 1710.1 | 1.25 |
| vfs:3layer | 8 | 22.3k | 22309 | 200.4 | 1601.8 | 1.29 |
| vfs:nested8 | 8 | 18.8k | 18843 | 243.6 | 1134.4 | 1.09 |

### read.large.rand

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 275.7k | 1077 | 2.76 | 7.14 | 1.00 |
| base:fstream | 1 | 306.9k | 1199 | 3.12 | 5.62 | 1.11 |
| base:posix | 1 | 322.9k | 1261 | 2.81 | 4.38 | 1.17 |
| vfs:native | 1 | 324.0k | 1266 | 2.87 | 4.53 | 1.18 |
| vfs:memory | 1 | 1.49M | 5803 | 0.52 | 0.83 | 5.39 |
| vfs:archive | 1 | 574.7k | 2245 | 1.51 | 2.55 | 2.08 |
| vfs:archive-xor | 1 | 433.0k | 1692 | 2.03 | 2.92 | 1.57 |
| vfs:archive-lz4 | 1 | 41.4k | 162 | 20.3 | 49.7 | 0.15 |
| vfs:archive+native | 1 | 459.6k | 1795 | 1.51 | 2.81 | 1.67 |
| vfs:memory+archive | 1 | 449.7k | 1757 | 1.56 | 2.50 | 1.63 |
| vfs:3layer | 1 | 589.7k | 2304 | 1.46 | 2.34 | 2.14 |
| vfs:nested8 | 1 | 377.7k | 1475 | 2.29 | 3.33 | 1.37 |
| base:stdio | 2 | 661.0k | 2582 | 2.55 | 4.12 | 1.00 |
| base:fstream | 2 | 540.4k | 2111 | 3.38 | 4.48 | 0.82 |
| base:posix | 2 | 1.10M | 4305 | 1.61 | 2.50 | 1.67 |
| vfs:native | 2 | 795.3k | 3107 | 2.29 | 3.28 | 1.20 |
| vfs:memory | 2 | 2.36M | 9206 | 0.68 | 1.72 | 3.57 |
| vfs:archive | 2 | 1.13M | 4406 | 1.41 | 2.40 | 1.71 |
| vfs:archive-xor | 2 | 833.9k | 3257 | 1.82 | 2.66 | 1.26 |
| vfs:archive-lz4 | 2 | 76.4k | 298 | 23.1 | 51.0 | 0.12 |
| vfs:archive+native | 2 | 966.5k | 3775 | 1.82 | 2.97 | 1.46 |
| vfs:memory+archive | 2 | 1.04M | 4060 | 1.67 | 2.66 | 1.57 |
| vfs:3layer | 2 | 1.10M | 4312 | 1.30 | 2.34 | 1.67 |
| vfs:nested8 | 2 | 531.8k | 2077 | 2.71 | 5.68 | 0.80 |
| base:stdio | 4 | 1.12M | 4369 | 2.45 | 5.52 | 1.00 |
| base:fstream | 4 | 1.10M | 4286 | 3.12 | 4.69 | 0.98 |
| base:posix | 4 | 1.48M | 5765 | 1.88 | 3.96 | 1.32 |
| vfs:native | 4 | 1.09M | 4244 | 2.45 | 5.10 | 0.97 |
| vfs:memory | 4 | 2.42M | 9448 | 0.83 | 18.2 | 2.16 |
| vfs:archive | 4 | 1.40M | 5452 | 1.82 | 4.17 | 1.25 |
| vfs:archive-xor | 4 | 1.48M | 5781 | 1.72 | 4.90 | 1.32 |
| vfs:archive-lz4 | 4 | 142.1k | 555 | 24.6 | 53.3 | 0.13 |
| vfs:archive+native | 4 | 1.57M | 6125 | 1.72 | 4.22 | 1.40 |
| vfs:memory+archive | 4 | 1.79M | 6980 | 1.82 | 3.54 | 1.60 |
| vfs:3layer | 4 | 1.96M | 7648 | 1.72 | 3.18 | 1.75 |
| vfs:nested8 | 4 | 863.4k | 3373 | 2.55 | 5.47 | 0.77 |
| base:stdio | 8 | 1.97M | 7688 | 2.50 | 5.99 | 1.00 |
| base:fstream | 8 | 1.11M | 4352 | 3.49 | 7.87 | 0.57 |
| base:posix | 8 | 2.00M | 7803 | 1.98 | 4.17 | 1.01 |
| vfs:native | 8 | 1.44M | 5622 | 2.40 | 5.21 | 0.73 |
| vfs:memory | 8 | 2.43M | 9500 | 0.89 | 58.9 | 1.24 |
| vfs:archive | 8 | 2.34M | 9139 | 1.82 | 4.06 | 1.19 |
| vfs:archive-xor | 8 | 1.44M | 5621 | 2.24 | 6.04 | 0.73 |
| vfs:archive-lz4 | 8 | 172.0k | 672 | 24.4 | 62.0 | 0.09 |
| vfs:archive+native | 8 | 2.15M | 8407 | 1.93 | 4.06 | 1.09 |
| vfs:memory+archive | 8 | 2.06M | 8053 | 1.88 | 4.06 | 1.05 |
| vfs:3layer | 8 | 2.35M | 9193 | 1.82 | 3.96 | 1.20 |
| vfs:nested8 | 8 | 1.76M | 6865 | 2.45 | 5.47 | 0.89 |

### meta.open

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 240.7k | - | 3.59 | 8.59 | 1.00 |
| base:fstream | 1 | 237.0k | - | 4.01 | 6.46 | 0.98 |
| base:posix | 1 | 247.7k | - | 3.59 | 7.50 | 1.03 |
| vfs:native | 1 | 192.8k | - | 4.48 | 21.2 | 0.80 |
| vfs:memory | 1 | 908.9k | - | 0.89 | 1.93 | 3.78 |
| vfs:archive | 1 | 1.36M | - | 0.68 | 0.83 | 5.63 |
| vfs:archive-xor | 1 | 839.0k | - | 0.68 | 2.24 | 3.49 |
| vfs:archive-lz4 | 1 | 1.27M | - | 0.68 | 1.09 | 5.28 |
| vfs:archive+native | 1 | 357.0k | - | 1.93 | 10.3 | 1.48 |
| vfs:memory+archive | 1 | 366.7k | - | 2.50 | 3.59 | 1.52 |
| vfs:3layer | 1 | 271.2k | - | 2.08 | 8.44 | 1.13 |
| vfs:nested8 | 1 | 120.8k | - | 6.88 | 34.6 | 0.50 |
| base:stdio | 2 | 392.2k | - | 4.22 | 13.9 | 1.00 |
| base:fstream | 2 | 375.5k | - | 4.17 | 15.2 | 0.96 |
| base:posix | 2 | 425.1k | - | 3.80 | 12.3 | 1.08 |
| vfs:native | 2 | 340.9k | - | 4.90 | 18.6 | 0.87 |
| vfs:memory | 2 | 1.16M | - | 1.20 | 2.97 | 2.97 |
| vfs:archive | 2 | 1.61M | - | 1.09 | 1.46 | 4.11 |
| vfs:archive-xor | 2 | 1.57M | - | 0.83 | 2.40 | 4.00 |
| vfs:archive-lz4 | 2 | 1.87M | - | 0.89 | 1.25 | 4.77 |
| vfs:archive+native | 2 | 563.9k | - | 2.08 | 8.02 | 1.44 |
| vfs:memory+archive | 2 | 1.30M | - | 1.04 | 3.28 | 3.31 |
| vfs:3layer | 2 | 671.5k | - | 2.19 | 7.55 | 1.71 |
| vfs:nested8 | 2 | 190.9k | - | 8.65 | 29.8 | 0.49 |
| base:stdio | 4 | 775.4k | - | 4.32 | 12.8 | 1.00 |
| base:fstream | 4 | 673.5k | - | 5.10 | 8.91 | 0.87 |
| base:posix | 4 | 836.6k | - | 4.17 | 6.88 | 1.08 |
| vfs:native | 4 | 626.7k | - | 5.57 | 14.9 | 0.81 |
| vfs:memory | 4 | 1.07M | - | 3.33 | 6.56 | 1.37 |
| vfs:archive | 4 | 1.21M | - | 2.40 | 5.99 | 1.56 |
| vfs:archive-xor | 4 | 1.16M | - | 3.12 | 6.20 | 1.49 |
| vfs:archive-lz4 | 4 | 1.03M | - | 3.70 | 6.82 | 1.32 |
| vfs:archive+native | 4 | 757.1k | - | 4.53 | 9.74 | 0.98 |
| vfs:memory+archive | 4 | 958.9k | - | 3.91 | 7.29 | 1.24 |
| vfs:3layer | 4 | 712.5k | - | 5.31 | 8.75 | 0.92 |
| vfs:nested8 | 4 | 319.9k | - | 10.9 | 30.2 | 0.41 |
| base:stdio | 8 | 1.03M | - | 4.38 | 12.9 | 1.00 |
| base:fstream | 8 | 984.8k | - | 5.00 | 14.2 | 0.96 |
| base:posix | 8 | 1.14M | - | 4.38 | 12.2 | 1.11 |
| vfs:native | 8 | 710.7k | - | 7.66 | 20.2 | 0.69 |
| vfs:memory | 8 | 769.7k | - | 8.49 | 15.3 | 0.75 |
| vfs:archive | 8 | 795.1k | - | 8.02 | 15.1 | 0.77 |
| vfs:archive-xor | 8 | 790.9k | - | 8.18 | 15.4 | 0.77 |
| vfs:archive-lz4 | 8 | 802.2k | - | 8.02 | 15.2 | 0.78 |
| vfs:archive+native | 8 | 575.4k | - | 10.8 | 20.3 | 0.56 |
| vfs:memory+archive | 8 | 751.0k | - | 7.71 | 15.5 | 0.73 |
| vfs:3layer | 8 | 494.3k | - | 14.7 | 25.7 | 0.48 |
| vfs:nested8 | 8 | 268.4k | - | 23.6 | 56.6 | 0.26 |

### meta.stat

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 97.8k | - | 8.44 | 37.5 | 1.00 |
| vfs:native | 1 | 135.1k | - | 3.85 | 27.2 | 1.38 |
| vfs:memory | 1 | 270.2k | - | 3.23 | 5.10 | 2.76 |
| vfs:archive | 1 | 358.3k | - | 2.50 | 3.44 | 3.66 |
| vfs:archive-xor | 1 | 283.5k | - | 1.93 | 6.67 | 2.90 |
| vfs:archive-lz4 | 1 | 353.8k | - | 2.50 | 3.02 | 3.62 |
| vfs:archive+native | 1 | 152.9k | - | 4.84 | 11.8 | 1.56 |
| vfs:memory+archive | 1 | 270.5k | - | 2.66 | 7.87 | 2.77 |
| vfs:3layer | 1 | 151.6k | - | 5.16 | 10.7 | 1.55 |
| vfs:nested8 | 1 | 106.3k | - | 5.42 | 45.3 | 1.09 |
| base:stdio | 2 | 309.1k | - | 3.96 | 20.0 | 1.00 |
| vfs:native | 2 | 263.1k | - | 5.94 | 24.2 | 0.85 |
| vfs:memory | 2 | 432.5k | - | 3.02 | 10.3 | 1.40 |
| vfs:archive | 2 | 967.2k | - | 1.46 | 2.29 | 3.13 |
| vfs:archive-xor | 2 | 881.0k | - | 1.41 | 4.38 | 2.85 |
| vfs:archive-lz4 | 2 | 1.27M | - | 1.04 | 5.62 | 4.09 |
| vfs:archive+native | 2 | 320.7k | - | 4.17 | 21.7 | 1.04 |
| vfs:memory+archive | 2 | 526.9k | - | 3.28 | 7.66 | 1.70 |
| vfs:3layer | 2 | 302.0k | - | 4.48 | 17.0 | 0.98 |
| vfs:nested8 | 2 | 190.5k | - | 6.51 | 45.5 | 0.62 |
| base:stdio | 4 | 502.5k | - | 5.89 | 34.5 | 1.00 |
| vfs:native | 4 | 470.9k | - | 3.70 | 28.5 | 0.94 |
| vfs:memory | 4 | 986.1k | - | 2.03 | 9.38 | 1.96 |
| vfs:archive | 4 | 975.8k | - | 3.49 | 10.4 | 1.94 |
| vfs:archive-xor | 4 | 866.4k | - | 3.59 | 10.4 | 1.72 |
| vfs:archive-lz4 | 4 | 758.5k | - | 3.80 | 12.6 | 1.51 |
| vfs:archive+native | 4 | 550.8k | - | 4.22 | 27.8 | 1.10 |
| vfs:memory+archive | 4 | 805.1k | - | 2.29 | 11.3 | 1.60 |
| vfs:3layer | 4 | 557.4k | - | 2.03 | 29.3 | 1.11 |
| vfs:nested8 | 4 | 422.9k | - | 7.19 | 28.1 | 0.84 |
| base:stdio | 8 | 1.03M | - | 4.32 | 17.0 | 1.00 |
| vfs:native | 8 | 872.2k | - | 6.35 | 14.8 | 0.85 |
| vfs:memory | 8 | 1.19M | - | 5.10 | 11.1 | 1.15 |
| vfs:archive | 8 | 1.26M | - | 4.84 | 11.8 | 1.22 |
| vfs:archive-xor | 8 | 1.35M | - | 4.69 | 11.7 | 1.31 |
| vfs:archive-lz4 | 8 | 1.23M | - | 5.57 | 12.6 | 1.19 |
| vfs:archive+native | 8 | 730.6k | - | 8.70 | 17.4 | 0.71 |
| vfs:memory+archive | 8 | 985.5k | - | 7.39 | 15.0 | 0.96 |
| vfs:3layer | 8 | 643.1k | - | 10.0 | 19.6 | 0.62 |
| vfs:nested8 | 8 | 321.1k | - | 22.3 | 37.3 | 0.31 |

### meta.miss

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 533.6k | - | 1.46 | 11.9 | 1.00 |
| vfs:native | 1 | 160.9k | - | 2.55 | 23.8 | 0.30 |
| vfs:memory | 1 | 261.3k | - | 3.07 | 5.42 | 0.49 |
| vfs:archive | 1 | 280.0k | - | 3.02 | 4.48 | 0.52 |
| vfs:archive-xor | 1 | 407.9k | - | 1.67 | 6.82 | 0.76 |
| vfs:archive-lz4 | 1 | 272.0k | - | 3.70 | 4.69 | 0.51 |
| vfs:archive+native | 1 | 144.5k | - | 5.52 | 11.8 | 0.27 |
| vfs:memory+archive | 1 | 252.7k | - | 3.28 | 11.2 | 0.47 |
| vfs:3layer | 1 | 129.0k | - | 7.71 | 30.4 | 0.24 |
| vfs:nested8 | 1 | 122.4k | - | 4.22 | 40.1 | 0.23 |
| base:stdio | 2 | 457.2k | - | 3.23 | 7.71 | 1.00 |
| vfs:native | 2 | 292.2k | - | 5.16 | 23.9 | 0.64 |
| vfs:memory | 2 | 531.1k | - | 2.19 | 12.1 | 1.16 |
| vfs:archive | 2 | 481.3k | - | 3.18 | 4.64 | 1.05 |
| vfs:archive-xor | 2 | 797.5k | - | 2.13 | 3.54 | 1.74 |
| vfs:archive-lz4 | 2 | 891.9k | - | 1.56 | 5.05 | 1.95 |
| vfs:archive+native | 2 | 266.3k | - | 5.62 | 26.3 | 0.58 |
| vfs:memory+archive | 2 | 416.0k | - | 4.27 | 8.70 | 0.91 |
| vfs:3layer | 2 | 267.2k | - | 4.84 | 26.7 | 0.58 |
| vfs:nested8 | 2 | 201.1k | - | 6.30 | 42.9 | 0.44 |
| base:stdio | 4 | 762.4k | - | 3.07 | 9.69 | 1.00 |
| vfs:native | 4 | 555.2k | - | 3.54 | 18.3 | 0.73 |
| vfs:memory | 4 | 771.7k | - | 1.15 | 18.6 | 1.01 |
| vfs:archive | 4 | 612.3k | - | 5.10 | 20.2 | 0.80 |
| vfs:archive-xor | 4 | 693.9k | - | 3.59 | 17.7 | 0.91 |
| vfs:archive-lz4 | 4 | 788.9k | - | 4.17 | 13.4 | 1.03 |
| vfs:archive+native | 4 | 494.8k | - | 4.22 | 24.7 | 0.65 |
| vfs:memory+archive | 4 | 568.8k | - | 5.31 | 21.1 | 0.75 |
| vfs:3layer | 4 | 440.5k | - | 6.15 | 36.3 | 0.58 |
| vfs:nested8 | 4 | 361.1k | - | 7.34 | 39.3 | 0.47 |
| base:stdio | 8 | 1.78M | - | 2.40 | 9.22 | 1.00 |
| vfs:native | 8 | 565.3k | - | 13.3 | 22.6 | 0.32 |
| vfs:memory | 8 | 711.1k | - | 9.43 | 19.1 | 0.40 |
| vfs:archive | 8 | 734.1k | - | 8.96 | 19.1 | 0.41 |
| vfs:archive-xor | 8 | 716.5k | - | 9.38 | 19.1 | 0.40 |
| vfs:archive-lz4 | 8 | 725.2k | - | 9.79 | 19.3 | 0.41 |
| vfs:archive+native | 8 | 512.7k | - | 13.4 | 24.9 | 0.29 |
| vfs:memory+archive | 8 | 647.2k | - | 9.58 | 21.7 | 0.36 |
| vfs:3layer | 8 | 444.8k | - | 16.1 | 26.9 | 0.25 |
| vfs:nested8 | 8 | 320.9k | - | 21.4 | 38.6 | 0.18 |

### meta.list

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 122 | - | 4060.9 | 34246.4 | 1.00 |
| vfs:native | 1 | 84 | - | 7727.5 | 46747.1 | 0.69 |
| vfs:memory | 1 | 125 | - | 2943.2 | 28964.6 | 1.03 |
| vfs:archive | 1 | 129 | - | 7029.5 | 13946.6 | 1.06 |
| vfs:archive-xor | 1 | 129 | - | 7106.6 | 14573.1 | 1.06 |
| vfs:archive-lz4 | 1 | 132 | - | 2800.3 | 27829.0 | 1.09 |
| vfs:archive+native | 1 | 124 | - | 3041.3 | 30394.3 | 1.02 |
| vfs:memory+archive | 1 | 127 | - | 2868.4 | 30078.3 | 1.05 |
| vfs:3layer | 1 | 163 | - | 2613.1 | 25794.2 | 1.34 |
| vfs:nested8 | 1 | 90 | - | 7297.0 | 51103.6 | 0.74 |
| base:stdio | 2 | 200 | - | 4120.2 | 39612.1 | 1.00 |
| vfs:native | 2 | 159 | - | 7597.6 | 51490.9 | 0.79 |
| vfs:memory | 2 | 246 | - | 6475.4 | 25472.0 | 1.23 |
| vfs:archive | 2 | 260 | - | 5640.3 | 28340.9 | 1.30 |
| vfs:archive-xor | 2 | 243 | - | 7141.8 | 32815.8 | 1.22 |
| vfs:archive-lz4 | 2 | 255 | - | 6796.4 | 29785.3 | 1.28 |
| vfs:archive+native | 2 | 251 | - | 5685.1 | 29031.1 | 1.26 |
| vfs:memory+archive | 2 | 239 | - | 6251.0 | 28251.2 | 1.19 |
| vfs:3layer | 2 | 252 | - | 6463.0 | 28188.5 | 1.26 |
| vfs:nested8 | 2 | 170 | - | 7717.1 | 48802.7 | 0.85 |
| base:stdio | 4 | 449 | - | 5933.7 | 39644.1 | 1.00 |
| vfs:native | 4 | 228 | - | 16373.7 | 55632.7 | 0.51 |
| vfs:memory | 4 | 598 | - | 5101.6 | 23140.6 | 1.33 |
| vfs:archive | 4 | 571 | - | 5223.5 | 19936.7 | 1.27 |
| vfs:archive-xor | 4 | 537 | - | 6086.5 | 19399.3 | 1.19 |
| vfs:archive-lz4 | 4 | 571 | - | 4424.0 | 23642.5 | 1.27 |
| vfs:archive+native | 4 | 564 | - | 5526.0 | 19158.7 | 1.25 |
| vfs:memory+archive | 4 | 526 | - | 5513.5 | 26253.2 | 1.17 |
| vfs:3layer | 4 | 598 | - | 4256.1 | 20601.4 | 1.33 |
| vfs:nested8 | 4 | 228 | - | 17030.8 | 40192.1 | 0.51 |
| base:stdio | 8 | 266 | - | 29472.7 | 48312.9 | 1.00 |
| vfs:native | 8 | 160 | - | 50505.4 | 65471.8 | 0.60 |
| vfs:memory | 8 | 496 | - | 14692.6 | 31474.7 | 1.86 |
| vfs:archive | 8 | 533 | - | 13427.3 | 30255.8 | 2.00 |
| vfs:archive-xor | 8 | 474 | - | 15961.4 | 28370.5 | 1.78 |
| vfs:archive-lz4 | 8 | 459 | - | 17012.0 | 24533.5 | 1.72 |
| vfs:archive+native | 8 | 462 | - | 16281.8 | 30999.0 | 1.73 |
| vfs:memory+archive | 8 | 463 | - | 16345.6 | 34652.1 | 1.74 |
| vfs:3layer | 8 | 464 | - | 16323.7 | 35506.6 | 1.74 |
| vfs:nested8 | 8 | 162 | - | 49837.9 | 65763.9 | 0.61 |

### write.create

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 4.7k | 293 | 178.9 | 879.5 | 1.00 |
| base:fstream | 1 | 4.3k | 270 | 104.1 | 1084.1 | 0.92 |
| base:posix | 1 | 4.5k | 281 | 182.9 | 587.8 | 0.96 |
| vfs:native | 1 | 4.7k | 293 | 182.0 | 1359.9 | 1.00 |
| vfs:memory | 1 | 27.3k | 1705 | 32.5 | 109.1 | 5.82 |
| vfs:memory+archive | 1 | 25.2k | 1578 | 34.3 | 182.9 | 5.39 |
| base:stdio | 2 | 10.1k | 634 | 169.2 | 625.0 | 1.00 |
| base:fstream | 2 | 7.1k | 446 | 256.4 | 906.0 | 0.70 |
| base:posix | 2 | 9.9k | 618 | 169.1 | 603.1 | 0.97 |
| vfs:native | 2 | 8.1k | 506 | 187.7 | 635.7 | 0.80 |
| vfs:memory | 2 | 27.9k | 1746 | 57.0 | 166.7 | 2.75 |
| vfs:memory+archive | 2 | 28.1k | 1754 | 58.4 | 159.1 | 2.77 |
| base:stdio | 4 | 19.9k | 1241 | 161.5 | 766.5 | 1.00 |
| base:fstream | 4 | 13.5k | 845 | 154.7 | 921.6 | 0.68 |
| base:posix | 4 | 16.2k | 1015 | 132.7 | 684.6 | 0.82 |
| vfs:native | 4 | 15.7k | 981 | 139.4 | 1952.3 | 0.79 |
| vfs:memory | 4 | 113.0k | 7065 | 10.6 | 415.7 | 5.69 |
| vfs:memory+archive | 4 | 122.8k | 7673 | 8.65 | 94.9 | 6.18 |
| base:stdio | 8 | 70.9k | 4430 | 51.0 | 194.7 | 1.00 |
| base:fstream | 8 | 50.3k | 3146 | 90.7 | 338.6 | 0.71 |
| base:posix | 8 | 53.4k | 3340 | 56.5 | 295.3 | 0.75 |
| vfs:native | 8 | 59.0k | 3689 | 66.4 | 1233.6 | 0.83 |
| vfs:memory | 8 | 156.6k | 9786 | 21.8 | 636.4 | 2.21 |
| vfs:memory+archive | 8 | 172.0k | 10750 | 10.8 | 74.8 | 2.43 |

### write.churn

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 18.4k | - | 40.0 | 175.8 | 1.00 |
| base:fstream | 1 | 20.0k | - | 38.7 | 168.0 | 1.09 |
| base:posix | 1 | 13.8k | - | 70.5 | 199.3 | 0.75 |
| vfs:native | 1 | 11.3k | - | 52.6 | 414.2 | 0.61 |
| vfs:memory | 1 | 86.4k | - | 11.0 | 11.7 | 4.68 |
| vfs:memory+archive | 1 | 115.3k | - | 7.60 | 11.0 | 6.25 |
| base:stdio | 2 | 22.2k | - | 49.0 | 270.1 | 1.00 |
| base:fstream | 2 | 23.5k | - | 62.4 | 242.0 | 1.06 |
| base:posix | 2 | 25.4k | - | 66.5 | 316.0 | 1.14 |
| vfs:native | 2 | 22.5k | - | 57.8 | 406.6 | 1.02 |
| vfs:memory | 2 | 128.3k | - | 12.6 | 61.3 | 5.79 |
| vfs:memory+archive | 2 | 172.9k | - | 10.5 | 14.1 | 7.80 |
| base:stdio | 4 | 64.7k | - | 41.1 | 310.7 | 1.00 |
| base:fstream | 4 | 65.9k | - | 44.3 | 245.2 | 1.02 |
| base:posix | 4 | 61.7k | - | 39.7 | 160.0 | 0.95 |
| vfs:native | 4 | 57.4k | - | 55.8 | 204.8 | 0.89 |
| vfs:memory | 4 | 242.8k | - | 10.3 | 41.1 | 3.75 |
| vfs:memory+archive | 4 | 239.3k | - | 10.2 | 42.0 | 3.70 |
| base:stdio | 8 | 99.1k | - | 46.8 | 465.9 | 1.00 |
| base:fstream | 8 | 93.2k | - | 46.6 | 491.9 | 0.94 |
| base:posix | 8 | 105.9k | - | 45.4 | 437.8 | 1.07 |
| vfs:native | 8 | 82.3k | - | 61.1 | 533.3 | 0.83 |
| vfs:memory | 8 | 302.5k | - | 12.8 | 188.8 | 3.05 |
| vfs:memory+archive | 8 | 272.9k | - | 17.2 | 182.6 | 2.75 |

### write.large.seq

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 930 | 930 | 906.4 | 1731.2 | 1.00 |
| base:posix | 1 | 1.1k | 1140 | 824.3 | 1375.1 | 1.23 |
| vfs:native | 1 | 1.3k | 1251 | 775.8 | 1414.6 | 1.34 |
| vfs:memory | 1 | 922 | 922 | 379.5 | 12867.9 | 0.99 |
| vfs:memory+archive | 1 | 861 | 861 | 370.6 | 16521.7 | 0.93 |
| base:stdio | 2 | 2.4k | 2357 | 850.9 | 1530.4 | 1.00 |
| base:posix | 2 | 2.6k | 2618 | 689.7 | 1375.7 | 1.11 |
| vfs:native | 2 | 2.6k | 2600 | 728.9 | 1377.0 | 1.10 |
| vfs:memory | 2 | 1.6k | 1567 | 500.8 | 16823.8 | 0.66 |
| vfs:memory+archive | 2 | 1.4k | 1426 | 437.3 | 17857.2 | 0.61 |
| base:stdio | 4 | 7.4k | 7371 | 371.2 | 1314.4 | 1.00 |
| base:posix | 4 | 7.3k | 7319 | 387.0 | 925.9 | 0.99 |
| vfs:native | 4 | 7.4k | 7361 | 408.0 | 805.1 | 1.00 |
| vfs:memory | 4 | 3.4k | 3428 | 339.2 | 17559.5 | 0.47 |
| vfs:memory+archive | 4 | 3.6k | 3628 | 335.5 | 19515.5 | 0.49 |
| base:stdio | 8 | 9.4k | 9382 | 431.0 | 1716.8 | 1.00 |
| base:posix | 8 | 8.7k | 8749 | 424.1 | 3068.4 | 0.93 |
| vfs:native | 8 | 7.7k | 7740 | 443.9 | 1603.9 | 0.82 |
| vfs:memory | 8 | 4.6k | 4552 | 416.1 | 30834.9 | 0.49 |
| vfs:memory+archive | 8 | 3.6k | 3627 | 474.1 | 31577.1 | 0.39 |

### write.overwrite

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 99.8k | 390 | 6.88 | 25.2 | 1.00 |
| base:posix | 1 | 95.2k | 372 | 6.51 | 20.3 | 0.95 |
| vfs:native | 1 | 104.4k | 408 | 8.12 | 13.9 | 1.05 |
| vfs:memory | 1 | 724.5k | 2830 | 1.09 | 1.35 | 7.26 |
| vfs:memory+archive | 1 | 723.7k | 2827 | 1.09 | 1.35 | 7.25 |
| base:stdio | 2 | 148.2k | 579 | 8.65 | 26.2 | 1.00 |
| base:posix | 2 | 190.1k | 743 | 7.60 | 20.8 | 1.28 |
| vfs:native | 2 | 176.2k | 688 | 7.66 | 23.6 | 1.19 |
| vfs:memory | 2 | 643.2k | 2513 | 2.03 | 3.02 | 4.34 |
| vfs:memory+archive | 2 | 608.8k | 2378 | 2.08 | 3.33 | 4.11 |
| base:stdio | 4 | 425.9k | 1664 | 7.19 | 21.9 | 1.00 |
| base:posix | 4 | 415.6k | 1623 | 4.48 | 18.8 | 0.98 |
| vfs:native | 4 | 402.8k | 1573 | 5.73 | 21.1 | 0.95 |
| vfs:memory | 4 | 1.29M | 5024 | 1.09 | 2.87 | 3.02 |
| vfs:memory+archive | 4 | 1.25M | 4889 | 1.09 | 2.92 | 2.94 |
| base:stdio | 8 | 776.3k | 3032 | 4.58 | 15.8 | 1.00 |
| base:posix | 8 | 867.2k | 3388 | 2.55 | 9.01 | 1.12 |
| vfs:native | 8 | 847.6k | 3311 | 3.85 | 11.7 | 1.09 |
| vfs:memory | 8 | 2.04M | 7986 | 0.94 | 2.92 | 2.63 |
| vfs:memory+archive | 8 | 1.84M | 7173 | 0.94 | 3.02 | 2.37 |

### write.append

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 72.8k | 284 | 11.6 | 49.7 | 1.00 |
| base:posix | 1 | 78.5k | 307 | 11.4 | 26.5 | 1.08 |
| vfs:native | 1 | 71.8k | 280 | 12.6 | 38.4 | 0.99 |
| vfs:memory | 1 | 80.3k | 314 | 3.04 | 403.6 | 1.10 |
| vfs:memory+archive | 1 | 87.1k | 340 | 2.97 | 388.7 | 1.20 |
| base:stdio | 2 | 124.0k | 484 | 9.42 | 51.5 | 1.00 |
| base:posix | 2 | 132.2k | 516 | 9.64 | 38.2 | 1.07 |
| vfs:native | 2 | 145.7k | 569 | 12.8 | 58.6 | 1.17 |
| vfs:memory | 2 | 190.0k | 742 | 4.22 | 215.7 | 1.53 |
| vfs:memory+archive | 2 | 179.6k | 702 | 4.11 | 268.0 | 1.45 |
| base:stdio | 4 | 259.9k | 1015 | 13.3 | 70.1 | 1.00 |
| base:posix | 4 | 334.7k | 1307 | 8.77 | 38.7 | 1.29 |
| vfs:native | 4 | 259.3k | 1013 | 12.9 | 58.5 | 1.00 |
| vfs:memory | 4 | 304.7k | 1190 | 3.13 | 222.2 | 1.17 |
| vfs:memory+archive | 4 | 413.7k | 1616 | 3.02 | 219.7 | 1.59 |
| base:stdio | 8 | 570.8k | 2230 | 5.23 | 33.3 | 1.00 |
| base:posix | 8 | 601.6k | 2350 | 4.30 | 27.1 | 1.05 |
| vfs:native | 8 | 570.9k | 2230 | 4.69 | 30.7 | 1.00 |
| vfs:memory | 8 | 738.8k | 2886 | 1.64 | 157.1 | 1.29 |
| vfs:memory+archive | 8 | 678.8k | 2652 | 1.64 | 195.7 | 1.19 |

<!-- apk-assets:begin -->
## Assets inside the APK (bench APK)

`AndroidAssetBackend` needs an `AAssetManager`, which a command-line process started from `adb shell` does not have, so these targets run in a separate NativeActivity APK built by [`bench/android/build.sh`](../../bench/android/build.sh) on the same benchmark core (`bench/BenchCore.cpp`). The APK carries the same dataset as the command-line run (generated by `tests/android-apk/gen_assets.py --bench`) under `assets/bench/data/`, plus `assets/bench/plain.vfs` packed from it and stored uncompressed. At start it extracts the dataset and `plain.vfs` to the app's internal storage.

- `base:stdio` / `vfs:archive`: the extracted copies in internal storage (f2fs)
- `base:aasset`: raw `AAssetManager_open` / `AAsset_read` / `AAsset_seek64` (`AASSET_MODE_RANDOM`), `AAsset_getLength64` as stat, `AAssetDir` as list
- `vfs:asset`: `AndroidAssetBackend` on `assets/bench/data` in probe mode (no manifest)
- `vfs:asset+lst`: the same with the `assets.lst` manifest
- `vfs:archive-apk`: `plain.vfs` read in place from the APK through `AssetBackend::archiveSource` + `ArchiveBackend`
- Run: `am start -n com.vfs.bench/android.app.NativeActivity` with the screen on (a foreground app, unlike the `adb shell` run above), `--threads 1,2,4,8`, median of 3, warm cache; results read with `run-as com.vfs.bench`. `meta.list` comes from a second launch of the same APK with `--filter meta.list`.
- Table: ops/s and, in parentheses, the multiple of `base:stdio` of the same APK run. The loose dataset files are stored uncompressed in the APK (the build script's default)


| Scenario | Threads | base:stdio | base:aasset | asset | asset+lst | archive-apk | archive |
|---|---:|---:|---:|---:|---:|---:|---:|
| read.small | 1 | 135.4k | 110.6k (0.82) | 74.2k (0.55) | 78.8k (0.58) | 464.6k (3.43) | 558.8k (4.13) |
| read.small | 8 | 646.8k | 72.5k (0.11) | 49.9k (0.077) | 49.8k (0.077) | 1.93M (2.99) | 1.90M (2.94) |
| read.medium | 1 | 25.0k | 24.0k (0.96) | 14.9k (0.60) | 10.8k (0.43) | 15.4k (0.62) | 31.6k (1.26) |
| read.medium | 8 | 83.0k | 33.2k (0.40) | 35.4k (0.43) | 36.5k (0.44) | 73.3k (0.88) | 97.6k (1.18) |
| read.large.seq | 1 | 6.5k | 8.6k (1.34) | 4.2k (0.65) | 4.3k (0.66) | 4.0k (0.61) | 8.7k (1.35) |
| read.large.seq | 8 | 24.2k | 17.9k (0.74) | 18.5k (0.76) | 18.5k (0.76) | 21.2k (0.88) | 25.0k (1.04) |
| read.large.rand | 1 | 459.8k | 625.8k (1.36) | 565.4k (1.23) | 372.3k (0.81) | 553.4k (1.20) | 635.3k (1.38) |
| read.large.rand | 8 | 1.56M | 1.71M (1.09) | 1.80M (1.15) | 1.76M (1.12) | 1.90M (1.22) | 2.08M (1.33) |
| meta.open | 1 | 154.5k | 191.1k (1.24) | 108.3k (0.70) | 107.0k (0.69) | 1.46M (9.45) | 1.41M (9.13) |
| meta.open | 8 | 989.6k | 54.6k (0.055) | 39.6k (0.04) | 42.2k (0.043) | 3.93M (3.97) | 3.83M (3.87) |
| meta.stat | 1 | 141.2k | 140.2k (0.99) | 4.0k (0.029) | 137.9k (0.98) | 1.09M (7.70) | 1.51M (10.69) |
| meta.stat | 8 | 1.03M | 61.6k (0.059) | 2.5k (0.0024) | 92.8k (0.09) | 6.73M (6.50) | 6.75M (6.52) |
| meta.miss | 1 | 419.7k | 863.2k (2.06) | 4.2k (0.01) | 982.8k (2.34) | 706.7k (1.68) | 1.01M (2.41) |
| meta.miss | 8 | 2.17M | 405.5k (0.19) | 2.6k (0.0012) | 4.41M (2.03) | 4.64M (2.14) | 4.68M (2.16) |
| meta.list | 1 | 161 | 0.7 (0.0044) | 0.3 (0.0019) | 311 (1.94) | 363 (2.26) | 171 (1.06) |
| meta.list | 8 | 1.2k | 0.6 (0.00049) | 0.3 (0.00024) | 1.1k (0.89) | 1.1k (0.91) | 1.1k (0.87) |

Same APK built with `COMPRESS_DATA=1`, so the loose dataset files are deflated as Gradle does for ordinary assets (`plain.vfs` stays stored). `read.small`, `read.medium` and `read.large.seq` come from one launch; `read.large.rand` and `meta.open` / `meta.stat` / `meta.miss` from a second launch. `read.large.rand` was not measured for `base:aasset` and the `vfs:asset` targets: random reads inside the deflated 64 MiB asset did not complete one 1-thread run of `base:aasset` in 9 minutes, so they were left out. `meta.list` does not depend on compression and was not rerun:


| Scenario | Threads | base:stdio | base:aasset | asset | asset+lst | archive-apk | archive |
|---|---:|---:|---:|---:|---:|---:|---:|
| read.small | 1 | 132.5k | 43.6k (0.33) | 42.5k (0.32) | 37.9k (0.29) | 434.3k (3.28) | 379.7k (2.87) |
| read.small | 8 | 575.0k | 42.2k (0.073) | 43.3k (0.075) | 43.1k (0.075) | 1.98M (3.44) | 1.60M (2.78) |
| read.medium | 1 | 19.1k | 1.9k (0.099) | 1.6k (0.084) | 1.5k (0.079) | 14.5k (0.76) | 20.5k (1.07) |
| read.medium | 8 | 62.4k | 8.9k (0.14) | 8.6k (0.14) | 8.7k (0.14) | 77.1k (1.24) | 67.2k (1.08) |
| read.large.seq | 1 | 4.5k | 484 (0.11) | 486 (0.11) | 483 (0.11) | 7.8k (1.73) | 5.4k (1.20) |
| read.large.seq | 8 | 16.9k | 2.2k (0.13) | 2.2k (0.13) | 2.2k (0.13) | 18.5k (1.09) | 18.6k (1.10) |
| read.large.rand | 1 | 308.3k | - | - | - | 342.3k (1.11) | 465.7k (1.51) |
| read.large.rand | 8 | 1.18M | - | - | - | 1.87M (1.58) | 1.83M (1.55) |
| meta.open | 1 | 144.8k | 139.1k (0.96) | 119.6k (0.83) | 114.7k (0.79) | 779.0k (5.38) | 725.2k (5.01) |
| meta.open | 8 | 935.5k | 61.8k (0.066) | 44.0k (0.047) | 37.6k (0.04) | 3.52M (3.76) | 3.60M (3.85) |
| meta.stat | 1 | 154.4k | 108.2k (0.70) | 3.3k (0.021) | 100.1k (0.65) | 823.5k (5.33) | 1.34M (8.68) |
| meta.stat | 8 | 1.12M | 57.9k (0.052) | 2.0k (0.0018) | 34.6k (0.031) | 4.98M (4.45) | 5.98M (5.34) |
| meta.miss | 1 | 270.5k | 739.0k (2.73) | 3.5k (0.013) | 982.8k (3.63) | 1.04M (3.84) | 946.0k (3.50) |
| meta.miss | 8 | 1.92M | 253.1k (0.13) | 1.9k (0.00099) | 3.83M (1.99) | 4.13M (2.15) | 4.49M (2.34) |

Full tables of the APK run (loose files stored):

#### APK read.small

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 135.4k | 529 | 7.08 | 10.4 | 1.00 |
| vfs:archive | 1 | 558.8k | 2183 | 1.61 | 2.29 | 4.13 |
| base:aasset | 1 | 110.6k | 432 | 8.70 | 11.3 | 0.82 |
| vfs:asset | 1 | 74.2k | 290 | 11.6 | 40.5 | 0.55 |
| vfs:asset+lst | 1 | 78.8k | 308 | 10.5 | 40.4 | 0.58 |
| vfs:archive-apk | 1 | 464.6k | 1815 | 1.93 | 2.76 | 3.43 |
| base:stdio | 2 | 236.9k | 926 | 7.81 | 23.5 | 1.00 |
| vfs:archive | 2 | 915.7k | 3577 | 1.98 | 2.81 | 3.86 |
| base:aasset | 2 | 32.4k | 126 | 33.5 | 432.7 | 0.14 |
| vfs:asset | 2 | 41.3k | 161 | 47.0 | 108.4 | 0.17 |
| vfs:asset+lst | 2 | 43.5k | 170 | 37.7 | 133.2 | 0.18 |
| vfs:archive-apk | 2 | 718.2k | 2805 | 2.66 | 3.44 | 3.03 |
| base:stdio | 4 | 351.7k | 1374 | 7.29 | 10.7 | 1.00 |
| vfs:archive | 4 | 1.75M | 6828 | 1.98 | 3.07 | 4.97 |
| base:aasset | 4 | 45.5k | 178 | 30.5 | 409.7 | 0.13 |
| vfs:asset | 4 | 27.6k | 108 | 99.7 | 544.1 | 0.08 |
| vfs:asset+lst | 4 | 25.5k | 100 | 104.1 | 548.3 | 0.07 |
| vfs:archive-apk | 4 | 1.60M | 6245 | 2.34 | 3.07 | 4.55 |
| base:stdio | 8 | 646.8k | 2526 | 7.71 | 12.0 | 1.00 |
| vfs:archive | 8 | 1.90M | 7435 | 1.93 | 2.76 | 2.94 |
| base:aasset | 8 | 72.5k | 283 | 10.4 | 410.5 | 0.11 |
| vfs:asset | 8 | 49.9k | 195 | 67.9 | 730.6 | 0.08 |
| vfs:asset+lst | 8 | 49.8k | 194 | 65.0 | 738.2 | 0.08 |
| vfs:archive-apk | 8 | 1.93M | 7548 | 2.29 | 3.28 | 2.99 |

#### APK read.medium

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 25.0k | 6247 | 37.9 | 96.1 | 1.00 |
| vfs:archive | 1 | 31.6k | 7899 | 29.8 | 46.9 | 1.26 |
| base:aasset | 1 | 24.0k | 5993 | 40.2 | 81.8 | 0.96 |
| vfs:asset | 1 | 14.9k | 3719 | 64.8 | 114.6 | 0.60 |
| vfs:asset+lst | 1 | 10.8k | 2696 | 75.9 | 328.4 | 0.43 |
| vfs:archive-apk | 1 | 15.4k | 3848 | 47.0 | 141.4 | 0.62 |
| base:stdio | 2 | 22.7k | 5682 | 43.4 | 85.4 | 1.00 |
| vfs:archive | 2 | 24.6k | 6147 | 37.4 | 88.8 | 1.08 |
| base:aasset | 2 | 21.1k | 5265 | 42.9 | 91.1 | 0.93 |
| vfs:asset | 2 | 14.8k | 3706 | 66.1 | 134.8 | 0.65 |
| vfs:asset+lst | 2 | 12.7k | 3177 | 141.2 | 411.3 | 0.56 |
| vfs:archive-apk | 2 | 41.4k | 10354 | 45.8 | 69.5 | 1.82 |
| base:stdio | 4 | 89.6k | 22396 | 40.9 | 77.6 | 1.00 |
| vfs:archive | 4 | 58.8k | 14695 | 32.4 | 62.1 | 0.66 |
| base:aasset | 4 | 35.4k | 8860 | 92.9 | 313.7 | 0.40 |
| vfs:asset | 4 | 32.4k | 8109 | 97.4 | 389.3 | 0.36 |
| vfs:asset+lst | 4 | 34.2k | 8543 | 95.8 | 299.3 | 0.38 |
| vfs:archive-apk | 4 | 36.5k | 9117 | 55.6 | 89.2 | 0.41 |
| base:stdio | 8 | 83.0k | 20754 | 52.6 | 117.1 | 1.00 |
| vfs:archive | 8 | 97.6k | 24404 | 38.0 | 68.9 | 1.18 |
| base:aasset | 8 | 33.2k | 8301 | 196.0 | 863.3 | 0.40 |
| vfs:asset | 8 | 35.4k | 8857 | 123.9 | 1007.1 | 0.43 |
| vfs:asset+lst | 8 | 36.5k | 9119 | 120.0 | 966.5 | 0.44 |
| vfs:archive-apk | 8 | 73.3k | 18333 | 50.4 | 97.4 | 0.88 |

#### APK read.large.seq

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 6.5k | 6453 | 143.0 | 300.5 | 1.00 |
| vfs:archive | 1 | 8.7k | 8713 | 109.5 | 179.1 | 1.35 |
| base:aasset | 1 | 8.6k | 8625 | 93.0 | 167.4 | 1.34 |
| vfs:asset | 1 | 4.2k | 4190 | 206.8 | 500.2 | 0.65 |
| vfs:asset+lst | 1 | 4.3k | 4275 | 216.5 | 394.5 | 0.66 |
| vfs:archive-apk | 1 | 4.0k | 3953 | 210.3 | 631.9 | 0.61 |
| base:stdio | 2 | 6.8k | 6771 | 132.3 | 218.8 | 1.00 |
| vfs:archive | 2 | 7.2k | 7243 | 129.5 | 1096.1 | 1.07 |
| base:aasset | 2 | 7.2k | 7185 | 115.5 | 343.1 | 1.06 |
| vfs:asset | 2 | 5.3k | 5281 | 177.7 | 1137.6 | 0.78 |
| vfs:asset+lst | 2 | 5.6k | 5584 | 163.2 | 275.6 | 0.82 |
| vfs:archive-apk | 2 | 6.2k | 6173 | 199.9 | 412.1 | 0.91 |
| base:stdio | 4 | 16.1k | 16114 | 125.5 | 188.9 | 1.00 |
| vfs:archive | 4 | 24.4k | 24380 | 154.7 | 201.9 | 1.51 |
| base:aasset | 4 | 16.8k | 16801 | 135.9 | 299.3 | 1.04 |
| vfs:asset | 4 | 16.0k | 15984 | 234.9 | 385.6 | 0.99 |
| vfs:asset+lst | 4 | 16.6k | 16589 | 239.4 | 386.4 | 1.03 |
| vfs:archive-apk | 4 | 20.5k | 20489 | 187.6 | 220.5 | 1.27 |
| base:stdio | 8 | 24.2k | 24191 | 168.4 | 234.4 | 1.00 |
| vfs:archive | 8 | 25.0k | 25048 | 161.6 | 235.2 | 1.04 |
| base:aasset | 8 | 17.9k | 17917 | 128.4 | 381.3 | 0.74 |
| vfs:asset | 8 | 18.5k | 18484 | 215.8 | 514.7 | 0.76 |
| vfs:asset+lst | 8 | 18.5k | 18464 | 261.9 | 1079.9 | 0.76 |
| vfs:archive-apk | 8 | 21.2k | 21216 | 203.8 | 282.7 | 0.88 |

#### APK read.large.rand

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 459.8k | 1796 | 2.03 | 2.66 | 1.00 |
| vfs:archive | 1 | 635.3k | 2481 | 1.46 | 2.03 | 1.38 |
| base:aasset | 1 | 625.8k | 2445 | 0.52 | 5.57 | 1.36 |
| vfs:asset | 1 | 565.4k | 2209 | 1.61 | 2.45 | 1.23 |
| vfs:asset+lst | 1 | 372.3k | 1454 | 2.14 | 3.44 | 0.81 |
| vfs:archive-apk | 1 | 553.4k | 2162 | 1.46 | 1.98 | 1.20 |
| base:stdio | 2 | 754.0k | 2945 | 1.25 | 1.67 | 1.00 |
| vfs:archive | 2 | 959.3k | 3747 | 1.88 | 2.92 | 1.27 |
| base:aasset | 2 | 1.09M | 4251 | 0.42 | 5.00 | 1.44 |
| vfs:asset | 2 | 1.08M | 4229 | 1.67 | 2.45 | 1.44 |
| vfs:asset+lst | 2 | 1.11M | 4318 | 1.61 | 2.45 | 1.47 |
| vfs:archive-apk | 2 | 1.08M | 4201 | 1.72 | 2.40 | 1.43 |
| base:stdio | 4 | 796.2k | 3110 | 2.34 | 3.59 | 1.00 |
| vfs:archive | 4 | 1.17M | 4561 | 1.56 | 2.40 | 1.47 |
| base:aasset | 4 | 981.5k | 3834 | 0.57 | 5.94 | 1.23 |
| vfs:asset | 4 | 988.1k | 3860 | 1.93 | 2.86 | 1.24 |
| vfs:asset+lst | 4 | 1.03M | 4006 | 1.82 | 2.71 | 1.29 |
| vfs:archive-apk | 4 | 1.09M | 4263 | 1.72 | 2.66 | 1.37 |
| base:stdio | 8 | 1.56M | 6104 | 2.29 | 3.18 | 1.00 |
| vfs:archive | 8 | 2.08M | 8119 | 1.72 | 2.71 | 1.33 |
| base:aasset | 8 | 1.71M | 6661 | 0.57 | 5.26 | 1.09 |
| vfs:asset | 8 | 1.80M | 7020 | 1.82 | 3.12 | 1.15 |
| vfs:asset+lst | 8 | 1.76M | 6864 | 2.08 | 3.18 | 1.12 |
| vfs:archive-apk | 8 | 1.90M | 7431 | 1.88 | 2.87 | 1.22 |

#### APK meta.open

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 154.5k | - | 5.05 | 15.4 | 1.00 |
| vfs:archive | 1 | 1.41M | - | 0.62 | 1.30 | 9.13 |
| base:aasset | 1 | 191.1k | - | 4.90 | 7.86 | 1.24 |
| vfs:asset | 1 | 108.3k | - | 8.96 | 11.8 | 0.70 |
| vfs:asset+lst | 1 | 107.0k | - | 9.06 | 12.0 | 0.69 |
| vfs:archive-apk | 1 | 1.46M | - | 0.62 | 0.78 | 9.45 |
| base:stdio | 2 | 313.8k | - | 5.10 | 14.7 | 1.00 |
| vfs:archive | 2 | 2.18M | - | 0.83 | 0.99 | 6.94 |
| base:aasset | 2 | 55.3k | - | 16.6 | 301.7 | 0.18 |
| vfs:asset | 2 | 26.4k | - | 66.1 | 233.9 | 0.08 |
| vfs:asset+lst | 2 | 34.6k | - | 59.5 | 115.5 | 0.11 |
| vfs:archive-apk | 2 | 2.21M | - | 0.83 | 0.99 | 7.05 |
| base:stdio | 4 | 515.7k | - | 4.63 | 6.61 | 1.00 |
| vfs:archive | 4 | 2.19M | - | 0.78 | 0.99 | 4.25 |
| base:aasset | 4 | 63.9k | - | 16.4 | 616.6 | 0.12 |
| vfs:asset | 4 | 39.5k | - | 65.0 | 466.1 | 0.08 |
| vfs:asset+lst | 4 | 39.4k | - | 61.7 | 484.5 | 0.08 |
| vfs:archive-apk | 4 | 2.09M | - | 0.83 | 1.04 | 4.05 |
| base:stdio | 8 | 989.6k | - | 4.79 | 7.34 | 1.00 |
| vfs:archive | 8 | 3.83M | - | 0.89 | 1.25 | 3.87 |
| base:aasset | 8 | 54.6k | - | 19.5 | 1978.6 | 0.06 |
| vfs:asset | 8 | 39.6k | - | 191.5 | 772.2 | 0.04 |
| vfs:asset+lst | 8 | 42.2k | - | 180.7 | 725.5 | 0.04 |
| vfs:archive-apk | 8 | 3.93M | - | 0.94 | 1.20 | 3.97 |

#### APK meta.stat

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 141.2k | - | 5.21 | 20.5 | 1.00 |
| vfs:archive | 1 | 1.51M | - | 0.47 | 0.62 | 10.69 |
| base:aasset | 1 | 140.2k | - | 6.20 | 13.2 | 0.99 |
| vfs:asset | 1 | 4.0k | - | 245.7 | 280.7 | 0.03 |
| vfs:asset+lst | 1 | 137.9k | - | 6.41 | 12.6 | 0.98 |
| vfs:archive-apk | 1 | 1.09M | - | 0.78 | 1.09 | 7.70 |
| base:stdio | 2 | 408.1k | - | 3.28 | 5.62 | 1.00 |
| vfs:archive | 2 | 3.28M | - | 0.52 | 0.68 | 8.04 |
| base:aasset | 2 | 82.6k | - | 9.95 | 197.6 | 0.20 |
| vfs:asset | 2 | 3.0k | - | 254.2 | 6430.9 | 0.01 |
| vfs:asset+lst | 2 | 48.1k | - | 23.9 | 367.8 | 0.12 |
| vfs:archive-apk | 2 | 2.65M | - | 0.68 | 0.94 | 6.49 |
| base:stdio | 4 | 630.7k | - | 5.47 | 14.7 | 1.00 |
| vfs:archive | 4 | 6.47M | - | 0.47 | 0.68 | 10.26 |
| base:aasset | 4 | 75.0k | - | 11.6 | 701.2 | 0.12 |
| vfs:asset | 4 | 2.4k | - | 344.2 | 33465.9 | 0.00 |
| vfs:asset+lst | 4 | 60.1k | - | 15.9 | 870.1 | 0.10 |
| vfs:archive-apk | 4 | 6.55M | - | 0.47 | 0.62 | 10.39 |
| base:stdio | 8 | 1.03M | - | 4.17 | 11.5 | 1.00 |
| vfs:archive | 8 | 6.75M | - | 0.47 | 0.57 | 6.52 |
| base:aasset | 8 | 61.6k | - | 17.6 | 1698.9 | 0.06 |
| vfs:asset | 8 | 2.5k | - | 342.2 | 77869.5 | 0.00 |
| vfs:asset+lst | 8 | 92.8k | - | 7.71 | 723.4 | 0.09 |
| vfs:archive-apk | 8 | 6.73M | - | 0.47 | 0.73 | 6.50 |

#### APK meta.miss

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 419.7k | - | 2.03 | 3.28 | 1.00 |
| vfs:archive | 1 | 1.01M | - | 0.83 | 0.99 | 2.41 |
| base:aasset | 1 | 863.2k | - | 0.99 | 1.09 | 2.06 |
| vfs:asset | 1 | 4.2k | - | 236.9 | 272.3 | 0.01 |
| vfs:asset+lst | 1 | 982.8k | - | 0.94 | 1.09 | 2.34 |
| vfs:archive-apk | 1 | 706.7k | - | 1.56 | 1.88 | 1.68 |
| base:stdio | 2 | 753.4k | - | 2.50 | 4.22 | 1.00 |
| vfs:archive | 2 | 1.11M | - | 0.73 | 1.15 | 1.48 |
| base:aasset | 2 | 870.9k | - | 0.99 | 1.77 | 1.16 |
| vfs:asset | 2 | 4.1k | - | 239.7 | 343.8 | 0.01 |
| vfs:asset+lst | 2 | 1.04M | - | 0.89 | 2.08 | 1.38 |
| vfs:archive-apk | 2 | 1.26M | - | 0.73 | 2.03 | 1.67 |
| base:stdio | 4 | 1.11M | - | 1.88 | 2.66 | 1.00 |
| vfs:archive | 4 | 2.34M | - | 0.73 | 1.04 | 2.12 |
| base:aasset | 4 | 338.0k | - | 1.35 | 125.1 | 0.31 |
| vfs:asset | 4 | 3.1k | - | 243.2 | 13679.3 | 0.00 |
| vfs:asset+lst | 4 | 2.08M | - | 0.78 | 0.99 | 1.88 |
| vfs:archive-apk | 4 | 2.26M | - | 0.73 | 1.56 | 2.04 |
| base:stdio | 8 | 2.17M | - | 1.88 | 3.38 | 1.00 |
| vfs:archive | 8 | 4.68M | - | 0.73 | 1.25 | 2.16 |
| base:aasset | 8 | 405.5k | - | 1.72 | 166.1 | 0.19 |
| vfs:asset | 8 | 2.6k | - | 249.7 | 62304.9 | 0.00 |
| vfs:asset+lst | 8 | 4.41M | - | 0.78 | 0.94 | 2.03 |
| vfs:archive-apk | 8 | 4.64M | - | 0.73 | 1.04 | 2.14 |

#### APK meta.list

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 161 | - | 3985.4 | 20564.0 | 1.00 |
| vfs:archive | 1 | 171 | - | 6123.5 | 11206.0 | 1.06 |
| base:aasset | 1 | 1 | - | 1443267.8 | 1458019.3 | 0.00 |
| vfs:asset | 1 | 0 | - | 2913073.5 | 2917735.0 | 0.00 |
| vfs:asset+lst | 1 | 311 | - | 2523.6 | 5173.3 | 1.94 |
| vfs:archive-apk | 1 | 363 | - | 2691.8 | 3699.5 | 2.26 |
| base:stdio | 2 | 342 | - | 4106.6 | 18679.2 | 1.00 |
| vfs:archive | 2 | 680 | - | 2840.3 | 3298.0 | 1.99 |
| base:aasset | 2 | 1 | - | 2885561.5 | 2906706.6 | 0.00 |
| vfs:asset | 2 | 0 | - | 5822182.5 | 5830010.2 | 0.00 |
| vfs:asset+lst | 2 | 672 | - | 2823.0 | 3227.1 | 1.97 |
| vfs:archive-apk | 2 | 696 | - | 2861.7 | 3268.1 | 2.04 |
| base:stdio | 4 | 689 | - | 3970.7 | 17161.1 | 1.00 |
| vfs:archive | 4 | 1.1k | - | 3492.1 | 4142.2 | 1.63 |
| base:aasset | 4 | 1 | - | 5809033.9 | 7041368.0 | 0.00 |
| vfs:asset | 4 | 0 | - | 12405980.7 | 24865281.6 | 0.00 |
| vfs:asset+lst | 4 | 1.0k | - | 3865.8 | 4292.4 | 1.48 |
| vfs:archive-apk | 4 | 1.0k | - | 3913.4 | 4070.6 | 1.47 |
| base:stdio | 8 | 1.2k | - | 4681.2 | 14851.4 | 1.00 |
| vfs:archive | 8 | 1.1k | - | 5142.4 | 17488.4 | 0.87 |
| base:aasset | 8 | 1 | - | 14079674.8 | 14089685.4 | 0.00 |
| vfs:asset | 8 | 0 | - | 23067164.8 | 70941598.6 | 0.00 |
| vfs:asset+lst | 8 | 1.1k | - | 5282.5 | 13693.6 | 0.89 |
| vfs:archive-apk | 8 | 1.1k | - | 5750.1 | 15485.7 | 0.91 |


Full tables of the APK run (loose files deflated):

#### APK (deflated) read.small

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 132.5k | 518 | 7.03 | 13.1 | 1.00 |
| vfs:archive | 1 | 379.7k | 1483 | 2.40 | 4.43 | 2.87 |
| base:aasset | 1 | 43.6k | 170 | 22.4 | 27.1 | 0.33 |
| vfs:asset | 1 | 42.5k | 166 | 23.1 | 27.8 | 0.32 |
| vfs:asset+lst | 1 | 37.9k | 148 | 23.5 | 73.7 | 0.29 |
| vfs:archive-apk | 1 | 434.3k | 1696 | 2.03 | 2.92 | 3.28 |
| base:stdio | 2 | 210.9k | 824 | 8.18 | 17.4 | 1.00 |
| vfs:archive | 2 | 731.3k | 2857 | 2.50 | 3.23 | 3.47 |
| base:aasset | 2 | 53.9k | 210 | 33.7 | 121.2 | 0.26 |
| vfs:asset | 2 | 51.3k | 201 | 31.3 | 156.9 | 0.24 |
| vfs:asset+lst | 2 | 44.2k | 173 | 31.4 | 223.2 | 0.21 |
| vfs:archive-apk | 2 | 728.5k | 2846 | 2.34 | 4.58 | 3.45 |
| base:stdio | 4 | 335.1k | 1309 | 8.18 | 12.4 | 1.00 |
| vfs:archive | 4 | 927.9k | 3625 | 2.24 | 3.02 | 2.77 |
| base:aasset | 4 | 49.2k | 192 | 50.1 | 447.9 | 0.15 |
| vfs:asset | 4 | 48.8k | 191 | 52.2 | 415.2 | 0.15 |
| vfs:asset+lst | 4 | 49.2k | 192 | 52.4 | 403.2 | 0.15 |
| vfs:archive-apk | 4 | 1.06M | 4141 | 2.34 | 3.44 | 3.16 |
| base:stdio | 8 | 575.0k | 2246 | 8.70 | 12.9 | 1.00 |
| vfs:archive | 8 | 1.60M | 6269 | 2.66 | 3.65 | 2.79 |
| base:aasset | 8 | 42.2k | 165 | 86.5 | 876.1 | 0.07 |
| vfs:asset | 8 | 43.3k | 169 | 81.9 | 853.0 | 0.08 |
| vfs:asset+lst | 8 | 43.1k | 168 | 79.3 | 861.9 | 0.07 |
| vfs:archive-apk | 8 | 1.98M | 7732 | 2.45 | 3.54 | 3.44 |

#### APK (deflated) read.medium

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 19.1k | 4780 | 49.6 | 94.7 | 1.00 |
| vfs:archive | 1 | 20.5k | 5133 | 45.8 | 101.6 | 1.07 |
| base:aasset | 1 | 1.9k | 471 | 524.7 | 571.0 | 0.10 |
| vfs:asset | 1 | 1.6k | 389 | 533.9 | 1802.7 | 0.08 |
| vfs:asset+lst | 1 | 1.5k | 384 | 537.3 | 1798.1 | 0.08 |
| vfs:archive-apk | 1 | 14.5k | 3635 | 64.7 | 133.3 | 0.76 |
| base:stdio | 2 | 37.6k | 9399 | 51.8 | 87.4 | 1.00 |
| vfs:archive | 2 | 38.9k | 9721 | 49.1 | 85.7 | 1.03 |
| base:aasset | 2 | 3.3k | 835 | 608.2 | 889.1 | 0.09 |
| vfs:asset | 2 | 3.3k | 819 | 610.2 | 1006.9 | 0.09 |
| vfs:asset+lst | 2 | 3.3k | 821 | 532.3 | 680.6 | 0.09 |
| vfs:archive-apk | 2 | 33.1k | 8286 | 56.9 | 85.6 | 0.88 |
| base:stdio | 4 | 38.2k | 9547 | 50.5 | 91.4 | 1.00 |
| vfs:archive | 4 | 66.5k | 16634 | 56.0 | 82.6 | 1.74 |
| base:aasset | 4 | 6.5k | 1628 | 610.3 | 807.0 | 0.17 |
| vfs:asset | 4 | 6.5k | 1628 | 610.7 | 813.9 | 0.17 |
| vfs:asset+lst | 4 | 6.1k | 1535 | 613.2 | 1715.5 | 0.16 |
| vfs:archive-apk | 4 | 38.9k | 9731 | 55.8 | 101.6 | 1.02 |
| base:stdio | 8 | 62.4k | 15597 | 66.4 | 116.6 | 1.00 |
| vfs:archive | 8 | 67.2k | 16803 | 57.7 | 92.7 | 1.08 |
| base:aasset | 8 | 8.9k | 2236 | 609.6 | 8190.8 | 0.14 |
| vfs:asset | 8 | 8.6k | 2144 | 612.3 | 8728.3 | 0.14 |
| vfs:asset+lst | 8 | 8.7k | 2165 | 613.6 | 7519.1 | 0.14 |
| vfs:archive-apk | 8 | 77.1k | 19285 | 52.8 | 93.3 | 1.24 |

#### APK (deflated) read.large.seq

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 4.5k | 4465 | 215.9 | 306.3 | 1.00 |
| vfs:archive | 1 | 5.4k | 5412 | 162.8 | 246.0 | 1.21 |
| base:aasset | 1 | 484 | 484 | 2040.9 | 2103.1 | 0.11 |
| vfs:asset | 1 | 486 | 486 | 2030.3 | 2081.9 | 0.11 |
| vfs:asset+lst | 1 | 483 | 483 | 2045.0 | 2092.8 | 0.11 |
| vfs:archive-apk | 1 | 7.8k | 7832 | 124.2 | 185.9 | 1.75 |
| base:stdio | 2 | 8.2k | 8231 | 219.9 | 360.9 | 1.00 |
| vfs:archive | 2 | 9.9k | 9939 | 181.9 | 329.1 | 1.21 |
| base:aasset | 2 | 851 | 851 | 2324.3 | 2625.0 | 0.10 |
| vfs:asset | 2 | 767 | 767 | 2340.7 | 6883.8 | 0.09 |
| vfs:asset+lst | 2 | 852 | 852 | 2326.9 | 2563.9 | 0.10 |
| vfs:archive-apk | 2 | 9.2k | 9156 | 220.4 | 263.1 | 1.11 |
| base:stdio | 4 | 9.9k | 9885 | 204.8 | 330.5 | 1.00 |
| vfs:archive | 4 | 8.4k | 8391 | 234.1 | 458.5 | 0.85 |
| base:aasset | 4 | 1.6k | 1608 | 2352.4 | 6519.7 | 0.16 |
| vfs:asset | 4 | 1.6k | 1613 | 2347.6 | 6724.8 | 0.16 |
| vfs:asset+lst | 4 | 1.6k | 1586 | 2349.2 | 7438.9 | 0.16 |
| vfs:archive-apk | 4 | 15.0k | 15048 | 263.1 | 328.6 | 1.52 |
| base:stdio | 8 | 16.9k | 16904 | 257.8 | 512.4 | 1.00 |
| vfs:archive | 8 | 18.6k | 18580 | 206.2 | 316.7 | 1.10 |
| base:aasset | 8 | 2.2k | 2229 | 2352.1 | 18400.9 | 0.13 |
| vfs:asset | 8 | 2.2k | 2226 | 2349.2 | 19358.0 | 0.13 |
| vfs:asset+lst | 8 | 2.2k | 2220 | 2354.7 | 18373.6 | 0.13 |
| vfs:archive-apk | 8 | 18.5k | 18525 | 222.7 | 315.0 | 1.10 |

#### APK (deflated) read.large.rand

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 308.3k | 1204 | 2.81 | 6.93 | 1.00 |
| vfs:archive | 1 | 465.7k | 1819 | 1.77 | 2.81 | 1.51 |
| vfs:archive-apk | 1 | 342.3k | 1337 | 2.60 | 3.85 | 1.11 |
| base:stdio | 2 | 385.8k | 1507 | 2.50 | 3.59 | 1.00 |
| vfs:archive | 2 | 521.1k | 2036 | 1.61 | 2.40 | 1.35 |
| vfs:archive-apk | 2 | 482.8k | 1886 | 1.82 | 2.76 | 1.25 |
| base:stdio | 4 | 693.6k | 2709 | 2.92 | 6.56 | 1.00 |
| vfs:archive | 4 | 979.3k | 3825 | 1.88 | 2.76 | 1.41 |
| vfs:archive-apk | 4 | 1.20M | 4696 | 2.29 | 8.28 | 1.73 |
| base:stdio | 8 | 1.18M | 4592 | 2.92 | 6.41 | 1.00 |
| vfs:archive | 8 | 1.83M | 7164 | 2.14 | 5.10 | 1.56 |
| vfs:archive-apk | 8 | 1.87M | 7308 | 2.19 | 5.83 | 1.59 |

#### APK (deflated) meta.open

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 144.8k | - | 5.99 | 15.4 | 1.00 |
| vfs:archive | 1 | 725.2k | - | 1.09 | 3.18 | 5.01 |
| base:aasset | 1 | 139.1k | - | 6.56 | 11.3 | 0.96 |
| vfs:asset | 1 | 119.6k | - | 7.76 | 13.0 | 0.83 |
| vfs:asset+lst | 1 | 114.7k | - | 7.81 | 15.7 | 0.79 |
| vfs:archive-apk | 1 | 779.0k | - | 1.04 | 2.34 | 5.38 |
| base:stdio | 2 | 291.9k | - | 5.88 | 16.0 | 1.00 |
| vfs:archive | 2 | 1.98M | - | 0.89 | 1.09 | 6.77 |
| base:aasset | 2 | 70.3k | - | 13.3 | 300.2 | 0.24 |
| vfs:asset | 2 | 36.2k | - | 27.4 | 380.9 | 0.12 |
| vfs:asset+lst | 2 | 70.7k | - | 11.0 | 166.4 | 0.24 |
| vfs:archive-apk | 2 | 1.81M | - | 0.99 | 1.41 | 6.21 |
| base:stdio | 4 | 587.4k | - | 5.42 | 11.3 | 1.00 |
| vfs:archive | 4 | 2.05M | - | 0.94 | 1.15 | 3.48 |
| base:aasset | 4 | 73.4k | - | 9.43 | 584.9 | 0.12 |
| vfs:asset | 4 | 76.3k | - | 8.70 | 518.5 | 0.13 |
| vfs:asset+lst | 4 | 68.5k | - | 9.95 | 468.8 | 0.12 |
| vfs:archive-apk | 4 | 1.67M | - | 0.94 | 5.88 | 2.84 |
| base:stdio | 8 | 935.5k | - | 5.73 | 15.0 | 1.00 |
| vfs:archive | 8 | 3.60M | - | 1.04 | 2.60 | 3.85 |
| base:aasset | 8 | 61.8k | - | 14.7 | 1969.7 | 0.07 |
| vfs:asset | 8 | 44.0k | - | 20.9 | 2001.6 | 0.05 |
| vfs:asset+lst | 8 | 37.6k | - | 30.4 | 1379.8 | 0.04 |
| vfs:archive-apk | 8 | 3.52M | - | 1.04 | 2.92 | 3.76 |

#### APK (deflated) meta.stat

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 154.4k | - | 5.78 | 11.5 | 1.00 |
| vfs:archive | 1 | 1.34M | - | 0.62 | 0.78 | 8.68 |
| base:aasset | 1 | 108.2k | - | 7.34 | 23.0 | 0.70 |
| vfs:asset | 1 | 3.3k | - | 297.1 | 333.8 | 0.02 |
| vfs:asset+lst | 1 | 100.1k | - | 7.81 | 25.2 | 0.65 |
| vfs:archive-apk | 1 | 823.5k | - | 1.04 | 1.25 | 5.34 |
| base:stdio | 2 | 249.6k | - | 6.09 | 15.0 | 1.00 |
| vfs:archive | 2 | 2.38M | - | 0.62 | 0.83 | 9.55 |
| base:aasset | 2 | 39.7k | - | 23.6 | 367.3 | 0.16 |
| vfs:asset | 2 | 1.7k | - | 539.3 | 13277.4 | 0.01 |
| vfs:asset+lst | 2 | 79.9k | - | 10.4 | 108.5 | 0.32 |
| vfs:archive-apk | 2 | 1.39M | - | 0.57 | 0.68 | 5.58 |
| base:stdio | 4 | 580.7k | - | 5.16 | 14.6 | 1.00 |
| vfs:archive | 4 | 2.91M | - | 0.62 | 0.78 | 5.01 |
| base:aasset | 4 | 53.0k | - | 12.9 | 850.8 | 0.09 |
| vfs:asset | 4 | 2.0k | - | 414.9 | 35254.4 | 0.00 |
| vfs:asset+lst | 4 | 53.4k | - | 10.9 | 539.0 | 0.09 |
| vfs:archive-apk | 4 | 1.77M | - | 0.62 | 1.77 | 3.05 |
| base:stdio | 8 | 1.12M | - | 5.31 | 12.8 | 1.00 |
| vfs:archive | 8 | 5.98M | - | 0.57 | 1.67 | 5.35 |
| base:aasset | 8 | 57.9k | - | 16.4 | 1934.3 | 0.05 |
| vfs:asset | 8 | 2.0k | - | 406.8 | 82003.6 | 0.00 |
| vfs:asset+lst | 8 | 34.6k | - | 30.6 | 1679.4 | 0.03 |
| vfs:archive-apk | 8 | 4.98M | - | 0.57 | 2.60 | 4.46 |

#### APK (deflated) meta.miss

| target | threads | ops/s | MiB/s | p50 (µs) | p99 (µs) | vs stdio |
|---|---:|---:|---:|---:|---:|---:|
| base:stdio | 1 | 270.5k | - | 3.23 | 7.24 | 1.00 |
| vfs:archive | 1 | 946.0k | - | 0.89 | 1.09 | 3.50 |
| base:aasset | 1 | 739.0k | - | 1.15 | 1.30 | 2.73 |
| vfs:asset | 1 | 3.5k | - | 286.1 | 309.5 | 0.01 |
| vfs:asset+lst | 1 | 982.8k | - | 0.94 | 1.09 | 3.63 |
| vfs:archive-apk | 1 | 1.04M | - | 0.89 | 1.04 | 3.83 |
| base:stdio | 2 | 575.3k | - | 2.60 | 7.24 | 1.00 |
| vfs:archive | 2 | 1.04M | - | 0.94 | 3.96 | 1.80 |
| base:aasset | 2 | 467.5k | - | 1.15 | 27.2 | 0.81 |
| vfs:asset | 2 | 3.4k | - | 290.5 | 339.8 | 0.01 |
| vfs:asset+lst | 2 | 599.1k | - | 2.03 | 2.76 | 1.04 |
| vfs:archive-apk | 2 | 1.81M | - | 0.83 | 1.04 | 3.15 |
| base:stdio | 4 | 1.07M | - | 2.14 | 3.65 | 1.00 |
| vfs:archive | 4 | 4.04M | - | 0.83 | 1.04 | 3.79 |
| base:aasset | 4 | 237.6k | - | 3.28 | 133.0 | 0.22 |
| vfs:asset | 4 | 3.0k | - | 294.0 | 6036.5 | 0.00 |
| vfs:asset+lst | 4 | 3.69M | - | 0.89 | 1.51 | 3.46 |
| vfs:archive-apk | 4 | 4.03M | - | 0.83 | 1.09 | 3.78 |
| base:stdio | 8 | 1.92M | - | 2.19 | 5.88 | 1.00 |
| vfs:archive | 8 | 4.49M | - | 0.89 | 2.40 | 2.34 |
| base:aasset | 8 | 253.1k | - | 1.51 | 375.8 | 0.13 |
| vfs:asset | 8 | 1.9k | - | 311.3 | 49259.8 | 0.00 |
| vfs:asset+lst | 8 | 3.83M | - | 0.94 | 2.71 | 2.00 |
| vfs:archive-apk | 8 | 4.13M | - | 0.83 | 1.04 | 2.15 |
<!-- apk-assets:end -->
