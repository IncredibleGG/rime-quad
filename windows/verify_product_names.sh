#!/usr/bin/env bash
#
# windows/verify_product_names.sh — Windows 端的產品識別碼逐列對帳
#
# ══ 為什麼需要這支 ═════════════════════════════════════════════════
#
# `scripts/verify_product_ids.sh` 第 6 項做的正是這件事,但它的表上只有
# `android/` 與 `apple/` 的列 —— `windows/` 一列都沒有。也就是說 Windows 端
# 的落地識別碼(寫進使用者磁碟與登錄檔的名字)目前**沒有任何東西在看**,
# 而那正是那支腳本自己在檔頭警告的形狀:
#
#   改了顯示名、沒改識別碼,而編譯期、單元測試、發布關卡全部是綠的,
#   只有使用者裝上去才發現。
#
# Windows 端的症狀特別安靜,因為這裡的名字有一半是**目錄名**:
#   · `%APPDATA%\<名>` 改一半 → 解除安裝去刪一個不存在的資料夾,然後回報成功;
#   · `%LOCALAPPDATA%\<名>\diagnostics` 改一半 → 記錄檔寫到舊資料夾,
#     而 doctor 說「找不到記錄檔」;
#   · 設定檔名改一半 → 使用者調過的設定變回預設,畫面上只是一片乾淨。
#
# 這支腳本的表要**與 scripts/verify_product_ids.sh 的 landed_rows 合併**
# (見報告)。在合併之前,它是 Windows 端唯一的守門。
#
# ══ 做了哪六件事 ═══════════════════════════════════════════════════
#
#   1. 落地識別碼逐列比對 product.env,**每一列都自帶反向測試**
#      (把那一列改回舊名,必須只多紅那一列)。
#   2. 資料夾名只有一份:`\<名>` 這種路徑用法只准出現在 winshared/winutil.cc。
#   3. .iss 裡沒有產品名的字面值 —— 值一律由 make_installer.sh 傳進來。
#   4. windows/ 底下沒有殘留的舊產品名。
#   5. windows/ 的 .sh 沒有把識別碼寫死(它們要讀 product_win.sh)。
#   6. GUID 的三份寫法(guids.cc 的位元組、它上面的註解、verify_installer.sh
#      刻意寫死的第二意見)是同一個值。見 windows/guid_cross.py。
#
# 每一項的反向測試都是**真的植入一個違規**,不是「看起來有掃到」,
# 而且比對的是**輸出的內容**不是只有結束碼 —— 一個會被崩潰滿足的反向測試
# 等於沒有反向測試(第 6 項就是這樣被抓到的,見 CI run #71)。
# 沒有反向測試的守門腳本,在它自己壞掉的那天會安靜地全綠。
#
# 用法: windows/verify_product_names.sh
# 離開碼: 0 全過;1 有項目失敗。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# 值的唯一來源。**這支腳本裡不准出現產品名的字面值** ——
# 一支自己寫死了答案的對帳腳本,對帳的是它自己。
# shellcheck disable=SC1091
. "${SCRIPT_DIR}/product_win.sh"

# .iss 相對 repo 根的路徑(product_win.sh 給的是相對 windows/ 的)。
RS_WIN_ISS_REL_FULL="windows/${RS_WIN_ISS_REL}"

