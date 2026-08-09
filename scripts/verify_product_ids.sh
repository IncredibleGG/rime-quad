#!/usr/bin/env bash
#
# verify_product_ids.sh — 守住「產品識別碼只有一份」這件事
#
# 為什麼需要它:
#
#   2026-08-09 把「RimeQuad / 四端」換成 LuminaKey 的時候,同一個 Android IME id
#   在九支腳本裡各寫死了一份。改名之所以痛,不是因為要改的地方多,是因為沒有人
#   知道到底有幾個地方 —— grep 找得到的只是**字面寫出來的那些**,拼接出來的
#   (`$PKG/.RimeInputMethodService`)、格式化出來的、當成路徑一段用掉的
#   都找不到。改完之後最典型的下場是「顯示名改了、識別碼沒改」,而那種半套
#   在編譯期、單元測試、發布關卡全部是綠的,只有使用者裝上去才發現。
#
#   所以這一關做三件事:
#     1. shell 與 python 兩邊的讀取器對同一份 product.env 必須得出**逐字相同**
#        的結果(組合值是兩邊各推一次的,這一項就是在盯那個「各推一次」)。
#     2. 兩個讀取器必須真的在讀檔案。改了 product.env 而輸出不變 = 有人在
#        讀取器裡也寫死了一份。
#     3. scripts/、android/、apple/ 底下(scripts/lib/ 以外)的 .sh 與 .py
#        不准再出現識別碼字面值;
#        core/ docs/ tools/ README + android/ 與 apple/ 的原始碼不准殘留舊名。
#     4. **落地的識別碼**(寫進使用者磁碟的檔名與魔術字串)必須與 product.env
#        逐字相同,而且改名前的那一份相容宣告必須還在。
#
#   ⚠ 2026-08-09 的合併稽核:這一關在**六個落地識別碼兩端全部不一致**的情況下
#   6/6 全綠。原因是第 3 項只 find scripts/(不含 apple/scripts/)、第 4 項只掃
#   core/ docs/ tools/ README/build.yml —— `android/` 與 `apple/` 不在任何一項的
#   範圍內。也就是說,它當時正是自己在上面警告的那個樣子:改了顯示名、沒改
#   識別碼,而編譯期、單元測試、發布關卡全部是綠的。範圍因此擴大,並多了第 6 項。
#
#   ⚠ 每一項都自帶**反向測試**:先植入一個一定要被抓到的違規,確認這一關真的
#   會紅,再跑真的檢查。沒有反向測試的守門腳本,在它自己壞掉的那天會安靜地
#   全綠 —— 這個專案已經吃過好幾次那種虧(見 docs/coordination.md §3)。
#
# 用法: ./scripts/verify_product_ids.sh
# 離開碼: 0 全過;1 有項目失敗。

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

PASS=0; FAIL=0
ok()  { echo "  [PASS] $*"; PASS=$((PASS+1)); }
bad() { echo "  [FAIL] $*" >&2; FAIL=$((FAIL+1)); }
step(){ echo; echo "=== $* ==="; }

TMP="$(mktemp -d "${TMPDIR:-/tmp}/product-ids.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# ─────────────────────────────────────────────────────────────────────────────
step "1. shell 與 python 的讀取器輸出一致"

bash "$HERE/lib/product.sh" --dump > "$TMP/sh.txt" 2>"$TMP/sh.err"
SH_RC=$?
python3 "$HERE/lib/product.py" --dump > "$TMP/py.txt" 2>"$TMP/py.err"
PY_RC=$?

if [ "$SH_RC" -ne 0 ]; then
  bad "product.sh --dump 失敗"; sed 's/^/         /' "$TMP/sh.err" >&2
elif [ ! -s "$TMP/sh.txt" ]; then
  bad "product.sh --dump 沒有輸出 —— 空的輸出和 python 的空輸出會互相比對成功"
elif [ "$PY_RC" -ne 0 ]; then
  bad "product.py --dump 失敗"; sed 's/^/         /' "$TMP/py.err" >&2
elif [ ! -s "$TMP/py.txt" ]; then
  bad "product.py --dump 沒有輸出"
elif diff -u "$TMP/sh.txt" "$TMP/py.txt" > "$TMP/diff.txt"; then
  ok "兩邊各 $(grep -c . "$TMP/sh.txt") 個值,逐字相同"
