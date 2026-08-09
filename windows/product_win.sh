# shellcheck shell=bash
#
# windows/product_win.sh — Windows 端的產品識別。**值不在這裡。**
#
#   . "$(dirname "${BASH_SOURCE[0]}")/product_win.sh"
#   echo "$RS_WIN_DATA_FOLDER"
#
# 這支只做兩件事:
#   1. 把 scripts/lib/product.sh 讀進來(真正的來源是 scripts/lib/product.env);
#   2. 推導幾個「Windows 才有」的值 —— 資料夾名、設定檔名、.iss 的路徑、
#      安裝程式檔名、安裝記錄的字首。
#
# 為什麼要有這一層而不是每支腳本各推一次:
#
#   `%APPDATA%\<資料夾名>` 的最後一段要用 `${...##*\\}` 切出來,而
#   `installer/<字根>.iss` 要把字根接上副檔名。這種一行的推導,四支腳本
#   各寫一份的時候不會有人覺得是問題 —— 直到其中一支寫成 `${...#*\\}`
#   (少一個 #,切法完全不同)。那支腳本會拿到一個看起來很像路徑的字串,
#   然後去檢查一個不存在的資料夾,並且**報告成功**。
#
#   這正是 winshared/winutil.cc 的 RimeUserDataDir() 在 C++ 那一側解掉的
#   同一個問題,理由也一樣:同一個名字不可以有第二個版本。
#
# ⚠ 這裡不准出現任何產品名的字面值。要新增一個值,先問它是不是應該
#   進 scripts/lib/product.env(四端共用的)還是只有 Windows 要(推導在這裡)。

_rs_win_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "${_rs_win_dir}/../scripts/lib/product.sh"

# ── 使用者資料目錄 ────────────────────────────────────────────────
# product.env 給的是完整的 `%APPDATA%\<資料夾名>`。腳本要的是最後那一段:
# 它會被接到 cygpath 轉出來的 $APPDATA 後面。
RS_WIN_DATA_DIR_SPEC="${RS_WINDOWS_USER_DATA_DIR}"
case "${RS_WIN_DATA_DIR_SPEC}" in
  '%APPDATA%\'*) ;;
  *) echo "product_win.sh: WINDOWS_USER_DATA_DIR 不是 '%APPDATA%\\…' 的形式:${RS_WIN_DATA_DIR_SPEC}" >&2
     return 1 2>/dev/null || exit 1 ;;
esac
RS_WIN_DATA_FOLDER="${RS_WIN_DATA_DIR_SPEC##*\\}"
[ -n "${RS_WIN_DATA_FOLDER}" ] || {
  echo "product_win.sh: 切不出資料夾名" >&2; return 1 2>/dev/null || exit 1; }

# ── 簡體字形的顯示名 ─────────────────────────────────────────────
# 語言列上 zh-Hans-CN 那一份的描述。簡體使用者的輸入法清單上不該出現
# 一串繁體字(見 tsf/guids.cc)。**只換字形,不換產品名。**
RS_PRODUCT_NAME_ZH_HANS="${RS_PRODUCT_NAME_ZH/輸入法/输入法}"

# ── 其餘推導 ──────────────────────────────────────────────────────
# 安裝目錄的最後一段。`{autopf}\<這個>`,與 .iss 的 DefaultDirName 同一個值。
RS_WIN_INSTALL_FOLDER="${RS_PRODUCT_NAME}"
# 使用者資料目錄裡的設定檔(service/settings_store.cc 的 kSettingsFile)。
RS_WIN_SETTINGS_FILE="${RS_PRODUCT_ID_ROOT}.settings"
# 安裝程式腳本與它的產物。產物名就是 CI 的 artifact 名 ——
# scripts/publish_desktop.sh 照那個名字抓,兩者不可以各寫一份。
RS_WIN_ISS_REL="installer/${RS_PRODUCT_ID_ROOT}.iss"
RS_WIN_SETUP_BASE="${RS_CI_ARTIFACT_WINDOWS_SETUP}"
RS_WIN_SETUP_EXE="${RS_WIN_SETUP_BASE}.exe"
# 安裝記錄(/LOG)裡我們自己那幾行的字首,對應 .iss [Code] 的 LogTag。
RS_WIN_LOG_TAG="${RS_PRODUCT_NAME}:"

export RS_WIN_DATA_DIR_SPEC RS_WIN_DATA_FOLDER RS_WIN_INSTALL_FOLDER \
       RS_WIN_SETTINGS_FILE RS_WIN_ISS_REL RS_WIN_SETUP_BASE \
       RS_WIN_SETUP_EXE RS_WIN_LOG_TAG RS_PRODUCT_NAME_ZH_HANS
unset _rs_win_dir

# --dump:給 windows/verify_product_names.sh 與 CI 拿去用。
if [ "${BASH_SOURCE[0]}" = "${0}" ] && [ "${1:-}" = "--dump" ]; then
  for _k in $(compgen -v | grep -E '^RS_WIN_|^RS_PRODUCT_NAME_ZH_HANS$' | sort); do
    printf '%s=%s\n' "$_k" "${!_k}"
  done
fi
