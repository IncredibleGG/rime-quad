#!/usr/bin/env bash
#
# run_kit_tests.sh — 跑 LuminaKeyKit 的單元測試,並且**證明它們真的會紅**
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
PKG="${ROOT}/apple/LuminaKey"
MIN_TESTS="${MIN_TESTS:-180}"

cd "${PKG}" || exit 1

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }

# ---------------------------------------------------------------- 正向
log "swift test"
OUT="$(swift test 2>&1)"
RC=$?
if [ "${RC}" -ne 0 ]; then
  # ⚠ 只印 tail 的話,失敗的斷言多半已經被後面幾百行「passed」擠出畫面。
  #   這裡先把**失敗的那幾行**單獨抓出來 —— 修的時候需要的就只有這些。
  echo "!! 單元測試失敗。失敗的斷言:" >&2
  FAILED_LINES="$(grep -E "error:|XCTAssert|: failed|' failed" <<< "${OUT}" || true)"
  printf '%s\n' "${FAILED_LINES}" | sed -n '1,60p' >&2
  echo "--- 最後 40 行 ---" >&2
  printf '%s\n' "${OUT}" | tail -40 >&2
  exit 1
fi

printf '%s\n' "${OUT}" | tail -12

# xcodebuild/swift-testing 的輸出格式:
#   Executed 87 tests, with 0 failures (0 unexpected) in 0.123 (0.456) seconds
COUNT="$(awk '/Executed [0-9]+ test/ { n=$2 } END { print n+0 }' <<< "${OUT}")"
echo "已執行測試數: ${COUNT}"

if [ "${COUNT}" -lt "${MIN_TESTS}" ]; then
  echo "!! 只跑了 ${COUNT} 項測試,下限是 ${MIN_TESTS}。" >&2
  echo "   測試沒跑跟測試通過看起來一模一樣,所以這裡把它變成失敗。" >&2
  exit 1
fi

if grep -qE '[1-9][0-9]* test.* skipped' <<< "${OUT}"; then
  echo "!! 有測試被略過。略過不算通過。" >&2
  exit 1
fi

# ---------------------------------------------------------------- 變異
# 格式: <檔案>|<原字串>|<替換>|<這個變異該打紅哪一組>
# ⚠ **不要把 printf 接進會提早結束的指令(grep -q / head)。**
#   本腳本開了 `pipefail`,而 `grep -q` 找到第一個符合就結束 ——
#   輸出一大(207 項測試之後就夠大了)printf 就會吃到 SIGPIPE,
#   整條管線的結束碼變成 141,於是 `if ! ... grep -q` 恆為真。
#   症狀是**四個明明打中的變異被報成「紅的不是那一組」**,
#   而且只在測試變多之後才開始發生 —— 看起來像變異測試壞了,
#   其實是這支腳本自己壞了。一律改用 herestring(<<<),不經管線。
MUTATIONS=(
  "Sources/LuminaKeyKit/KeyMapper.swift|public static let unicodeKeysymBase: Int32 = 0x0100_0000|public static let unicodeKeysymBase: Int32 = 0x0200_0000|KeyMapperTests"
  "Sources/LuminaKeyKit/CandidateLayout.swift|let itemHeight = measured.map(\\.height).max() ?? 0|let itemHeight = measured.map(\\.height).min() ?? 0|CandidateLayoutTests"
  "Sources/LuminaKeyKit/CommitPolicy.swift|if s.menuCount > 0 { return .keepComposing }|if s.menuCount > 99 { return .keepComposing }|CommitPolicyTests"
  "Sources/LuminaKeyKit/ThemeParser.swift|w.lines = c.child(\"lines\").int(1, min: 0, max: 16)|w.lines = c.child(\"lines\").int(2, min: 0, max: 16)|ThemeParserTests"
  "Sources/LuminaKeyKit/StatusFace.swift|public static let latin = \"En\"|public static let latin = \"英\"|StatusFaceTests"
  # ⚠ 這一格原本指向 InputModeBinding.simplificationOption,而變異測試
  #   自己抓到了那個錯:紅的是 InputModeBindingTests,不是 SessionOptionsTests。
  #   兩組都會碰到那一行,所以它證明不了「SessionOptionsTests 在測什麼」。
  #   換成只有 SessionOptions 這一層碰得到的規則:「文字」頁明確選繁體時,
  #   simplification 必須是 false(而且要蓋過輸入模式)。
  "Sources/LuminaKeyKit/SessionOptions.swift|case .traditional: out[\"simplification\"] = false|case .traditional: out[\"simplification\"] = true|SessionOptionsTests"
  "Sources/LuminaKeyKit/SettingsCatalog.swift|blurb: T(\"打勾的方案才會出現在切換清單裡。可以拖曳排序,最上面那一個是預設。\",|blurb: T(\"改 schema_list\",|SettingsCatalogTests"
  "Sources/LuminaKeyKit/ArchiveGuard.swift|\"yaml\", \"yml\", \"txt\", \"ocd2\", \"gram\", \"json\", \"md\", \"lua\",|\"yaml\", \"yml\", \"txt\", \"ocd2\", \"gram\", \"json\", \"md\", \"lua\", \"bin\",|ArchiveGuardTests"
  "Sources/LuminaKeyKit/UserPhrases.swift|out.insert(p, at: 0)|out.append(p)|UserPhrasesTests"
  "Sources/LuminaKeyKit/IPC.swift|if case .finished = state { return true }|if false { return true }|IPCTests"
  # 改名之後的資料搬遷。把排除清單換成「只看名字完全相符的那幾個」——
  # `*.userdb` 與 `build/` 就會被搬過去,而那是這一組存在的理由:
  # 複製一個舊版正開著的 LevelDB = 一份壞掉但看起來正常的使用者詞典。
  "Sources/LuminaKeyKit/KeyRemap.swift|if mods.contains(.control) || mods.contains(.super_) { return stroke }|if false { return stroke }|KeyRemapTests"
  "Sources/LuminaKeyKit/KeyRemap.swift|        exchangeAtPositions(position(of: a), position(of: b))|        exchangeAtPositions(a, b)|KeyRemapTests"
  "Sources/LuminaKeyKit/LegacyDataMigration.swift|for name in names where !shouldSkip(name) {|for name in names where !skippedNames.contains(name) {|LegacyDataMigrationTests"
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
  elif ! grep -q -- "${group}" <<< "${MOUT}"; then
    echo "!! 測試紅了,但紅的不是 ${group}。變異打到了別的地方:" >&2
    FAILED_LINES="$(grep -E "error:|failed" <<< "${MOUT}" || true)"
    printf '%s\n' "${FAILED_LINES}" | sed -n '1,5p' >&2
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