else
  bad "shell 與 python 讀出來的產品識別不一致 —— 兩邊各有一套名字正是這次要根除的"
  sed 's/^/         /' "$TMP/diff.txt" >&2
fi

# ─────────────────────────────────────────────────────────────────────────────
step "2. 讀取器真的在讀 product.env(反向測試)"
# 一個把值寫死在自己身上的讀取器,前一項會完美通過。

mkdir -p "$TMP/lib"
cp "$HERE/lib/product.env" "$HERE/lib/product.sh" "$HERE/lib/product.py" "$TMP/lib/"
SENTINEL="org.sentinel.notaproduct"
python3 - "$TMP/lib/product.env" "$SENTINEL" <<'PY'
import io, sys
f, sentinel = sys.argv[1], sys.argv[2]
s = io.open(f, encoding="utf-8").read()
old = [l for l in s.splitlines() if l.startswith("ANDROID_APP_ID=")]
if len(old) != 1:
    raise SystemExit("product.env 裡 ANDROID_APP_ID= 出現 %d 次(需要正好 1 次)" % len(old))
io.open(f, "w", encoding="utf-8").write(s.replace(old[0], "ANDROID_APP_ID=" + sentinel))
PY
if [ $? -ne 0 ]; then
  bad "植入哨兵值失敗,第 2 項沒有驗到"
else
  M_SH="$(bash "$TMP/lib/product.sh" --dump 2>/dev/null | grep -c "$SENTINEL")"
  M_PY="$(python3 "$TMP/lib/product.py" --dump 2>/dev/null | grep -c "$SENTINEL")"
  # 改一個 ANDROID_APP_ID,推導值(IME id、套件路徑、NetworkGate 類別名與路徑)
  # 應該跟著變,所以命中數必須 > 1 —— 只變一行代表推導那幾行是寫死的。
  if [ "${M_SH:-0}" -gt 1 ] && [ "${M_PY:-0}" -gt 1 ]; then
    ok "改 product.env 後兩邊都跟著變(shell $M_SH 處、python $M_PY 處)"
  else
    bad "改了 product.env 而讀取器輸出沒跟著變(shell $M_SH 處、python $M_PY 處)—— 有人在讀取器裡也寫死了一份"
  fi
fi

# ─────────────────────────────────────────────────────────────────────────────
# 識別碼的字面樣式。scripts/lib/ 以外的腳本不准出現這些字。
HARDCODE_RE='org\.(luminakey|rimequad)|(luminakey|rimequad)-(store|layouts|backup|update|current-keyboard)|kRimeQuad|rimequad_lua_|rimequad-path-sandbox|__rimequad_sandbox|LuminaKey|RimeQuad'

# scripts/、android/、apple/ 底下的 .sh 與 .py。三個排除:
#   scripts/lib/                —— 值的合法住址
#   本檔                        —— 規則本身要把那些字寫出來才能拿去比對
#   apple/scripts/verify_names.py —— macOS 那一側的同型規則檔,同一個理由
# (scripts/lua_sandbox/ 的 .lua 與 .c 不在掃描範圍:它們裡面的 RIMEQUAD_* 全域
#  與 __rimequad_sandbox 標記是 patches/ 那顆沙盒定義的,見 product.env。)
#
# ⚠ 掃描範圍原本只有 scripts/。2026-08-09 的合併稽核發現 apple/scripts/ 與
#   android/ 從來沒有被看過一眼,所以加進來。
# ⚠ windows 是後來補上的,而補的理由要留著:Windows 端改完名之後,實測把
#    winutil.cc 的資料夾名改回舊值,這支腳本**照樣 12/12 全綠** —— 因為
#    windows/ 從來不在任何一個掃描根裡。這正是這支腳本檔頭自己警告的那種
#    失敗:改了顯示名、沒改識別碼,而編譯期、單元測試、發布關卡全部是綠的。
SCRIPT_ROOTS="scripts android apple windows"

