#!/usr/bin/env bash
#
# verify_app_bundle.sh — 斷言 RimeQuad.app 的結構與 IMKit 宣告
#
# ── 這支腳本存在的理由 ──────────────────────────────────────────────────
# CI 上沒有登入的圖形工作階段,系統不會從 ~/Library/Input Methods 載入輸入法,
# 所以「候選窗有沒有出現」「打不打得出字」在這裡驗不了。
#
# 但**「為什麼載不起來」的絕大多數原因驗得了**,而且它們的共同症狀是
# 「裝得起來、選單裡看得到、按鍵完全沒反應,而且沒有任何錯誤訊息」:
#
#   · Info.plist 少了 InputMethodConnectionName / ServerControllerClass
#   · Swift 類別名被 mangle,ServerControllerClass 指向一個不存在的類別
#   · TISInputSourceID 沒有以 bundle id 為前綴
#   · 執行檔沒有連上 InputMethodKit
#   · 資源(主題、方案資料)沒有進 bundle
#
# 每一條都是「編得出來但不能用」的具體形態,所以每一條都要有斷言。
#
# 用法:
#   verify_app_bundle.sh [路徑到 .app]
#   verify_app_bundle.sh --expect-fail <路徑>   # 反向測試用
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

EXPECT_FAIL=0
if [ "${1:-}" = "--expect-fail" ]; then EXPECT_FAIL=1; shift; fi
APP="${1:-${ROOT}/apple/build/RimeQuad.app}"
PLIST="${APP}/Contents/Info.plist"
BIN="${APP}/Contents/MacOS/RimeQuad"

fails=0
ok()   { printf '  ✓ %s\n' "$1"; }
bad()  { printf '  ✗ %s\n' "$1" >&2; fails=$((fails + 1)); }

check() {  # check <說明> <條件的結束碼>
  if [ "$2" -eq 0 ]; then ok "$1"; else bad "$1"; fi
}

plist_get() {  # plist_get <key path>
  /usr/libexec/PlistBuddy -c "Print :$1" "${PLIST}" 2>/dev/null
}

echo "=== 驗證 ${APP} ==="

# ── 1. bundle 結構 ─────────────────────────────────────────
[ -d "${APP}" ]; check ".app 存在" $?
[ -f "${PLIST}" ]; check "Contents/Info.plist 存在" $?
[ -x "${BIN}" ]; check "Contents/MacOS/RimeQuad 可執行" $?
[ -f "${APP}/Contents/PkgInfo" ]; check "Contents/PkgInfo 存在" $?

if [ ! -f "${PLIST}" ] || [ ! -x "${BIN}" ]; then
  echo "!! bundle 根本不完整,後面的檢查沒有意義" >&2
  [ "${EXPECT_FAIL}" -eq 1 ] && { echo "  （反向測試:如預期失敗）"; exit 0; }
  exit 1
fi

/usr/bin/plutil -lint "${PLIST}" >/dev/null 2>&1; check "Info.plist 是合法 plist" $?

# ── 2. IMKit 的必要宣告 ────────────────────────────────────
BUNDLE_ID="$(plist_get CFBundleIdentifier)"
[ -n "${BUNDLE_ID}" ]; check "CFBundleIdentifier: ${BUNDLE_ID}" $?

EXEC="$(plist_get CFBundleExecutable)"
[ "${EXEC}" = "RimeQuad" ]; check "CFBundleExecutable == RimeQuad（實際:${EXEC}）" $?

PKGTYPE="$(plist_get CFBundlePackageType)"
[ "${PKGTYPE}" = "APPL" ]; check "CFBundlePackageType == APPL" $?

CONN="$(plist_get InputMethodConnectionName)"
[ -n "${CONN}" ]; check "InputMethodConnectionName: ${CONN}" $?

CTRL="$(plist_get InputMethodServerControllerClass)"
[ -n "${CTRL}" ]; check "InputMethodServerControllerClass: ${CTRL}" $?

plist_get tsInputMethodCharacterRepertoireKey | grep -q Hani
check "tsInputMethodCharacterRepertoireKey 含 Hani" $?

plist_get "LSBackgroundOnly" | grep -qi true
check "LSBackgroundOnly（輸入法不該有 Dock 圖示）" $?

MINOS="$(plist_get LSMinimumSystemVersion)"
case "${MINOS}" in
  __*__|"") bad "LSMinimumSystemVersion 沒有被替換（值是 '${MINOS}'）" ;;
  *) ok "LSMinimumSystemVersion: ${MINOS}" ;;
esac
VER="$(plist_get CFBundleShortVersionString)"
case "${VER}" in
  __*__|"") bad "CFBundleShortVersionString 沒有被替換" ;;
  *) ok "CFBundleShortVersionString: ${VER}" ;;
esac

# ── 3. 輸入模式 ────────────────────────────────────────────
MODES="$(/usr/libexec/PlistBuddy -c "Print :ComponentInputModeDict:tsInputModeListKey" \
         "${PLIST}" 2>/dev/null | grep -c 'TISInputSourceID')"
