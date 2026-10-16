#!/usr/bin/env python3
"""verify_crx3.py — sanity-check a CRX3 file: magic, header protobuf parse,
RSA-SHA256 signature over SignedData, crx_id match, zip integrity.

Usage: verify_crx3.py <file.crx> <pem_key>
"""
import hashlib
import subprocess
import sys
import zipfile
import io
from pathlib import Path


def read_varint(b, i):
    shift = 0
    val = 0
    while True:
        x = b[i]
        i += 1
        val |= (x & 0x7F) << shift
        if not (x & 0x80):
            return val, i
        shift += 7


def parse_fields(b):
    """Yield (field_number, wire_type, payload) for a protobuf message."""
    i = 0
    while i < len(b):
        key, i = read_varint(b, i)
        field, wire = key >> 3, key & 7
        if wire == 2:
            ln, i = read_varint(b, i)
            yield field, wire, b[i:i + ln]
            i += ln
        elif wire == 0:
            v, i = read_varint(b, i)
            yield field, wire, v
        else:
            raise ValueError(f"unsupported wire type {wire}")


def main(crx_path, pem_path):
    data = Path(crx_path).read_bytes()
    assert data[:4] == b"Cr24", "bad magic"
    version = int.from_bytes(data[4:8], "little")
    hlen = int.from_bytes(data[8:12], "little")
    header = data[12:12 + hlen]
    zip_bytes = data[12 + hlen:]
    assert version == 3, f"version={version}"
    print(f"magic OK, version {version}, header {hlen} B, zip {len(zip_bytes)} B")

    proof = signed_data = None
    for f, w, payload in parse_fields(header):
        if f == 2:
            proof = payload
        elif f == 2000:
            signed_data = payload
    assert proof and signed_data, "missing fields"

    pub = sig = None
    for f, w, payload in parse_fields(proof):
        if f == 1:
            pub = payload
        elif f == 2:
            sig = payload
    crx_id = None
    for f, w, payload in parse_fields(signed_data):
        if f == 1:
            crx_id = payload
    assert pub and sig and crx_id is not None, "missing proof fields"

    # crx_id must equal sha256(pub)[:16]
    expect = hashlib.sha256(pub).digest()[:16]
    assert crx_id == expect, "crx_id mismatch"
    ext_id = "".join(chr(ord("a") + n) for b in crx_id for n in (b >> 4, b & 15))
    print(f"crx_id OK -> EXT_ID {ext_id}")

    # verify RSA-SHA256 signature over signed_data
    p = subprocess.run(
        ["openssl", "dgst", "-sha256", "-verify", "/dev/stdin", "-signature", "/dev/stdin"],
        input=pub + sig + signed_data,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    # /dev/stdin trick is awkward; do it with temp files instead
    import tempfile, os
    with tempfile.TemporaryDirectory() as td:
        pp = Path(td) / "pub.der"
        sp = Path(td) / "sig.bin"
        dp = Path(td) / "data.bin"
        pp.write_bytes(pub)
        sp.write_bytes(sig)
        dp.write_bytes(signed_data)
        p = subprocess.run(
            ["openssl", "dgst", "-sha256", "-verify", str(pp), "-signature", str(sp), str(dp)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out = p.stdout.decode().strip()
        print("signature:", out)
        assert p.returncode == 0 and "Verified OK" in out, "signature verify FAILED"

    z = zipfile.ZipFile(io.BytesIO(zip_bytes))
    names = z.namelist()
    assert "manifest.json" in names, "manifest.json missing at zip root"
    bad = z.testzip()
    assert bad is None, f"corrupt zip entry {bad}"
    print(f"zip OK: {len(names)} entries, manifest.json at root")
    print("ALL CHECKS PASSED")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