# apple/ 的建置與驗證腳本可以寫死名字,**條件是有人盯著它們**:
# `apple/scripts/verify_names.py` 逐條斷言那些字面值彼此相符,而且它自己有
# `--self-test`(把違規真的植入原始碼,確認會紅)。這裡列成一張帶理由的表,
# 而不是一句 `-not -path apple/*`:
#   · 新增的 apple 腳本若寫死識別碼,不在表上 → 這一關紅,得有人**明著**決定;
#   · 表上的檔案不見了 → 也紅(見下面的「陳舊條目」檢查),表不會活得比它的對象久。
HARDCODE_ALLOW="\
apple/scripts/build_app.sh|.app 與執行檔名;verify_names.py §6 逐條斷言
apple/scripts/build_pkg.sh|pkg 識別碼 == <bundle id>.pkg;verify_names.py §6
apple/scripts/package_core.sh|核心層產物的說明文字;verify_names.py §8
apple/scripts/run_kit_tests.sh|Swift 套件路徑 apple/LuminaKey/… 與變異測試的靶
apple/scripts/verify_app_bundle.sh|bundle 內的名字;verify_names.py §6
apple/scripts/verify_pkg.sh|裝完之後的 bundle id;verify_names.py §6
apple/scripts/verify_single_egress.sh|Swift 套件路徑
apple/scripts/verify_user_dict.sh|寫出去的 custom_phrase 掛載檔;標記另由第 6 項釘住
windows/verify_product_names.sh|Windows 那一側的同型規則檔,自帶 20 列逐列反向測試"

scan_scripts() {   # scan_scripts <root>
  local root="$1" d
  for d in $SCRIPT_ROOTS; do
    [ -d "$root/$d" ] || continue
    find "$root/$d" -type f \( -name '*.sh' -o -name '*.py' \) \
         -not -path "$root/scripts/lib/*" \
         -not -name 'verify_product_ids.sh' \
         -not -name 'verify_names.py' -print0 2>/dev/null
  done \
    | xargs -0 -r grep -nE "$HARDCODE_RE" 2>/dev/null \
    | sed "s|^$root/||" \
    | grep -vFf <(printf '%s\n' "$HARDCODE_ALLOW" | cut -d'|' -f1 | sed 's|$|:|') \
    || true
}

step "3. scripts/、android/、apple/ 底下沒有寫死的識別碼"
# 先確認這個掃描器抓得到東西 —— 一個永遠回空的掃描器會讓這一關恆綠。
# 三處植入,三個掃描根各一個,證明每一個根都真的走到了。
mkdir -p "$TMP/plant/scripts" "$TMP/plant/android" "$TMP/plant/apple/scripts"
printf 'IME_ID="org.luminakey.ime/.RimeInputMethodService"\n' > "$TMP/plant/scripts/planted.sh"
printf 'PREFS = "rimequad-store.json"\n' > "$TMP/plant/android/planted.py"
printf 'BACKUP="luminakey-backup.json"\n' > "$TMP/plant/apple/scripts/planted.sh"
N_PLANT="$(scan_scripts "$TMP/plant" | grep -c . || true)"
if [ "${N_PLANT:-0}" -lt 3 ]; then
  bad "掃描器抓不到植入的違規($N_PLANT/3)—— 這一關本身壞了,不是通過"
  scan_scripts "$TMP/plant" | sed 's/^/         /' >&2
else
  HITS="$(scan_scripts "$ROOT")"
  if [ -z "$HITS" ]; then
    ok "三個掃描根對植入的違規都報紅($N_PLANT 處),而真的腳本裡一處都沒有"
  else
    bad "這些腳本仍然把識別碼寫死(請改讀 scripts/lib/product.sh 或 product.py):"
    printf '%s\n' "$HITS" | sed 's/^/         /' >&2
  fi
fi

# 陳舊的允許條目 = 一張活得比對象久的清單。它會安靜地放行一個同名的新檔案。
STALE=""
while IFS='|' read -r f why; do
  [ -n "$f" ] || continue
  [ -f "$ROOT/$f" ] || STALE="$STALE $f"
done <<EOF_ALLOW
$HARDCODE_ALLOW
EOF_ALLOW
if [ -z "$STALE" ]; then
  ok "允許清單上的 $(printf '%s\n' "$HARDCODE_ALLOW" | grep -c .) 個檔案都還在"
else
  bad "允許清單指著不存在的檔案(請刪掉條目):$STALE"
fi
# 允許清單把 apple/ 交給 verify_names.py,所以那一支必須還在、而且還在做這件事。
if [ -f "$ROOT/apple/scripts/verify_names.py" ] \
   && grep -q -- '--self-test' "$ROOT/apple/scripts/verify_names.py" \
   && grep -q '沒有非預期的舊名字殘留' "$ROOT/apple/scripts/verify_names.py"; then
  ok "apple/scripts/verify_names.py 還在,而且還有自我反向測試與舊名殘留掃描"
