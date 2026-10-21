#!/usr/bin/env python3
# Generate the ChaCha20 test-vector header Rfc8439Vectors.h from the RFC 8439 text.
# Usage: curl -sSLo rfc8439.txt https://www.rfc-editor.org/rfc/rfc8439.txt && python3 gen_rfc8439.py rfc8439.txt > Rfc8439Vectors.h
import re
import sys

lines = open(sys.argv[1], encoding="ascii").read().splitlines()


def hexdump(start):
    """Read the hex dump starting at line start and return a list of bytes"""
    out = []
    i = start
    while i < len(lines):
        m = re.match(r"^\s+(\d{3})  (.*)$", lines[i])
        if not m:
            # Skip pagination (blank lines, page footers, page headers)
            if lines[i].strip() == "" or "Nir & Langley" in lines[i] or lines[i].startswith("RFC 8439"):
                i += 1
                if out and i < len(lines) and re.match(r"^\s+\d{3}  ", lines[i]) is None and lines[i].strip() != "" \
                        and "Nir & Langley" not in lines[i] and not lines[i].startswith("RFC 8439"):
                    break
                continue
            break
        hexpart = m.group(2).split("  ")[0]
        out += [int(x, 16) for x in hexpart.split()]
        i += 1
    return out


def find(label, start=0):
    for i in range(start, len(lines)):
        if lines[i].strip() == label:
            return i
    raise KeyError(label)


def carr(data):
    body = ", ".join("0x%02x" % b for b in data)
    return "{" + body + "}"


blocks = []
encs = []

# 2.3.2: ChaCha20 block function
key = list(range(32))
nonce = [0, 0, 0, 0x09, 0, 0, 0, 0x4a, 0, 0, 0, 0]
blocks.append((key, nonce, 1, hexdump(find("Serialized Block:") + 1)))

# 2.4.2: sunscreen
nonce = [0, 0, 0, 0, 0, 0, 0, 0x4a, 0, 0, 0, 0]
encs.append((key, nonce, 1, hexdump(find("Plaintext Sunscreen:") + 1), hexdump(find("Ciphertext Sunscreen:") + 1)))

# Appendix A.1 / A.2
a1 = find("A.1.  The ChaCha20 Block Functions")
a2 = find("A.2.  ChaCha20 Encryption")
a3 = find("A.3.  Poly1305 Message Authentication Code")
i = a1
while True:
    try:
        k = find("Key:", i)
    except KeyError:
        break
    if k > a3:
        break
    key = hexdump(k + 1)
    n = find("Nonce:", k)
    nonce = hexdump(n + 1)
    j = n
    while "Block Counter" not in lines[j]:
        j += 1
    counter = int(lines[j].split("=")[1])
    if k < a2:
        ks = find("Keystream:", j)
        blocks.append((key, nonce, counter, hexdump(ks + 1)))
        i = ks + 1
    else:
        p = find("Plaintext:", j)
        c = find("Ciphertext:", p)
        encs.append((key, nonce, counter, hexdump(p + 1), hexdump(c + 1)))
        i = c + 1

print("// This file is generated automatically by gen_rfc8439.py from the RFC 8439 text. Do not edit by hand.")
print("// Contains all ChaCha20 test vectors from §2.3.2, §2.4.2, and Appendix A.1 (block function) and A.2 (encryption).")
print("#pragma once\n\n#include <cstdint>\n#include <vector>\n")
print("struct ChaChaBlockVector\n{\n\tstd::vector<uint8_t> key;\n\tstd::vector<uint8_t> nonce;\n\tuint32_t counter;\n\tstd::vector<uint8_t> keystream;\n};\n")
print("struct ChaChaEncryptVector\n{\n\tstd::vector<uint8_t> key;\n\tstd::vector<uint8_t> nonce;\n\tuint32_t counter;\n\tstd::vector<uint8_t> plaintext;\n\tstd::vector<uint8_t> ciphertext;\n};\n")
print("inline const std::vector<ChaChaBlockVector>& rfc8439BlockVectors()\n{\n\tstatic const std::vector<ChaChaBlockVector> v = {")
for key, nonce, counter, ks in blocks:
    assert len(key) == 32 and len(nonce) == 12 and len(ks) == 64
    print("\t\t{%s,\n\t\t %s,\n\t\t %d,\n\t\t %s}," % (carr(key), carr(nonce), counter, carr(ks)))
print("\t};\n\treturn v;\n}\n")
print("inline const std::vector<ChaChaEncryptVector>& rfc8439EncryptVectors()\n{\n\tstatic const std::vector<ChaChaEncryptVector> v = {")
for key, nonce, counter, pt, ct in encs:
    assert len(key) == 32 and len(nonce) == 12 and len(pt) == len(ct) and len(pt) > 0
    print("\t\t{%s,\n\t\t %s,\n\t\t %d,\n\t\t %s,\n\t\t %s}," % (carr(key), carr(nonce), counter, carr(pt), carr(ct)))
print("\t};\n\treturn v;\n}")
print("// blocks=%d encrypts=%d" % (len(blocks), len(encs)), file=sys.stderr)
