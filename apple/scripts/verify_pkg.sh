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
#   3. **系統認不認得這個輸入來源**(試試看):lsregister 與 TISCreateInputSourceList。
#      runner 沒有登入的圖形工作階段,登錄這一層有沒有效是我們要問出來的事。
#      第 3 層失敗**不算失敗**,但會把原因印出來 —— 那也是結論。
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${ROOT}/apple/build"
FAIL=0

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
ok()   { printf '  \033[1;32m✓\033[0m %s\n' "$*"; }
bad()  { printf '  \033[1;31m✗\033[0m %s\n' "$*"; FAIL=1; }
note() { printf '  \033[1;33m·\033[0m %s\n' "$*"; }

PKG="$(ls "${BUILD}/dist/"RimeQuad-*.pkg 2>/dev/null | head -1)"
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
DEST="${HOME}/Library/Input Methods/RimeQuad.app"
rm -rf "${DEST}"
if installer -pkg "${PKG}" -target CurrentUserHomeDirectory >/tmp/rimequad-install.log 2>&1; then
  ok "installer 回報成功"
else
  bad "installer 失敗:"
  sed 's/^/      /' /tmp/rimequad-install.log | tail -20
fi

for path in \
  "${DEST}" \
  "${DEST}/Contents/MacOS/RimeQuad" \
  "${DEST}/Contents/Info.plist" \
  "${DEST}/Contents/Resources/RimeQuad.icns" \
  "${DEST}/Contents/Resources/RimeQuad.tiff" \
  "${DEST}/Contents/Resources/en.lproj/InfoPlist.strings" \
  "${DEST}/Contents/Resources/zh-Hant.lproj/InfoPlist.strings" \
  "${DEST}/Contents/Resources/zh-Hans.lproj/InfoPlist.strings" \
  "${DEST}/Contents/Resources/RimeQuadSettings.app/Contents/MacOS/RimeQuad" \
  "${DEST}/Contents/Resources/RimeQuadSettings.app/Contents/Info.plist" \
; do
  if [ -e "${path}" ]; then ok "存在:${path#${HOME}/}"; else bad "缺少:${path#${HOME}/}"; fi
done

# 圖示真的讀得出來嗎。**這一條是為了上一輪那個「空白方框」而加的** ——
# 檔案存在不等於 ImageIO 讀得懂(手寫的 TIFF 就是讀不懂)。
for img in "${DEST}/Contents/Resources/RimeQuad.icns" \
           "${DEST}/Contents/Resources/RimeQuad.tiff"; do
  if sips -g pixelWidth -g pixelHeight "${img}" >/tmp/rimequad-sips.log 2>&1 \
     && grep -q pixelWidth /tmp/rimequad-sips.log; then
    ok "圖示解得開:$(basename "${img}") $(grep -E 'pixel(Width|Height)' /tmp/rimequad-sips.log | tr -d ' \n')"
  else
    bad "圖示解不開(這正是「空白方框」的樣子):$(basename "${img}")"
    sed 's/^/      /' /tmp/rimequad-sips.log
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
  if "${LSREGISTER}" -dump 2>/dev/null | grep -q "org.rimequad.inputmethod.RimeQuad"; then
    ok "LaunchServices 登錄得到這個 bundle"
  else
    note "LaunchServices 的 dump 裡看不到它(runner 上不一定代表壞掉)"
  fi
else
  note "找不到 lsregister"
fi

PROBE="${ROOT}/apple/scripts/tis_probe.swift"
if [ -f "${PROBE}" ] && command -v swiftc >/dev/null 2>&1; then
  if swiftc -O -o /tmp/tis_probe "${PROBE}" >/tmp/rimequad-probe-build.log 2>&1; then
    OUT="$(/tmp/tis_probe 2>&1)"
    RC=$?
    printf '%s\n' "${OUT}" | sed 's/^/      /'
    if printf '%s\n' "${OUT}" | grep -q "org.rimequad.inputmethod.RimeQuad"; then
      ok "TISCreateInputSourceList 看得到 RimeQuad —— 系統真的接受了這個輸入法"
    else
      note "TIS 查詢跑得起來(結束碼 ${RC}),但清單裡沒有 RimeQuad。"
      note "這是預期內的:TIS 只掃描**有登入的圖形工作階段**的 ~/Library/Input Methods,"
      note "而 GitHub runner 是 SSH/背景工作階段。這一層只有人在真 Mac 上驗得到。"
    fi
  else
    note "tis_probe 編不起來:"
    tail -5 /tmp/rimequad-probe-build.log | sed 's/^/      /'
  fi
else
  note "沒有 tis_probe.swift 或沒有 swiftc,跳過"
fi

echo
if [ "${FAIL}" -eq 0 ]; then
  echo "pkg 驗證通過 ✓(第 3 層的結果見上面,它不影響成敗)"
else
  echo "!! pkg 驗證失敗" >&2
fi
exit "${FAIL}"
