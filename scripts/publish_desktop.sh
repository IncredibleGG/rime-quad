#!/usr/bin/env bash
# publish_desktop.sh <macos|windows> <run_id>
#
# 把 GitHub Actions 的產物組成「使用者下載得下來、裝得起來」的包，上傳到 R2。
#
# 為什麼需要這一步、而不是直接把 CI artifact 丟給使用者：
#   - CI artifact 要登入 GitHub 才下載得到，而且是被 zip 過的 zip。
#   - Windows 的產物只有三個執行檔，**不含 librime 的執行期資料**。少了它
#     服務進程起得來、輸入法註冊得上去、然後一個字都打不出來。
#   - macOS 的 .app 從瀏覽器下載會被打上隔離屬性，不解掉的話 TIS 不會載入它，
#     而且失敗時完全沒有錯誤訊息。
#   兩者都屬於「裝得起來但不能用」——這個專案已經吃過太多次這種虧。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# 產品識別、R2 路徑與 CI artifact 名字的唯一來源,見 scripts/lib/product.env。
# shellcheck source=lib/product.sh
. "$ROOT/scripts/lib/product.sh"
PLATFORM="${1:?用法: publish_desktop.sh <macos|windows> <run_id>}"
RUN_ID="${2:?缺 run_id}"

REPO="IncredibleGG/$RS_GITHUB_REPO"
TOK=$(tr -d " \t\r\n" < "$HOME/.github-token")
BUCKET="$RS_R2_REMOTE"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# token 不經由參數列傳給 curl：參數列會出現在 `bash -x` 的追蹤與 ps 裡。
# --config - 從 stdin 讀，追蹤裡只看得到 "--config -"。
api() { printf 'header = "Authorization: token %s"\n' "$TOK" | curl -sS --config - "$@"; }

SHA=$(api "https://api.github.com/repos/$REPO/actions/runs/$RUN_ID" \
      | python3 -c 'import json,sys; print(json.load(sys.stdin)["head_sha"])')
SHORT=${SHA:0:7}
STAMP=$(date -u +%Y%m%d-%H%M)

