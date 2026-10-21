#!/usr/bin/env python3
# Copy an APK and add a native library (stored uncompressed).
# aapt2 writes UTF-8 entry names without the zip UTF-8 flag; zipfile's append mode would re-encode them
# (cp437 -> UTF-8, mangling non-ASCII asset names), so the entries are rewritten with names decoded as UTF-8.
# Usage: add_lib.py <in.apk> <out.apk> <lib.so> <name in apk>
import sys, zipfile

src, dst, lib, arcname = sys.argv[1:5]
with zipfile.ZipFile(src, metadata_encoding="utf-8") as zin, zipfile.ZipFile(dst, "w") as zout:
    for info in zin.infolist():
        zi = zipfile.ZipInfo(info.filename, info.date_time)
        zi.compress_type = info.compress_type
        zi.external_attr = info.external_attr
        zout.writestr(zi, zin.read(info))
    zout.write(lib, arcname, zipfile.ZIP_STORED)
