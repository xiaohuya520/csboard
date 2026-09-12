#!/usr/bin/env bash
# 一次 CI 运行里反复刷新 cs_matches.json。
#
# 为什么需要这个脚本:GitHub 的 schedule 触发根本不可靠。
# 实测本仓库 cron 写成 */10,26 小时里只调度到 1 次,数据直接断流 20 小时以上 ——
# 光靠 cron 是撑不起"实时比分"的,而 workflow_dispatch 又要 Actions 写权限(手上 token 没有)。
# 所以改成:只要抢到一次运行机会,就把这段时间用满 —— 进来后自己按间隔轮询,
# 一直跑到 RUN_FOR 秒为止。push 触发走的是同一条路径,手动戳一下也能续上近两小时。
set -u

REFRESH_EVERY=${REFRESH_EVERY:-480}   # 两轮之间睡多久(秒),默认 8 分钟
RUN_FOR=${RUN_FOR:-6900}              # 这次运行总共持续多久(秒),默认 115 分钟
REPO=${REPO:-xiaohuya520/csboard}
CDN="https://cdn.jsdelivr.net/gh/${REPO}@main/cs_matches.json"
PURGE="https://purge.jsdelivr.net/gh/${REPO}@main/cs_matches.json"

git config user.name  "csboard-bot"
git config user.email "csboard-bot@users.noreply.github.com"

round=0
end=$(( SECONDS + RUN_FOR ))
while [ "$SECONDS" -lt "$end" ]; do
  round=$(( round + 1 ))
  echo "=== round $round  (t=${SECONDS}s / ${RUN_FOR}s) ==="

  python3 tools/update_matches.py --out cs_matches.json || echo "warn: 抓取失败,本轮跳过"

  if [ -s cs_matches.json ]; then
    git add -A cs_matches.json tools/bo3_cache.json
    if git diff --cached --quiet; then
      echo "no change"
    else
      git commit -m "data: refresh CS2 matches" || echo "warn: commit 失败"
      # 别的运行可能也在提交,先 rebase 再推
      git pull --rebase --autostash origin main || { git rebase --abort || true; }
      if git push; then
        # jsDelivr 会按分支缓存文件,不清就一直拿到旧的。
        # purge 必须走专门的端点(只吃 GET,POST 会 405),而且不是秒生效 ——
        # 实测等 20 秒去取还是旧版,所以这里最多重试 5 轮、每轮等 25 秒,
        # 拿本地文件的 md5 跟 CDN 上的比,对上了才算真清干净。
        want=$(md5sum < cs_matches.json | cut -d' ' -f1)
        for i in 1 2 3 4 5; do
          curl -fsS --max-time 30 "$PURGE" > /dev/null 2>&1 || echo "warn: purge 第 $i 次失败"
          sleep 25
          got=$(curl -fsS --max-time 25 "$CDN" | md5sum | cut -d' ' -f1 || true)
          echo "purge try $i: local=$want cdn=$got"
          if [ "$got" = "$want" ]; then break; fi
        done
      else
        echo "warn: push 失败,回滚本地改动"
        git reset --hard origin/main || true
      fi
    fi
  fi

  left=$(( end - SECONDS ))
  if [ "$left" -le 0 ]; then break; fi
  nap=$REFRESH_EVERY
  if [ "$left" -lt "$nap" ]; then nap=$left; fi
  echo "sleep ${nap}s"
  sleep "$nap"
done
echo "=== loop done: $round rounds ==="
