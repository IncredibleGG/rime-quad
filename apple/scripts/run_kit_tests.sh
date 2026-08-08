#!/usr/bin/env bash
#
# run_kit_tests.sh — 跑 RimeQuadKit 的單元測試,並且**證明它們真的會紅**
#
# ── 為什麼不是一句 `swift test` 就好 ────────────────────────────────────
# 本專案已經被「測試是綠的,因為它沒在測」咬過兩次:發布關卡的升級測試因為
# 步驟順序寫反被判「略過」而報全綠;LayoutEscapeTest 的清單寫死四份,
# 12 份裡有 8 份從沒被檢查過,而那幾份都真的有死路。
#
# 所以這支腳本做三件 `swift test` 不會做的事:
#
#   1. **斷言真的有跑**:解析 "Executed N tests",N 必須達到下限,
#      而且 skipped 必須是 0。測試檔被誤刪、target 沒被納入、
#      整組因為沒有相依而沒編 —— 這幾種都會讓 N 掉下來。
#   2. **變異測試**:對四個不同檔案各植入一個真的違規,斷言測試會紅。
#      這證明的不只是「有跑」,而是「**對應的那一組**斷言真的在斷言」。
#      一個檔案一個變異,所以哪一組沒在測看得出來是哪一組。
#   3. **收拾現場**:trap 保證原始碼一定被還原,即使中途失敗或被中斷。
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PKG="${ROOT}/apple/RimeQuad"
MIN_TESTS="${MIN_TESTS:-60}"

cd "${PKG}" || exit 1

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }

# ---------------------------------------------------------------- 正向
log "swift test"
OUT="$(swift test 2>&1)"
RC=$?
printf '%s\n' "${OUT}" | tail -40

if [ "${RC}" -ne 0 ]; then
  echo "!! 單元測試失敗" >&2
  exit 1
fi

# xcodebuild/swift-testing 的輸出格式:
#   Executed 87 tests, with 0 failures (0 unexpected) in 0.123 (0.456) seconds
COUNT="$(printf '%s\n' "${OUT}" | awk '/Executed [0-9]+ test/ { n=$2 } END { print n+0 }')"
echo "已執行測試數: ${COUNT}"

if [ "${COUNT}" -lt "${MIN_TESTS}" ]; then
  echo "!! 只跑了 ${COUNT} 項測試,下限是 ${MIN_TESTS}。" >&2
  echo "   測試沒跑跟測試通過看起來一模一樣,所以這裡把它變成失敗。" >&2
  exit 1
fi

if printf '%s\n' "${OUT}" | grep -qE '[1-9][0-9]* test.* skipped'; then
  echo "!! 有測試被略過。略過不算通過。" >&2
  exit 1
fi

# ---------------------------------------------------------------- 變異
# 格式: <檔案>|<原字串>|<替換>|<這個變異該打紅哪一組>
MUTATIONS=(
  "Sources/RimeQuadKit/KeyMapper.swift|public static let unicodeKeysymBase: Int32 = 0x0100_0000|public static let unicodeKeysymBase: Int32 = 0x0200_0000|KeyMapperTests"
  "Sources/RimeQuadKit/CandidateLayout.swift|let itemHeight = measured.map(\\.height).max() ?? 0|let itemHeight = measured.map(\\.height).min() ?? 0|CandidateLayoutTests"
  "Sources/RimeQuadKit/CommitPolicy.swift|if s.menuCount > 0 { return .keepComposing }|if s.menuCount > 99 { return .keepComposing }|CommitPolicyTests"
  "Sources/RimeQuadKit/ThemeParser.swift|w.lines = c.child(\"lines\").int(1, min: 0, max: 16)|w.lines = c.child(\"lines\").int(2, min: 0, max: 16)|ThemeParserTests"
  "Sources/RimeQuadKit/StatusFace.swift|public static let latin = \"En\"|public static let latin = \"英\"|StatusFaceTests"
)

BACKUP_DIR="$(mktemp -d)"
restore() {
  for f in "${BACKUP_DIR}"/*.bak; do
    [ -e "$f" ] || continue
    dest="$(cat "${f%.bak}.path")"
    cp "$f" "${PKG}/${dest}"
  done
  rm -rf "${BACKUP_DIR}"
}
trap restore EXIT INT TERM

mutation_failures=0
i=0
for m in "${MUTATIONS[@]}"; do
  i=$((i + 1))
  IFS='|' read -r file from to group <<< "${m}"
  log "變異 ${i}/${#MUTATIONS[@]}: ${file} —— 應該打紅 ${group}"

  if ! grep -qF -- "${from}" "${PKG}/${file}"; then
    echo "!! 在 ${file} 裡找不到要變異的字串,變異測試本身壞了:" >&2
    echo "   ${from}" >&2
    mutation_failures=$((mutation_failures + 1))
    continue
  fi

  cp "${PKG}/${file}" "${BACKUP_DIR}/m${i}.bak"
  printf '%s' "${file}" > "${BACKUP_DIR}/m${i}.path"

  python3 - "${PKG}/${file}" "${from}" "${to}" <<'PY'
import sys
path, frm, to = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(path, encoding="utf-8").read()
assert frm in s, frm
open(path, "w", encoding="utf-8").write(s.replace(frm, to, 1))
PY

  MOUT="$(swift test 2>&1)"
  MRC=$?
  cp "${BACKUP_DIR}/m${i}.bak" "${PKG}/${file}"

  if [ "${MRC}" -eq 0 ]; then
    echo "!! 植入違規之後測試仍然通過 —— ${group} 沒有在測它宣稱在測的東西。" >&2
    mutation_failures=$((mutation_failures + 1))
  elif ! printf '%s\n' "${MOUT}" | grep -q "${group}"; then
    echo "!! 測試紅了,但紅的不是 ${group}。變異打到了別的地方:" >&2
    printf '%s\n' "${MOUT}" | grep -E "error:|failed" | head -5 >&2
    mutation_failures=$((mutation_failures + 1))
  else
    echo "  ✓ ${group} 如預期變紅"
  fi
done

if [ "${mutation_failures}" -ne 0 ]; then
  echo "!! ${mutation_failures} 個變異沒有被抓到" >&2
  exit 1
fi

echo
echo "單元測試 ${COUNT} 項全過,${#MUTATIONS[@]} 個變異全部被抓到 ✓"
