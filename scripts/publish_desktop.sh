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
PLATFORM="${1:?用法: publish_desktop.sh <macos|windows> <run_id>}"
RUN_ID="${2:?缺 run_id}"

REPO=IncredibleGG/rime-quad
TOK=$(tr -d " \t\r\n" < "$HOME/.github-token")
BUCKET=r2:tgapk/rime
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
  fetch_artifact rime-macos-app "$WORK/dl"
  TGZ=$(ls "$WORK"/dl/RimeQuad-app-*.tar.gz)
  ARCH=$(basename "$TGZ" .tar.gz); ARCH=${ARCH##*-}
  # 反向確認：包裡真的有 .app 而且有可執行檔，不是空殼
  # 注意：不可寫成 `tar tzf ... | grep -q`。set -o pipefail 之下，grep 命中即結束
  # 會讓 tar 收到 SIGPIPE，整條 pipeline 判失敗——**命中反而變成失敗**。
  LIST=$(tar tzf "$TGZ")
  grep -q 'RimeQuad.app/Contents/MacOS/RimeQuad' <<<"$LIST" \
    || { echo "包裡沒有 RimeQuad.app/Contents/MacOS/RimeQuad" >&2; exit 1; }
  grep -q 'Contents/Resources/SharedSupport/.*schema.yaml' <<<"$LIST" \
    || { echo "包裡沒有方案資料——裝得起來但一個字都打不出來" >&2; exit 1; }

  NAME="RimeQuad-macos-$ARCH-$STAMP-$SHORT.tar.gz"
  cp "$TGZ" "$WORK/$NAME"
  upload "$WORK/$NAME" "macos/$NAME"
  upload "$WORK/$NAME" "macos/RimeQuad-latest.tar.gz"

  # .pkg 是主要下載。手動裝那條路徑(解壓→xattr→搬到隱藏目錄→登出)四步
  # 任何一步錯都是**靜默失敗** —— 使用者第一次就把 .app 放進了 /Applications,
  # 系統照樣登錄它、行程也起得來,但永遠不會出現在輸入來源清單裡。
  fetch_artifact rime-macos-installer "$WORK/pkg"
  PKG=$(ls "$WORK"/pkg/RimeQuad-*.pkg)
  # 這裡只驗「它是不是一個真的 .pkg」—— Ubuntu 上沒有 xar / pkgutil,拆不開它。
  # **內容由 CI 驗**:apple/scripts/verify_pkg.sh 在 macOS runner 上真的裝一次,
  # 斷言檔案落在 ~/Library/Input Methods,並斷言 PackageInfo 的 install-location。
  # 那一關才擋得住「少了 enable_currentUserHome → 裝到磁碟根目錄,而 Installer
  # 仍然報成功」這種靜默失敗。這裡再驗一次只是防手滑傳錯檔案。
  head -c4 "$PKG" | grep -q "xar!" \
    || { echo "$PKG 不是 xar 封存(.pkg 應該是)" >&2; exit 1; }
  [ "$(stat -c%s "$PKG")" -gt 3000000 ] \
    || { echo ".pkg 只有 $(stat -c%s "$PKG") bytes —— 不可能帶著方案資料" >&2; exit 1; }
  PNAME="RimeQuad-macos-$ARCH-$STAMP-$SHORT.pkg"
  cp "$PKG" "$WORK/$PNAME"
  upload "$WORK/$PNAME" "macos/$PNAME"
  upload "$WORK/$PNAME" "macos/RimeQuad-latest.pkg"

  cat > "$WORK/README.txt" <<TXT
RimeQuad macOS  $STAMP  ($SHORT, $ARCH)

⚠ 這是第一個可安裝的版本，還沒有任何人在真的 Mac 上用過它。
   CI 驗過的是「編得起來、結構正確、105 項單元測試綠、核心層打得出你好」。
   候選窗會不會出現、在你的 app 裡打不打得出字——都還沒有被驗證過。

安裝(建議)

  雙擊 RimeQuad-latest.pkg,下一步到底。它會裝到正確位置並處理隔離屬性。
  裝完登出再登入,然後:系統設定 → 鍵盤 → 輸入來源 → + → 繁體中文/簡體中文 → RimeQuad

手動安裝(進階,四步任何一步錯都是靜默失敗)

  tar xzf RimeQuad-latest.tar.gz
  xattr -dr com.apple.quarantine RimeQuad.app      # ← 不做這步，系統會靜默地不載入它
  cp -R RimeQuad.app ~/Library/Input\\ Methods/
  然後登出再登入（第一次安裝最保險），到
  系統設定 → 鍵盤 → 輸入來源 → + → 繁體中文 → RimeQuad

  這個版本是 ad-hoc 簽章、只有 $ARCH，不是可散布的正式版本。

移除

  rm -rf ~/Library/Input\\ Methods/RimeQuad.app
  rm -rf ~/Library/Application\\ Support/RimeQuad     # 使用者詞典也會一起刪

資料放在 ~/Library/Application Support/RimeQuad，
刻意避開 Squirrel 的 ~/Library/Rime——共用使用者詞典會互相踩。

首次部署要花幾秒到幾十秒，那段時間沒有候選是正常的。

commit: $SHA
TXT
  upload "$WORK/README.txt" "macos/README-latest.txt"
  ;;

# -------------------------------------------------------------- Windows ----
windows)
  require_data
  fetch_artifact "rime-windows-x64" "$WORK/dl"
  BIN=$(find "$WORK/dl" -name rime_tsf.dll -printf '%h\n' | head -1)
  [ -n "$BIN" ] || { echo "artifact 裡找不到 rime_tsf.dll" >&2; exit 1; }
  for f in rime_tsf.dll rime_service.exe; do
    [ -f "$BIN/$f" ] || { echo "缺 $f" >&2; exit 1; }
  done

  PKG="$WORK/RimeQuad-windows-x64"
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
  crlf() { sed -e 's/$/\r/' > "$1"; }

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
echo   RimeQuad 註冊中...
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
echo           選 RimeQuad，然後用 Win+空白鍵切換。
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
RimeQuad Windows x64  $STAMP  ($SHORT)