PASS=0; FAIL=0
ok()   { echo "  [PASS] $*"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $*" >&2; FAIL=$((FAIL+1)); }
step() { echo; echo "=== $* ==="; }

TMP="$(mktemp -d "${TMPDIR:-/tmp}/win-product-names.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# 改名前的那一份,由新值推導 —— 不另外寫死(見 product.env 檔頭)。
lg() { printf '%s' "${1//${RS_PRODUCT_ID_ROOT}/${RS_LEGACY_ID_ROOT}}"; }
LG_FOLDER="${RS_WIN_DATA_FOLDER//${RS_PRODUCT_NAME}/${RS_LEGACY_PRODUCT_NAME}}"

# ─────────────────────────────────────────────────────────────────────────────
# 落地的識別碼。每一列:<描述>|<相對路徑>|<必須逐字出現的整段宣告>
#
# 比對的是**整段宣告**而不是只有值。只找值的話,一個躺在註解或測試裡的
# 同名字串就能讓這一項恆綠。
# ── ⚠ 2026-08-09:三列換了住址,一列消失了 ────────────────────────
#
# 使用者可見字串全部搬進 `windows/common/ui_strings.cc`(英/繁/簡三語),
# 因為 §6.7 的掃描要求「catalog 以外命中數必須是 0」。所以帶著產品名的
# 那幾列跟著搬:設定窗標題、系統匣提示、語言列按鈕提示。
#
# ⚠ **搬家不等於可以放掉。** 它們仍然是寫到畫面與系統匣上的名字,
#   而這支腳本存在的理由是「改名改一半在編譯期、單元測試、發布關卡
#   全部是綠的」。所以三列只是換 rel,比對的內容一樣嚴格 ——
#   而且現在一列同時吃到繁體與簡體兩個值,比以前更嚴。
#
# ⚠ **「關於方塊」那一列刪掉了,而且這件事要說清楚。**
#   舊的關於方塊印著 `LuminaKey 輸入法(Windows,x64)`。這一輪它整段
#   換成 §4.11 的診斷區塊(永遠英文、等寬、不在地化),裡面**沒有**
#   產品名 —— 產品名現在只出現在視窗標題上,而那是上面第一列在管的。
#   刪掉一列本來是這種守門腳本腐爛的典型方式,所以寫在這裡:
#   它不是「驗不到了」,是**那個位置已經沒有名字可以驗**。
landed_rows() {
  cat <<ROWS
使用者資料夾名(唯一決定處)|windows/winshared/winutil.cc|static const wchar_t kUserDataFolderName[] = L"${RS_WIN_DATA_FOLDER}";
具名管道|windows/winshared/winutil.cc|L"\\\\\\\\.\\\\pipe\\\\${RS_PRODUCT_ID_ROOT}." + sid + L".v1"
服務結束事件|windows/winshared/winutil.cc|L"Local\\\\${RS_PRODUCT_NAME}ServiceQuit." + sid
服務單一實例鎖|windows/winshared/winutil.cc|L"Local\\\\${RS_PRODUCT_NAME}Service." + sid
設定視窗事件|windows/winshared/winutil.cc|L"Local\\\\${RS_PRODUCT_NAME}Settings." + CurrentUserSidString()
設定檔名|windows/common/settings.h|constexpr const char* kSettingsFileName = "${RS_WIN_SETTINGS_FILE}";
設定檔表頭|windows/common/settings.cc|"# ${RS_PRODUCT_NAME} 設定。由設定介面寫入,也可以自己改。\\n"
候選窗類別名|windows/service/cand_window.cc|constexpr wchar_t kClassName[] = L"${RS_PRODUCT_NAME}CandidateWindow";
設定窗類別名|windows/service/settings_window.cc|constexpr wchar_t kClass[] = L"${RS_PRODUCT_NAME}SettingsWindow";
設定窗類別名(瘦 DLL 側)|windows/winshared/winutil.cc|static const wchar_t kSettingsWindowClassName[] = L"${RS_PRODUCT_NAME}SettingsWindow";
設定窗標題|windows/common/ui_strings.cc|L"${RS_PRODUCT_NAME_ZH} 設定", L"${RS_PRODUCT_NAME_ZH_HANS} 设置")
系統匣提示|windows/common/ui_strings.cc|X(kTrayTip, L"${RS_PRODUCT_NAME} Input Method", L"${RS_PRODUCT_NAME_ZH}",
COM 類別描述|windows/tsf/guids.h|#define RIME_TEXT_SERVICE_DESC L"${RS_PRODUCT_NAME_ZH}"
語言設定檔描述 zh-Hant-TW|windows/tsf/guids.cc|&GUID_RimeProfile,     L"${RS_PRODUCT_NAME_ZH}"}
語言設定檔描述 zh-Hans-CN|windows/tsf/guids.cc|&GUID_RimeProfileHans, L"${RS_PRODUCT_NAME_ZH_HANS}"}
語言設定檔描述 zh-Hant-HK|windows/tsf/guids.cc|&GUID_RimeProfileHK,   L"${RS_PRODUCT_NAME_ZH}"}
語言列按鈕提示|windows/common/ui_strings.cc|L"${RS_PRODUCT_NAME_ZH}設定", L"${RS_PRODUCT_NAME_ZH_HANS}设置")
線路上的服務版本|windows/service/pipe_server.cc|ok.service_version = "${RS_PRODUCT_ID_ROOT}-windows/0.2";
doctor 報告暫存檔|windows/setup/doctor.cc|L"${RS_PRODUCT_ID_ROOT}-doctor.txt"
doctor 引擎暫存檔|windows/setup/doctor.cc|L"${RS_PRODUCT_ID_ROOT}-doctor-engine.txt"
ROWS
}

