#!/usr/bin/env python3
"""Generate the CSR3 logo resource pack for the "csres" flash partition.

Fetches the CS2 world team ranking from bo3.gg's public API (same host and
headers as update_matches.py), downloads each team's real logo, resizes it to
48x48 and 20x20 RGB565 and packs up to 200 teams:

    64B header | alias table (32B each) | 48px block | 20px block

header (little-endian u32): [0]"CSRP" [1]version=1 [2]slot_count [3]alias_count
    [4]alias_off [5]pix48_off [6]pix20_off [7]crc32(body) [8]total_size
alias entry: char id[24] | u16 slot | u16 rsvd | u32 color

Designed to run in GitHub Actions (the build runner can reach bo3.gg; local
networks often cannot). NEVER fails the build: any problem just prints a
warning and produces no pack, and the firmware falls back to the embedded
105-team bitmaps.
"""
from __future__ import annotations

import gzip
import io
import json
import os
import struct
import sys
import urllib.error
import urllib.request
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from update_matches import HDRS, COLORS, PALETTE, norm  # noqa: E402

BASE = "https://api.bo3.gg/api/v1"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ALIAS_FILE = os.path.join(ROOT, "tools", "team_aliases.json")

OUT = sys.argv[1] if len(sys.argv) > 1 else "build/csres.bin"
TARGET_TEAMS = 200

IMG_HDRS = {
    "User-Agent": HDRS["User-Agent"],
    "Referer": "https://bo3.gg/",
    "Accept": "image/avif,image/webp,image/png,image/*,*/*;q=0.8",
}

# 排名接口按 filter 语法排好候选逐个试(CI 实测:第一个可用,单页上限 100,
# 翻页拿满 200;返回条目最多的候选赢)。
TEAM_ENDPOINTS = (
    "teams?page%5Blimit%5D=100",
    "teams?page%5Blimit%5D=100&sort=-rating",
    "teams?filter%5Bteams.game_id%5D%5Beq%5D=1&sort=-rating&page%5Blimit%5D=100",
)


def fetch(url: str, hdrs: dict, timeout: int = 30) -> bytes | None:
    try:
        req = urllib.request.Request(url, headers=hdrs)
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read()
            if resp.headers.get("Content-Encoding") == "gzip":
                raw = gzip.decompress(raw)
            return raw
    except (urllib.error.URLError, urllib.error.HTTPError, OSError) as exc:
        print("warn: %s (%s)" % (url[:110], exc), file=sys.stderr)
        return None


def fetch_json(path: str):
    raw = fetch("%s/%s" % (BASE, path), HDRS)
    if not raw:
        return None
    try:
        return json.loads(raw.decode("utf-8", "replace"))
    except ValueError as exc:
        print("warn: bad JSON from %s (%s)" % (path, exc), file=sys.stderr)
        return None


def as_list(d):
    """bo3.gg 有时包 {'data':[...]} 有时直接给数组,都吃下。"""
    if d is None:
        return []
    if isinstance(d, list):
        return d
    if isinstance(d, dict):
        for k in ("data", "teams", "items", "results"):
            v = d.get(k)
            if isinstance(v, list):
                return v
    return []


def pick(d: dict, keys: tuple, *path):
    """按候选键取第一个非空值;支持一层嵌套(如 team.logo_url)。"""
    for k in keys:
        if isinstance(d, dict) and d.get(k):
            return d[k]
    for p in path:
        sub = d.get(p) if isinstance(d, dict) else None
        if isinstance(sub, dict):
            for k in keys:
                if sub.get(k):
                    return sub[k]
    return None


def load_embedded_ids() -> dict:
    """归一化名 -> 内嵌 logo id(让包里的队伍也能用内嵌 id 命中)。"""
    table: dict[str, str] = {}
    try:
        with open(ALIAS_FILE, encoding="utf-8") as fh:
            for row in json.load(fh):
                logo_id, name, repo = row[0], row[1], row[2]
                for v in (logo_id, name, repo):
                    if v:
                        table.setdefault(norm(v), logo_id)
    except (OSError, ValueError):
        pass
    return table


