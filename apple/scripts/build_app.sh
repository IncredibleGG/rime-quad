#!/usr/bin/env bash
#
# build_app.sh — 組出 RimeQuad.app(IMKit 輸入法 + 內嵌的設定介面)
#
# 先決條件:apple/scripts/build_macos.sh 已經跑過(產出 apple/build/prefix 的
# 靜態庫)。CI 上這兩支是接連跑的。
#
# 產出:
#   apple/build/RimeQuad.app
#     Contents/MacOS/RimeQuad                       輸入法本體
#     Contents/Resources/RimeQuadSettings.app       設定介面(同一份執行檔)
#
# ⚠ 這是**CI 產物,不是可散布的版本**:
#   · 只有跑 CI 的那一個架構($(uname -m)),沒有做 universal binary。
#   · 只有 ad-hoc 簽章(codesign -s -)。真的要給別人裝需要 Developer ID + 公證。
#   · librime 的靜態庫沒有指定部署目標,實際的最低系統版本可能高於
#     Info.plist 宣告的 LSMinimumSystemVersion。**不要**拿這份去發佈。
#
set -euo pipefail

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PKG="${ROOT}/apple/RimeQuad"
BUILD="${ROOT}/apple/build"
PREFIX="${BUILD}/prefix"
APP="${BUILD}/RimeQuad.app"
SETTINGS_APP="${APP}/Contents/Resources/RimeQuadSettings.app"

MIN_OS="${MACOSX_DEPLOYMENT_TARGET:-13.0}"
ARCH="$(uname -m)"
VERSION="${RIMEQUAD_VERSION:-0.3.0}"
BUILD_NO="$(cd "${ROOT}" && git rev-list --count HEAD 2>/dev/null || echo 1)"

# ---------------------------------------------------------------- 前置檢查
LIBS=(
  "${PREFIX}/lib/librime.a"
  "${PREFIX}/lib/libopencc.a"
  "${PREFIX}/lib/libmarisa.a"
  "${PREFIX}/lib/libleveldb.a"
  "${PREFIX}/lib/libyaml-cpp.a"
  "${PREFIX}/lib/libglog.a"
)
for a in "${LIBS[@]}"; do
  [[ -f "${a}" ]] || die "缺少 ${a} —— 先跑 apple/scripts/build_macos.sh"
done
command -v swiftc >/dev/null 2>&1 || die "找不到 swiftc"

rm -rf "${APP}"
mkdir -p "${APP}/Contents/MacOS" "${APP}/Contents/Resources"

# ---------------------------------------------------------------- 門面層
log "編譯 rime_shell.cc"
clang++ -std=c++17 -O2 -c \
  -I "${PREFIX}/include" -I "${ROOT}/core/include" \
  "${ROOT}/core/src/rime_shell.cc" -o "${BUILD}/rime_shell.o" \
  || die "rime_shell.cc 編譯失敗"