else
  bad "apple/ 的名字檢查不見了(或不再掃舊名殘留)—— 上面那張允許清單就變成一個洞"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 舊名殘留。改名最典型的失敗是「改了一半」,而一半的那一半通常在文件裡。
LEGACY_RE='rimequad|RimeQuad|RIMEQUAD'
# 刻意保留舊名的地方:
#   product.env            —— 保留舊名的**唯一**合法住址,每一條都寫了理由
#   decisions/product-name.md —— 決策紀錄,舊值是紀錄的一部分
#   coordination.md        —— §5 是各端當時寫下的紀錄,規矩是只加不刪
#
# 另外:規範裡「讀取端要認得舊名」這種相容條款**必須**寫出舊名才有用,所以
# 放行**同一行**寫了「舊名」的句子。這不是漏洞:忘了改的地方不會自稱舊名,
# 而要繞過它就得明著打上那兩個字 —— 那正好是我們想逼人做的那個動作。
LEGACY_LABEL='舊名'
# ⚠ 掃描目標原本只有前五個。2026-08-09 的合併稽核:**四端的原始碼一個都不在
#   裡面**,於是 Android 的六個落地識別碼整批停在舊名,而這一關 6/6 全綠。
#   現在加上 android/ 與 apple/ 的原始碼。
#
#   apple/ 只掃 Sources / AppSources / SettingsSources。它的 Tests/ 與 scripts/
#   由 `apple/scripts/verify_names.py` §9 的 MUST_KEEP / MAY_MENTION 表管
#   —— 那張表比「同一行寫舊名」更嚴(它還要求那些相容片段**必須存在**),
#   在這裡再掃一次只會逼人把同一件事寫兩遍。第 3 項會斷言那支腳本還在。
LEGACY_TARGETS="core docs tools README.md .github/workflows/build.yml \
android/app/src android/testdata \
apple/LuminaKey/Sources apple/LuminaKey/AppSources apple/LuminaKey/SettingsSources \
windows/common windows/service windows/tsf windows/winshared windows/installer"
scan_legacy() {   # scan_legacy <root>
  local root="$1" t targets=""
  for t in $LEGACY_TARGETS; do
    [ -e "$root/$t" ] && targets="$targets $root/$t"
  done
  [ -n "$targets" ] || return 0
  # shellcheck disable=SC2086
  grep -rnE "$LEGACY_RE" $targets 2>/dev/null \
    | grep -v '^'"$root"'/docs/decisions/product-name\.md:' \
    | grep -v '^'"$root"'/docs/coordination\.md:' \
    | grep -vF "$LEGACY_LABEL" \
    | sed "s|^$root/||" \
    || true
}

step "4. core/ docs/ tools/ README 與 android/ apple/ 的原始碼沒有殘留舊產品名"
mkdir -p "$TMP/legacy/docs" "$TMP/legacy/core" "$TMP/legacy/tools" \
         "$TMP/legacy/.github/workflows" "$TMP/legacy/android/app/src" \
         "$TMP/legacy/apple/LuminaKey/Sources"
: > "$TMP/legacy/README.md"
: > "$TMP/legacy/.github/workflows/build.yml"
printf '使用者資料在 ~/Library/Application Support/RimeQuad。\n' > "$TMP/legacy/docs/planted.md"
# 三處植入,三個掃描區各一個 —— 只在 docs/ 植入的話,新加的那兩個目錄
# 就算根本沒被走到也看不出來(那正是這次要修的失敗形狀)。
printf 'const val FILE_NAME = "rimequad-store.json"\n' \
  > "$TMP/legacy/android/app/src/Planted.kt"
printf 'public static let fileName = "rimequad-store.json"\n' \
  > "$TMP/legacy/apple/LuminaKey/Sources/Planted.swift"
N_LEG="$(scan_legacy "$TMP/legacy" | grep -c . || true)"
if [ "${N_LEG:-0}" -lt 3 ]; then
  bad "舊名掃描器只抓到 $N_LEG/3 處植入 —— 有掃描區沒被走到,這一關本身壞了"
  scan_legacy "$TMP/legacy" | sed 's/^/         /' >&2