# check_landed <root>：印出不符的列(空輸出 = 全部相符)
check_landed() {
  local root="$1" desc rel want
  landed_rows | while IFS='|' read -r desc rel want; do
    [ -n "$desc" ] || continue
    if [ ! -f "$root/$rel" ]; then
      printf '%s → %s(找不到這個檔案)\n' "$desc" "$rel"
    elif ! grep -qF -- "$want" "$root/$rel"; then
      printf '%s → %s 裡找不到:%s\n' "$desc" "$rel" "$want"
    fi
  done
}

step "1. 落地的識別碼逐一符合 scripts/lib/product.env"

# 逐字取代的植入器。用 python 而不是 sed:宣告裡有 `\`、`"`、`+`,
# 交給正規式去比對就是在賭哪個字元不會被當成語法。
cat > "$TMP/plant_row.py" <<'PLANT_PY'
import io, sys
path, want = sys.argv[1], sys.argv[2]
pairs = [(sys.argv[3], sys.argv[4]), (sys.argv[5], sys.argv[6])]
other = want
for new, old in pairs:
    if new and new in other:
        other = other.replace(new, old)
if other == want:
    sys.exit("植入不了(這一列不含產品名也不含字根):%s" % want)
s = io.open(path, encoding="utf-8").read()
if s.count(want) != 1:
    sys.exit("在 %s 裡出現 %d 次(需要 1 次):%s" % (path, s.count(want), want))
io.open(path, "w", encoding="utf-8").write(s.replace(want, other, 1))
PLANT_PY

ALL_FILES="$(landed_rows | cut -d'|' -f2 | sort -u)"
copy_table_files() {   # copy_table_files <目的地>
  local dest="$1" f
  rm -rf "$dest"
  for f in $ALL_FILES; do
    [ -f "$ROOT/$f" ] || continue
    mkdir -p "$dest/$(dirname "$f")"
    cp "$ROOT/$f" "$dest/$f"
  done
}

# ⚠ 比的是「比植入前**多**紅了哪幾列」,不是「總共紅幾列」。樹上本來就有
#   不符的時候,用總數去比會爆出一堆雜訊把真正那一行淹掉 ——
#   而被雜訊淹掉的守門和沒有守門是同一件事。
copy_table_files "$TMP/base"
check_landed "$TMP/base" | grep . > "$TMP/base-red.txt" || true

