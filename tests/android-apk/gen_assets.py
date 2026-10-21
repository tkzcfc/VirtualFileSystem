#!/usr/bin/env python3
# Generate the test assets for the AndroidAssetBackend test APK.
# Usage: gen_assets.py <assets_dir> <archive_src_dir>
# Content patterns must match AssetApkTest.cpp (binByte / textLine).
import os, sys

assets, arc = sys.argv[1], sys.argv[2]

def put(root, rel, data):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode("utf-8"))

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
