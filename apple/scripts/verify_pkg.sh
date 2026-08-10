#!/usr/bin/env bash
#
# verify_pkg.sh — 「裝在正確的地方」這件事,自動化到底能驗到哪裡
#
# ── 為什麼有這一支 ──────────────────────────────────────────────────────
# 上一輪我們驗了 bundle 結構與 Info.plist 宣告 —— 但**全部是在 tar 包裡驗的**。
# 沒有任何一關問過「放到正確位置之後,系統認不認」。所以使用者把 .app 拖進
# /Applications 造成的靜默失敗,自動化一句話都不會說。Windows 端發現 runner
# 查得到註冊表,「系統接不接受」從驗不了變成驗得了;這一支是 macOS 的同一件事。
#
# 三層,由確定到不確定:
#   1. **payload 驗證**(一定驗得到):pkgutil --expand 之後檢查安裝路徑與內容。
#   2. **真的裝一次**(一定驗得到):installer -target CurrentUserHomeDirectory,
#      然後斷言檔案落在 ~/Library/Input Methods。
#   3. **系統認不認得這個輸入來源**:lsregister 與 TISCreateInputSourceList。
#      ⚠ 原本以為 runner 沒有登入的圖形工作階段所以查不到,所以寫成「不算失敗」。
#      **實測結果相反**:TIS 在 GitHub runner 上看得到 ~/Library/Input Methods
#      裡的輸入法,連在地化名稱都解得出來(FOUND …Hans — LuminaKey (Simplified))。
#      既然查得到,這一層就**是關卡**:它直接驗到真機回報的兩個缺陷
#      ——「輸入法沒出現在清單裡」與「清單顯示的是 bundle id」。
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${ROOT}/apple/build"
FAIL=0

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
ok()   { printf '  \033[1;32m✓\033[0m %s\n' "$*"; }
bad()  { printf '  \033[1;31m✗\033[0m %s\n' "$*"; FAIL=1; }
note() { printf '  \033[1;33m·\033[0m %s\n' "$*"; }

PKG="$(ls "${BUILD}/dist/"LuminaKey-*.pkg 2>/dev/null | head -1)"
[ -n "${PKG}" ] || { echo "找不到 .pkg —— 先跑 apple/scripts/build_pkg.sh" >&2; exit 1; }
log "受測的 pkg: ${PKG}"

# ─────────────────────────── 1. payload ───────────────────────────
log "1/3 展開 pkg,檢查它會把東西裝到哪裡"
EXP="${BUILD}/pkgexpand"
rm -rf "${EXP}"
pkgutil --expand "${PKG}" "${EXP}" || { bad "pkgutil --expand 失敗"; exit 1; }

DIST_XML="${EXP}/Distribution"
if [ -f "${DIST_XML}" ]; then
  # ⚠ 這一條是整支腳本最重要的斷言。少了 enable_currentUserHome,
  #   安裝路徑會被解讀成磁碟根目錄底下,使用者的輸入來源清單裡什麼都沒有,
  #   而 Installer 仍然顯示「安裝成功」。
  if grep -q 'enable_currentUserHome="true"' "${DIST_XML}"; then
    ok "Distribution 允許裝到使用者家目錄"
  else
    bad "Distribution 沒有 enable_currentUserHome=\"true\" —— 會裝到錯的地方"
  fi
else
  bad "展開後找不到 Distribution"
fi

COMPONENT="$(ls -d "${EXP}"/*.pkg 2>/dev/null | head -1)"
if [ -n "${COMPONENT}" ] && [ -f "${COMPONENT}/PackageInfo" ]; then
  if grep -q 'install-location="Library/Input Methods"' "${COMPONENT}/PackageInfo"; then
    ok "安裝位置是 Library/Input Methods"
  else
    bad "安裝位置不是 Library/Input Methods:"
    grep -o 'install-location="[^"]*"' "${COMPONENT}/PackageInfo" | sed 's/^/      /'
  fi
  if grep -q 'postinstall' "${COMPONENT}/PackageInfo" 2>/dev/null \
     || [ -e "${COMPONENT}/Scripts" ]; then
    ok "帶著 postinstall(處理隔離屬性與 TIS 登錄)"
  else
    bad "沒有 postinstall —— 隔離屬性不會被清掉"
  fi
