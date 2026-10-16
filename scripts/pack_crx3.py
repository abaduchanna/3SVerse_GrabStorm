#!/usr/bin/env python3
"""pack_crx3.py — pack a Chrome extension directory into a signed CRX3 file.

Uses only the Python stdlib + the `openssl` CLI (available on ubuntu-latest).
Format reference: components/crx_file/crx3.proto in Chromium.

  CRX3 = "Cr24" + u32(version=3) + u32(header_len) + header + zip_bytes
  header (protobuf CrxFileHeader):
    field 2    sha256_with_rsa : AsymmetricKeyProof {1: public_key, 2: signature}
    field 2000 signed_data     : SignedData {1: crx_id}
  signature = RSA PKCS#1 v1.5 SHA-256 over the serialized SignedData bytes
  crx_id    = SHA256(SPKI-DER public key)[:16]
  chrome extension id = hex(crx_id) with 0-9a-f mapped onto a-p

Usage:
  pack_crx3.py <ext_dir> <pem_key> <out.crx>
Prints the extension id on stdout as: EXT_ID <id>
"""
import base64
import hashlib
import subprocess
import sys
import zipfile
from pathlib import Path


def pb_tag(field: int, wire: int = 2) -> bytes:
    """Varint protobuf tag."""
    v = (field << 3) | wire
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def pb_bytes(field: int, value: bytes) -> bytes:
    return pb_tag(field) + encode_varint(len(value)) + value


def encode_varint(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def openssl(*args: str, data: bytes | None = None) -> bytes:
    p = subprocess.run(
        ["openssl", *args],
        input=data,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if p.returncode != 0:
        raise RuntimeError(f"openssl {args[0]} failed: {p.stderr.decode(errors='replace')}")
    return p.stdout


def main(ext_dir: str, pem: str, out_crx: str) -> None:
    ext_dir = Path(ext_dir)
    pem = Path(pem)

    # 1. zip the extension (files at archive root, manifest.json required)
    zip_path = Path(out_crx).with_suffix(".zip.tmp")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted(ext_dir.rglob("*")):
            if f.is_file():
                z.write(f, f.relative_to(ext_dir).as_posix())
    zip_bytes = zip_path.read_bytes()

    # 2. public key (SPKI DER) and crx_id
    pub_der = openssl("rsa", "-in", str(pem), "-pubout", "-outform", "DER")
    crx_id = hashlib.sha256(pub_der).digest()[:16]
    ext_id = "".join(chr(ord("a") + n) for b in crx_id for n in (b >> 4, b & 15))

    # 3. SignedData {1: crx_id} -> sign with RSA-SHA256
    signed_data = pb_bytes(1, crx_id)
    sig = openssl("dgst", "-sha256", "-sign", str(pem), data=signed_data)

    # 4. header = {2: proof{1: pub, 2: sig}, 2000: signed_data}
    proof = pb_bytes(1, pub_der) + pb_bytes(2, sig)
    header = pb_bytes(2, proof) + pb_bytes(2000, signed_data)

    out = Path(out_crx)
    out.write_bytes(b"Cr24" + (3).to_bytes(4, "little") + len(header).to_bytes(4, "little") + header + zip_bytes)
    zip_path.unlink()

    print(f"EXT_ID {ext_id}")
    print(f"WROTE {out} ({out.stat().st_size} bytes, zip {len(zip_bytes)} bytes)")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2], sys.argv[3])