PLANT_BAD=0; PLANT_N=0
while IFS='|' read -r desc rel want; do
  [ -n "$desc" ] || continue
  PLANT_N=$((PLANT_N+1))
  copy_table_files "$TMP/plant"
  if [ ! -f "$TMP/plant/$rel" ]; then
    bad "反向測試:【$desc】指的 $rel 不存在"; PLANT_BAD=$((PLANT_BAD+1)); continue
  fi
  if ! python3 "$TMP/plant_row.py" "$TMP/plant/$rel" "$want" \
        "$RS_PRODUCT_NAME" "$RS_LEGACY_PRODUCT_NAME" \
        "$RS_PRODUCT_ID_ROOT" "$RS_LEGACY_ID_ROOT" 2>"$TMP/plant.err"; then
    # 最常見的原因是這一列**在樹上本來就不符**,於是沒有東西可以拿來植入。
    # 那不是反向測試壞了,是先修下面正向的那一條。
    bad "反向測試:【$desc】沒驗到 —— $(cat "$TMP/plant.err")"
    PLANT_BAD=$((PLANT_BAD+1)); continue
  fi
  NEW_RED="$(check_landed "$TMP/plant" | grep . | grep -vxFf "$TMP/base-red.txt" || true)"
  N_RED="$(printf '%s' "$NEW_RED" | grep -c . || true)"
  if [ "${N_RED:-0}" -ne 1 ] || ! printf '%s' "$NEW_RED" | grep -qF "$desc"; then
    bad "植入【$desc】之後沒有正好多紅那一列(多紅了 ${N_RED:-0} 列)"
    printf '%s\n' "$NEW_RED" | sed 's/^/         /' >&2
    PLANT_BAD=$((PLANT_BAD+1))
  fi
done <<EOF_ROWS
$(landed_rows)
EOF_ROWS

if [ "$PLANT_BAD" -eq 0 ] && [ "$PLANT_N" -gt 0 ]; then
  ok "$PLANT_N 列逐列植入舊名,每一列都只紅在它自己那一行"
else
  bad "$PLANT_N 列裡有 $PLANT_BAD 列的反向測試沒跑到(先看上面那幾行的原因)"
fi

LANDED_MISS="$(check_landed "$ROOT")"
if [ -z "$LANDED_MISS" ]; then
  ok "寫進使用者磁碟與登錄檔的名字,全部照 product.env"
else
  bad "這些落地的名字跟 product.env 對不上:"
  printf '%s\n' "$LANDED_MISS" | sed 's/^/         /' >&2
fi

# ─────────────────────────────────────────────────────────────────────────────
step "2. 使用者資料夾名只有一份"
#
# ⚠ 這一項是被真的缺陷逼出來的。tsf/trace.cc 原本自己抄了一份
#   `L"\<資料夾名>\diagnostics\tsf.log"`,而 winutil.h 上明明白白寫著
#   「這一格必須只有一份」。改名時那是**兩個**地方要改,漏掉的那一處
#   不會編譯失敗、不會有錯誤訊息,只會讓記錄檔寫進一個叫舊名字的資料夾。
#
# 判準:資料夾名**當成路徑的一段**用(前後緊鄰反斜線)只准出現在
# winshared/winutil.cc。顯示名(`LuminaKey 輸入法`)不受這一條管 ——
# 那是給人看的字,不是路徑。
FOLDER_OWNER="windows/winshared/winutil.cc"
scan_folder_dupes() {   # scan_folder_dupes <root>
  grep -rnE "\\\\\\\\${RS_WIN_DATA_FOLDER}\\\\\\\\|\\\\\\\\${RS_WIN_DATA_FOLDER}\"" \
       "$1/windows" --include='*.cc' --include='*.h' 2>/dev/null \
    | sed "s|^$1/||" \
    | grep -v "^${FOLDER_OWNER}:" \
    || true
}
mkdir -p "$TMP/dupe/windows/tsf"
printf 'static const wchar_t kTail[] = L"\\\\%s\\\\diagnostics\\\\x.log";\n' \
  "$RS_WIN_DATA_FOLDER" > "$TMP/dupe/windows/tsf/planted.cc"
