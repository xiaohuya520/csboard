#!/usr/bin/env python3
"""清 jsDelivr 上 cs_matches.json 的缓存,并等到 CDN 内容与本地文件一致。

jsDelivr purge 端点的坑(全部实测踩过):
  1. 只接受 GET,POST 会 405。
  2. 返回 200 **不代表真的清掉了**。响应体 per-path 里可能是
     {"throttled": true, "throttlingReset": N} —— 节流期间 purge 是空转的,
     CDN 继续吐旧数据,而 HTTP 状态码照样 200。这正是之前"每轮都 purge ok、
     线上数据却停在几小时前"的原因:刷得越勤,越是被节流。
  3. 配额很紧,实测大约 10 次/小时的滚动窗口,超了 throttlingReset 会一下
     跳到 3000+ 秒(将近一小时)。所以这里**一次调用最多清两遍**:
     第一遍被节流就按 reset 退避后再来一次,之后只轮询比对,绝不再清 ——
     反复清只会把节流窗口越顶越长。
  4. 清完不是立刻生效:边缘节点会先短暂 404 再回源,一般几十秒后才拿到新版。

退出码:0=CDN 已是最新;1=超时未同步(数据本身没问题,下轮再清);2=被节流。
被节流时会打印 RESET=<秒>,调用方据此把下次清理时间推后。

用法:python3 tools/purge_cdn.py cs_matches.json [--settle 20] [--max-wait 600]
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


def one_purge(purge_url, key):
    """清一次,返回 (throttled, reset_seconds)。"""
    try:
        body = json.loads(fetch(purge_url).decode())
    except urllib.error.HTTPError as exc:
        print(f"purge HTTP {exc.code} {exc.read().decode()[:120]}")
        return False, 0
    except Exception as exc:  # noqa: BLE001
        print(f"purge err {exc!r:.120}")
        return False, 0
    info = (body.get("paths") or {}).get(key) or {}
    reset = int(info.get("throttlingReset") or 0)
    thr = bool(info.get("throttled"))
    print(f"purge: throttled={thr} reset={reset}s")
    return thr, reset


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--repo", default="xiaohuya520/csboard")
    ap.add_argument("--branch", default="main")
    ap.add_argument("--settle", type=int, default=20, help="两次比对之间等多少秒")
    ap.add_argument("--max-wait", type=int, default=600, help="最多轮询多少秒")
    a = ap.parse_args()

    rel = a.path.split("/")[-1]
    key = f"/gh/{a.repo}@{a.branch}/{rel}"
    cdn = f"https://cdn.jsdelivr.net{key}"
    purge = f"https://purge.jsdelivr.net{key}"

    want = hashlib.md5(open(a.path, "rb").read()).hexdigest()
    deadline = time.time() + a.max_wait

    thr, reset = one_purge(purge, key)
    if thr and reset > 0:
        print(f"RESET={reset}")
        left = deadline - time.time()
        if reset + 5 < left:
            print(f"被节流,等 {reset + 5}s 后重试一次")
            time.sleep(reset + 5)
            thr, reset = one_purge(purge, key)
            if thr:
                print(f"RESET={reset}")
                print("仍在节流窗口内,放弃(再清只会把窗口顶得更长)")
                return 2
        else:
            print("节流窗口比剩余时间还长,放弃,留给下一轮")
            return 2

    # 之后只轮询比对,不再消耗配额
    while time.time() < deadline:
        time.sleep(a.settle)
        try:
            got = hashlib.md5(fetch(cdn)).hexdigest()
        except urllib.error.HTTPError as exc:
            print(f"cdn HTTP {exc.code}(刚清完的边缘节点会短暂 404,正常)")
            continue
        except Exception as exc:  # noqa: BLE001
            print(f"cdn err {exc!r:.120}")
            continue
        print(f"local={want} cdn={got} {'MATCH' if got == want else 'still old'}")
        if got == want:
            print("CDN 已是最新版本")
            return 0

    print("超时:CDN 仍未同步到最新版(数据本身已进仓库,下轮继续清)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
