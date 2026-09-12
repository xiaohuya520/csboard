#!/usr/bin/env python3
"""Build cs_matches.json for the CS board firmware from bo3.gg's public API.

Output schema is what main/cs_data.c parses:
  {"updated": "MM-DD HH:MM", "source": "...", "matches": [ ... ]}

Why bo3.gg and not Sofascore:
  Sofascore simply does not expose per-map data. Verified by exhausting every
  sub-endpoint (/rounds /periods /maps /statistics all 404) and by grepping the
  match page's __NEXT_DATA__ (no map name anywhere). Its periodN fields are a
  boolean "who took map N", not round scores. On top of that Sofascore answers
  403 to GitHub runner IPs (browser UA included), so a hosted pipeline cannot
  use it at all. bo3.gg gives us map_name + per-map round scores and answers
  the runner just fine.

Verified bo3.gg quirks (do not "simplify" these away):
  * Date-range filters are silently ignored -- filter[matches.start_date][gte]
    returns the whole 80k-row table. So we filter by status and prune by date
    locally.
  * include=team1,team2 is ignored too; names must be looked up by id.
  * The working filter spelling is filter[<table>.<col>][eq]=... ; the shorter
    filter[match_id][eq]= is silently ignored and returns unrelated rows.
"""
from __future__ import annotations

import argparse
import gzip
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ALIAS_FILE = os.path.join(HERE, "team_aliases.json")
CACHE_FILE = os.path.join(HERE, "bo3_cache.json")

BASE = "https://api.bo3.gg/api/v1"
BEIJING = timezone(timedelta(hours=8))

MAX_LIVE = 6
MAX_FINISHED = 9
MAX_UPCOMING = 8
SIZE_BUDGET = 12000           # bytes; firmware buffer is 16384

# 只留这个级别以上的赛事,否则板子上全是青训/海选噪音。
# tier_rank: 1=s 2=a 3=b 4=c 5=d
MAX_TIER_RANK = 3

HDRS = {
    # bo3.gg 会挡掉没有 Origin 的请求(实测:本机与运行器都要求这两个头)。
    "User-Agent": ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                   "(KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36"),
    "Origin": "https://bo3.gg",
    "Referer": "https://bo3.gg/",
    "Accept": "application/json",
    "Accept-Encoding": "gzip",
}

MAP_CN = {
    "mirage": "荒漠迷城", "inferno": "炼狱小镇", "nuke": "核子危机",
    "ancient": "远古遗迹", "anubis": "阿努比斯", "dust2": "炙热沙城",
    "overpass": "死亡游乐园", "train": "列车停放站", "vertigo": "殒命大厦",
    "cache": "死城之谜", "cobblestone": "古堡激战", "cbble": "古堡激战",
}

# Brand colours for the teams we ship logos for. Unknown teams get a stable
# hash-derived colour so a team always looks the same on the board.
COLORS = {
    "navi": "#F2E14C", "vitality": "#FFD928", "g2": "#E4AE39", "faze": "#B01C24",
    "mouz": "#E43B2F", "spirit": "#8C6239", "astralis": "#E43B2F", "falcons": "#00B36B",
    "virtuspro": "#F58220", "mongolz": "#E4A11B", "eternalfire": "#1E90FF",
    "aurora": "#2AA3EF", "heroic": "#0E5C8A", "liquid": "#1E4FA8", "complexity": "#1F3C88",
    "furia": "#111111", "pain": "#E4002B", "imperial": "#C8102E", "betboom": "#0A5C36",
    "9pandas": "#C8102E", "cloud9": "#2B8FE8", "big": "#0B0B0B", "gamerlegion": "#6B4FBB",
    "tyloo": "#D7182A", "rareatom": "#7B4FBF", "ence": "#123B6B", "og": "#1B6E4F",
    "fnatic": "#F57C20", "nip": "#0F5E3A", "m80": "#5A5AE6", "wildcard": "#7D5BD6",
    "nrg": "#2C2C2C", "mibr": "#0E7C4A", "koi": "#2B3EE0", "legacy": "#0E7C4A",
    "nemiga": "#D0202F", "itb": "#1F4B8F", "atk": "#E23B2E", "b8": "#3AA0E8",
    "passionua": "#1B7DE0", "sharks": "#1F6FEB", "sangal": "#7A5CD6", "monte": "#E8A33D",
}

