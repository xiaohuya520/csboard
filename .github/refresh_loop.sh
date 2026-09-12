#!/usr/bin/env bash
# 一次 CI 运行里反复刷新 cs_matches.json。
#
# 为什么需要这个脚本:GitHub 的 schedule 触发根本不可靠。
# 实测本仓库 cron 写成 */10,26 小时里只调度到 1 次,数据直接断流 20 小时以上 ——
# 光靠 cron 是撑不起"实时比分"的,而 workflow_dispatch 又要 Actions 写权限(手上 token 没有)。
# 所以改成:只要抢到一次运行机会,就把这段时间用满 —— 进来后自己按间隔轮询,
# 一直跑到 RUN_FOR 秒为止。push 触发走的是同一条路径,手动戳一下也能续上近两小时。
#
# 刷新和清缓存是两件事,节奏故意不一样:
#   数据  每 8 分钟刷一次(仓库里的 json 尽量新)
#   缓存  至少隔 15 分钟才清一次 —— jsDelivr 的 purge 对同一路径按滚动窗口限流
#         (实测约 10 次/小时),超了就返回 {"throttled": true} 并直接空转,
#         HTTP 照样 200,而且窗口会被顶到 3000 秒以上。清理是否真生效由
#         purge_cdn.py 比对 md5 判定,不再信状态码;被节流时它会打印 RESET=<秒>,
#         这里据此把下一次清理时间推后,不去硬顶配额。
set -u

REFRESH_EVERY=${REFRESH_EVERY:-480}   # 两轮抓取之间睡多久(秒),默认 8 分钟
PURGE_EVERY=${PURGE_EVERY:-900}       # 两次清缓存的最小间隔(秒),默认 15 分钟
RUN_FOR=${RUN_FOR:-6900}              # 这次运行总共持续多久(秒),默认 115 分钟

git config user.name  "csboard-bot"
git config user.email "csboard-bot@users.noreply.github.com"

round=0
last_purge=0
next_purge_at=0        # 第一次进来就清;之后按 PURGE_EVERY / 节流反馈排期
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
        now=$(date +%s)
        if [ "$now" -ge "$next_purge_at" ]; then
          # purge_cdn.py 最多清两遍(被节流会退避重试一次),然后靠比对 md5 确认
          out=$(python3 tools/purge_cdn.py cs_matches.json 2>&1) || true
          echo "$out"
          reset=$(echo "$out" | grep -o 'RESET=[0-9]*' | tail -1 | cut -d= -f2 || true)
          last_purge=$now
          next_purge_at=$(( now + PURGE_EVERY ))
          # 被节流就把下次清理推到窗口之后,别硬顶配额
          if [ -n "${reset:-}" ] && [ "$reset" -gt "$PURGE_EVERY" ]; then
            next_purge_at=$(( now + reset + 60 ))
            echo "被节流,下次清理推迟到 $(( (reset + 60) / 60 )) 分钟后"
          fi
        else
          echo "距下次清缓存还有 $(( next_purge_at - now ))s,本轮跳过(省配额)"
        fi
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