def image_to_rgb565(png: bytes, edge: int):
    try:
        from PIL import Image
    except ImportError:
        print("warn: Pillow missing", file=sys.stderr)
        return None
    try:
        im = Image.open(io.BytesIO(png)).convert("RGBA")
        im = im.resize((edge, edge), Image.LANCZOS)
        # 透明像素压到近黑底(与屏幕底色一致),避免透出随机噪声
        bg = Image.new("RGBA", (edge, edge), (10, 14, 19, 255))
        bg.alpha_composite(im)
        px = bg.convert("RGB").load()
        words = []
        for y in range(edge):
            for x in range(edge):
                r, g, b = px[x, y]
                words.append(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
        return struct.pack("<%dH" % len(words), *words)
    except Exception as exc:  # noqa: BLE001
        print("warn: decode/resize failed (%s)" % exc, file=sys.stderr)
        return None


def main() -> int:
    teams = []
    for ep in TEAM_ENDPOINTS:
        acc = []
        for page in (1, 2, 3):
            d = as_list(fetch_json(f"{ep}&page%5Bnumber%5D={page}"))
            if not d:
                break
            acc += d
            if len(d) < 100:          # 不足一页 = 到底了
                break
        print("endpoint %s -> %d rows" % (ep.split('?')[0], len(acc)))
        if len(acc) > len(teams):
            teams = acc
        if len(teams) >= TARGET_TEAMS:
            break
    if not teams:
        print("no team ranking available; shipping without pack")
        return 0

    embedded = load_embedded_ids()
    slots: list[dict] = []          # {"aliases": [...], "color": u32, "png": bytes}
    seen: set[str] = set()

    for t in teams:
        if len(slots) >= TARGET_TEAMS:
            break
        if not isinstance(t, dict):
            continue
        name = pick(t, ("name", "title", "nickname")) or ""
        slug = pick(t, ("slug", "short_name", "abbreviation")) or ""
        key = norm(slug) or norm(name)
        if not key or key in seen:
            continue
        url = pick(t, ("logo_url", "image_url", "logo", "image",
                       "logo_url_dark", "image_url_dark", "icon"),
                   "team", "attributes")
        if not url:
            continue
        url = str(url)
        if url.startswith("//"):
            url = "https:" + url
        elif url.startswith("/"):
            url = "https://bo3.gg" + url
        elif not url.startswith("http"):
            continue
        png = fetch(url, IMG_HDRS, timeout=25)
        if not png:
            continue
        # 校验确实是可解码图片(有些 CDN 404 也返回 200 + HTML)
        if image_to_rgb565(png, 8) is None:
            continue

        color_hex = COLORS.get(key, "") or COLORS.get(norm(name), "")
        if color_hex:
            color = int(color_hex.lstrip("#"), 16)
        else:
            color = int(PALETTE[sum(key.encode()) % len(PALETTE)].lstrip("#"), 16)

        aliases = {key, norm(name)}
        if embedded.get(key):
            aliases.add(embedded[key])          # 内嵌 id,如 "the-mongolz"
        aliases.discard("")
        seen.add(key)
        slots.append({"aliases": sorted(aliases), "color": color, "png": png})
        print("  [%3d] %-22s %s" % (len(slots), name[:22], url[:60]))

    if not slots:
        print("no downloadable logos; shipping without pack")
        return 0

    n = len(slots)
    alias_rows = []
    for slot, s in enumerate(slots):
        for a in s["aliases"]:
            if len(a.encode("utf-8")) < 24:
                alias_rows.append((a, slot, s["color"]))
    alias_n = len(alias_rows)
    alias_off = 64
    p48_off = alias_off + alias_n * 32
    p20_off = p48_off + n * 48 * 48 * 2
    total = p20_off + n * 20 * 20 * 2

    body = bytearray()
    for a, slot, color in alias_rows:
        # 32B: id[24] + u16 slot + u16 rsvd + u32 color (与固件 pack_alias_t 对齐)
        body += a.encode("ascii", "ignore")[:23].ljust(24, b"\0")
        body += struct.pack("<HHI", slot, 0, color)
    assert len(body) == p48_off - 64

    for s in slots:
        body += image_to_rgb565(s["png"], 48)
    for s in slots:
        body += image_to_rgb565(s["png"], 20)

    hdr = struct.pack("<4s8I", b"CSRP", 1, n, alias_n,
                      alias_off, p48_off, p20_off,
                      zlib.crc32(bytes(body)) & 0xFFFFFFFF, total)
    hdr += b"\0" * (64 - len(hdr))

    os.makedirs(os.path.dirname(os.path.abspath(OUT)), exist_ok=True)
    with open(OUT, "wb") as fh:
        fh.write(hdr + bytes(body))
    print("pack: %d teams, %d aliases, %d bytes -> %s"
          % (n, alias_n, total + 64, OUT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
