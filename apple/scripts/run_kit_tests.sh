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
# ⚠ 下界訂得太鬆等於沒有下界(main 的 0072f4f 就是為了這件事)。
# 這個數字是實測值減一點餘裕,不是隨手寫的:本輪實跑 328 項。
MIN_TESTS="${MIN_TESTS:-320}"

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
# ⚠ **原字串與替換字串裡不可以出現 `|`。** 分隔字元就是它,而 Swift 的 `||`
#   正好是兩個 —— 那一列會被拆成六段,group 變成一段程式碼碎片,
#   而症狀是 verify_names.py 說「測試裡沒有 XxxTests」,看起來像測試不見了。
#   已經踩過一次(換鍵那一條)。要變異含 `||` 的判斷,先把它拆成兩行。
# ⚠ **原字串與替換字串裡也不可以出現錢字號。** 這些是 bash 的**雙引號**字面值,
#   $0 會被展開成腳本名 —— 症狀同樣是「在檔案裡找不到要變異的字串」,
#   而那時你會以為是錨點寫錯了。Swift 的 $0 很常見,要變異含它的行,
#   先把閉包參數寫成具名的(fix4-macmod 的 EffectiveSchemaList 那一格就是)。
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
  # 這一格就是這一輪 main 紅燈的迴歸釘:主題檔先用了 candidates.syllables,
  # 而解析器不認得 —— 症狀是 intl-ios-* 兩份各刷一則 unknown_field(syllables)。
  # 把 syllables 從 candidatesKeys 拿掉就重演一次,RepoConformanceTests 必須紅。
  # ⚠ 這不只是「多認一個欄位」:規範 §8.6.6.3.5 第 1 點要求桌面端**完整解析**
  #   這個它不渲染的區塊,少解析就是 §10 第 9 條在這裡無聲地破掉。
  "Sources/LuminaKeyKit/ThemeParser.swift|candidateStyleKeys.union([\"bar\", \"window\", \"syllables\"])|candidateStyleKeys.union([\"bar\", \"window\"])|RepoConformanceTests"
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
  # 編碼欄的空白正規化。把「拿掉所有空白」改回「收成一個空白」——
  # 那正是「自己加的詞」那一頁被下架一輪的那個 bug:檔案讀得進去、
  # 清單裡看得到、而 librime 永遠查不到,**一句錯誤訊息都沒有**。
  # 這一格在問的是:如果它再壞一次,測試會不會紅?
  "Sources/LuminaKeyKit/UserPhrases.swift|where !CharacterSet.whitespacesAndNewlines.contains(s) {|where !CharacterSet.newlines.contains(s) {|UserPhrasesTests"
  "Sources/LuminaKeyKit/IPC.swift|if case .finished = state { return true }|if false { return true }|IPCTests"
  # 改名之後的資料搬遷。把排除清單換成「只看名字完全相符的那幾個」——
  # `*.userdb` 與 `build/` 就會被搬過去,而那是這一組存在的理由:
  # 複製一個舊版正開著的 LevelDB = 一份壞掉但看起來正常的使用者詞典。
  "Sources/LuminaKeyKit/KeyRemap.swift|if mods.contains(.super_) { return stroke }    // ⌘A 是全選,不是儲存|if false { return stroke }                     // ⌘A 是全選,不是儲存|KeyRemapTests"
  "Sources/LuminaKeyKit/KeyRemap.swift|        exchangeAtPositions(position(of: a), position(of: b))|        exchangeAtPositions(a, b)|KeyRemapTests"
  "Sources/LuminaKeyKit/LegacyDataMigration.swift|for name in names where !shouldSkip(name) {|for name in names where !skippedNames.contains(name) {|LegacyDataMigrationTests"
  # ── 這一輪(fix3-mac)的四條真機缺陷,一條一格 ───────────────────────
  # M-1:少要 flagsChanged = 輕點 Shift 切中英整條路徑不存在。
  # ⚠ 不能變異 `mask = keyDown | flagsChanged` —— 分隔字元就是 `|`(見上方警告)。
  "Sources/LuminaKeyKit/InputModeSwitch.swift|public static let flagsChanged: UInt64 = 1 << 12|public static let flagsChanged: UInt64 = 1 << 10|InputModeSwitchTests"
  # M-2:狀態列把「中／En」兩態並排 = 讓使用者猜現在是哪一個。
  "Sources/LuminaKeyKit/ThemeModel.swift|StatusItem(source: .inputMode, tap: KeyAction(.inputModeToggle, raw: \"input_mode:toggle\")),|StatusItem(source: .inputModePair, tap: KeyAction(.inputModeToggle, raw: \"input_mode:toggle\")),|DesktopStatusBarTests"
  # M-2:「設定 › 外觀 › 顯示狀態列」變回一顆死鍵。
  "Sources/LuminaKeyKit/AppearanceOverrides.swift|t.statusBar.show = settings.showStatusBar.resolved(themeValue: theme.statusBar.show)|t.statusBar.show = theme.statusBar.show|AppearanceOverridesTests"
  # M-3:沒有 default.custom.yaml 時回空清單 —— 這正是「引擎有方案、清單沒東西」。
  "Sources/LuminaKeyKit/EffectiveSchemaList.swift|var base = readBaseSchemaList(text: baseText)|var base: [String] = []|EffectiveSchemaListTests"
  # M-3:範本覆蓋掉使用者自己的 default.custom.yaml(他勾的方案與候選數就沒了)。
  "Sources/LuminaKeyKit/UserDataSeed.swift|if fm.fileExists(atPath: dst.path) { kept.append(name); continue }|if false { kept.append(name); continue }|UserDataSeedTests"
  # M-4:滾輪方向反了 —— 這種缺陷畫面完全正常,只有真的去滾才知道。
  "Sources/LuminaKeyKit/CandidatePaging.swift|return delta > 0 ? .previous : .next|return delta > 0 ? .next : .previous|ScrollPagerTests"
  # ── 這一輪(fix4-macmod)修的兩件事 ────────────────────────────────
  #
  # (1) 覆核者實測到的迴歸:組字中按 Caps Lock 會清掉組字。
  #     把放行清單改回「每一顆修飾鍵都放行」就重演一次。
  "Sources/LuminaKeyKit/InputModeSwitch.swift|public static let switchKeyCodes: Set<UInt16> = [56, 60]|public static let switchKeyCodes: Set<UInt16> = [54, 55, 56, 57, 58, 59, 60, 61, 62]|ModifierGateTests"
  #
  # (2) 九種拆法。**三處接線 × 三種拆法**,一格一種。
  #     前三格(核心行為改壞)在上面已經有了:AppearanceOverrides / EffectiveSchemaList /
  #     CandidatePaging 各一。底下六格是覆核者實測「拆得掉而 CI 不紅」的那五刀
  #     加上第六刀,判準在 Tests/LuminaKeyKitTests/SwiftSourceScanner.swift。
  #
  # ⚠ 這六格變異的是 AppSources/,而 swift test 不編譯那個目錄 ——
  #   紅的來源是那幾組守門真的去讀了原始碼的結構,不是編譯失敗。
  #
  # M-2 資料流切斷:effectiveTheme 不再套「設定 › 外觀」的覆寫(定義留著)。
  "AppSources/AppContext.swift|AppearanceOverrides.apply(themes.current, settings: settings.current)|themes.current|StatusBarWiringTests"
  # M-2 呼叫點刪掉:候選窗改畫 themes.current(effectiveTheme 定義留著、沒有人叫它)。
  "AppSources/LuminaKeyInputController.swift|panel.show(theme: AppContext.shared.effectiveTheme, snapshot: snap,|panel.show(theme: AppContext.shared.themes.current, snapshot: snap,|StatusBarWiringTests"
  # M-3 資料流切斷:sharedDir 傳 nil,退回上游那一份時永遠找不到 default.yaml。
  "AppSources/AppContext.swift|SchemaListReader.resolve(userDir: userDataDir, sharedDir: sharedDataDir)|SchemaListReader.resolve(userDir: userDataDir, sharedDir: nil)|SchemaListWiringTests"
  # M-3 呼叫點刪掉:退回只讀 default.custom.yaml(SchemaListReader 定義留著)。
  "AppSources/AppContext.swift|let lists = SchemaListReader.resolve(userDir: userDataDir, sharedDir: sharedDataDir)|let lists = EffectiveSchemaList(ids: RimeConfigPatch.readSchemaList(userDir: userDataDir), source: .custom)|SchemaListWiringTests"
  # M-4 資料流切斷:算出了要翻哪一頁,卻沒有送出去。
  "AppSources/CandidateView.swift|if step != .none { onChangePage?(step) }|if step != .none { _ = step }|PagingWiringTests"
  # M-4 呼叫點刪掉:activateServer 不再接 panel.onChangePage(changePage 定義留著)。
  "AppSources/LuminaKeyInputController.swift|AppContext.shared.panel.onChangePage = { [weak self] step in|let pageSink: (PageStep) -> Void = { [weak self] step in|PagingWiringTests"
  #
  # (3) 退回上游那一份時只列磁碟上真的有的方案。
  #     不濾的話,全新安裝的畫面上會出現兩列「已勾選 + 找不到檔案」。
  "Sources/LuminaKeyKit/EffectiveSchemaList.swift|if let ids = installedIds { base = base.filter { id in ids.contains(id) } }|_ = installedIds|EffectiveSchemaListTests"
  #
  # (4) **判準自己**:把「抹掉註解與字串」拿掉,上面那六格就退化成 grep。
  #     這一格在問:名字只寫在註解裡的時候,它還說不說得出「沒接線」。
  "Tests/LuminaKeyKitTests/SwiftSourceScanner.swift|init(_ raw: String) { self.text = SwiftSource.blankOutNoise(raw) }|init(_ raw: String) { self.text = raw }|SwiftSourceTests"
)

