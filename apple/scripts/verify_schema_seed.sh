#!/usr/bin/env bash
#
# verify_schema_seed.sh — 證明「使用者目錄裡沒有 default.custom.yaml」真的會讓
#                         引擎壞掉,不是只有畫面難看
#
# ── 這一關在回答什麼 ────────────────────────────────────────────────────
# 使用者在真 Mac 上回報:設定裡「啟用的方案」一列都沒有。查到的原因是
# `apple/scripts/build_app.sh` 只複製了 `core/data/shared`,從來沒有把
# `core/data/user/default.custom.yaml` 帶上,所以那個檔案在使用者機器上不存在。
#
# 「不存在會怎樣」是可以**問**的,不必用猜的。同一支 rime_console、
# 同一份 shared 資料,兩臂只差使用者目錄:
#
#   A(對照):core/data/user —— 有 collect_data.sh 產生的 default.custom.yaml
#   B(重現):一個空的暫存目錄 —— 就是沒有範本時使用者機器上的樣子
#
# ⚠ **實測結果比原本預期的更糟**(2026-08-10, run #101):
#   B 不是「列出上游那一份清單」,而是**部署直接失敗、一個方案都沒有**。
#   上游 rime-prelude 的 default.yaml 列了 cangjie5 / quick5,而我們沒有打包
#   那兩本詞庫 —— 部署整個任務就掛了。所以真機上的症狀不是「清單長得不一樣」,
#   是**引擎根本沒有可用的方案**,而畫面上只看得到一個空清單。
#
# ── 判準(這一關最容易變成空洞的地方) ──────────────────────────────────
# 「B 沒有 luna_pinyin_tw」單獨看是空洞的:B 什麼都沒印也會讓它成立。
# 所以 B 必須落在兩種**說得出來**的情形之一:
#   (a) 部署失敗(結束碼非 0)—— 那本身就是缺陷的樣子,而且要把原因印出來;
#   (b) 部署成功且列得出方案 —— 那就斷言那份清單裡沒有我們打包的預設方案。
# 「結束碼 0 又一個方案都沒有」是第三種:那代表這一關自己壞了,判失敗。
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONSOLE="${ROOT}/apple/build/rime_console"
SHARED="${ROOT}/core/data/shared"
USER_WITH="${ROOT}/core/data/user"
TEMPLATE="${USER_WITH}/default.custom.yaml"

# 我們真正打包、而上游 default.yaml **沒有**列的預設方案。
SHIPPED_ONLY="luna_pinyin_tw"

fails=0
ok()   { printf '  ✓ %s\n' "$1"; }
bad()  { printf '  ✗ %s\n' "$1" >&2; fails=$((fails + 1)); }
note() { printf '  · %s\n' "$1"; }

[ -x "${CONSOLE}" ] || { echo "錯誤: 找不到可執行的 ${CONSOLE}" >&2; exit 2; }
[ -d "${SHARED}" ]  || { echo "錯誤: 找不到 ${SHARED}" >&2; exit 2; }
[ -s "${TEMPLATE}" ] || { echo "錯誤: 找不到 ${TEMPLATE}(先跑 scripts/collect_data.sh)" >&2; exit 2; }

WORK="$(mktemp -d)"
cleanup() { rm -rf "${WORK}"; }
trap cleanup EXIT INT TERM

ARM_OUT=""; ARM_RC=0; ARM_SCHEMAS=""; ARM_N=0
run_arm() {  # run_arm <user_dir>
  ARM_OUT="$("${CONSOLE}" "${SHARED}" "$1" - 2>&1)"
  ARM_RC=$?
  # rime_console 的 deploy-only 模式會印機器可讀的 "[schema] <id><TAB><name>"。
  ARM_SCHEMAS="$(printf '%s\n' "${ARM_OUT}" \
    | awk -F'\t' '/^\[schema\] /{ sub(/^\[schema\] /, "", $1); print $1 }')"
  ARM_N="$(printf '%s\n' "${ARM_SCHEMAS}" | awk 'NF' | wc -l | tr -d ' ')"
}

has() {  # has <清單> <id>
  case "
$1
" in
    *"
$2
"*) return 0 ;;
    *) return 1 ;;
  esac
}

# 部署那幾行(給人看失敗原因用)。
deploy_lines() {
  printf '%s\n' "$1" | grep -E '^\[deploy\]|部署失敗|Error|error' | head -8 | sed 's/^/      /'
}

echo "=== A(對照):使用者目錄有 default.custom.yaml ==="
run_arm "${USER_WITH}"
A_RC="${ARM_RC}"; A_SCHEMAS="${ARM_SCHEMAS}"; A_N="${ARM_N}"
printf '%s\n' "${A_SCHEMAS}" | sed 's/^/    /'
if [ "${A_RC}" -eq 0 ] && has "${A_SCHEMAS}" "${SHIPPED_ONLY}"; then
  ok "A 部署成功(${A_N} 個方案),而且有 ${SHIPPED_ONLY}"
else
  bad "A 就不對了(結束碼 ${A_RC},${A_N} 個方案)—— **對照組壞了,這一關不算數**"
  deploy_lines "${ARM_OUT}"
fi

echo
echo "=== B(重現):使用者目錄是空的(= .app 沒有附範本時的樣子)==="
mkdir -p "${WORK}/user"
run_arm "${WORK}/user"
B_RC="${ARM_RC}"; B_SCHEMAS="${ARM_SCHEMAS}"; B_N="${ARM_N}"; B_OUT="${ARM_OUT}"
printf '%s\n' "${B_SCHEMAS}" | sed 's/^/    /'
note "結束碼 ${B_RC},列出 ${B_N} 個方案"

if [ "${B_RC}" -ne 0 ]; then
  ok "B 部署失敗 —— 缺陷重現,而且比預期更糟:沒有範本時引擎連一個方案都沒有"
  echo "    失敗現場:"
  deploy_lines "${B_OUT}"
elif [ "${B_N}" -gt 0 ]; then
  if has "${B_SCHEMAS}" "${SHIPPED_ONLY}"; then
    bad "B 部署成功而且也有 ${SHIPPED_ONLY} —— 缺陷的機制與我們寫的不一樣,重查"
  else
    ok "B 部署成功但清單裡沒有 ${SHIPPED_ONLY}(缺陷重現:啟用的不是我們打包的那一份)"
    for s in cangjie5 quick5; do
      has "${B_SCHEMAS}" "$s" && note "B 出現 ${s}(我們沒有打包它 —— 上游 default.yaml 的清單)"
    done
  fi
else
  bad "B 結束碼 0 卻一個方案都沒有 —— 分不出「缺陷重現」與「這一關自己壞了」"
fi

echo
if [ "${fails}" -ne 0 ]; then
  echo "!! ${fails} 項失敗" >&2
  exit 1
fi
echo "兩臂對照成立:少了 default.custom.yaml,引擎就不是我們打包的那一份 ✓"
