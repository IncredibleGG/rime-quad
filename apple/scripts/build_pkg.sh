#!/usr/bin/env bash
#
# build_pkg.sh — 做一個雙擊就能裝的 .pkg
#
# ── 為什麼一定要有 .pkg ──────────────────────────────────────────────────
# 真機回報:使用者拿到 .app,第一直覺是把它拖進 /Applications。那是任何人
# 拿到一個 .app 的正確直覺,而結果是**靜默失敗** —— 系統把它當一般 app 登錄,
# 行程也起得來,但它永遠不會出現在「輸入來源」清單裡,而且沒有任何錯誤訊息。
#
# 舊的安裝流程是「解壓 → 跑一行 xattr → 複製到一個隱藏目錄 → 登出再登入」,
# 四個步驟裡任何一步做錯都是同一種靜默失敗。這種東西不該由使用者執行。
#
# 產出:apple/build/dist/RimeQuad-<版本>-<架構>.pkg
#   · 裝到 ~/Library/Input Methods(不需要管理員密碼)
#   · postinstall 自動處理隔離屬性,並把 app 叫起來讓 TIS 登錄它
#   · 裝完的結語畫面告訴使用者下一步去哪裡按
#
set -euo pipefail

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${ROOT}/apple/build"
APP="${BUILD}/RimeQuad.app"
DIST="${BUILD}/dist"
WORK="${BUILD}/pkgwork"

VERSION="${RIMEQUAD_VERSION:-0.3.0}"
ARCH="$(uname -m)"
PKG_ID="org.rimequad.inputmethod.RimeQuad.pkg"
OUT="${DIST}/RimeQuad-${VERSION}-${ARCH}.pkg"

[[ -d "${APP}" ]] || die "找不到 ${APP} —— 先跑 apple/scripts/build_app.sh"
command -v pkgbuild >/dev/null 2>&1 || die "找不到 pkgbuild"
command -v productbuild >/dev/null 2>&1 || die "找不到 productbuild"

rm -rf "${WORK}"
mkdir -p "${WORK}/payload" "${WORK}/scripts" "${WORK}/resources" "${DIST}"

log "準備 payload"
cp -R "${APP}" "${WORK}/payload/RimeQuad.app"

# ---------------------------------------------------------------- postinstall
# ⚠ **這支腳本不准失敗。** 每一行都帶 `|| true`:安裝已經成功了,
#   後處理沒做到頂多是使用者要多按一次,而 exit != 0 會讓 Installer 顯示
#   「安裝失敗」,然後他會把裝好的東西刪掉。
cat > "${WORK}/scripts/postinstall" <<'POSTINSTALL'
#!/bin/sh
# $2 = 完整安裝路徑(例如 /Users/xxx/Library/Input Methods)
TARGET="$2/RimeQuad.app"
[ -d "$TARGET" ] || exit 0

# 從網路下載來的東西帶著隔離屬性,TIS 會拒絕載入,而且不會說為什麼。
/usr/bin/xattr -dr com.apple.quarantine "$TARGET" 2>/dev/null || true

# 把 app 叫起來一次,系統才會登錄這個輸入法。
# 安裝腳本可能以 root 身分執行,所以要切回真正的登入使用者 ——
# 用 root 打開的話它登錄到 root 的工作階段,使用者的清單裡還是什麼都沒有。
CONSOLE_USER="$(/usr/bin/stat -f %Su /dev/console 2>/dev/null || echo "")"
if [ -n "$CONSOLE_USER" ] && [ "$CONSOLE_USER" != "root" ]; then
  CONSOLE_UID="$(/usr/bin/id -u "$CONSOLE_USER" 2>/dev/null || echo "")"
  if [ -n "$CONSOLE_UID" ]; then
    /bin/launchctl asuser "$CONSOLE_UID" /usr/bin/open "$TARGET" 2>/dev/null || true
  fi
else
  /usr/bin/open "$TARGET" 2>/dev/null || true
fi
exit 0
POSTINSTALL
chmod +x "${WORK}/scripts/postinstall"

# ---------------------------------------------------------------- 結語
cat > "${WORK}/resources/conclusion.html" <<'HTML'
<html><body style="font-family:-apple-system;font-size:13px;line-height:1.6">
<p><b>裝好了。還差最後一步 —— 這一步只有你做得到。</b></p>
<ol>
<li>打開「系統設定」 › 「鍵盤」 › 「文字輸入」 › 「輸入來源」旁的「編輯…」</li>
<li>按左下角的 <b>+</b>,選「中文（繁體）」或「中文（簡體）」,
    找到 <b>RimeQuad</b>,加入。</li>
<li>用 <b>Control + Space</b> 切到 RimeQuad,開始打字。</li>
</ol>
<p>設定在<b>選單列右上角的輸入法圖示</b>裡:方案、外觀、詞庫、離線開關都在那裡。</p>
<hr>
<p><b>Almost done — this last step is yours.</b></p>
<ol>
<li>Open System Settings › Keyboard › Text Input › Input Sources › Edit…</li>
<li>Click <b>+</b>, pick Chinese (Traditional) or Chinese (Simplified),
    find <b>RimeQuad</b>, and add it.</li>
<li>Press <b>Control + Space</b> to switch to RimeQuad and start typing.</li>
</ol>
<p>Settings live in the input-method icon in the menu bar.</p>
</body></html>
HTML

# ---------------------------------------------------------------- 元件
log "pkgbuild"
pkgbuild \
  --root "${WORK}/payload" \
  --install-location "Library/Input Methods" \
  --scripts "${WORK}/scripts" \
  --identifier "${PKG_ID}" \
  --version "${VERSION}" \
  "${WORK}/RimeQuad-component.pkg" >/dev/null || die "pkgbuild 失敗"

# ---------------------------------------------------------------- 產品
# ⚠ `enable_currentUserHome="true"` 是「裝到自己的家目錄、不用管理員密碼」
#   的開關。少了它,`--install-location Library/Input Methods` 會被解讀成
#   **磁碟根目錄底下**的那個路徑,裝完之後使用者的清單裡什麼都沒有。
cat > "${WORK}/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>RimeQuad</title>
    <organization>org.rimequad</organization>
    <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_currentUserHome="true" enable_anywhere="false" enable_localSystem="false"/>
    <conclusion file="conclusion.html" mime-type="text/html"/>
    <pkg-ref id="${PKG_ID}"/>
    <choices-outline>
        <line choice="default">
            <line choice="${PKG_ID}"/>
        </line>
    </choices-outline>
    <choice id="default"/>
    <choice id="${PKG_ID}" visible="false">
        <pkg-ref id="${PKG_ID}"/>
    </choice>
    <pkg-ref id="${PKG_ID}" version="${VERSION}" onConclusion="none">RimeQuad-component.pkg</pkg-ref>
</installer-gui-script>
XML

log "productbuild"
productbuild \
  --distribution "${WORK}/distribution.xml" \
  --package-path "${WORK}" \
  --resources "${WORK}/resources" \
  "${OUT}" >/dev/null || die "productbuild 失敗"

log "完成: ${OUT}"
ls -lh "${OUT}" | sed 's/^/    /'