# 「這一組真的紅了嗎」。
#
# ⚠ **XCTest 的失敗行在兩個平台上不是同一個格式。** 實測(run #98):
#     Darwin: /a/F.swift:76: error: -[LuminaKeyKitTests.KeyMapperTests testX] : XCTAssertEqual failed:
#     Linux : /a/F.swift:76: error: KeyMapperTests.testX : XCTAssertEqual failed:
#   第一版只寫了 Linux 那一種(在 Linux 沙盒上開發),於是在 macos runner 上
#   **21 個變異全部被判成「紅的不是那一組」** —— 守門在該綠的時候紅。
#   這是本專案第四次踩到「同一支工具在別台機器上不是同一支工具」。
#
# ⚠ 同樣不可以退回「整份輸出 grep 組名」:verbose 的輸出裡每一個**通過**的
#   測試也印自己的名字,那一條會恆為真(那是這個比對器一開始存在的理由)。
#
# 所以底下自帶一個自我測試,而且**每一次都跑**:四個樣本,兩種平台的失敗行
# 各一、一行通過的、一行別組的失敗。
group_red_pattern() { printf 'error: (-\[[A-Za-z0-9_]+\.)?%s[ .]' "$1"; }

group_went_red() {  # group_went_red <組名> <輸出>
  grep -qE -- "$(group_red_pattern "$1")" <<< "$2"
}