N_DUPE_PLANT="$(scan_folder_dupes "$TMP/dupe" | grep -c . || true)"
if [ "${N_DUPE_PLANT:-0}" -lt 1 ]; then
  bad "掃描器抓不到植入的第二份資料夾名 —— 這一項本身壞了,不是通過"
else
  DUPE="$(scan_folder_dupes "$ROOT")"
  if [ -z "$DUPE" ]; then
    ok "資料夾名當路徑用的地方只有 ${FOLDER_OWNER}(植入的第二份會被抓到)"
  else
    bad "有人又拼了一份資料夾名。改名時這裡會被漏掉,而漏掉是靜默失敗:"
    printf '%s\n' "$DUPE" | sed 's/^/         /' >&2
    echo "         請改成問 winshared 的 RimeUserDataFolderName()。" >&2
  fi
fi

# ─────────────────────────────────────────────────────────────────────────────
step "3. .iss 裡沒有產品名的字面值"
#
# 值一律由 make_installer.sh 從 product.env 讀出來、用 ISCC 的 /D 傳進去。
# 這一項守的是「有人為了方便,把一個名字直接打回 .iss 裡」。
ISS="${SCRIPT_DIR}/${RS_WIN_ISS_REL}"
iss_literals() {   # iss_literals <檔案>
  # 只看**指示詞與程式碼**:註解行(`;` 開頭,含第 1 行的檔名)不算 ——
  # 註解本來就要講得出這件事在講什麼。再去掉刻意保留的 GitHub repo 名。
  grep -v '^[[:space:]]*;' "$1" \
    | sed "s|github.com/[^/]*/${RS_GITHUB_REPO}||g" \
    | grep -E "${RS_PRODUCT_NAME}|${RS_LEGACY_PRODUCT_NAME}|${RS_PRODUCT_ID_ROOT}|${RS_LEGACY_ID_ROOT}" \
    || true
}
if [ ! -f "$ISS" ]; then
  bad "找不到 $ISS(product.env 的 PRODUCT_ID_ROOT 推出來的路徑)"
else
  cp "$ISS" "$TMP/iss-probe.iss"
  printf 'AppName=%s\n' "$RS_PRODUCT_NAME_ZH" >> "$TMP/iss-probe.iss"
  if [ -z "$(iss_literals "$TMP/iss-probe.iss")" ]; then
    bad "植入一行寫死的 AppName,這一項竟然沒抓到 —— 它本身壞了"
  else
    HITS="$(iss_literals "$ISS")"
    if [ -z "$HITS" ]; then
      ok ".iss 的指示詞裡一個產品名都沒有(植入一行寫死的會被抓到)"
    else
      bad ".iss 裡有寫死的產品名(請改用 make_installer.sh 傳進來的 {#…}):"
      printf '%s\n' "$HITS" | sed 's/^/         /' >&2
    fi
  fi
  # 四個必要的 ISPP 定義守衛。少了它們,傳參那一條斷掉時 .iss 會安靜地
  # 用回一個寫死的預設值 —— 而每一關都是綠的。
  MISSING_GUARD=""
  for d in ProductName ProductNameZh ProductIdRoot SetupBaseName; do
    grep -qF "#ifndef ${d}" "$ISS" || MISSING_GUARD="$MISSING_GUARD ${d}"
  done
  if [ -z "$MISSING_GUARD" ]; then
    ok ".iss 對四個產品定義都有 #ifndef/#error 守衛(少傳就編不過)"
  else
    bad ".iss 少了這幾個定義的守衛:$MISSING_GUARD"
  fi
fi

