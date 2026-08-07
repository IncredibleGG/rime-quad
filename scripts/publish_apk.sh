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
#   ./publish_apk.sh --notes "更新說明"   # 寫進 version.json 的 notes
#   ./publish_apk.sh --dir rime/test      # 發到測試路徑(驗證升級流程用)
#   ./publish_apk.sh --allow-downgrade    # 明知 versionCode 較低仍要發
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APK="$ROOT/android/app/build/outputs/apk/debug/app-debug.apk"
BASE_URL="https://pub-d6a54d2e5f5947e2b0b23fb8e27ce0a5.r2.dev"
REMOTE_SUBDIR="rime"                # 只動 rime/ 底下,bucket 內其他路徑屬於別的專案
UPDATE_LATEST=1
ALLOW_DIRTY=0
ALLOW_DOWNGRADE=0
NOTES=""
NOTES_SET=0

while [ $# -gt 0 ]; do
  case "$1" in
    --apk)         APK="$2"; shift 2 ;;
    --no-latest)   UPDATE_LATEST=0; shift ;;
    --allow-dirty) ALLOW_DIRTY=1; shift ;;
    --allow-downgrade) ALLOW_DOWNGRADE=1; shift ;;
    --notes)       NOTES="$2"; NOTES_SET=1; shift 2 ;;
    --dir)         REMOTE_SUBDIR="$2"; shift 2 ;;
    -h|--help)     sed -n '2,22p' "$0"; exit 0 ;;
    *) echo "未知參數: $1" >&2; exit 2 ;;
  esac
done

die() { echo "錯誤: $*" >&2; exit 1; }

# bucket 內其他路徑屬於別的專案。打錯一個字就可能覆蓋掉別人的發布,
# 所以這裡是硬性的護欄而不是註解。
case "$REMOTE_SUBDIR" in
  rime|rime/*) ;;
  *) die "--dir 只接受 rime 或 rime/ 底下的路徑,收到:$REMOTE_SUBDIR" ;;
esac
REMOTE_DIR="r2:tgapk/$REMOTE_SUBDIR"

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
if [ -n "$DIRTY" ]; then
  echo "[警告] 工作區未提交,版本名標記 -dirty(你指定了 --allow-dirty)"
fi

mkdir -p "$ROOT/release"
cp "$APK" "$ROOT/release/$NAME"
SIZE="$(stat -c%s "$ROOT/release/$NAME")"
SHA256="$(sha256sum "$ROOT/release/$NAME" | cut -d' ' -f1)"

# ---------------------------------------------------------- 版本號 ---
# versionCode / versionName **從 APK 本身讀**,不從 build.gradle.kts 猜。
# 那份檔案裡的推導邏輯改了、或有人用 -Prime.versionCode 覆寫了,唯一可信的
# 來源就只剩 APK 自己。version.json 說的必須是使用者裝下去真正會拿到的東西。
AAPT="$HOME/Android/Sdk/build-tools/35.0.0/aapt2"
[ -x "$AAPT" ] || die "找不到 aapt2($AAPT),無法讀出 APK 的 versionCode"
BADGING="$("$AAPT" dump badging "$ROOT/release/$NAME" 2>/dev/null)" \
  || die "aapt2 讀不了這個 APK,多半是壞檔"
VERSION_CODE="$(printf '%s' "$BADGING" | sed -n "s/.*versionCode='\([0-9]*\)'.*/\1/p" | head -1)"
VERSION_NAME="$(printf '%s' "$BADGING" | sed -n "s/.*versionName='\([^']*\)'.*/\1/p" | head -1)"
[ -n "$VERSION_CODE" ] || die "APK 裡讀不到 versionCode"
[ -n "$VERSION_NAME" ] || die "APK 裡讀不到 versionName"

# notes 沒指定就取 commit 的標題行。留空字串也可以(--notes "")。
if [ "$NOTES_SET" -eq 0 ]; then
  NOTES="$(git log -1 --format=%s 2>/dev/null || true)"
fi

echo "=== 待發布 ==="
echo "  檔名        : $NAME"
echo "  大小        : $SIZE bytes ($((SIZE / 1024 / 1024)) MB)"
echo "  sha256      : $SHA256"
echo "  versionCode : $VERSION_CODE"
echo "  versionName : $VERSION_NAME"
echo "  notes       : $NOTES"
echo "  目的地      : $REMOTE_DIR"
printf '%s' "$BADGING" | grep -E "^package:|^native-code:" | sed 's/^/  /'

# ------------------------------------------------------ 單調性檢查 ---
# versionCode 必須單調遞增,否則 Android 自己的升級語意與 app 內的更新
# 判斷同時失效 —— 而且使用者按下「更新」會裝不上去(系統不允許降級)。
# 已經發布出去的那個號碼是唯一的基準,所以去線上讀回來比。
PUBLISHED_JSON="$(curl -sS --max-time 20 "$BASE_URL/$REMOTE_SUBDIR/version.json" 2>/dev/null || true)"
PUBLISHED_CODE="$(printf '%s' "$PUBLISHED_JSON" \
  | sed -n 's/.*"version_code"[[:space:]]*:[[:space:]]*\([0-9]*\).*/\1/p' | head -1)"
