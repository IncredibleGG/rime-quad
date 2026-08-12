#!/usr/bin/env bash
#
# verify_console.sh — 用 rime_console 斷言「按鍵打得出預期的字」
#
# 這支腳本是 macOS 核心驗證的斷言本體。抽出來獨立成檔，是為了讓它自己
# 也能被反向測試 —— 見 --expect-fail。
#
# 用法:
#   verify_console.sh <keys> <schema> <expect>
#   verify_console.sh --expect-fail <keys> <schema> <wrong-expect>
#
#   <schema> 傳一個單獨的 "-" 代表**不要傳方案 id 給 rime_console**
#   （[diag] 這一輪加的對照組）。rime_console 的第 5 個引數不給時，它
#   不會走 rs_select_schema，session 就停在預設方案上。傳與不傳各跑一次，
#   紅的時候兩邊的差別會直接印在一起。定案之後這個分支可以拔掉。
#
# ── 為什麼斷言要這樣寫（不要放寬）─────────────────────────────
#
# rime_console 有三處會印出 COMMIT，格式各不相同:
#
#   tools/rime_console.cc:70   "  [%s] >>> COMMIT: \"%s\"\n"   ← dump()，有兩格縮排與 [tag]
#   tools/rime_console.cc:231  "  >>> COMMIT:\"%s\""           ← 政策迴圈中途，冒號後無空格
#   tools/rime_console.cc:255  "\n>>> COMMIT: \"%s\"\n"        ← 最終累計結果，行首、獨佔一行
#
# 只有第三處代表「整串輸入最後真正上屏的內容」。若斷言不錨定行首，
# 中途某次 dump 印出的部分結果就足以讓步驟變綠 —— 那是「測試沒在測」，
# 正是 docs/handoff-macos.md §7 點名要防的失敗類型。
#
# 因此斷言是 **^...$ 完全相等**，不是 contains:
#   - 錨定行首 ^ 排除掉有縮排的 dump 行
#   - 錨定行尾 $ 讓「你好嗎」不會被當成「你好」通過
#
# 另外要求 rime_console 以 0 結束。部署失敗時它會 return 1，
# 若只看輸出不看結束碼，部署炸掉但湊巧印過中途 COMMIT 一樣會綠。
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONSOLE="${ROOT}/apple/build/rime_console"
SHARED="${ROOT}/core/data/shared"
USER_DIR="${ROOT}/core/data/user"

EXPECT_FAIL=0
if [ "${1:-}" = "--expect-fail" ]; then EXPECT_FAIL=1; shift; fi

KEYS="${1:?需要 keys}"
SCHEMA="${2:?需要 schema}"
EXPECT="${3:?需要 expect}"

[ -x "${CONSOLE}" ] || { echo "錯誤: 找不到可執行的 ${CONSOLE}" >&2; exit 2; }
[ -d "${SHARED}" ]  || { echo "錯誤: 找不到 ${SHARED}" >&2; exit 2; }

run_case() {
  local out rc
  if [ "${SCHEMA}" = "-" ]; then
    out="$("${CONSOLE}" "${SHARED}" "${USER_DIR}" "${KEYS}" 2>&1)"
  else
    out="$("${CONSOLE}" "${SHARED}" "${USER_DIR}" "${KEYS}" 1 "${SCHEMA}" 2>&1)"
  fi
  rc=$?

  # glog 的部署訊息很吵，成功時只留下非 glog 的行
  printf '%s\n' "${out}" | grep -vE '^[WIE][0-9]{8} ' || true

  if [ "${rc}" -ne 0 ]; then
    echo "  !! rime_console 結束碼 ${rc}（非 0）" >&2
    printf '%s\n' "${out}" >&2
    return 1
  fi

  # 完全相等，且只認行首那一行（最終累計結果）
  if printf '%s\n' "${out}" | grep -q "^>>> COMMIT: \"${EXPECT}\"$"; then
    return 0
  fi

  echo "  !! 最終 COMMIT 不是「${EXPECT}」" >&2
  echo "  --- 實際的最終 COMMIT 行 ---" >&2
  printf '%s\n' "${out}" | grep -n '^>>> COMMIT:' >&2 || echo "  （完全沒有最終 COMMIT 行）" >&2
  return 1
}

if [ "${EXPECT_FAIL}" -eq 1 ]; then
  # 反向測試:故意餵一個錯的預期值，斷言本體必須判定失敗。
  # 這是 §7 要求的「實際植入一個違規，驗證它會紅」。
  # 少了這一關，斷言邏輯自己壞掉（例如 grep 樣式永遠成立）不會有人發現。
  echo "=== 反向測試: keys=${KEYS} schema=${SCHEMA} 故意預期「${EXPECT}」(應該失敗) ==="
  if run_case >/dev/null 2>&1; then
    echo "!! 反向測試失敗:斷言對錯誤的預期值仍然通過 —— 斷言邏輯本身是壞的。" >&2
    exit 1
  fi
  echo "  ✓ 斷言正確地判定失敗（代表它真的會紅）"
  exit 0
fi

echo "=== 案例: keys=${KEYS} schema=${SCHEMA} expect=${EXPECT} ==="
if run_case; then
  echo "  ✓ 最終 COMMIT == 「${EXPECT}」"
  exit 0
fi
exit 1
