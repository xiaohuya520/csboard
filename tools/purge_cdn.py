#!/usr/bin/env python3
"""清 jsDelivr 上 cs_matches.json 的缓存,并等到 CDN 内容与本地文件一致。

jsDelivr purge 端点的三个坑(都踩过):
  1. 只接受 GET,POST 会 405。
  2. 返回 200 **不代表真的清掉了**。响应体里 per-path 会有
     {"throttled": true, "throttlingReset": N} —— 节流期间 purge 是空转的,
     CDN 继续吐旧数据,而 HTTP 状态码照样 200。这正是之前"每次都 purge ok,
     线上数据却停在几小时前"的原因:刷新脚本一轮一清,全被节流。
     处理:看到 throttled 就按 throttlingReset 退避后重试。
  3. 清完不是立刻生效,一般要几十秒才能取到新版,所以要轮询比对 md5。

用法:python3 tools/purge_cdn.py cs_matches.json [--repo owner/name] [--settle 20] [--max-wait 900]
"""
import argparse
import hashlib
import json
import sys
import time
import urllib.error
import urllib.request

UA = "Mozilla/5.0 (compatible; csboard-bot)"


def fetch(url, timeout=30):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def md5_remote(url):
    return hashlib.md5(fetch(url)).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--repo", default="xiaohuya520/csboard")
    ap.add_argument("--branch", default="main")
    ap.add_argument("--settle", type=int, default=20, help="purge 后等多少秒再去比对")
    ap.add_argument("--max-wait", type=int, default=900, help="最多折腾多少秒")
    a = ap.parse_args()

    rel = a.path.split("/")[-1]
    cdn = f"https://cdn.jsdelivr.net/gh/{a.repo}@{a.branch}/{rel}"
    purge = f"https://purge.jsdelivr.net/gh/{a.repo}@{a.branch}/{rel}"

    want = hashlib.md5(open(a.path, "rb").read()).hexdigest()
    deadline = time.time() + a.max_wait
    round_no = 0

    while time.time() < deadline:
        round_no += 1
        throttled, reset = False, 0
        try:
            body = json.loads(fetch(purge).decode())
            info = (body.get("paths") or {}).get(f"/gh/{a.repo}@{a.branch}/{rel}") or {}
            throttled = bool(info.get("throttled"))
            reset = int(info.get("throttlingReset") or 0)
            print(f"purge #{round_no}: throttled={throttled} reset={reset}s")
        except urllib.error.HTTPError as exc:
            print(f"purge #{round_no}: HTTP {exc.code} {exc.read().decode()[:120]}")
        except Exception as exc:  # noqa: BLE001
            print(f"purge #{round_no}: err {exc!r:.120}")

        if throttled and reset > 0:
            left = deadline - time.time()
            if reset + 5 >= left:
                print("节流窗口比剩余时间还长,放弃,留给下一轮")
                return 1
            print(f"被节流,等 {reset + 5}s 再试")
            time.sleep(reset + 5)
            continue

        time.sleep(a.settle)
        try:
            got = md5_remote(cdn)
        except Exception as exc:  # noqa: BLE001
            print(f"cdn fetch err {exc!r:.120}")
            continue
        print(f"  local={want} cdn={got} {'MATCH' if got == want else 'still old'}")
        if got == want:
            print("CDN 已是最新版本")
            return 0

    print("超时:CDN 仍未同步到最新版(不影响本轮数据,下轮继续清)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
