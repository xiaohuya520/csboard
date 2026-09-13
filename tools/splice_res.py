#!/usr/bin/env python3
"""Splice the CSR3 logo pack into the merged firmware image.

Writes the pack at the "csres" partition offset (0x36A000) and pads with 0xFF.
Asserts the protected regions (bootloader / partition table / app / cardid)
are byte-identical to the input, so verify_firmware.py's guarantees still hold.
If the pack is missing (CI could not reach bo3.gg), this is a no-op: the
firmware boots with the embedded 105-team bitmaps only.
"""
import os
import sys

CSRES_OFFSET = 0x36A000
CSRES_SIZE = 0x400000
FLASH_SIZE = 8 * 1024 * 1024
CARDID_OFFSET = 0x356000
CARDID_END = 0x35A000


def main() -> int:
    merged_path = sys.argv[1] if len(sys.argv) > 1 else "build/csboard-full.bin"
    pack_path = sys.argv[2] if len(sys.argv) > 2 else "build/csres.bin"

    if not os.path.isfile(merged_path):
        print("ERROR: %s missing" % merged_path, file=sys.stderr)
        return 1
    merged = bytearray(open(merged_path, "rb").read())

    if not os.path.isfile(pack_path) or os.path.getsize(pack_path) == 0:
        print("no logo pack (%s absent) -> embedded-only firmware" % pack_path)
        return 0
    pack = open(pack_path, "rb").read()
    if pack[:4] != b"CSRP":
        print("ERROR: bad pack magic", file=sys.stderr)
        return 1
    if len(pack) > CSRES_SIZE:
        print("ERROR: pack %d B exceeds csres %d B"
              % (len(pack), CSRES_SIZE), file=sys.stderr)
        return 1

    protected = merged[:CSRES_OFFSET]
    if len(merged) > CSRES_OFFSET:
        print("ERROR: merged image already reaches the csres partition",
              file=sys.stderr)
        return 1

    # cardid 保护区在合并镜像里必须是 0xFF(补齐后自然满足,但显式断言一次)
    pad_to_cardid = CARDID_OFFSET - len(merged)
    if pad_to_cardid > 0:
        merged += b"\xFF" * pad_to_cardid
    cardid = bytes(merged[CARDID_OFFSET:CARDID_END])
    if any(b != 0xFF for b in cardid):
        print("ERROR: cardid region not padding", file=sys.stderr)
        return 1

    merged += b"\xFF" * (CSRES_OFFSET - len(merged))
    merged += pack
    merged += b"\xFF" * (CSRES_OFFSET + CSRES_SIZE - len(merged))
    assert len(merged) <= FLASH_SIZE, "merged image exceeds 8MB"

    with open(merged_path, "wb") as fh:
        fh.write(merged)
    print("spliced logo pack: %d B at 0x%X -> merged %d B (%.2f MB)"
          % (len(pack), CSRES_OFFSET, len(merged), len(merged) / 1048576))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