⚠ 這是第一個可安裝的版本，**還沒有任何人在真的 Windows 上用過它**。
   目前為止一顆鍵都沒有被人按過。CI 驗過的是編得起來、COM 匯出正確、
   DLL 沒有多餘相依、58 個單元測試綠、以及經由真的具名管道打出「你好」。
   在記事本裡打不打得出字、候選窗會不會出現——都還沒有被驗證過。

   DLL 沒有簽章。TSF 的 DLL 會被載入到每一個接受文字輸入的程式裡，
   請先在你不介意重開的機器上試。

安裝(建議)

  雙擊 RimeQuad-Setup-x64-latest.exe,它會自己要求管理員權限。
  裝完:設定 → 時間與語言 → 語言與地區 → 中文 → 選項 → 新增鍵盤 → RimeQuad

手動安裝(進階,用 zip 那一份)

  1. 把整個資料夾解壓到一個固定位置（例如 C:\\RimeQuad）。
     ⚠ 之後不要搬動——註冊表記的是這個路徑。
  2. 雙擊 install.bat（它會自己要求管理員權限）。
  3. 設定 → 時間與語言 → 語言與地區 → 中文 → 選項 → 新增鍵盤 → RimeQuad
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

  NAME="RimeQuad-windows-x64-$STAMP-$SHORT.zip"
  (cd "$WORK" && zip -qr "$NAME" RimeQuad-windows-x64)
  # 反向確認：包裡真的有資料
  ZLIST=$(unzip -l "$WORK/$NAME")
  grep -q 'data/shared/.*schema.yaml' <<<"$ZLIST" \
    || { echo "包裡沒有方案資料——裝得起來但一個字都打不出來" >&2; exit 1; }
  upload "$WORK/$NAME" "windows/$NAME"
  upload "$WORK/$NAME" "windows/RimeQuad-windows-x64-latest.zip"

  # Setup.exe 是主要下載:雙擊、自己跳 UAC、裝到 Program Files、
  # 詞典放 %APPDATA%(裝在 Program Files 底下的 data\user 會因權限寫不進去)、
  # 「新增或移除程式」裡有解除安裝項目。zip 那份留給想手動放的人。
  fetch_artifact RimeQuad-Setup-x64 "$WORK/setup"
  SETUP=$(ls "$WORK"/setup/RimeQuad-Setup-x64.exe)
  [ -s "$SETUP" ] || { echo "Setup.exe 是空的" >&2; exit 1; }
  # 反向確認:它是 PE 執行檔,不是誰放錯的文字檔
  head -c2 "$SETUP" | grep -q "MZ" \
    || { echo "Setup.exe 不是 PE 執行檔" >&2; exit 1; }
  SNAME="RimeQuad-Setup-x64-$STAMP-$SHORT.exe"
  cp "$SETUP" "$WORK/$SNAME"
  upload "$WORK/$SNAME" "windows/$SNAME"
  upload "$WORK/$SNAME" "windows/RimeQuad-Setup-x64-latest.exe"
  upload "$PKG/README.txt" "windows/README-latest.txt"
  ;;
*)
  echo "未知平台: $PLATFORM" >&2; exit 1 ;;
esac

echo
echo "完成。commit $SHORT"
