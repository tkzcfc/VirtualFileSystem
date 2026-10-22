#!/usr/bin/env python3
# Generate the test assets for the AndroidAssetBackend test APK, or (with --bench) the vfs_bench dataset for the bench APK.
# Usage: gen_assets.py <assets_dir> <archive_src_dir>
#        gen_assets.py --bench <data_dir> [scale]
# Content patterns must match AssetApkTest.cpp (binByte / textLine) and, for --bench, BenchCore.cpp (makeContent / buildDataset).
import os, sys

MASK = (1 << 64) - 1
WORDS = [w.encode() for w in ["vfs ", "archive ", "mount ", "layer ", "stream ", "read ", "write ", "native ",
                              "memory ", "copy-up ", "index ", "entry ", "\n", "0123 ", "data ", "path "]]


def make_content(size, seed):
    """Byte-for-byte port of makeContent() in bench/BenchCore.cpp."""
    out = bytearray()
    x = (seed * 0x9E3779B97F4A7C15 + 1) & MASK
    while len(out) < size:
        x ^= (x << 13) & MASK
        x ^= x >> 7
        x ^= (x << 17) & MASK
        if (x & 7) == 0:
            out.append((x >> 24) & 0xFF)
        out += WORDS[(x >> 8) & 15]
    # A word that crosses the end is cut off, as in the C++ version
    return bytes(out[:size])


def gen_bench(data, scale):
    """Same files as buildDataset() in bench/BenchCore.cpp, plus a manifest (assets.lst) for the asset backend."""
    small_count = max(100, int(10000 * scale))
    medium_count = max(10, int(200 * scale))
    small_size, medium_size = 4 * 1024, 256 * 1024
    large_size = max(4, int(64.0 * scale)) * 1024 * 1024
    rels = []
    pool = make_content(small_size * 64 + 4096, 1)
    for i in range(small_count):
        off = (i * 4099) % (len(pool) - small_size)
        d = bytearray(pool[off:off + small_size])
        d[0], d[1], d[2] = i & 0xFF, (i >> 8) & 0xFF, (i >> 16) & 0xFF
        rels.append(("small/f%06d.bin" % i, bytes(d)))
    for i in range(medium_count):
        rels.append(("medium/m%06d.bin" % i, make_content(medium_size, 100 + i)))
    rels.append(("large.bin", make_content(large_size, 7)))
    for rel, d in rels:
        put(data, rel, d)
    put(data, "assets.lst", "\n".join(r for r, _ in rels) + "\n")

def put(root, rel, data):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode("utf-8"))

if sys.argv[1] == "--bench":
    gen_bench(sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 1.0)
    sys.exit(0)

assets, arc = sys.argv[1], sys.argv[2]

BIN_SIZE = 300000
binary = bytes(((i * 31 + (i >> 8)) & 0xFF) for i in range(BIN_SIZE))
text = "".join("line %06d lorem ipsum dolor sit amet\n" % i for i in range(6000))

files = {
    "hello.txt": "Hello, assets!\n",
    "data/big.bin": binary,                      # stored uncompressed (aapt2 -0 bin): fd / pread path
    "data/big.txt": text,                        # compressed by aapt2: AAsset_read / seek path
    "nested/a/b/c.txt": "deep file\n",
    "nested/only-subdirs/x/y.txt": "only subdirs above me\n",
    "unicode/\u4e2d\u6587.txt": "\u4f60\u597d\n",
    "unicode/caf\u00e9.txt": "caf\u00e9\n",
}
for rel, data in files.items():
    put(assets, rel, data)
# Manifest for manifest mode: every file path (except the manifest itself), relative to the assets root.
put(assets, "assets.lst", "\n".join(sorted(files) + ["game.vfs"]) + "\n")   # game.vfs is packed later by build.sh

# Source tree for game.vfs (packed by vfstool, stored uncompressed in the APK).
put(arc, "config.json", '{"level": 3}\n')
put(arc, "scripts/main.lua", "print('hi')\n" * 500)   # packed with LZ4
put(arc, "textures/t.bin", binary[:65536])