else
  LEG="$(scan_legacy "$ROOT")"
  if [ -z "$LEG" ]; then
    ok "三個掃描區對植入的舊名都報紅,而真的原始碼裡一處都沒有"
  else
    bad "還有地方叫舊名字(改名改一半就是這樣開始的):"
    printf '%s\n' "$LEG" | sed 's/^/         /' >&2
  fi
fi

# ─────────────────────────────────────────────────────────────────────────────
step "5. 稽核用的 UA 樣式擋得住新名字"
# 改名之後最容易漏的一條:守門的樣式還在擋舊名,新名字大搖大擺地走出去。
# shellcheck disable=SC1091
. "$HERE/lib/product.sh"
UA_MISS=""
for probe in "$RS_PRODUCT_ID_ROOT" "$RS_LEGACY_ID_ROOT"; do
  printf '%s' "$probe-android/1.0" | grep -qiE "$RS_SELF_ID_UA_PATTERN" \
    || UA_MISS="$UA_MISS $probe"
done
if [ -z "$UA_MISS" ]; then
  ok "SELF_ID_UA_PATTERN 抓得到新舊兩個名字的 User-Agent"
else
  bad "SELF_ID_UA_PATTERN 漏掉:$UA_MISS —— 用這個名字自報家門的 UA 會通過稽核"
fi
# 反向對照:一個不含產品名的 UA 不可以被判成自報家門,否則第 7 項會永遠紅,
# 然後被當成雜訊關掉。
if printf '%s' "Mozilla/5.0" | grep -qiE "$RS_SELF_ID_UA_PATTERN"; then
  bad "SELF_ID_UA_PATTERN 把 Mozilla/5.0 也判成自報家門 —— 樣式太寬"
else
  ok "不含產品名的 UA 不會被誤判"
fi

# ──────────────────────────────────────────────────────────────────────────────
# 落地的識別碼。**這是 2026-08-09 那次全綠漏掉的東西。**
#
# 「落地」= 寫進使用者磁碟的檔名與魔術字串。前面幾項守的是「不要出現舊名」與
# 「不要寫死在腳本裡」,但那兩件事都擋不住這一種:**四端各自抄一份,而且抄得
# 不一樣。** 抄的時候不會出現舊名(那一端本來就沒改過)、也不在 scripts/ 底下
# (它們在 .kt / .swift 裡),於是第 3、4 項一句話都不會說。
#
# 症狀一律是**沒有錯誤訊息**:備份被判成「這不是備份」、裝過的方案變成空清單、
# 調過的鍵位回到原樣 —— 畫面上只是一片乾淨,使用者會以為東西本來就沒有。
#
# 每一列:<描述>|<相對路徑>|<必須逐字出現的整段宣告>
# 比對的是**整段宣告**而不是只有值。只找值的話,一個躺在註解或測試裡的同名
# 字串就能讓這一項恆綠 —— 那正是這個專案吃過好幾次虧的形狀。
AND_MAIN="android/app/src/main/java/${RS_ANDROID_PKG_PATH}"
APPLE_KIT="apple/LuminaKey/Sources/LuminaKeyKit"

# 改名前的那一份。**由新值推導,不另外寫死** —— 兩邊各寫一份就又回到
# 「同一個名字有兩個版本」的老問題(見 product.env 檔頭)。
lg() { printf '%s' "${1//$RS_PRODUCT_ID_ROOT/$RS_LEGACY_ID_ROOT}"; }
L_MANIFEST="$(lg "$RS_BACKUP_MANIFEST")"
L_KIND="$(lg "$RS_BACKUP_KIND")"
L_STORE="$(lg "$RS_STORE_REGISTRY_FILE")"
L_LAYOUTS="$(lg "$RS_LAYOUTS_FILE")"
L_MARKER="$(lg "$RS_CUSTOM_PHRASE_MARKER")"