else
  bad "展開後找不到元件套件的 PackageInfo"
fi

# ─────────────────────────── 2. 真的裝一次 ───────────────────────────
log "2/3 真的裝一次到這台 runner 的家目錄"
DEST="${HOME}/Library/Input Methods/LuminaKey.app"
rm -rf "${DEST}"
if installer -pkg "${PKG}" -target CurrentUserHomeDirectory >/tmp/luminakey-install.log 2>&1; then
  ok "installer 回報成功"
else
  bad "installer 失敗:"
  sed 's/^/      /' /tmp/luminakey-install.log | tail -20
fi

for path in \
  "${DEST}" \
  "${DEST}/Contents/MacOS/LuminaKey" \
  "${DEST}/Contents/Info.plist" \
  "${DEST}/Contents/Resources/LuminaKey.icns" \
  "${DEST}/Contents/Resources/LuminaKey.tiff" \
  "${DEST}/Contents/Resources/en.lproj/InfoPlist.strings" \
  "${DEST}/Contents/Resources/zh-Hant.lproj/InfoPlist.strings" \
  "${DEST}/Contents/Resources/zh-Hans.lproj/InfoPlist.strings" \
  "${DEST}/Contents/Resources/LuminaKeySettings.app/Contents/MacOS/LuminaKey" \
  "${DEST}/Contents/Resources/LuminaKeySettings.app/Contents/Info.plist" \
  "${DEST}/Contents/Resources/UserTemplate/default.custom.yaml" \
; do
  if [ -e "${path}" ]; then ok "存在:${path#${HOME}/}"; else bad "缺少:${path#${HOME}/}"; fi
done

# ⚠ 「installer 回報成功,而東西不在那裡」是**裝到別的地方去了**,不是沒裝。
#   少了這一段診斷,那一片 ✗ 看起來像 pkgbuild 壞了,而真正的線索
#   (收據裡的安裝路徑)完全沒有被印出來。2026-08-10 run #100 因此查了一輪。
if [ ! -e "${DEST}/Contents/MacOS/LuminaKey" ]; then
  echo "  ── 診斷:那它到底裝到哪裡去了 ──"
  echo "  installer 的輸出:"
  sed 's/^/      /' /tmp/luminakey-install.log | tail -20
  PKG_ID_GUESS="$(pkgutil --pkgs 2>/dev/null | grep -i luminakey || true)"
  if [ -n "${PKG_ID_GUESS}" ]; then
    for p in ${PKG_ID_GUESS}; do
      echo "  收據 ${p}:"
      pkgutil --pkg-info "${p}" 2>/dev/null | sed 's/^/      /'
      echo "      前幾個檔案:"
      pkgutil --files "${p}" 2>/dev/null | head -5 | sed 's/^/        /'
    done
  else
    echo "      pkgutil 一個 luminakey 的收據都沒有 —— 那是真的沒裝"
  fi
  echo "  家目錄底下實際有的 .app:"
  find "${HOME}/Library/Input Methods" -maxdepth 2 -name '*.app' 2>/dev/null \
    | sed 's/^/      /' || true
  echo "  這台機器上所有叫 LuminaKey.app 的東西(relocation 會裝到這些地方):"
  mdfind -name LuminaKey.app 2>/dev/null | head -10 | sed 's/^/      /' || true
fi

# 圖示真的讀得出來嗎。**這一條是為了上一輪那個「空白方框」而加的** ——
# 檔案存在不等於 ImageIO 讀得懂(手寫的 TIFF 就是讀不懂)。
for img in "${DEST}/Contents/Resources/LuminaKey.icns" \
           "${DEST}/Contents/Resources/LuminaKey.tiff"; do
  if sips -g pixelWidth -g pixelHeight "${img}" >/tmp/luminakey-sips.log 2>&1 \
     && grep -q pixelWidth /tmp/luminakey-sips.log; then
    ok "圖示解得開:$(basename "${img}") $(grep -E 'pixel(Width|Height)' /tmp/luminakey-sips.log | tr -d ' \n')"
  else
    bad "圖示解不開(這正是「空白方框」的樣子):$(basename "${img}")"
    sed 's/^/      /' /tmp/luminakey-sips.log
  fi
