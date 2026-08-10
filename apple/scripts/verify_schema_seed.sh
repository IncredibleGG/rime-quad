#!/usr/bin/env bash
#
# verify_schema_seed.sh — 證明「使用者目錄裡沒有 default.custom.yaml」真的會讓
#                         引擎的方案清單變成錯的
#
# ── 這一關在回答什麼 ────────────────────────────────────────────────────
# 使用者在真 Mac 上回報:設定裡「啟用的方案」一列都沒有,而引擎在用一個他
# 沒選過的方案。查到的原因是 `apple/scripts/build_app.sh` 只複製了
# `core/data/shared`,從來沒有把 `core/data/user/default.custom.yaml` 帶上,
# 所以那個檔案在使用者機器上不存在。
#
# 但「不存在」到底會不會改變 librime 實際部署出來的清單,是可以**問**的,
# 不必用猜的。這支腳本用同一支 rime_console、同一份 shared 資料跑兩臂,
# 只差使用者目錄:
#
#   A(對照):core/data/user —— 有 collect_data.sh 產生的 default.custom.yaml
#   B(重現):一個空的暫存目錄 —— 就是沒有範本時使用者機器上的樣子
#
# 斷言 A 有 `luna_pinyin_tw`(朙月拼音·臺灣正體,我們真正打包的預設方案),
# 而 B 沒有。B 那一臂列出來的是上游 rime-prelude 的 `default.yaml` 那一份。
#
# ⚠ **「B 什麼都沒印」也會讓「B 沒有 luna_pinyin_tw」成立** —— 那是空洞的通過,
#   而且看起來跟真的重現一模一樣。所以 B 也必須列出至少一個方案,
#   否則判定為「這一關自己壞了」而不是「沒有違規」。
#
# ⚠ 這支腳本**不修**任何東西,它只是把缺陷變成一個可重跑的事實。
#   修在 `LuminaKeyKit/UserDataSeed.swift` 與 `build_app.sh`。
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONSOLE="${ROOT}/apple/build/rime_console"
SHARED="${ROOT}/core/data/shared"
USER_WITH="${ROOT}/core/data/user"
TEMPLATE="${USER_WITH}/default.custom.yaml"

# 我們真正打包、而上游 default.yaml **沒有**列的方案。
SHIPPED_ONLY="luna_pinyin_tw"

fails=0
ok()  { printf '  ✓ %s\n' "$1"; }
bad() { printf '  ✗ %s\n' "$1" >&2; fails=$((fails + 1)); }

[ -x "${CONSOLE}" ] || { echo "錯誤: 找不到可執行的 ${CONSOLE}" >&2; exit 2; }
[ -d "${SHARED}" ]  || { echo "錯誤: 找不到 ${SHARED}" >&2; exit 2; }
[ -s "${TEMPLATE}" ] || { echo "錯誤: 找不到 ${TEMPLATE}(先跑 scripts/collect_data.sh)" >&2; exit 2; }

WORK="$(mktemp -d)"
cleanup() { rm -rf "${WORK}"; }
trap cleanup EXIT INT TERM

# rime_console 的 deploy-only 模式(按鍵傳 "-")會印機器可讀的
# "[schema] <id><TAB><name>"。這裡只要 id。
schemas_of() {  # schemas_of <user_dir>
  local out
  out="$("${CONSOLE}" "${SHARED}" "$1" - 2>&1)"
  # ⚠ 不看結束碼:B 那一臂本來就可能因為上游清單裡有我們沒打包的方案而失敗,
  #   而那正是要展示的後果之一。真正的判準是它列出了哪些方案。
  printf '%s\n' "${out}" \
    | awk -F'\t' '/^\[schema\] /{ sub(/^\[schema\] /, "", $1); print $1 }'
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

echo "=== A(對照):使用者目錄有 default.custom.yaml ==="
A="$(schemas_of "${USER_WITH}")"
printf '%s\n' "${A}" | sed 's/^/    /'
if has "${A}" "${SHIPPED_ONLY}"; then
  ok "A 有 ${SHIPPED_ONLY}"
else
  bad "A 沒有 ${SHIPPED_ONLY} —— **對照組就不對了**,這一關不算數"
fi

echo
echo "=== B(重現):使用者目錄是空的(= .app 沒有附範本時的樣子)==="
mkdir -p "${WORK}/user"
B="$(schemas_of "${WORK}/user")"
printf '%s\n' "${B}" | sed 's/^/    /'

N_B="$(printf '%s\n' "${B}" | awk 'NF' | wc -l | tr -d ' ')"
if [ "${N_B}" -gt 0 ]; then
  ok "B 列出了 ${N_B} 個方案(所以下一條斷言不是空洞的)"
else
  bad "B 一個方案都沒列出來 —— 分不出「修好了」與「這一關壞了」"
fi

if has "${B}" "${SHIPPED_ONLY}"; then
  bad "B 竟然也有 ${SHIPPED_ONLY} —— 那這個缺陷的機制與我們寫的不一樣,重查"
else
  ok "B 沒有 ${SHIPPED_ONLY}(缺陷重現:沒有範本 = 我們打包的預設方案沒有被啟用)"
fi

# 反過來:上游那一份列了我們沒有打包的方案,部署會報錯。這一條只印不判 ——
# 上游哪天改掉那份清單是他們的事,不該讓我們的 CI 紅。
for s in cangjie5 quick5; do
  if has "${B}" "$s"; then
    echo "  · B 出現 ${s}(我們沒有打包 ${s}.schema.yaml —— 這是部署報錯的來源)"
  fi
done

echo
if [ "${fails}" -ne 0 ]; then
  echo "!! ${fails} 項失敗" >&2
  exit 1
fi
echo "兩臂對照成立:少了 default.custom.yaml,引擎啟用的就不是我們打包的那一份 ✓"