self_test_matcher() {
  local g="KeyMapperTests" bad=0
  local darwin="/a/F.swift:76: error: -[LuminaKeyKitTests.KeyMapperTests testX] : XCTAssertEqual failed:"
  local linuxln="/a/F.swift:76: error: KeyMapperTests.testX : XCTAssertEqual failed:"
  local passing="Test Case '-[LuminaKeyKitTests.KeyMapperTests testX]' passed (0.001 seconds)"
  local other="/a/F.swift:9: error: -[LuminaKeyKitTests.ThemeParserTests testY] : failed"
  group_went_red "$g" "$darwin"  || { echo "!! 比對器認不得 Darwin 的失敗行" >&2; bad=1; }
  group_went_red "$g" "$linuxln" || { echo "!! 比對器認不得 Linux 的失敗行" >&2; bad=1; }
  group_went_red "$g" "$passing" && { echo "!! 比對器把「通過」也算成紅的" >&2; bad=1; }
  group_went_red "$g" "$other"   && { echo "!! 比對器把別組的紅算到自己頭上" >&2; bad=1; }
  [ "$bad" -eq 0 ] || exit 1
  echo "  ✓ 「紅的是哪一組」比對器的自我測試通過（Darwin / Linux 兩種格式）"
}
self_test_matcher

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
  # 見檔案上方 group_went_red 的註解:只認失敗行,而且兩個平台的格式不同。
  elif ! group_went_red "${group}" "${MOUT}"; then
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