if [ -n "$PUBLISHED_CODE" ]; then
  echo "  線上版本    : versionCode $PUBLISHED_CODE"
  if [ "$VERSION_CODE" -le "$PUBLISHED_CODE" ]; then
    if [ "$ALLOW_DOWNGRADE" -eq 0 ]; then
      echo >&2
      echo "要發布的 versionCode ($VERSION_CODE) 不大於線上的 ($PUBLISHED_CODE)。" >&2
      echo "使用者的 app 不會認為這是新版本;就算認了,系統也會拒絕降級安裝。" >&2
      echo "versionCode 由 HEAD commit 的時間推導,先提交一個新 commit 再建置," >&2
      echo "或用 -Prime.versionCode=<n> 明確指定。確定要照發再加 --allow-downgrade。" >&2
      exit 1
    fi
    echo "[警告] versionCode 沒有遞增,你指定了 --allow-downgrade"
  fi
else
  echo "  線上版本    : 讀不到(第一次發布,或線上還是舊格式的 version.json)"
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
if [ "$UPDATE_LATEST" -eq 1 ]; then
  upload "$ROOT/release/$NAME" "$REMOTE_DIR/rime-latest.apk"
fi

# version.json:app 內檢查更新讀的就是這一份。
#
# version_code 是**唯一**用來判斷新舊的欄位。version_name 只給人看 ——
# 拿字串比大小遲早比出 "0.10.0" < "0.9.0"。
#
# notes 用 python 轉義,不然說明裡有一個雙引號或反斜線就會產生壞掉的 JSON,
# 而那份 JSON 會被幾百台裝置抓下去。
NOTES_JSON="$(NOTES="$NOTES" python3 -c 'import json,os;print(json.dumps(os.environ["NOTES"]))')"
cat > "$ROOT/release/version.json" <<JSON
{
  "version_code": $VERSION_CODE,
  "version_name": "$VERSION_NAME",
  "build_stamp": "$STAMP-$SHA",
  "commit": "$SHA",
  "file": "$NAME",
  "size": $SIZE,
  "sha256": "$SHA256",
  "url": "$BASE_URL/$REMOTE_SUBDIR/$NAME",
  "latest_url": "$BASE_URL/$REMOTE_SUBDIR/rime-latest.apk",
  "notes": $NOTES_JSON
}
JSON
python3 -c 'import json,sys;json.load(open(sys.argv[1]))' "$ROOT/release/version.json" \
  || die "產生出來的 version.json 不是合法 JSON,已中止(沒有上傳)"
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
verify "$BASE_URL/$REMOTE_SUBDIR/$NAME" "$SIZE" || FAIL=1
if [ "$UPDATE_LATEST" -eq 1 ]; then
  verify "$BASE_URL/$REMOTE_SUBDIR/rime-latest.apk" "$SIZE" || FAIL=1
fi
verify "$BASE_URL/$REMOTE_SUBDIR/version.json" "" || FAIL=1

# 確認拿到的真的是 APK 而不是錯誤頁面
MAGIC="$(curl -sS -r 0-3 "$BASE_URL/$REMOTE_SUBDIR/$NAME" | xxd -p 2>/dev/null || true)"
if [ "$MAGIC" = "504b0304" ]; then
  echo "  [OK] 檔頭是 PK\\x03\\x04,確實是 zip/APK"
else
  echo "  [失敗] 檔頭是 '$MAGIC',不是合法的 APK"; FAIL=1
fi


# 對外拿回來的 version.json 必須真的含 version_code —— app 沒有它就
# 判斷不了新舊,而那個失敗會安靜到沒有人發現。
ONLINE_CODE="$(curl -sS --max-time 20 "$BASE_URL/$REMOTE_SUBDIR/version.json" \
  | sed -n 's/.*"version_code"[[:space:]]*:[[:space:]]*\([0-9]*\).*/\1/p' | head -1)"
if [ "$ONLINE_CODE" = "$VERSION_CODE" ]; then
  echo "  [OK] 線上 version.json 的 version_code = $ONLINE_CODE"
else
  echo "  [失敗] 線上 version.json 的 version_code 是 '$ONLINE_CODE',預期 $VERSION_CODE"
  FAIL=1
fi

[ "$FAIL" -eq 0 ] || die "發布驗證未通過,請勿把網址交給使用者"

echo
echo "=== 發布完成 ==="
echo "  固定版本   : $BASE_URL/$REMOTE_SUBDIR/$NAME"
if [ "$UPDATE_LATEST" -eq 1 ]; then
  echo "  最新版本   : $BASE_URL/$REMOTE_SUBDIR/rime-latest.apk"
fi
echo "  版本資訊   : $BASE_URL/$REMOTE_SUBDIR/version.json"
echo "  versionCode: $VERSION_CODE"
echo "  sha256     : $SHA256"