landed_rows() {
  cat <<ROWS
備份 manifest 檔名|$AND_MAIN/store/BackupFormat.kt|const val MANIFEST_NAME = "$RS_BACKUP_MANIFEST"
備份 kind|$AND_MAIN/store/BackupFormat.kt|const val KIND = "$RS_BACKUP_KIND"
容器內 registry entry|$AND_MAIN/store/BackupFormat.kt|const val REGISTRY_ENTRY = "schema/$RS_STORE_REGISTRY_FILE"
容器內 layout entry|$AND_MAIN/store/BackupFormat.kt|const val LAYOUT_ENTRY = "layout/$RS_LAYOUTS_FILE"
安裝帳本檔名 Android|$AND_MAIN/store/InstalledRegistry.kt|const val FILE_NAME = "$RS_STORE_REGISTRY_FILE"
自訂鍵位檔名 Android|$AND_MAIN/keyboard/UserLayoutStore.kt|const val FILE_NAME = "$RS_LAYOUTS_FILE"
偏好檔名 市集|$AND_MAIN/store/StoreController.kt|const val PREFS = "$RS_ANDROID_PREFS_STORE"
偏好檔名 鍵盤|$AND_MAIN/home/KeyboardChoice.kt|const val KEYBOARD_PREFS = "$RS_ANDROID_PREFS_KEYBOARD"
偏好檔名 更新|$AND_MAIN/update/UpdateController.kt|const val PREFS = "$RS_ANDROID_PREFS_UPDATE"
安裝帳本檔名 macOS|$APPLE_KIT/InstalledRegistry.swift|let fileName = "$RS_STORE_REGISTRY_FILE"
掛載標記 macOS|$APPLE_KIT/UserPhrases.swift|let marker = "# $RS_CUSTOM_PHRASE_MARKER: custom_phrase
掛載標記 驗證腳本|apple/scripts/verify_user_dict.sh|# $RS_CUSTOM_PHRASE_MARKER: custom_phrase
ROWS
}