# ---- 抓 artifact ----------------------------------------------------------
fetch_artifact() {   # fetch_artifact <名稱前綴> <解到哪>
  local want="$1" dest="$2" url
  url=$(api "https://api.github.com/repos/$REPO/actions/runs/$RUN_ID/artifacts" \
        | WANT="$want" python3 -c '
import json,sys,os
want=os.environ["WANT"]
for a in json.load(sys.stdin)["artifacts"]:
    if a["name"].startswith(want):
        print(a["archive_download_url"]); break
else:
    sys.exit("找不到 artifact: "+want)
')
  mkdir -p "$dest"
  api -L "$url" -o "$WORK/a.zip"
  unzip -qo "$WORK/a.zip" -d "$dest"
  rm -f "$WORK/a.zip"
}

# ⚠ 這支腳本裡有**兩個**名字。它們現在剛好一樣,但不是同一件事:
#
#   APP_BASE            這一份產物實際叫什麼(.app 的名字、Setup.exe 的字根、
#                       使用者在系統設定裡看到的那個字、資料目錄的名字)。
#                       它由 apple/ 與 windows/ 那兩條線決定,所以**從產物本身
#                       量出來**,不在這裡猜。那兩條線改名之後,README 裡每一句
#                       都會自動跟著改。
#   $RS_R2_ARTIFACT_BASE R2 上的檔名。**刻意不改** —— 應用內升級與已經發出去的
#                       連結指著它,改了會斷。見 scripts/lib/product.env。
#
# 兩者混用的下場是「說明書上的名字和包裡的名字對不上」,而 macOS 那條手動安裝
# 路徑對不上時是**靜默失敗**:四步做完,系統一聲不吭地不載入它。
#
# 量出來之後要對照白名單。沒有這一步的話,artifact 裡多一個叫 Foo-app-x.tar.gz
# 的東西就會讓整份 README 講一個不存在的產品。
check_app_base() {   # check_app_base <量到的名字>
  local got="$1" b
  [ -n "$got" ] || { echo "量不出產物的名字" >&2; exit 1; }
  for b in $RS_DESKTOP_APP_BASES; do
    [ "$got" = "$b" ] && { echo "[name] 產物叫 $got"; return 0; }
  done
  echo "產物叫 $got,不在允許的名單裡($RS_DESKTOP_APP_BASES)。" >&2
  echo "改名了就把新名字加進 scripts/lib/product.env 的 DESKTOP_APP_BASES。" >&2
  exit 1
}

# 執行期資料是兩端共用的前置。沒有它就別發。
require_data() {
  [ -d "$ROOT/core/data/shared" ] || { echo "core/data/shared 不存在，先跑 scripts/collect_data.sh" >&2; exit 1; }
  local n; n=$(find "$ROOT/core/data/shared" -name '*.schema.yaml' | wc -l)
  [ "$n" -ge 4 ] || { echo "core/data/shared 只有 $n 個 schema，資料不完整" >&2; exit 1; }
  echo "[data] $n 個 schema"
}

upload() {  # upload <本機檔> <遠端相對路徑>
  local f="$1" rel="$2"
  rclone copyto "$f" "$BUCKET/$rel" --s3-no-check-bucket
  local want got
  want=$(stat -c%s "$f")
  got=$(curl -sSI "https://pub-d6a54d2e5f5947e2b0b23fb8e27ce0a5.r2.dev/rime/$rel" \
        | awk 'tolower($1)=="content-length:"{print $2+0}' | tail -1)
  [ "$want" = "$got" ] || { echo "上傳後大小對不上: $rel ($want vs $got)" >&2; exit 1; }
  echo "[ok] $rel  ($want bytes)"
}

case "$PLATFORM" in
# ---------------------------------------------------------------- macOS ----
macos)
  fetch_artifact "$RS_CI_ARTIFACT_MACOS_APP" "$WORK/dl"
  TGZ=$(ls "$WORK"/dl/*-app-*.tar.gz)
  ARCH=$(basename "$TGZ" .tar.gz); ARCH=${ARCH##*-}
  APP_BASE=$(basename "$TGZ"); APP_BASE=${APP_BASE%%-app-*}
  check_app_base "$APP_BASE"
  # 反向確認：包裡真的有 .app 而且有可執行檔，不是空殼
  # 注意：不可寫成 `tar tzf ... | grep -q`。set -o pipefail 之下，grep 命中即結束
  # 會讓 tar 收到 SIGPIPE，整條 pipeline 判失敗——**命中反而變成失敗**。
  LIST=$(tar tzf "$TGZ")
  grep -q "$APP_BASE.app/Contents/MacOS/$APP_BASE" <<<"$LIST" \
    || { echo "包裡沒有 $APP_BASE.app/Contents/MacOS/$APP_BASE" >&2; exit 1; }
  grep -q 'Contents/Resources/SharedSupport/.*schema.yaml' <<<"$LIST" \
    || { echo "包裡沒有方案資料——裝得起來但一個字都打不出來" >&2; exit 1; }

  NAME="$RS_R2_ARTIFACT_BASE-macos-$ARCH-$STAMP-$SHORT.tar.gz"
  LATEST_TGZ="$RS_R2_ARTIFACT_BASE-latest.tar.gz"
  cp "$TGZ" "$WORK/$NAME"
  upload "$WORK/$NAME" "$RS_R2_MACOS_DIR/$NAME"
  upload "$WORK/$NAME" "$RS_R2_MACOS_DIR/$LATEST_TGZ"

  # .pkg 是主要下載。手動裝那條路徑(解壓→xattr→搬到隱藏目錄→登出)四步
  # 任何一步錯都是**靜默失敗** —— 使用者第一次就把 .app 放進了 /Applications,
  # 系統照樣登錄它、行程也起得來,但永遠不會出現在輸入來源清單裡。
  fetch_artifact "$RS_CI_ARTIFACT_MACOS_PKG" "$WORK/pkg"
  PKG=$(ls "$WORK"/pkg/*.pkg)
  # 這裡只驗「它是不是一個真的 .pkg」—— Ubuntu 上沒有 xar / pkgutil,拆不開它。
  # **內容由 CI 驗**:apple/scripts/verify_pkg.sh 在 macOS runner 上真的裝一次,
  # 斷言檔案落在 ~/Library/Input Methods,並斷言 PackageInfo 的 install-location。
  # 那一關才擋得住「少了 enable_currentUserHome → 裝到磁碟根目錄,而 Installer
  # 仍然報成功」這種靜默失敗。這裡再驗一次只是防手滑傳錯檔案。
  head -c4 "$PKG" | grep -q "xar!" \
    || { echo "$PKG 不是 xar 封存(.pkg 應該是)" >&2; exit 1; }
  [ "$(stat -c%s "$PKG")" -gt 3000000 ] \
    || { echo ".pkg 只有 $(stat -c%s "$PKG") bytes —— 不可能帶著方案資料" >&2; exit 1; }
  PNAME="$RS_R2_ARTIFACT_BASE-macos-$ARCH-$STAMP-$SHORT.pkg"
  LATEST_PKG="$RS_R2_ARTIFACT_BASE-latest.pkg"
  cp "$PKG" "$WORK/$PNAME"
  upload "$WORK/$PNAME" "$RS_R2_MACOS_DIR/$PNAME"
  upload "$WORK/$PNAME" "$RS_R2_MACOS_DIR/$LATEST_PKG"

  cat > "$WORK/README.txt" <<TXT
$APP_BASE macOS  $STAMP  ($SHORT, $ARCH)

⚠ 這是第一個可安裝的版本，還沒有任何人在真的 Mac 上用過它。
   CI 驗過的是「編得起來、結構正確、105 項單元測試綠、核心層打得出你好」。
   候選窗會不會出現、在你的 app 裡打不打得出字——都還沒有被驗證過。

安裝(建議)

  雙擊 $LATEST_PKG,下一步到底。它會裝到正確位置並處理隔離屬性。
  裝完登出再登入,然後:系統設定 → 鍵盤 → 輸入來源 → + → 繁體中文/簡體中文 → $APP_BASE

手動安裝(進階,四步任何一步錯都是靜默失敗)

  tar xzf $LATEST_TGZ
  xattr -dr com.apple.quarantine $APP_BASE.app      # ← 不做這步，系統會靜默地不載入它
  cp -R $APP_BASE.app ~/Library/Input\\ Methods/
  然後登出再登入（第一次安裝最保險），到
  系統設定 → 鍵盤 → 輸入來源 → + → 繁體中文 → $APP_BASE

  這個版本是 ad-hoc 簽章、只有 $ARCH，不是可散布的正式版本。

移除

  rm -rf ~/Library/Input\\ Methods/$APP_BASE.app
  rm -rf ~/Library/Application\\ Support/$APP_BASE     # 使用者詞典也會一起刪

資料放在 ~/Library/Application Support/$APP_BASE，
刻意避開 Squirrel 的 ~/Library/Rime——共用使用者詞典會互相踩。

首次部署要花幾秒到幾十秒，那段時間沒有候選是正常的。

commit: $SHA
TXT
  upload "$WORK/README.txt" "$RS_R2_MACOS_DIR/README-latest.txt"
  ;;

# -------------------------------------------------------------- Windows ----
windows)
  require_data
  # Setup.exe 先抓,因為產物實際叫什麼名字是從它的檔名量出來的,
  # 而底下的 zip 內容與 README 都要用那個名字。
  fetch_artifact "$RS_CI_ARTIFACT_WINDOWS_SETUP" "$WORK/setup"
  SETUP=$(ls "$WORK"/setup/*-Setup-x64.exe)
  APP_BASE=$(basename "$SETUP"); APP_BASE=${APP_BASE%-Setup-x64.exe}
  check_app_base "$APP_BASE"
  LATEST_SETUP="$RS_R2_ARTIFACT_BASE-Setup-x64-latest.exe"

  fetch_artifact "$RS_CI_ARTIFACT_WINDOWS" "$WORK/dl"
  BIN=$(find "$WORK/dl" -name rime_tsf.dll -printf '%h\n' | head -1)
  [ -n "$BIN" ] || { echo "artifact 裡找不到 rime_tsf.dll" >&2; exit 1; }
  for f in rime_tsf.dll rime_service.exe; do
    [ -f "$BIN/$f" ] || { echo "缺 $f" >&2; exit 1; }
  done

  # 資料夾名字用 R2 上的字根:它會原封不動變成 zip 裡的頂層目錄,而使用者
  # 是照著 README 去找那個資料夾的。
  PKGDIR="$RS_R2_ARTIFACT_BASE-windows-x64"
  PKG="$WORK/$PKGDIR"
  mkdir -p "$PKG/data"
  cp "$BIN"/rime_tsf.dll "$BIN"/rime_service.exe "$PKG/"
  # rime_console.exe 在 artifact 裡是另一個目錄（console/bin），不在 ime/bin。
  # README 叫使用者「打不出字時先跑它」——它必須真的在包裡，否則那句話是空的。
  CONSOLE=$(find "$WORK/dl" -name rime_console.exe | head -1)
  [ -n "$CONSOLE" ] || { echo "artifact 裡找不到 rime_console.exe，而 README 叫使用者跑它" >&2; exit 1; }
  cp "$CONSOLE" "$PKG/"
  cp -R "$ROOT/core/data/shared" "$PKG/data/shared"
  mkdir -p "$PKG/data/user"

  # ⚠ 這兩個 .bat 有兩個非寫不可的細節,漏了會「雙擊完全沒反應」:
  #
  #   1. **行尾必須是 CRLF。** 用 Linux 的 heredoc 寫出來是 LF,而 cmd.exe 對
  #      LF-only 的批次檔在 `if errorlevel 1 (` 這種括號區塊上會直接壞掉 ——
  #      而且是安靜地壞掉,沒有錯誤訊息。
  #   2. **檔名只能用 ASCII。** Linux 的 zip 不設 UTF-8 檔名旗標,Windows 檔案
  #      總管解出來的中文檔名是亂碼,雙擊等於在點一個不存在的東西。
  #
  # 另外加了自我提權:沒有管理員權限時自己用 PowerShell 重新叫起來,
  # 而不是印一行「請以系統管理員身分執行」然後關掉 —— 使用者看不到那行字。
  # @@APP@@ 換成產物實際的名字。heredoc 維持 <<'BAT'(逐字):.bat 裡有
  # %~dp0、反斜線路徑與 errorlevel 區塊,交給 shell 去展開只會多一種出錯的
  # 方式,而 .bat 出錯的樣子是「雙擊完全沒反應」。
  crlf() { sed -e "s/@@APP@@/$APP_BASE/g" -e 's/$/\r/' > "$1"; }

  crlf "$PKG/install.bat" <<'BAT'
@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"

net session >nul 2>&1
if errorlevel 1 (
  echo 需要系統管理員權限，正在重新啟動...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)

echo.
echo   @@APP@@ 註冊中...
echo.

if not exist "%~dp0rime_tsf.dll" (
  echo   [x] 找不到 rime_tsf.dll。請確認整個資料夾都解壓出來了。
  pause
  exit /b 1
)

regsvr32 /s "%~dp0rime_tsf.dll"
if errorlevel 1 (
  echo   [x] 註冊失敗（regsvr32 回傳錯誤）。
  pause
  exit /b 1
)

echo   [v] 已註冊。
echo.
echo   接下來：設定 - 時間與語言 - 語言與地區 - 中文 - 選項 - 新增鍵盤
echo           選 @@APP@@，然後用 Win+空白鍵切換。
echo.
pause
BAT

  crlf "$PKG/uninstall.bat" <<'BAT'
@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"

net session >nul 2>&1
if errorlevel 1 (
  echo 需要系統管理員權限，正在重新啟動...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)

regsvr32 /s /u "%~dp0rime_tsf.dll"
taskkill /IM rime_service.exe /F >nul 2>&1
echo   [v] 已移除註冊。使用者詞典在 data\user，要一起刪請自行刪除。
pause
BAT

  cat > "$PKG/README.txt" <<TXT
$APP_BASE Windows x64  $STAMP  ($SHORT)

⚠ 這是第一個可安裝的版本，**還沒有任何人在真的 Windows 上用過它**。
   目前為止一顆鍵都沒有被人按過。CI 驗過的是編得起來、COM 匯出正確、
   DLL 沒有多餘相依、58 個單元測試綠、以及經由真的具名管道打出「你好」。
   在記事本裡打不打得出字、候選窗會不會出現——都還沒有被驗證過。

   DLL 沒有簽章。TSF 的 DLL 會被載入到每一個接受文字輸入的程式裡，
   請先在你不介意重開的機器上試。

安裝(建議)

  雙擊 $LATEST_SETUP,它會自己要求管理員權限。
  裝完:設定 → 時間與語言 → 語言與地區 → 中文 → 選項 → 新增鍵盤 → $APP_BASE

手動安裝(進階,用 zip 那一份)

  1. 把整個資料夾解壓到一個固定位置（例如 C:\\$APP_BASE）。
     ⚠ 之後不要搬動——註冊表記的是這個路徑。
  2. 雙擊 install.bat（它會自己要求管理員權限）。
  3. 設定 → 時間與語言 → 語言與地區 → 中文 → 選項 → 新增鍵盤 → $APP_BASE
  4. Win+空白鍵切換輸入法。

移除

  雙擊 uninstall.bat。

資料夾內容

  rime_tsf.dll      瘦 DLL。只做 TSF 協議、按鍵映射、IPC，不含 librime。
  rime_service.exe  服務進程。librime 與候選窗在這裡。DLL 會自動啟動它。
  rime_console.exe  不經 UI 直接驗核心的工具。打不出字時先跑它：
                      rime_console.exe data\\shared data\\user nihao 1 luna_pinyin_tw
                    最後一行應該是   >>> COMMIT: "你好"
                    它成功而輸入法不行 → 問題在 UI 或 IPC，不在引擎。
  data\\shared       方案與詞庫。刪掉的話就打不出字了。
  data\\user         你的使用者詞典。

已知未完成
  - 候選窗不讀主題檔（規範還有六個缺口沒定義，先做等於各端自己發明一套）
  - 沒有系統匣圖示、沒有設定介面、沒有安裝程式
  - 只有 x64，arm64 還沒做

commit: $SHA
TXT

  NAME="$RS_R2_ARTIFACT_BASE-windows-x64-$STAMP-$SHORT.zip"
  (cd "$WORK" && zip -qr "$NAME" "$PKGDIR")
  # 反向確認：包裡真的有資料
  ZLIST=$(unzip -l "$WORK/$NAME")
  grep -q 'data/shared/.*schema.yaml' <<<"$ZLIST" \
    || { echo "包裡沒有方案資料——裝得起來但一個字都打不出來" >&2; exit 1; }
  upload "$WORK/$NAME" "$RS_R2_WINDOWS_DIR/$NAME"
  upload "$WORK/$NAME" "$RS_R2_WINDOWS_DIR/$RS_R2_ARTIFACT_BASE-windows-x64-latest.zip"

  # Setup.exe 是主要下載:雙擊、自己跳 UAC、裝到 Program Files、
  # 詞典放 %APPDATA%(裝在 Program Files 底下的 data\user 會因權限寫不進去)、
  # 「新增或移除程式」裡有解除安裝項目。zip 那份留給想手動放的人。
  [ -s "$SETUP" ] || { echo "Setup.exe 是空的" >&2; exit 1; }
  # 反向確認:它是 PE 執行檔,不是誰放錯的文字檔
  head -c2 "$SETUP" | grep -q "MZ" \
    || { echo "Setup.exe 不是 PE 執行檔" >&2; exit 1; }
  SNAME="$RS_R2_ARTIFACT_BASE-Setup-x64-$STAMP-$SHORT.exe"
  cp "$SETUP" "$WORK/$SNAME"
  upload "$WORK/$SNAME" "$RS_R2_WINDOWS_DIR/$SNAME"
  upload "$WORK/$SNAME" "$RS_R2_WINDOWS_DIR/$LATEST_SETUP"
  upload "$PKG/README.txt" "$RS_R2_WINDOWS_DIR/README-latest.txt"
  ;;
*)
  echo "未知平台: $PLATFORM" >&2; exit 1 ;;
esac

echo
echo "完成。commit $SHORT"
