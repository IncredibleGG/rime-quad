#!/usr/bin/env bash
#
# verify_data.sh — 確認 collect_data.sh 產出的執行期資料真的能讓 librime 打出字
#
# 為什麼需要這支（而不是信任 collect_data.sh 自己的檢查）:
#
#   scripts/collect_data.sh 末尾的「完整性檢查」用了 grep -oP '...\K...'。
#   -P 與 \K 都是 GNU/PCRE 專屬，macOS 的 BSD grep 直接拒絕該選項並以 2 結束。
#   而那一行寫成  < <(grep -oP ... 2>/dev/null || true)  ——
#   2>/dev/null 吃掉錯誤訊息、|| true 吃掉結束碼、process substitution 裡的
#   失敗又不會觸發 set -e。三層消音之後，迴圈跑零次、missing 恆為 0，
#   於是它在 macOS 上**每一次都印出「所有 schema 引用的詞庫都齊全」**，
#   不管詞庫在不在。
#
#   這正是 docs/handoff-macos.md §7 點名的第二類陰險失敗:
#   「測試是綠的，因為它沒在測。」
#
#   collect_data.sh 是四端共用的，這一輪不動它（改它要一併驗 Android）。
#   本檔用 BSD/GNU 都成立的寫法把那份覆蓋率補回來，並且**硬失敗**，
#   不是印個警告就放行。
#
# 檢查項目:
#   1. 必要檔案存在且非空
#   2. opencc 詞典（.ocd2）有產出
#   3. schema_list 裡每個方案都有 .schema.yaml
#   4. 每個 .schema.yaml 引用的 dictionary 都有對應的 .dict.yaml
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SHARED="${ROOT}/core/data/shared"
USER_DIR="${ROOT}/core/data/user"

fail=0
note_ok()  { echo "  ✓ $*"; }
note_bad() { echo "  !! $*" >&2; fail=1; }

echo "=== 1. 必要檔案 ==="
for f in \
  "${SHARED}/essay.txt" \
  "${SHARED}/default.yaml" \
  "${SHARED}/luna_pinyin.dict.yaml" \
  "${SHARED}/luna_pinyin_tw.schema.yaml" \
  "${SHARED}/bopomofo_tw.schema.yaml" \
  "${SHARED}/terra_pinyin.dict.yaml" \
  "${SHARED}/stroke.dict.yaml" \
  "${SHARED}/t9_pinyin.schema.yaml" \
  "${USER_DIR}/default.custom.yaml" ; do
  if [ -s "${f}" ]; then note_ok "${f#${ROOT}/}"; else note_bad "缺少或空檔: ${f#${ROOT}/}"; fi
done

echo "=== 2. opencc 詞典 ==="
n_ocd2=$(find "${SHARED}/opencc" -name '*.ocd2' 2>/dev/null | wc -l | tr -d ' ')
if [ "${n_ocd2}" -gt 0 ]; then
  note_ok "${n_ocd2} 個 .ocd2"
else
  note_bad "沒有任何 .ocd2 —— 簡繁轉換會壞（host opencc 沒產出詞典）"
fi

echo "=== 3. schema_list 的方案都有 .schema.yaml ==="
for s in luna_pinyin_tw bopomofo_tw luna_pinyin t9_pinyin; do
  if [ -s "${SHARED}/${s}.schema.yaml" ]; then note_ok "${s}.schema.yaml"
  else note_bad "schema_list 列了 ${s} 但沒有 ${s}.schema.yaml"; fi
done

echo "=== 4. schema 引用的詞庫都在 ==="
# 這裡是補回 collect_data.sh 在 macOS 上失效的那一項。
# 用 awk 取代 grep -oP '\K'：BSD 與 GNU 的 awk 行為一致。
# 只認行首（可含縮排）的 "dictionary:"，避免撿到註解或其他欄位。
refs=0
for f in "${SHARED}"/*.schema.yaml; do
  [ -e "${f}" ] || continue
  while read -r d; do
    [ -z "${d}" ] && continue
    d="${d%\"}"; d="${d#\"}"
    d="${d%\'}"; d="${d#\'}"
    [ -z "${d}" ] && continue
    refs=$((refs + 1))
    if [ ! -f "${SHARED}/${d}.dict.yaml" ]; then
      note_bad "$(basename "${f}") 引用 dictionary: ${d}，但缺少 ${d}.dict.yaml"
    fi
  done < <(awk '/^[[:space:]]*dictionary:[[:space:]]/ { print $2 }' "${f}")
done

# 「一個引用都沒找到」本身就不可能 —— 那代表抽取器壞了，而不是資料乾淨。
# 少了這一條，抽取器將來若被改壞，這個檢查會再一次靜靜地全綠。
if [ "${refs}" -eq 0 ]; then
  note_bad "在所有 .schema.yaml 裡一個 dictionary: 都沒抽到 —— 抽取器壞了，不是資料乾淨"
else
  note_ok "檢查了 ${refs} 個 dictionary 引用"
fi

echo
if [ "${fail}" -eq 0 ]; then
  echo "=== 執行期資料檢查通過 ==="
else
  echo "=== 執行期資料檢查失敗 ===" >&2
fi
exit "${fail}"