# 相容條款:讀取端必須認得改名前的名字(docs/backup-format.md §1 與
# docs/coordination.md §5,兩份都寫成規範性)。少了任何一列 = 使用者升級之後
# 東西安靜地不見。拿掉舊名支援可以,但那必須是一個**明確的決定**,
# 不是因為沒有人記得它存在。
legacy_rows() {
  cat <<ROWS
備份 manifest 相容|$AND_MAIN/store/BackupFormat.kt|const val LEGACY_MANIFEST_NAME = "$L_MANIFEST"
備份 kind 相容|$AND_MAIN/store/BackupFormat.kt|const val LEGACY_KIND = "$L_KIND"
容器內 registry 相容|$AND_MAIN/store/BackupFormat.kt|const val LEGACY_REGISTRY_ENTRY = "schema/$L_STORE"
容器內 layout 相容|$AND_MAIN/store/BackupFormat.kt|const val LEGACY_LAYOUT_ENTRY = "layout/$L_LAYOUTS"
安裝帳本相容 Android|$AND_MAIN/store/InstalledRegistry.kt|const val LEGACY_FILE_NAME = "$L_STORE"
自訂鍵位相容 Android|$AND_MAIN/keyboard/UserLayoutStore.kt|const val LEGACY_FILE_NAME = "$L_LAYOUTS"
安裝帳本相容 macOS|$APPLE_KIT/LegacyDataMigration.swift|let legacyRegistryFileName = "$L_STORE"
掛載標記相容 macOS|$APPLE_KIT/UserPhrases.swift|["# $L_MARKER: custom_phrase
ROWS
}

# check_landed <root> <rows 函式>：印出不符的列(空輸出 = 全部相符)
check_landed() {
  local root="$1" fn="$2" desc rel want
  "$fn" | while IFS='|' read -r desc rel want; do
    [ -n "$desc" ] || continue
    if [ ! -f "$root/$rel" ]; then
      printf '%s → %s(找不到這個檔案)\n' "$desc" "$rel"
    elif ! grep -qF -- "$want" "$root/$rel"; then
      printf '%s → %s 裡找不到:%s\n' "$desc" "$rel" "$want"
    fi
  done
}

step "6. 落地的識別碼逐一符合 scripts/lib/product.env"

# 逐字取代的植入器。用 python 而不是 sed:宣告裡有 `.`、`/`、`"`,
# 交給正規式去比對就是在賭哪個字元不會被當成語法。
cat > "$TMP/plant_row.py" <<'PLANT_PY'
import io, os, sys
path, want, new_root, old_root = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
# 這一列是新名 → 改回舊名;本來就是舊名(相容宣告)→ 反過來改成新名。
other = want.replace(new_root, old_root)
if other == want:
    other = want.replace(old_root, new_root)
if other == want:
    sys.exit("植入不了(這一列不含產品字根):%s" % want)
s = io.open(path, encoding="utf-8").read()
if s.count(want) != 1:
    sys.exit("在 %s 裡出現 %d 次(需要 1 次):%s" % (path, s.count(want), want))
io.open(path, "w", encoding="utf-8").write(s.replace(want, other, 1))
PLANT_PY

# 反向測試:把每一列的宣告**逐列**改回另一個名字,那一列必須紅、而且只多紅那一列。
# 「看起來有掃到」不算 —— 上一次全綠正是因為掃的是一個不含 android/ 的範圍。
#
# ⚠ 比的是「比植入前**多**紅了哪幾列」,不是「總共紅幾列」。樹上本來就有不符
#   的時候(例如有人剛把一個常數改回舊值),用總數去比會讓這裡爆出二十行雜訊,
#   把真正的那一行淹掉 —— 而被雜訊淹掉的守門和沒有守門是同一件事。
ALL_FILES="$( { landed_rows; legacy_rows; } | cut -d'|' -f2 | sort -u )"
copy_table_files() {   # copy_table_files <目的地>
  local dest="$1" f
  rm -rf "$dest"
  for f in $ALL_FILES; do
    [ -f "$ROOT/$f" ] || continue
    mkdir -p "$dest/$(dirname "$f")"
    cp "$ROOT/$f" "$dest/$f"
  done
}
copy_table_files "$TMP/landed-base"
{ check_landed "$TMP/landed-base" landed_rows
  check_landed "$TMP/landed-base" legacy_rows; } | grep . > "$TMP/base-red.txt" || true

PLANT_BAD=0
PLANT_N=0
while IFS='|' read -r desc rel want; do
  [ -n "$desc" ] || continue
  PLANT_N=$((PLANT_N+1))
  copy_table_files "$TMP/landed"
  if [ ! -f "$TMP/landed/$rel" ]; then
    bad "反向測試:【$desc】指的 $rel 不存在"
    PLANT_BAD=$((PLANT_BAD+1)); continue
  fi
  if ! python3 "$TMP/plant_row.py" "$TMP/landed/$rel" "$want" \
        "$RS_PRODUCT_ID_ROOT" "$RS_LEGACY_ID_ROOT" 2>"$TMP/plant.err"; then
    # 最常見的原因是這一列**在樹上本來就不符**(下面的正向檢查會指名它),
    # 於是沒有東西可以拿來植入。那不是反向測試壞了,是先修正向的那一條。
    bad "反向測試:【$desc】沒驗到 —— $(cat "$TMP/plant.err")"
    PLANT_BAD=$((PLANT_BAD+1)); continue
  fi
  NEW_RED="$( { check_landed "$TMP/landed" landed_rows
                check_landed "$TMP/landed" legacy_rows; } \
              | grep . | grep -vxFf "$TMP/base-red.txt" || true )"
  N_RED="$(printf '%s' "$NEW_RED" | grep -c . || true)"
  if [ "${N_RED:-0}" -ne 1 ] || ! printf '%s' "$NEW_RED" | grep -qF "$desc"; then
    bad "植入【$desc】之後沒有正好多紅那一列(多紅了 ${N_RED:-0} 列)"
    printf '%s\n' "$NEW_RED" | sed 's/^/         /' >&2
    PLANT_BAD=$((PLANT_BAD+1))
  fi
done <<EOF_ROWS
$( landed_rows; legacy_rows )
EOF_ROWS

if [ "$PLANT_BAD" -eq 0 ] && [ "$PLANT_N" -gt 0 ]; then
  ok "$PLANT_N 列逐列植入另一個名字,每一列都只紅在它自己那一行"
else
  bad "$PLANT_N 列裡有 $PLANT_BAD 列的反向測試沒跑到(先看上面那幾行的原因)"
fi