# ─────────────────────────────────────────────────────────────────────────────
step "4. windows/ 底下沒有殘留舊產品名"
#
# 刻意保留的:GitHub repo 名(product.env 的 GITHUB_REPO,改它會牽動所有
# 推送腳本與 CI)。另外放行**同一行寫了「舊名」兩個字**的句子 —— 相容說明
# 與歷史註解必須寫得出舊名才有用,而忘了改的地方不會自稱舊名。
#
# ⚠ 樣式裡**沒有** `rime-quad`:那個字串是 product.env 的 GITHUB_REPO
#   (刻意保留),把它放進來只會逼人再寫一條例外。舊的管道名 `rime-quad.`
#   由第 1 項的「具名管道」那一列逐字盯著,不靠這裡的模糊比對。
LEGACY_RE="${RS_LEGACY_PRODUCT_NAME}|${RS_LEGACY_ID_ROOT}|RIMEQUAD|RIME 四端輸入法"
#
# 本檔排除:規則本身要把那些字寫出來才比對得了(與 apple/scripts/verify_names.py
# 在 scripts/verify_product_ids.sh 裡被排除是同一個理由)。
scan_legacy() {   # scan_legacy <root>
  grep -rnE "$LEGACY_RE" "$1/windows" "$1/.github/workflows/windows.yml" \
       --exclude=verify_product_names.sh 2>/dev/null \
    | grep -vF '舊名' \
    | sed "s|^$1/||" \
    || true
}
mkdir -p "$TMP/legacy/windows/service" "$TMP/legacy/.github/workflows"
: > "$TMP/legacy/.github/workflows/windows.yml"
printf 'constexpr const char* kF = "%s.settings";\n' "$(lg "$RS_WIN_SETTINGS_FILE")" \
  > "$TMP/legacy/windows/service/planted.cc"
N_LEG_PLANT="$(scan_legacy "$TMP/legacy" | grep -c . || true)"
if [ "${N_LEG_PLANT:-0}" -lt 1 ]; then
  bad "舊名掃描器抓不到植入的違規 —— 這一項本身壞了"
else
  LEG="$(scan_legacy "$ROOT")"
  if [ -z "$LEG" ]; then
    ok "windows/ 與 windows.yml 裡沒有殘留的舊產品名"
  else
    bad "還有地方叫舊名字(改名改一半就是這樣開始的):"
    printf '%s\n' "$LEG" | sed 's/^/         /' >&2
  fi
fi

# ─────────────────────────────────────────────────────────────────────────────
step "5. windows/ 的 .sh 沒有把識別碼寫死"
#
# 它們要讀 product_win.sh。兩個排除:product_win.sh(值的合法住址)與
# 本檔(規則本身要把那些字寫出來才比對得了)。
HARDCODE_RE="${RS_PRODUCT_NAME}|${RS_LEGACY_PRODUCT_NAME}|${RS_PRODUCT_ID_ROOT}|${RS_LEGACY_ID_ROOT}"
scan_scripts() {   # scan_scripts <root>
  find "$1/windows" -maxdepth 1 -type f -name '*.sh' \
       -not -name 'product_win.sh' \
       -not -name 'verify_product_names.sh' -print0 2>/dev/null \
    | xargs -0 -r grep -nE "$HARDCODE_RE" 2>/dev/null \
    | sed "s|^$1/||" \
    || true
}
mkdir -p "$TMP/scripts/windows"
printf 'USER_DIR="$APPDATA/%s"\n' "$RS_WIN_DATA_FOLDER" > "$TMP/scripts/windows/planted.sh"
N_SC_PLANT="$(scan_scripts "$TMP/scripts" | grep -c . || true)"
if [ "${N_SC_PLANT:-0}" -lt 1 ]; then
  bad "腳本掃描器抓不到植入的違規 —— 這一項本身壞了"
else
  SC="$(scan_scripts "$ROOT")"
  if [ -z "$SC" ]; then
    ok "windows/ 的 .sh 一個寫死的識別碼都沒有(都在讀 product_win.sh)"
  else
    bad "這些腳本仍然把識別碼寫死(請改讀 windows/product_win.sh):"
    printf '%s\n' "$SC" | sed 's/^/         /' >&2
  fi
