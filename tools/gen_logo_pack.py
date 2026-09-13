#!/usr/bin/env python3
"""Generate the CSR3 logo resource pack for the "csres" flash partition.

Fetches the CS2 team list from bo3.gg's public API (same host and headers as
update_matches.py), downloads each team's real logo, resizes it to 48x48 and
20x20 RGB565 and packs up to 200 teams:

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
import time
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

# 端点候选:CI 实测 teams?page[limit]=100 可用;limit=250 会 422(上限 100),
# page[number] 被忽略 —— 所以先试大 limit 一页拿满,不行再在翻页时用
# page[number] / page 两种风格自动探测。
TEAM_ENDPOINTS = (
    "teams?page%5Blimit%5D=300",
    "teams?page%5Blimit%5D=200",
    "teams?page%5Blimit%5D=100",
)


def _absorb(acc, seen, rows):
    """把 rows 里没见过的新行加进 acc,返回新增数。"""
    fresh = 0
    for t in rows:
        k = None
        if isinstance(t, dict):
            k = t.get("id") or norm(t.get("slug") or t.get("name") or "")
        if k and k in seen:
            continue
        if k:
            seen.add(k)
        acc.append(t)
        fresh += 1
    return fresh


def fetch_teams():
    """先试大 limit 一页拿满;不行则翻页(双风格探测),按新增行数判停。"""
    best = []
    for si, ep in enumerate(TEAM_ENDPOINTS):
        acc, seen = [], set()
        first = as_list(fetch_json(ep))
        if not first:
            print("endpoint#%d -> unavailable" % (si + 1))
            continue
        acc.extend(first)
        fresh = _absorb(acc, seen, first)   # 首页去重计数
        print("endpoint#%d page1 -> %d rows" % (si + 1, len(first)))
        if len(first) >= 100:               # 可能被截断,翻页补
            for page in range(2, 8):
                got = False
                for style in (lambda n: f"page%5Bnumber%5D={n}", lambda n: f"page={n}"):
                    d = as_list(fetch_json(f"{ep}&{style(page)}"))
                    if d and _absorb_probe(acc, seen, d):
                        got = True
                        break
                print("endpoint#%d page%d -> %s" % (si + 1, page, "new rows" if got else "no new rows"))
                if not got:
                    break
        print("endpoint#%d total unique rows: %d" % (si + 1, len(acc)))
        if len(acc) > len(best):
            best = acc
        if len(best) >= TARGET_TEAMS + 150:
            break
    return best


def _absorb_probe(acc, seen, rows):
    """探测性吸收:复制 seen 试加,有新行才真正落账(避免翻页风格污染数据)。"""
    trial = set(seen)
    add = []
    for t in rows:
        k = None
        if isinstance(t, dict):
            k = t.get("id") or norm(t.get("slug") or t.get("name") or "")
        if k and k in trial:
            continue
        if k:
            trial.add(k)
        add.append(t)
    if not add:
        return False
    for t in add:
        k = t.get("id") or norm(t.get("slug") or t.get("name") or "") if isinstance(t, dict) else None
        if k:
            seen.add(k)
        acc.append(t)
    return True


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


URL_KEYS = ("logo_url", "image_url", "logo", "image", "logo_url_dark",
            "image_url_dark", "icon", "avatar")


def extract_url(t: dict) -> str | None:
    """尽量把队标 URL 从各种可能的形态里挖出来(顶层/一层嵌套/dict.url)。"""
    def probe(v):
        if isinstance(v, str) and v.strip():
            return v.strip()
        if isinstance(v, dict):                      # image: {url: ...}
            for k in ("url", "original", "default", "src"):
                s = v.get(k)
                if isinstance(s, str) and s.strip():
                    return s.strip()
        return None
    for k in URL_KEYS:
        u = probe(t.get(k))
        if u:
            return u
    for sub in ("team", "attributes", "relations", "media"):
        s = t.get(sub)
        if isinstance(s, dict):
            for k in URL_KEYS:
                u = probe(s.get(k))
                if u:
                    return u
    return None


def absolutize(url: str) -> str | None:
    if url.startswith("//"):
        return "https:" + url
    if url.startswith("/"):
        return "https://bo3.gg" + url
    if url.startswith("http"):
        return url
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
    teams = fetch_teams()
    if not teams:
        print("no team ranking available; shipping without pack")
        return 0

    embedded = load_embedded_ids()
    slots: list[dict] = []
    seen: set[str] = set()
    n_nourl = n_fetchfail = n_badimg = 0

    for t in teams:
        if len(slots) >= TARGET_TEAMS:
            break
        if not isinstance(t, dict):
            continue
        name = (t.get("name") or t.get("title") or t.get("nickname") or "")
        slug = (t.get("slug") or t.get("short_name") or t.get("abbreviation") or "")
        key = norm(slug) or norm(name)
        if not key or key in seen:
            continue
        url = extract_url(t)
        if not url:
            n_nourl += 1
            continue
        url = absolutize(url)
        if not url:
            n_nourl += 1
            continue

        png = fetch(url, IMG_HDRS, timeout=25)
        if not png:                       # 限流重试一次
            time.sleep(1.5)
            png = fetch(url, IMG_HDRS, timeout=25)
        if not png:
            n_fetchfail += 1
            continue
        if image_to_rgb565(png, 8) is None:
            n_badimg += 1
            continue
        time.sleep(0.15)                  # 温和一点,别把 CDN 惹毛

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

    print("team scan done: %d slots, no-url=%d fetch-fail=%d bad-img=%d"
          % (len(slots), n_nourl, n_fetchfail, n_badimg))
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