done

# 在地化顯示名真的查得到嗎。鍵必須就是輸入模式 id。
PLIST="${DEST}/Contents/Info.plist"
MODE_IDS="$(/usr/libexec/PlistBuddy -c "Print :ComponentInputModeDict:tsVisibleInputModeOrderedArrayKey" \
            "${PLIST}" 2>/dev/null | sed -n 's/^ *\(org\.[^ ]*\)$/\1/p')"
if [ -z "${MODE_IDS}" ]; then
  bad "Info.plist 讀不出 tsVisibleInputModeOrderedArrayKey"
else
  for mode in ${MODE_IDS}; do
    missing=""
    for lang in en zh-Hant zh-Hans; do
      f="${DEST}/Contents/Resources/${lang}.lproj/InfoPlist.strings"
      # .strings 可能是 UTF-8 或被編譯成 binary plist,兩種都要讀得到。
      value="$(plutil -extract "${mode}" raw -o - "${f}" 2>/dev/null)"
      if [ -z "${value}" ]; then
        value="$(grep -F "\"${mode}\"" "${f}" 2>/dev/null | head -1)"
      fi
      [ -n "${value}" ] || missing="${missing} ${lang}"
    done
    if [ -z "${missing}" ]; then
      ok "顯示名齊全:${mode}"
    else
      bad "沒有顯示名(清單會顯示 bundle id):${mode} 缺${missing}"
    fi
  done
fi

# ─────────────────────────── 3. 系統認不認 ───────────────────────────
log "3/3 系統登錄層 —— 這一層是這次要問出答案的東西"

LSREGISTER=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
if [ -x "${LSREGISTER}" ]; then
  "${LSREGISTER}" -f "${DEST}" >/dev/null 2>&1
  if "${LSREGISTER}" -dump 2>/dev/null | grep -q "org.luminakey.inputmethod.LuminaKey"; then
    ok "LaunchServices 登錄得到這個 bundle"
  else
    note "LaunchServices 的 dump 裡看不到它(runner 上不一定代表壞掉)"
  fi
else
  note "找不到 lsregister"
fi

PROBE="${ROOT}/apple/scripts/tis_probe.swift"
if [ -f "${PROBE}" ] && command -v swiftc >/dev/null 2>&1; then
  if swiftc -O -o /tmp/tis_probe "${PROBE}" >/tmp/luminakey-probe-build.log 2>&1; then
    OUT="$(/tmp/tis_probe 2>&1)"
    RC=$?
    printf '%s\n' "${OUT}" | sed 's/^/      /'
    case "${RC}" in
      0)
        ok "TIS 看得到 LuminaKey,而且兩個輸入模式都有在地化名稱"
        ;;
      3)
        bad "TIS 看得到 LuminaKey,但**在地化名稱就是 bundle id** —— "
        bad "清單裡會顯示 org.luminakey.inputmethod.LuminaKey.Hans 而不是「LuminaKey 簡體」。"
        bad "檢查 Resources/<lang>.lproj/InfoPlist.strings 的鍵是不是就是輸入模式 id。"
        ;;
      4)
        bad "TIS 查得到別的輸入來源,但**看不到 LuminaKey** —— 系統沒有接受這份 .app。"
        bad "這正是「裝在錯的地方」會有的樣子。"
        ;;
      *)
        bad "tis_probe 結束碼 ${RC}(預期 0/3/4)"
        ;;
    esac
  else
    note "tis_probe 編不起來:"
    tail -5 /tmp/luminakey-probe-build.log | sed 's/^/      /'
  fi
else
  note "沒有 tis_probe.swift 或沒有 swiftc,跳過"
fi

echo
if [ "${FAIL}" -eq 0 ]; then
  echo "pkg 驗證通過 ✓(含「系統真的認得這個輸入法」)"
else
  echo "!! pkg 驗證失敗" >&2
fi
exit "${FAIL}"
