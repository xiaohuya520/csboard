#!/usr/bin/env bash
# 构建 + 合并 + 校验。CI 用 ./tools/validate.sh --firmware。
#
# 注意:本工程的队标与中文字体是**编译期编入 app** 的(见 main/cs_logo_data.c 与
# main/cs_font_cn16.c),没有需要注入的资源分区。旧工程那套"合并后再往 csres 分区
# 里打补丁"的步骤在这里被彻底去掉了 —— 少一个后处理,就少一类静默损坏。
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