# 文件與腳本裡「可以直接複製貼上」的 IME id。
#
# ⚠ 這一段是被 2026-08-09 的稽核逼出來的:`docs/emulator.md` 兩處寫著
#   `dev.rime.ime/.RimeInputMethodService` —— 一個**從來沒有存在過**的套件名
#   (改名前是 org.rimequad.ime)。它不是改名改壞的,是一直都錯,而前面每一項
#   都看不見它:它不含舊產品名(第 4 項掃不到)、也不在 scripts/ 的字面值
#   黑名單上(第 3 項的樣式只認 org.luminakey / org.rimequad)。
#   偏偏那是一段會被人整行貼進終端機的指令。
#
# 判準:任何 `<套件>/<服務類名>` 之中,**服務類名是我們自己那一個**的,
# 套件就必須等於 product.env 推出來的。dev.rime.imetest / dev.rime.inputmatrix
# 那些靶 app 的類名不同,不會被誤判。
# docs/coordination.md 除外:那份文件的規矩是只加不刪(見 §2),裡面那一則的
# 更正是以附註的方式加在 §5 末尾的。
IME_SVC="${RS_ANDROID_IME_SERVICE#.}"
scan_ime_ids() {   # scan_ime_ids <root>
  local root="$1"
  grep -rhoE "[A-Za-z0-9_.]+/\.?$IME_SVC" \
       "$root/docs" "$root/README.md" "$root/scripts" "$root/core" "$root/tools" \
       "$root/.github" 2>/dev/null \
    --exclude=coordination.md --exclude=verify_product_ids.sh \
    | sort -u || true
}
IME_BAD=""
for got in $(scan_ime_ids "$ROOT"); do
  [ "$got" = "$RS_ANDROID_IME_ID" ] || IME_BAD="$IME_BAD $got"
done
# 反向對照:掃描器要真的抓得到東西,否則「一個都沒有錯」與「一個都沒掃到」
# 長得一模一樣 —— 而後者正是這一關本來的狀態。
mkdir -p "$TMP/ime/docs"
printf 'adb shell ime set dev.rime.ime/.%s\n' "$IME_SVC" > "$TMP/ime/docs/planted.md"
N_IME_PLANT="$(scan_ime_ids "$TMP/ime" | grep -c . || true)"
N_IME_REAL="$(scan_ime_ids "$ROOT" | grep -c . || true)"
if [ "${N_IME_PLANT:-0}" -lt 1 ]; then
  bad "IME id 掃描器抓不到植入的錯誤 id —— 這一關本身壞了"
elif [ "${N_IME_REAL:-0}" -lt 1 ]; then
  bad "文件與腳本裡一個 IME id 都掃不到 —— 不是「全對」,是沒掃到"
elif [ -z "$IME_BAD" ]; then
  ok "文件與腳本裡的 $N_IME_REAL 個 IME id 都等於 $RS_ANDROID_IME_ID"
else
  bad "這些 IME id 對不上 product.env 的 $RS_ANDROID_IME_ID(照著貼會停在 verify_ime.sh 第 3 步):$IME_BAD"
  grep -rn "$IME_BAD" "$ROOT/docs" "$ROOT/scripts" 2>/dev/null | sed 's/^/         /' >&2
fi

LANDED_MISS="$(check_landed "$ROOT" landed_rows)"
if [ -z "$LANDED_MISS" ]; then
  ok "寫進使用者磁碟的名字,四端都照 product.env"
else
  bad "這些落地的名字跟 product.env 對不上(發布之後就是使用者的資料讀不到):"
  printf '%s\n' "$LANDED_MISS" | sed 's/^/         /' >&2
fi

LEGACY_MISS="$(check_landed "$ROOT" legacy_rows)"
if [ -z "$LEGACY_MISS" ]; then
  ok "讀取端都還認得改名前的名字(相容條款沒有被安靜地拿掉)"
else
  bad "相容條款不見了 —— 使用者升級之後東西會安靜地不見,而不是報錯:"
  printf '%s\n' "$LEGACY_MISS" | sed 's/^/         /' >&2
fi

# Windows 端的落地識別碼由它自己那份同型規則檔驗(20 列,逐列反向測試)。
# 放在這裡而不是抄一份進來:抄的那一份會腐爛,而腐爛的方式正好是「兩邊都綠」。
if [ -x "$ROOT/windows/verify_product_names.sh" ]; then
  if bash "$ROOT/windows/verify_product_names.sh" >/dev/null 2>&1; then
    ok "Windows 端的落地識別碼(windows/verify_product_names.sh)"
  else
    bad "Windows 端的落地識別碼對不上,細節跑 windows/verify_product_names.sh"
  fi
else
  bad "找不到 windows/verify_product_names.sh —— Windows 的識別碼現在沒有人在守"
fi

echo
echo "============================================"
echo " 通過 $PASS 項,失敗 $FAIL 項"
echo "============================================"
[ "$FAIL" -eq 0 ] || exit 1
