#!/usr/bin/env bash
# 构建 + 合并 + 校验 + (尽力)打包 200 队队标资源。CI 用 ./tools/validate.sh --firmware。
#
# 队标分两层:105 队内嵌在 app(cs_logo_data.c),另有 "csres" 资源分区容纳
# 世界前 200 队的扩展包(gen_logo_pack.py 在 CI 内联网抓取生成,splice_res.py
# 拼进整片镜像)。抓取/打包失败不影响构建 —— 固件自动退回内嵌层。
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

run_firmware_checks() (
    local build_dir

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py 不可用,请先激活 ESP-IDF 5.5.3" >&2
        return 1
    fi

    build_dir="$(mktemp -d /tmp/csboard-firmware.XXXXXX)"
    trap 'case "${build_dir}" in /tmp/csboard-firmware.*) rm -rf -- "${build_dir}" ;; esac' EXIT

    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${build_dir}" \
        -D "SDKCONFIG=${build_dir}/sdkconfig" build

    idf.py -B "${build_dir}" merge-bin -o "${build_dir}/csboard-full.bin"
    python3 tools/verify_firmware.py "${build_dir}"

    mkdir -p "${repo_root}/build"
    install -m 0644 "${build_dir}/csboard-full.bin" "${repo_root}/build/csboard-full.bin"
    echo "Firmware build: PASS"

    # 200 队队标资源包:要联网抓 bo3.gg 排名 + Pillow 解图,均为尽力而为;
    # 任一步失败就跳过,产物即纯内嵌固件(verify_firmware.py 在拼接前已通过,
    # splice_res.py 自身再断言保护区逐字节未动)。
    if python3 -m pip install --quiet pillow >/dev/null 2>&1 \
        && python3 tools/gen_logo_pack.py "${build_dir}/csres.bin" \
        && python3 tools/splice_res.py \
            "${repo_root}/build/csboard-full.bin" "${build_dir}/csres.bin"; then
        echo "Logo resource pack: OK"
    else
        echo "Logo resource pack: skipped (embedded-only firmware)"
    fi
)

run_data_check() {
    # 数据管道不依赖网络也能自检:语法 + 解析 + 体积预算。
    python3 -c "import ast,sys; ast.parse(open('tools/update_matches.py',encoding='utf-8').read())"
    python3 -c "import json; d=json.load(open('cs_matches.json',encoding='utf-8')); \
assert isinstance(d.get('matches'), list) and d['matches'], 'matches missing'; \
assert len(json.dumps(d,ensure_ascii=False).encode()) <= 16384, 'data over firmware buffer'; \
print('data check: OK (%d matches)' % len(d['matches']))"
}

cd "${repo_root}"
case "${mode}" in
    --all)
        run_data_check
        run_firmware_checks
        ;;
    --static)
        run_data_check
        ;;
    --firmware)
        run_firmware_checks
        ;;
    *)
        echo "Usage: $0 [--all|--static|--firmware]" >&2
        exit 2
        ;;
esac