# ---------------------------------------------------------------- Swift
# RimeQuadKit 的原始碼與 `swift test` 用的是**同一份檔案**,不是複製品。
# AppSources(輸入法)與 SettingsSources(設定介面)一起編成同一個執行檔,
# 靠 bundle id 分岔 —— 見 AppSources/main.swift 的檔頭。
SWIFT_SOURCES=()
while IFS= read -r f; do SWIFT_SOURCES+=("$f"); done < <(
  find "${PKG}/Sources/RimeQuadKit" -name '*.swift' | sort
  find "${PKG}/AppSources" -name '*.swift' | sort
  find "${PKG}/SettingsSources" -name '*.swift' | sort
)
[[ ${#SWIFT_SOURCES[@]} -gt 0 ]] || die "找不到任何 Swift 原始碼"
log "Swift 原始碼 ${#SWIFT_SOURCES[@]} 份"

# ⚠ Compression **不是** framework(ld: framework 'Compression' not found)——
#   import Compression 用的 compression_stream_* 在 libSystem 裡,不必明著連。
#   CryptoKit 與 UniformTypeIdentifiers 才是真的 framework。
# ⚠ 註解**不可以**寫在底下那串反斜線接續的參數中間 —— shell 會直接語法錯誤。
log "連結 RimeQuad(${ARCH}, macos${MIN_OS})"
swiftc \
  -swift-version 5 \
  -O \
  -target "${ARCH}-apple-macos${MIN_OS}" \
  -module-name RimeQuad \
  -import-objc-header "${PKG}/AppSources/RimeQuad-Bridging-Header.h" \
  -Xcc -I"${ROOT}/core/include" \
  -framework AppKit -framework InputMethodKit -framework Carbon \
  -framework CryptoKit -framework UniformTypeIdentifiers \
  "${SWIFT_SOURCES[@]}" \
  "${BUILD}/rime_shell.o" \
  "${LIBS[@]}" \
  -lc++ \
  -o "${APP}/Contents/MacOS/RimeQuad" \
  || die "swiftc 失敗"

# ---------------------------------------------------------------- 圖示
# ⚠ 兩種圖示,兩個用途,少任何一個都是「一塊空白方框」:
#     · CFBundleIconFile(.icns)  → 系統設定的輸入來源清單、Finder
#     · tsInputModeMenuIconFileKey(.tiff)→ 選單列上那顆小圖示
#   前一版兩個都沒有(手寫的 TIFF 缺 RowsPerStrip,ImageIO 讀不出來),
#   真機回報就是空白方框。
log "產生圖示"
ICONWORK="${BUILD}/iconwork"
rm -rf "${ICONWORK}"
mkdir -p "${ICONWORK}/RimeQuad.iconset"
python3 "${PKG}/Resources/make_icon.py" "${ICONWORK}" "16,32,64,128,256,512,1024" \
  || die "產生圖示 PNG 失敗"

for pair in "16 16x16" "32 16x16@2x" "32 32x32" "64 32x32@2x" \
            "128 128x128" "256 128x128@2x" "256 256x256" "512 256x256@2x" \
            "512 512x512" "1024 512x512@2x"; do
  set -- ${pair}
  cp "${ICONWORK}/icon_$1.png" "${ICONWORK}/RimeQuad.iconset/icon_$2.png"
done
iconutil -c icns "${ICONWORK}/RimeQuad.iconset" \
  -o "${APP}/Contents/Resources/RimeQuad.icns" || die "iconutil 失敗"

# 選單列圖示:16pt 與 @2x 的 32px 兩個 representation 放進同一份 TIFF。
# tiffutil 是系統自帶的,產出格式一定對 —— 手寫 IFD 的教訓見 make_icon.py。
tiffutil -cathidpicheck "${ICONWORK}/icon_16.png" "${ICONWORK}/icon_32.png" \
  -out "${APP}/Contents/Resources/RimeQuad.tiff" >/dev/null \
  || die "tiffutil 失敗"

# ---------------------------------------------------------------- 資源
log "組裝 bundle"
sed -e "s/__VERSION__/${VERSION}/" \
    -e "s/__BUILD__/${BUILD_NO}/" \
    -e "s/__MIN_OS__/${MIN_OS}/" \
    "${PKG}/Resources/Info.plist" > "${APP}/Contents/Info.plist"

printf 'APPL????' > "${APP}/Contents/PkgInfo"

# ⚠ 在地化顯示名。沒有 .lproj 時「輸入來源」清單顯示的是
#   `org.rimequad.inputmethod.RimeQuad.Hans` 這串 id —— 真機回報過,
#   而「全世界的麻瓜」不會去點一個長這樣的東西。
for lang in en zh-Hant zh-Hans; do
  mkdir -p "${APP}/Contents/Resources/${lang}.lproj"
  cp "${PKG}/Resources/${lang}.lproj/InfoPlist.strings" \
     "${APP}/Contents/Resources/${lang}.lproj/InfoPlist.strings"
done

# 主題:桌面端消費 candidates 區塊。**不**打包 core/layouts/ —— 桌面沒有軟鍵盤。
mkdir -p "${APP}/Contents/Resources/themes"
cp "${ROOT}/core/themes/"*.yaml "${APP}/Contents/Resources/themes/"

# librime 的執行期資料。collect_data.sh 產出的那一份。
if [[ -d "${ROOT}/core/data/shared" ]]; then
  mkdir -p "${APP}/Contents/Resources/SharedSupport"
  cp -R "${ROOT}/core/data/shared/." "${APP}/Contents/Resources/SharedSupport/"
else
  echo "[warn] core/data/shared 不存在,.app 裡不會有方案資料(先跑 scripts/collect_data.sh)" >&2
fi

# ---------------------------------------------------------------- 設定介面
log "組裝 RimeQuadSettings.app"
mkdir -p "${SETTINGS_APP}/Contents/MacOS" "${SETTINGS_APP}/Contents/Resources"
# **同一份執行檔**。複製而不是符號連結:符號連結指出 bundle 之外時
# codesign 會拒絕,而指進去的相對連結在 .pkg 安裝之後容易斷。
cp "${APP}/Contents/MacOS/RimeQuad" "${SETTINGS_APP}/Contents/MacOS/RimeQuad"
sed -e "s/__VERSION__/${VERSION}/" \
    -e "s/__BUILD__/${BUILD_NO}/" \
    -e "s/__MIN_OS__/${MIN_OS}/" \
    "${PKG}/Resources/Settings-Info.plist" > "${SETTINGS_APP}/Contents/Info.plist"
printf 'APPL????' > "${SETTINGS_APP}/Contents/PkgInfo"
cp "${APP}/Contents/Resources/RimeQuad.icns" "${SETTINGS_APP}/Contents/Resources/"
for lang in en zh-Hant zh-Hans; do
  mkdir -p "${SETTINGS_APP}/Contents/Resources/${lang}.lproj"
  cp "${PKG}/Resources/Settings/${lang}.lproj/InfoPlist.strings" \
     "${SETTINGS_APP}/Contents/Resources/${lang}.lproj/InfoPlist.strings"
done

# ---------------------------------------------------------------- 簽章
# 未簽章的 .app 在現行 macOS 上不會被 TIS 載入。ad-hoc 足以在本機測試,
# 但**不足以散布**(需要 Developer ID + 公證)。
#
# ⚠ 巢狀 bundle 要**由內而外**簽。先簽外層的話,之後動內層會讓外層的
#   簽章失效,而症狀是「輸入法選得到但完全載不起來」,沒有錯誤訊息。
log "ad-hoc 簽章(由內而外)"
codesign --force --sign - --timestamp=none "${SETTINGS_APP}" >/dev/null 2>&1 \
  || echo "[warn] 設定介面 codesign 失敗" >&2
codesign --force --sign - --timestamp=none "${APP}" >/dev/null 2>&1 \
  || echo "[warn] codesign 失敗(CI 上可以忽略;真機安裝需要簽章)" >&2

log "完成: ${APP}"
du -sh "${APP}" | sed 's/^/    /'