fi

# ─────────────────────────────────────────────────────────────────────────────
step "6. GUID 的三份寫法是同一個值"
#
# CLSID 與三份語言設定檔的 GUID 同時以三種形式存在:guids.cc 的位元組、
# 同一行上方的註解、以及 verify_installer.sh 刻意寫死的第二意見。
# 詳細的理由(以及三份分岔各自的症狀)見 windows/guid_cross.py 檔頭。
#
# ⚠ 這一項本身**不能**改成「從 guids.cc 讀出來再寫回 verify_installer.sh」。
#   那樣就沒有第二意見了,而第二意見正是這裡唯一有價值的東西。
if python3 "${SCRIPT_DIR}/guid_cross.py" "$ROOT" "$RS_WIN_ISS_REL" > "$TMP/guid.txt" 2>&1; then
  sed -n '2,$p' "$TMP/guid.txt" | sed 's/^/         /'
  ok "$(head -1 "$TMP/guid.txt" | sed 's/^  //')"
elif grep -q 'Traceback' "$TMP/guid.txt"; then
  bad "guid_cross.py 崩了(這**不是**「GUID 對不上」,是那支腳本本身壞了):"
  sed 's/^/         /' "$TMP/guid.txt" >&2
else
  bad "GUID 的三份寫法對不上:"
  sed 's/^/         /' "$TMP/guid.txt" >&2
fi

# 反向測試:把 verify_installer.sh 的第二意見改掉一個字元,必須被抓到。
mkdir -p "$TMP/guidprobe/windows/tsf" "$TMP/guidprobe/windows/installer"
cp "$ROOT/windows/tsf/guids.cc"     "$TMP/guidprobe/windows/tsf/"
cp "$ROOT/$RS_WIN_ISS_REL_FULL"     "$TMP/guidprobe/windows/$RS_WIN_ISS_REL"
sed 's/^EXPECT_CLSID=.*/EXPECT_CLSID=\x27{00000000-0000-0000-0000-000000000000}\x27/' \
  "$ROOT/windows/verify_installer.sh" > "$TMP/guidprobe/windows/verify_installer.sh"
#
# ⚠ 這裡比對的是**輸出的內容**,不是只看結束碼。
#   只看結束碼的話,一個在 print 上崩掉的 guid_cross.py 也會「通過」反向測試
#   —— 實測:CI run #71 正是如此,正向那一項因為 UnicodeEncodeError 整個
#   死掉,而這一行照樣印綠字說「它真的在比對」。
#   一個會被崩潰滿足的反向測試,等於沒有反向測試。
python3 "${SCRIPT_DIR}/guid_cross.py" "$TMP/guidprobe" "$RS_WIN_ISS_REL" \
  > "$TMP/guidprobe.out" 2>&1
PROBE_RC=$?
if [ "$PROBE_RC" -eq 0 ]; then
  bad "把第二意見的 CLSID 改成全零,這一項竟然沒抓到 —— 它本身壞了"
elif grep -q 'Traceback' "$TMP/guidprobe.out"; then
  bad "反向測試是靠 guid_cross.py **崩潰**才紅的,不是靠它比對出差異:"
  sed 's/^/         /' "$TMP/guidprobe.out" >&2
elif grep -q '00000000-0000-0000-0000-000000000000' "$TMP/guidprobe.out"; then
  ok "改掉第二意見的 CLSID 會被指名(這一項真的在比對)"
else
  bad "反向測試紅了,但輸出裡沒有指出是哪一個 GUID 對不上:"
  sed 's/^/         /' "$TMP/guidprobe.out" >&2
fi

echo
echo "============================================"
echo " 通過 $PASS 項,失敗 $FAIL 項"
echo "============================================"
[ "$FAIL" -eq 0 ] || exit 1
