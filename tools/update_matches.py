#!/usr/bin/env python3
"""Build cs_matches.json for the CS board firmware from Sofascore's public API.

Output schema is what main/cs_data.c parses:
  {"updated": "MM-DD HH:MM", "source": "...", "matches": [ ... ]}

Design notes:
  * One source, one shape. Anything the public API does not give us is omitted
    rather than faked (per-map round scores are unavailable, so maps carry the
    "who won this map" bit only; the firmware renders that as 胜/负).
  * "Notable" filter: an event is kept when either team resolves to a logo we
    ship, or its tournament is popular on Sofascore. Otherwise a day of
    academy/qualifier noise (85 events) would swamp the board.
  * Hard size budget: the firmware downloads into a 16 KB buffer and the board is
    a 240x320 screen. We cap counts and truncate strings, then assert the result.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import ssl
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ALIAS_FILE = os.path.join(HERE, "team_aliases.json")

BASE = "https://www.sofascore.com/api/v1"
CSGO_CATEGORY = 1572          # Counter Strike
BEIJING = timezone(timedelta(hours=8))

MAX_LIVE = 6
MAX_FINISHED = 9
MAX_UPCOMING = 8
SIZE_BUDGET = 12000           # bytes; firmware buffer is 16384

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


def http_json(url: str, attempts: int = 4, insecure: bool = False):
    ctx = None
    if insecure:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
    last = None
    for i in range(attempts):
        try:
            req = urllib.request.Request(url, headers={
                "User-Agent": "Mozilla/5.0 (compatible; csboard-pipeline)",
                "Accept": "application/json",
            })
            with urllib.request.urlopen(req, timeout=25, context=ctx) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except (urllib.error.URLError, urllib.error.HTTPError, ValueError, OSError) as exc:
            last = exc
            time.sleep(1.5 * (i + 1))
    raise RuntimeError("GET %s failed: %r" % (url, last))


def fetch_day(day: str, insecure: bool):
    url = "%s/category/%d/scheduled-events/%s" % (BASE, CSGO_CATEGORY, day)
    try:
        return http_json(url, insecure=insecure).get("events") or []
    except RuntimeError as exc:
        print("warn: %s" % exc, file=sys.stderr)
        return []


def popularity(event) -> int:
    uni = (event.get("tournament") or {}).get("uniqueTournament") or {}
    return int(uni.get("userCount") or 0)


def status_of(event) -> str:
    kind = ((event.get("status") or {}).get("type") or "").lower()
    if kind == "inprogress":
        return "live"
    if kind == "finished":
        return "finished"
    return "upcoming"


def score_of(event, side: str) -> int:
    s = event.get("%sScore" % side) or {}
    v = s.get("current")
    if v is None:
        v = s.get("display")
    try:
        return int(v or 0)
    except (TypeError, ValueError):
        return 0


def maps_of(event, status: str):
    """Per-map results. Sofascore exposes homeScore.periodN == 1 when the home
    team took map N, so we can report the winner even without round scores."""
    if status == "upcoming":
        return []
    out = []
    best_of = int(event.get("bestOf") or 3)
    for n in range(1, min(best_of, 5) + 1):
        h = (event.get("homeScore") or {}).get("period%d" % n) or 0
        a = (event.get("awayScore") or {}).get("period%d" % n) or 0
        if not h and not a:
            continue
        out.append({"name": "Map %d" % n, "cn": "第 %d 图" % n,
                    "s1": 0, "s2": 0, "winner": 1 if h > a else (2 if a > h else 0)})
    return out


def build_match(event, aliases: dict[str, str]):
    status = status_of(event)
    if status == "upcoming" and ((event.get("status") or {}).get("type") or "") in ("canceled", "postponed"):
        return None

    home = event.get("homeTeam") or {}
    away = event.get("awayTeam") or {}
    if not home.get("name") or not away.get("name"):
        return None

    ts = int(event.get("startTimestamp") or 0)
    if ts <= 0:
        return None
    when = datetime.fromtimestamp(ts, BEIJING)

    tour = event.get("tournament") or {}
    uni = tour.get("uniqueTournament") or {}
    # 层级:event = 联赛名,stage = 阶段名
    event_name = (uni.get("name") or tour.get("name") or "CS2")[:22]
    stage_name = (tour.get("name") or "")[:22]
    if stage_name and norm(stage_name) == norm(event_name):
        stage_name = ""

    t1_id = aliases.get(norm(home.get("name")), "")
    t2_id = aliases.get(norm(away.get("name")), "")

    return {
        "status": status,
        "event": event_name,
        "stage": stage_name,
        "date": when.strftime("%m-%d"),
        "time": when.strftime("%H:%M"),
        "bo": "BO%d" % int(event.get("bestOf") or 3),
        "team1": {"name": home["name"][:18], "logo": t1_id, "color": team_color(t1_id, home["name"])},
        "team2": {"name": away["name"][:18], "logo": t2_id, "color": team_color(t2_id, away["name"])},
        "score1": score_of(event, "home"),
        "score2": score_of(event, "away"),
        "maps": maps_of(event, status),
        "_pop": popularity(event) + (400 if (t1_id or t2_id) else 0),
        "_ts": ts,
    }


def encode(data: dict) -> bytes:
    return json.dumps(data, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "cs_matches.json"))
    ap.add_argument("--days-back", type=int, default=3)
    ap.add_argument("--days-fwd", type=int, default=7)
    ap.add_argument("--insecure", action="store_true",
                    help="skip TLS verification (only for hosts behind a MITM box)")
    args = ap.parse_args()

    aliases = load_aliases()
    today = datetime.now(BEIJING).date()

    events = []
    seen = set()
    for delta in range(-args.days_back, args.days_fwd + 1):
        day = (today + timedelta(days=delta)).isoformat()
        for ev in fetch_day(day, args.insecure):
            eid = ev.get("id")
            if eid in seen:
                continue
            seen.add(eid)
            events.append(ev)
    print("fetched %d raw events" % len(events))

    parsed = [m for m in (build_match(e, aliases) for e in events) if m]
    # 上榜条件:有队标可显示,或赛事在 Sofascore 上足够热。
    notable = [m for m in parsed if m["_pop"] > 300]
    if len(notable) < 6:
        notable = parsed
    print("notable %d / parsed %d" % (len(notable), len(parsed)))

    buckets: dict[str, list] = {"live": [], "finished": [], "upcoming": []}
    for m in notable:
        buckets[m["status"]].append(m)

    # 直播按热度排;已结束按时间倒序(最近的在最前);预告按时间正序。
    buckets["live"].sort(key=lambda m: -m["_pop"])
    buckets["finished"].sort(key=lambda m: -m["_ts"])
    buckets["upcoming"].sort(key=lambda m: m["_ts"])

    picks = (buckets["live"][:MAX_LIVE]
             + buckets["finished"][:MAX_FINISHED]
             + buckets["upcoming"][:MAX_UPCOMING])

    def compose(rows):
        out = {
            "updated": datetime.now(BEIJING).strftime("%m-%d %H:%M"),
            "source": "Sofascore",
            "matches": [],
        }
        for m in rows:
            out["matches"].append({k: v for k, v in m.items() if not k.startswith("_")})
        return out

    data = compose(picks)
    blob = encode(data)
    # 超预算就从尾部(预告)开始砍,直播永远保留。
    while len(blob) > SIZE_BUDGET and len(picks) > 4:
        picks = picks[:-1]
        data = compose(picks)
        blob = encode(data)

    if not data["matches"]:
        print("ERROR: no matches assembled; refusing to publish empty data", file=sys.stderr)
        return 1

    with open(args.out, "wb") as fh:
        fh.write(blob)

    n = len(data["matches"])
    print("wrote %s: %d matches (%d live / %d finished / %d upcoming), %d bytes"
          % (args.out, n,
             sum(1 for m in data["matches"] if m["status"] == "live"),
             sum(1 for m in data["matches"] if m["status"] == "finished"),
             sum(1 for m in data["matches"] if m["status"] == "upcoming"),
             len(blob)))
    if len(blob) > SIZE_BUDGET:
        print("ERROR: still over budget", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
