#!/usr/bin/env bash
#
# publish_apk.sh — 把 APK 發布到 R2
#
# 為什麼要有這支腳本而不是手打 rclone:
#   - 版本命名要一致(日期 + git sha),否則回溯不到是哪份程式碼建的
#   - 上傳後必須**實際驗證對外網址取得到**。rclone 說成功不代表拿得到 ——
#     我們實測過 R2 第一次嘗試會回 501 NotImplemented、第二次才成功,
#     只看 rclone 的結束碼會漏掉這類問題。
#   - rime-latest.apk 會被覆蓋。帶版號的那份不會,那是出問題時的退路。
#
# 用法:
#   ./publish_apk.sh                      # 用預設的 debug APK
#   ./publish_apk.sh --apk <路徑>
#   ./publish_apk.sh --no-latest          # 只發帶版號的,不動 latest
#   ./publish_apk.sh --allow-dirty        # 工作區未提交也照發(不建議)
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APK="$ROOT/android/app/build/outputs/apk/debug/app-debug.apk"
BASE_URL="https://pub-d6a54d2e5f5947e2b0b23fb8e27ce0a5.r2.dev"
REMOTE_DIR="r2:tgapk/rime"          # 只動 rime/ 底下,bucket 內其他路徑屬於別的專案
UPDATE_LATEST=1
ALLOW_DIRTY=0

while [ $# -gt 0 ]; do
  case "$1" in
    --apk)         APK="$2"; shift 2 ;;
    --no-latest)   UPDATE_LATEST=0; shift ;;
    --allow-dirty) ALLOW_DIRTY=1; shift ;;
    -h|--help)     sed -n '2,17p' "$0"; exit 0 ;;
    *) echo "未知參數: $1" >&2; exit 2 ;;
  esac
done

die() { echo "錯誤: $*" >&2; exit 1; }

[ -f "$APK" ] || die "找不到 APK: $APK"
command -v rclone >/dev/null || die "沒有 rclone。設定見 /home/lc/R2-ACCESS.md"

cd "$ROOT"
SHA="$(git rev-parse --short HEAD)"
DIRTY=""
git diff --quiet && git diff --cached --quiet || DIRTY="-dirty"
STAMP="$(date +%Y%m%d-%H%M)"
NAME="rime-android-debug-${STAMP}-${SHA}${DIRTY}.apk"

# 工作區髒代表這份 APK 對應不到任何一個 commit,回溯不了、也多半是別人建到
# 一半的產物。實際發生過:誤發了一份 agent 正在改的中途建置。預設擋下。
if [ -n "$DIRTY" ] && [ "$ALLOW_DIRTY" -eq 0 ]; then
  echo "工作區有未提交的變更,拒絕發布。" >&2
  git status --short | head -20 >&2
  echo >&2
  echo "這份 APK 對應不到任何 commit,而且很可能是別人正在改到一半的建置。" >&2
  echo "先提交,或確定要發再加 --allow-dirty。" >&2
  exit 1
fi
[ -n "$DIRTY" ] && echo "[警告] 工作區未提交,版本名標記 -dirty(你指定了 --allow-dirty)"

mkdir -p "$ROOT/release"
cp "$APK" "$ROOT/release/$NAME"
SIZE="$(stat -c%s "$ROOT/release/$NAME")"
SHA256="$(sha256sum "$ROOT/release/$NAME" | cut -d' ' -f1)"

echo "=== 待發布 ==="
echo "  檔名   : $NAME"
echo "  大小   : $SIZE bytes ($((SIZE / 1024 / 1024)) MB)"
echo "  sha256 : $SHA256"

# APK 基本資訊,順便確認這不是一個壞檔
AAPT="$HOME/Android/Sdk/build-tools/35.0.0/aapt2"
if [ -x "$AAPT" ]; then
  "$AAPT" dump badging "$ROOT/release/$NAME" 2>/dev/null \
    | grep -E "^package:|^native-code:" | sed 's/^/  /'
fi

# ---------------------------------------------------------------- 上傳 ---
upload() {
  local src="$1" dst="$2"
  echo "上傳 → $dst"
  # R2 偶爾第一次回 501 NotImplemented,rclone 會自行重試;這裡不因此中止。
  rclone copyto "$src" "$dst" --s3-no-check-bucket --stats-one-line 2>&1 \
    | grep -vE "^\s*$" | sed 's/^/    /' || true
}

upload "$ROOT/release/$NAME" "$REMOTE_DIR/$NAME"
[ "$UPDATE_LATEST" -eq 1 ] && upload "$ROOT/release/$NAME" "$REMOTE_DIR/rime-latest.apk"

# version.json:供日後 app 內檢查更新用
cat > "$ROOT/release/version.json" <<JSON
{
  "version_name": "$STAMP-$SHA",
  "commit": "$SHA",
  "file": "$NAME",
  "size": $SIZE,
  "sha256": "$SHA256",
  "url": "$BASE_URL/rime/$NAME",
  "latest_url": "$BASE_URL/rime/rime-latest.apk"
}
JSON
upload "$ROOT/release/version.json" "$REMOTE_DIR/version.json"

# ---------------------------------------------------------------- 驗證 ---
# 這一段才是重點:rclone 說成功不等於對外拿得到。
echo
echo "=== 驗證對外網址 ==="
verify() {
  local url="$1" want_size="$2"
  local head got_size code
  head="$(curl -sSI "$url" 2>&1)" || { echo "  [失敗] $url 取不到"; return 1; }
  code="$(printf '%s' "$head" | head -1 | awk '{print $2}')"
  got_size="$(printf '%s' "$head" | grep -i '^content-length:' | tr -d '\r' | awk '{print $2}')"
  [ "$code" = "200" ] || { echo "  [失敗] $url HTTP $code"; return 1; }
  if [ -n "$want_size" ] && [ "$got_size" != "$want_size" ]; then
    echo "  [失敗] $url 大小 $got_size 不等於 $want_size"; return 1
  fi
  echo "  [OK] $url  ($got_size bytes)"
}

FAIL=0
verify "$BASE_URL/rime/$NAME" "$SIZE" || FAIL=1
[ "$UPDATE_LATEST" -eq 1 ] && { verify "$BASE_URL/rime/rime-latest.apk" "$SIZE" || FAIL=1; }
verify "$BASE_URL/rime/version.json" "" || FAIL=1

# 確認拿到的真的是 APK 而不是錯誤頁面
MAGIC="$(curl -sS -r 0-3 "$BASE_URL/rime/$NAME" | xxd -p 2>/dev/null || true)"
if [ "$MAGIC" = "504b0304" ]; then
  echo "  [OK] 檔頭是 PK\\x03\\x04,確實是 zip/APK"
else
  echo "  [失敗] 檔頭是 '$MAGIC',不是合法的 APK"; FAIL=1
fi

[ "$FAIL" -eq 0 ] || die "發布驗證未通過,請勿把網址交給使用者"

echo
echo "=== 發布完成 ==="
echo "  固定版本: $BASE_URL/rime/$NAME"
[ "$UPDATE_LATEST" -eq 1 ] && echo "  最新版本: $BASE_URL/rime/rime-latest.apk"
echo "  sha256  : $SHA256"