[ "${MODES:-0}" -ge 1 ]; check "至少宣告一個輸入模式（找到 ${MODES:-0} 個）" $?

# TISInputSourceID 必須以 bundle id 為前綴,否則系統不會把它列進「輸入來源」。
BADIDS="$(/usr/libexec/PlistBuddy -c "Print :ComponentInputModeDict:tsInputModeListKey" \
          "${PLIST}" 2>/dev/null \
          | awk '/TISInputSourceID/ {print $3}' \
          | grep -v "^${BUNDLE_ID}" || true)"
[ -z "${BADIDS}" ]; check "每個 TISInputSourceID 都以 ${BUNDLE_ID} 為前綴" $?

# ── 4. 類別名:plist 與二進位必須對得上 ────────────────────
# ⚠ 這一條**不在這裡斷言**,在第 7 步的 --self-check 裡。
#
# 原本這裡是 `nm -gU | grep _OBJC_CLASS_$_...`,但那是在猜 IMKit 怎麼找類別;
# 實測在 macos-26 / arm64 上 grep 不到（符號表的形態與假設不同),而 .app
# 其實是好的 —— 也就是說這條斷言在**該綠的時候紅**,和「該紅的時候不紅」
# 一樣糟。IMKit 真正做的是 NSClassFromString(plist 裡那個字串),
# 所以改由二進位自己照做一次(SelfCheck.swift)。那才是這一問的正解。
#
# 這裡只留一個**不判定成敗**的診斷,讓下一個人看得到符號表長什麼樣。
echo "  · nm 診斷（不判定成敗）:"
nm "${BIN}" 2>/dev/null | grep -c . | sed 's/^/      符號數 /' || echo "      nm 讀不出符號表"
nm "${BIN}" 2>/dev/null | grep -i "${CTRL}" | head -3 | sed 's/^/      /' || true

# ── 5. 連結 ────────────────────────────────────────────────
otool -L "${BIN}" | grep -q InputMethodKit
check "連上 InputMethodKit.framework" $?
otool -L "${BIN}" | grep -q AppKit
check "連上 AppKit.framework" $?

# librime 是靜態連結的,所以不該出現在 otool -L。
# 「門面真的連進來了」同樣由 --self-check 回答（它直接呼叫 rs_abi_version 與
# rs_keysym_by_name —— 呼叫得動就是連進來了,比 grep 符號表可靠）。
otool -L "${BIN}" | grep -q "librime\.\(dylib\|[0-9]*\.dylib\)" && \
  bad "librime 被動態連結了 —— 這份 .app 換一台機器就跑不起來" || \
  ok "librime 是靜態連結"

# ── 6. 資源 ────────────────────────────────────────────────
THEMES="$(ls "${APP}/Contents/Resources/themes/"*.yaml 2>/dev/null | wc -l | tr -d ' ')"
[ "${THEMES}" -gt 0 ]; check "隨附主題 ${THEMES} 份" $?

# 桌面端不消費 core/layouts/,所以它**不該**在 bundle 裡。
# 這條是反過來的斷言:多帶東西也是錯的（會讓人以為桌面有軟鍵盤）。
[ ! -d "${APP}/Contents/Resources/layouts" ]
check "沒有打包 core/layouts/（桌面沒有軟鍵盤）" $?

[ -f "${APP}/Contents/Resources/RimeQuad.tiff" ]
check "選單列圖示存在" $?

if [ -d "${ROOT}/core/data/shared" ]; then
  [ -d "${APP}/Contents/Resources/SharedSupport" ]
  check "SharedSupport（方案與詞庫）已打包" $?
  ls "${APP}/Contents/Resources/SharedSupport/"*.schema.yaml >/dev/null 2>&1
  check "SharedSupport 裡有 .schema.yaml" $?
fi

# ── 7. 二進位的自我檢查 ────────────────────────────────────
# 這一步真的**執行**了 .app 的二進位（--self-check 模式不啟動 NSApplication）。
# 它會向 librime 問 PhysicalKeys 表裡的每一個 keysym 名稱,並解析隨附主題。
if "${BIN}" --self-check; then
  ok "二進位 --self-check 通過"
else
  bad "二進位 --self-check 失敗（見上方輸出）"
fi

echo
if [ "${EXPECT_FAIL}" -eq 1 ]; then
  if [ "${fails}" -eq 0 ]; then
    echo "!! 反向測試失敗:一個壞掉的 bundle 竟然通過了所有斷言。" >&2
    exit 1
  fi
  echo "  ✓ 反向測試:壞掉的 bundle 如預期被擋下（${fails} 項失敗）"
  exit 0
fi

if [ "${fails}" -ne 0 ]; then
  echo "!! ${fails} 項失敗" >&2
  exit 1
fi
echo "全部通過 ✓"
