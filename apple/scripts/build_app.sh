#!/usr/bin/env bash
#
# build_app.sh — 組出 RimeQuad.app（IMKit 輸入法）
#
# 先決條件:apple/scripts/build_macos.sh 已經跑過（產出 apple/build/prefix 的
# 靜態庫）。CI 上這兩支是接連跑的。
#
# 產出:
#   apple/build/RimeQuad.app
#
# ⚠ 這是**CI 產物,不是可散布的版本**:
#   · 只有跑 CI 的那一個架構($(uname -m)),沒有做 universal binary。
#     要做的話得先把 librime 與 5 個依賴各編兩份再 lipo。
#   · 只有 ad-hoc 簽章(codesign -s -)。真的要給別人裝需要 Developer ID + 公證,
#     那需要憑證,不是 CI 上憑空生得出來的東西。
#   · librime 的靜態庫沒有指定部署目標,所以實際的最低系統版本可能高於
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

MIN_OS="${MACOSX_DEPLOYMENT_TARGET:-13.0}"
ARCH="$(uname -m)"
VERSION="${RIMEQUAD_VERSION:-0.2.0}"
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
# 兩邊各自編譯,但任何一邊改壞了另一邊一定會發現。
SWIFT_SOURCES=()
while IFS= read -r f; do SWIFT_SOURCES+=("$f"); done < <(
  find "${PKG}/Sources/RimeQuadKit" -name '*.swift' | sort
  find "${PKG}/AppSources" -name '*.swift' | sort
)
[[ ${#SWIFT_SOURCES[@]} -gt 0 ]] || die "找不到任何 Swift 原始碼"
log "Swift 原始碼 ${#SWIFT_SOURCES[@]} 份"

log "連結 RimeQuad（${ARCH}, macos${MIN_OS}）"
swiftc \
  -swift-version 5 \
  -O \
  -target "${ARCH}-apple-macos${MIN_OS}" \
  -module-name RimeQuad \
  -import-objc-header "${PKG}/AppSources/RimeQuad-Bridging-Header.h" \
  -Xcc -I"${ROOT}/core/include" \
  -framework AppKit -framework InputMethodKit -framework Carbon \
  "${SWIFT_SOURCES[@]}" \
  "${BUILD}/rime_shell.o" \
  "${LIBS[@]}" \
  -lc++ \
  -o "${APP}/Contents/MacOS/RimeQuad" \
  || die "swiftc 失敗"

# ---------------------------------------------------------------- 資源
log "組裝 bundle"
sed -e "s/__VERSION__/${VERSION}/" \
    -e "s/__BUILD__/${BUILD_NO}/" \
    -e "s/__MIN_OS__/${MIN_OS}/" \
    "${PKG}/Resources/Info.plist" > "${APP}/Contents/Info.plist"

printf 'APPL????' > "${APP}/Contents/PkgInfo"

# 主題:桌面端消費 candidates 區塊。**不**打包 core/layouts/ —— 桌面沒有軟鍵盤。
mkdir -p "${APP}/Contents/Resources/themes"
cp "${ROOT}/core/themes/"*.yaml "${APP}/Contents/Resources/themes/"

# librime 的執行期資料。collect_data.sh 產出的那一份。
if [[ -d "${ROOT}/core/data/shared" ]]; then
  mkdir -p "${APP}/Contents/Resources/SharedSupport"
  cp -R "${ROOT}/core/data/shared/." "${APP}/Contents/Resources/SharedSupport/"
else
  echo "[warn] core/data/shared 不存在,.app 裡不會有方案資料（先跑 scripts/collect_data.sh）" >&2
fi

# 選單列圖示。目前是一個純色圓角方塊的佔位圖 ——
# 沒有圖示的話輸入法在「輸入來源」清單裡是一塊空白,使用者選不到。
python3 "${PKG}/Resources/make_icon.py" "${APP}/Contents/Resources/RimeQuad.tiff" \
  || die "產生選單圖示失敗"

# ---------------------------------------------------------------- 簽章
# 未簽章的 .app 在現行 macOS 上不會被 TIS 載入。ad-hoc 足以在本機測試,
# 但**不足以散布**(需要 Developer ID + 公證)。
log "ad-hoc 簽章"
codesign --force --sign - --timestamp=none "${APP}" >/dev/null 2>&1 \
  || echo "[warn] codesign 失敗（CI 上可以忽略；真機安裝需要簽章）" >&2

log "完成: ${APP}"
du -sh "${APP}" | sed 's/^/    /'