PALETTE = ["#2F81F7", "#2EA043", "#D29922", "#E5484D", "#8B5CF6",
           "#0E7490", "#B45309", "#BE185D", "#15803D", "#4338CA"]


def norm(s: str) -> str:
    return re.sub(r"[^a-z0-9]", "", (s or "").lower())


def clip_bytes(s: str, budget: int) -> str:
    """Trim to at most `budget` UTF-8 bytes so the firmware's fixed char arrays
    never cut a multi-byte sequence in half (which renders as garbage).

    Prefers a word boundary and marks the cut with an ellipsis.
    """
    s = (s or "").strip()
    if len(s.encode("utf-8")) <= budget:
        return s
    tail = "…"
    room = budget - len(tail.encode("utf-8"))
    out, used = [], 0
    for ch in s:
        n = len(ch.encode("utf-8"))
        if used + n > room:
            break
        out.append(ch)
        used += n
    cut = "".join(out)
    sp = cut.rfind(" ")
    if sp >= max(4, len(cut) // 2):
        cut = cut[:sp]
    return cut.rstrip() + tail


def load_aliases() -> dict[str, str]:
    """normalised name -> logo id, built from the shipped logo list + hand aliases."""
    table: dict[str, str] = {}
    try:
        with open(ALIAS_FILE, encoding="utf-8") as fh:
            for row in json.load(fh):
                logo_id, name, repo = row[0], row[1], row[2]
                for v in (logo_id, name, repo):
                    if v:
                        table[norm(v)] = logo_id
    except (OSError, ValueError) as exc:
        print("warn: cannot load %s (%s)" % (ALIAS_FILE, exc), file=sys.stderr)

    # Strip-the-decoration fallbacks.
    for variant, logo_id in list(table.items()):
        for prefix in ("team", "the"):
            if variant.startswith(prefix) and len(variant) > len(prefix):
                table.setdefault(variant[len(prefix):], logo_id)
        for suffix in ("esports", "gaming", "clan", "academy", "gg"):
            if variant.endswith(suffix) and len(variant) > len(suffix):
                table.setdefault(variant[: -len(suffix)], logo_id)
    return table


def team_color(logo_id: str, name: str) -> str:
    if logo_id and logo_id in COLORS:
        return COLORS[logo_id]
    return PALETTE[sum(name.encode("utf-8")) % len(PALETTE)]


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------
def api(path: str, attempts: int = 4):
    url = "%s/%s" % (BASE, path)
    last = None
    for i in range(attempts):
        try:
            req = urllib.request.Request(url, headers=HDRS)
            with urllib.request.urlopen(req, timeout=30) as resp:
                raw = resp.read()
                if resp.headers.get("Content-Encoding") == "gzip":
                    raw = gzip.decompress(raw)
                return json.loads(raw.decode("utf-8"))
        except (urllib.error.URLError, urllib.error.HTTPError, ValueError, OSError) as exc:
            last = exc
            time.sleep(1.5 * (i + 1))
    raise RuntimeError("GET %s failed: %r" % (url, last))


def api_soft(path: str):
    try:
        return api(path, attempts=2)
    except RuntimeError as exc:
        print("warn: %s" % exc, file=sys.stderr)
        return None


# ---------------------------------------------------------------------------
# 名字缓存:队名/赛事名只能按 id 查,缓存住才不至于每轮都打几十次接口
# ---------------------------------------------------------------------------
class NameCache:
    def __init__(self):
        self.teams: dict[str, str] = {}
        self.tours: dict[str, str] = {}
        try:
            with open(CACHE_FILE, encoding="utf-8") as fh:
                d = json.load(fh)
                self.teams = d.get("teams") or {}
                self.tours = d.get("tours") or {}
        except (OSError, ValueError):
            pass
        self.dirty = False

    def save(self):
        if not self.dirty:
            return
        try:
            with open(CACHE_FILE, "w", encoding="utf-8", newline="\n") as fh:
                json.dump({"teams": self.teams, "tours": self.tours}, fh,
                          ensure_ascii=False, sort_keys=True, indent=0)
        except OSError as exc:
            print("warn: cannot save cache (%s)" % exc, file=sys.stderr)

    def _resolve(self, table: dict, key: str, ids: list[str]) -> None:
        todo = [i for i in ids if i and i != "None" and i not in table]
        if not todo:
            return

        # 先试批量。filter[<表>.id][in]= 是否支持没实测过,所以用 total.count 校验:
        # bo3 对不支持的过滤是「静默忽略」而不是报错,不看 count 会拿到全表。
        # 必须整批循环着做 —— 只处理第一块的话,143 支队里会有 83 支解析不到,
        # 界面上就成了 "T759" 这种占位符。
        batch_ok = None
        got = 0
        pos = 0
        while pos < len(todo):
            chunk = todo[pos:pos + 60]
            pos += 60
            n = 0
            if batch_ok is not False:
                d = api_soft("%s?filter%%5B%s.id%%5D%%5Bin%%5D=%s&page%%5Blimit%%5D=%d"
                             % (key, key, ",".join(chunk), len(chunk)))
                if d and (d.get("total") or {}).get("count") == len(chunk):
                    for r in d.get("results") or []:
                        if r.get("name"):
                            table[str(r["id"])] = r["name"]
                            n += 1
                    batch_ok = True
            if not n:
                # 批量不通(或这一块没结果),退回逐条查
                if batch_ok is None:
                    batch_ok = False
                for tid in chunk:
                    d = api_soft("%s?filter%%5B%s.id%%5D%%5Beq%%5D=%s" % (key, key, tid))
                    for r in (d or {}).get("results") or []:
                        if r.get("name"):
                            table[str(r["id"])] = r["name"]
                            n += 1
                    time.sleep(0.05)
            got += n
        if got:
            self.dirty = True
        print("  resolved %d/%d %s names (batch=%s)"
              % (got, len(todo), key, {True: "yes", False: "no"}.get(batch_ok, "n/a")))

    def teams_of(self, ids) -> None:
        self._resolve(self.teams, "teams", sorted({str(i) for i in ids}))

    def tours_of(self, ids) -> None:
        self._resolve(self.tours, "tournaments", sorted({str(i) for i in ids}))

    def team(self, tid) -> str:
        return self.teams.get(str(tid), "")

    def tour(self, tid) -> str:
        return self.tours.get(str(tid), "")


# ---------------------------------------------------------------------------
# 取比赛
# ---------------------------------------------------------------------------
def fetch_status(status: str, sort: str, limit: int) -> list[dict]:
    d = api_soft("matches?filter%%5Bmatches.status%%5D%%5Beq%%5D=%s&sort=%s&page%%5Blimit%%5D=%d"
                 % (status, sort, limit))
    if not d:
        return []
    rows = d.get("results") or []
    print("  status=%-9s got %d rows (server says %s)"
          % (status, len(rows), (d.get("total") or {}).get("count")))
    return rows


def parse_when(s: str):
    if not s:
        return None
    try:
        # bo3 给的是 "2026-09-12T17:15:00.000+00:00"
        return datetime.fromisoformat(s.replace("Z", "+00:00"))
    except ValueError:
        return None


def map_label(raw: str) -> tuple[str, str]:
    """'de_inferno' -> ('inferno', '炼狱小镇')"""
    name = (raw or "").strip()
    if name.startswith("de_"):
        name = name[3:]
    return name, MAP_CN.get(norm(name), name[:11])


def build_maps(match: dict, games: list[dict], max_maps: int) -> list[dict]:
    """把 bo3 的 games 翻成固件要的每图比分。

    bo3 只给「胜方分/负方分」,归属要看比赛对象的 maps_score[]
    (第 N 项为 true 表示主队拿下第 N 图)。两者合起来才能还原主/客比分。
    """
    owner = match.get("maps_score") or []
    out = []
    games = sorted(games, key=lambda g: (g.get("number") or 0))
    for i, g in enumerate(games[:max_maps]):
        name, cn = map_label(g.get("map_name") or "")
        if not name:
            continue
        w = g.get("winner_clan_score")
        l = g.get("loser_clan_score")
        s1 = s2 = 0
        winner = 0
        if w is not None and l is not None:
            team1_won = bool(owner[i]) if i < len(owner) else True
            if team1_won:
                s1, s2 = int(w), int(l)
                winner = 1
            else:
                s1, s2 = int(l), int(w)
                winner = 2
        out.append({"name": name[:11], "cn": cn[:11],
                    "s1": s1, "s2": s2, "winner": winner})
    return out


def build_match(m: dict, aliases: dict[str, str], cache: NameCache):
    status = {"current": "live", "finished": "finished"}.get(m.get("status"), "upcoming")
    t1_id, t2_id = m.get("team1_id"), m.get("team2_id")
    if not t1_id or not t2_id:
        return None

    when = parse_when(m.get("start_date"))
    if not when:
        return None
    when = when.astimezone(BEIJING)

    n1 = cache.team(t1_id) or ("T%s" % t1_id)
    n2 = cache.team(t2_id) or ("T%s" % t2_id)
    tour = cache.tour(m.get("tournament_id")) or "CS2"

    logo1 = aliases.get(norm(n1), "")
    logo2 = aliases.get(norm(n2), "")

    # 预告也查:bo3 会把已选好的地图列出来,卡片上能显示图池。
    d = api_soft("games?filter%%5Bgames.match_id%%5D%%5Beq%%5D=%s&page%%5Blimit%%5D=5"
                 % m["id"])
    games = (d or {}).get("results") or []

    bo = int(m.get("bo_type") or 3)
    return {
        "status": status,
        "event": clip_bytes(tour, 27),
        "stage": "",
        "date": when.strftime("%m-%d"),
        "time": when.strftime("%H:%M"),
        "bo": "BO%d" % bo,
        "team1": {"name": clip_bytes(n1, 18), "logo": logo1, "color": team_color(logo1, n1)},
        "team2": {"name": clip_bytes(n2, 18), "logo": logo2, "color": team_color(logo2, n2)},
        "score1": int(m.get("team1_score") or 0),
        "score2": int(m.get("team2_score") or 0),
        "maps": build_maps(m, games, 5 if bo >= 5 else 3),
        "_tier": int(m.get("tier_rank") or 5),
        "_rate": float(m.get("rating") or 0),
        "_logo": 1 if (logo1 or logo2) else 0,
        "_ts": int(when.timestamp()),
    }


def encode(data: dict) -> bytes:
    return json.dumps(data, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "cs_matches.json"))
    ap.add_argument("--days-back", type=int, default=3)
    ap.add_argument("--days-fwd", type=int, default=7)
    args = ap.parse_args()

    aliases = load_aliases()
    cache = NameCache()
    now = datetime.now(BEIJING)
    lo = now - timedelta(days=args.days_back)
    hi = now + timedelta(days=args.days_fwd)

    print("bo3.gg pipeline: window %s .. %s" % (lo.strftime("%m-%d %H:%M"),
                                                hi.strftime("%m-%d %H:%M")))

    raw = []
    raw += fetch_status("current", "start_date", 50)
    raw += fetch_status("upcoming", "start_date", 100)
    raw += fetch_status("finished", "-start_date", 100)

    # 日期过滤只能在本地做(bo3 会静默忽略日期条件)。
    seen, rows = set(), []
    for m in raw:
        mid = m.get("id")
        w = parse_when(m.get("start_date"))
        if not mid or mid in seen or not w:
            continue
        w = w.astimezone(BEIJING)
        if not (lo <= w <= hi):
            continue
        seen.add(mid)
        rows.append(m)
    print("after local date pruning: %d matches" % len(rows))

    # 先把要用的名字一次解析掉,省请求
    cache.teams_of([m.get("team1_id") for m in rows] + [m.get("team2_id") for m in rows])
    cache.tours_of([m.get("tournament_id") for m in rows])

    parsed = []
    for m in rows:
        try:
            built = build_match(m, aliases, cache)
        except RuntimeError as exc:
            print("warn: skip match %s (%s)" % (m.get("id"), exc), file=sys.stderr)
            continue
        if built:
            parsed.append(built)
    print("parsed %d matches" % len(parsed))

    # 上榜条件:赛事级别够高,或者至少有一支队伍有队标可显示。
    notable = [m for m in parsed if m["_tier"] <= MAX_TIER_RANK or m["_logo"]]
    if len(notable) < 6:
        notable = parsed
    print("notable %d / parsed %d" % (len(notable), len(parsed)))

    buckets: dict[str, list] = {"live": [], "finished": [], "upcoming": []}
    for m in notable:
        buckets[m["status"]].append(m)

    # 直播按热度排;已结束按时间倒序;预告按时间正序。同级按赛事级别再排一次。
    buckets["live"].sort(key=lambda m: (m["_tier"], -m["_rate"]))
    buckets["finished"].sort(key=lambda m: -m["_ts"])
    buckets["upcoming"].sort(key=lambda m: (m["_ts"], m["_tier"]))

    picks = (buckets["live"][:MAX_LIVE]
             + buckets["finished"][:MAX_FINISHED]
             + buckets["upcoming"][:MAX_UPCOMING])

    def compose(rs):
        out = {
            "updated": datetime.now(BEIJING).strftime("%m-%d %H:%M"),
            "source": "bo3.gg",
            "matches": [],
        }
        for m in rs:
            out["matches"].append({k: v for k, v in m.items() if not k.startswith("_")})
        return out

    data = compose(picks)
    blob = encode(data)
    while len(blob) > SIZE_BUDGET and len(picks) > 4:
        picks = picks[:-1]
        data = compose(picks)
        blob = encode(data)

    if not data["matches"]:
        print("ERROR: no matches assembled; refusing to publish empty data", file=sys.stderr)
        return 1

    with open(args.out, "wb") as fh:
        fh.write(blob)
    cache.save()

    ms = data["matches"]
    with_scores = sum(1 for m in ms for mp in m.get("maps", []) if (mp["s1"] or mp["s2"]))
    print("wrote %s: %d matches (%d live / %d finished / %d upcoming), %d bytes"
          % (args.out, len(ms),
             sum(1 for m in ms if m["status"] == "live"),
             sum(1 for m in ms if m["status"] == "finished"),
             sum(1 for m in ms if m["status"] == "upcoming"),
             len(blob)))
    print("maps with real round scores: %d" % with_scores)
    for m in ms[:4]:
        print("   %-8s %-26s %s vs %s  %d:%d  maps=%s"
              % (m["status"], m["event"], m["team1"]["name"], m["team2"]["name"],
                 m["score1"], m["score2"],
                 [(mp["cn"], mp["s1"], mp["s2"]) for mp in m.get("maps", [])]))

    if len(blob) > SIZE_BUDGET:
        print("ERROR: still over budget", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
