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
#   ./publish_apk.sh --check-only --apk X # 只跑簽章與單調性關卡,不上傳、不需要 rclone
#
# --check-only 存在的理由:
#   CI 每次 push 都該回答「這份 APK 的簽章對不對」,但**不該**每次 push 都發布
#   (使用者手上的 rime-latest.apk 被無意間覆蓋是災難)。那道簽章檢查已經寫在
#   這支腳本裡了,再寫第二份遲早會與這一份漂移 —— 兩份檢查不一致時,
#   會過的那一份說了算,於是嚴格的那一份等於不存在。所以是同一支腳本加旗標。
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APK="$ROOT/android/app/build/outputs/apk/debug/app-debug.apk"
BASE_URL="https://pub-d6a54d2e5f5947e2b0b23fb8e27ce0a5.r2.dev"
REMOTE_SUBDIR="rime"                # 只動 rime/ 底下,bucket 內其他路徑屬於別的專案
UPDATE_LATEST=1
ALLOW_DIRTY=0
ALLOW_DOWNGRADE=0
CHECK_ONLY=0
NOTES=""
NOTES_SET=0

while [ $# -gt 0 ]; do
  case "$1" in
    --apk)         APK="$2"; shift 2 ;;
    --no-latest)   UPDATE_LATEST=0; shift ;;
    --allow-dirty) ALLOW_DIRTY=1; shift ;;
    --allow-downgrade) ALLOW_DOWNGRADE=1; shift ;;
    --check-only)  CHECK_ONLY=1; shift ;;
    --notes)       NOTES="$2"; NOTES_SET=1; shift 2 ;;
    --dir)         REMOTE_SUBDIR="$2"; shift 2 ;;
    -h|--help)     sed -n '2,29p' "$0"; exit 0 ;;
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
# --check-only 不上傳,所以不需要 rclone。CI 的快車道 job 沒有 R2 憑證也該能
# 回答「簽章對不對」—— 把 rclone 當成硬性前提會逼人另寫一份寬鬆的檢查。
if [ "$CHECK_ONLY" -eq 0 ]; then
  command -v rclone >/dev/null || die "沒有 rclone。設定見 /home/lc/R2-ACCESS.md"
fi

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

# build-tools 的位置不能寫死成 `$HOME/Android/Sdk/build-tools/35.0.0`。
# 那條路徑只在那一台 Ubuntu 上成立,而「只有那一台機器發得了版」正是
# 這支腳本現在要拆掉的單點。CI runner 的 SDK 在 $ANDROID_SDK_ROOT,
# 版本也不保證是 35.0.0。找不到就中止,不猜。
find_build_tool() {
  local tool="$1" sdk d
  for sdk in "${ANDROID_SDK_ROOT:-}" "${ANDROID_HOME:-}" "$HOME/Android/Sdk"; do
    [ -n "$sdk" ] && [ -d "$sdk/build-tools" ] || continue
    for d in $(ls -1 "$sdk/build-tools" 2>/dev/null | sort -Vr); do
      if [ -x "$sdk/build-tools/$d/$tool" ]; then
        printf '%s' "$sdk/build-tools/$d/$tool"; return 0
      fi
    done
  done
  return 1
}
AAPT="${RIME_AAPT2:-$(find_build_tool aapt2 || true)}"
[ -n "$AAPT" ] && [ -x "$AAPT" ] \
  || die "找不到 aapt2(找過 \$ANDROID_SDK_ROOT、\$ANDROID_HOME、~/Android/Sdk 底下的
build-tools)。沒有它讀不出 APK 的 versionCode,也驗不了簽章 —— 拒絕繼續。"
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

# ---------------------------------------------------------- 簽章 ---
# 這一關擋的是「發出去之後所有既有使用者都裝不上來」。
#
# Android 的規矩:APK 的簽章不同就無法覆蓋安裝,使用者只能移除重裝 ——
# 自訂詞庫與鍵位設定一起消失。而這支腳本原本一道簽章檢查都沒有,卻又支援
# `--apk <任意路徑>`,於是最容易發生的事就是把 CI 建的 APK 發出去:
# CI 上完全沒有簽章設定(workflow 裡零個 secrets),build.gradle.kts 會落入
# 「找不到 signing.properties」的退路,用 runner 每次隨機新生的 debug 金鑰簽,
# 而且每跑一次就換一把。那種 APK 看起來完全正常,aapt2 讀得出 versionCode,
# 檔頭也是 PK\x03\x04 —— 前面每一道關卡都會放行。
#
# 比對方式刻意不需要金鑰密碼:apksigner 讀 lineage 與 APK 自身就夠了。
#   1. APK 內嵌的輪替鏈必須與 ~/rime-signing/signing-lineage.bin 逐一相符;
#   2. API 28+ 的有效簽章者必須是鏈上最後一把(目前的正式金鑰);
#   3. API 26–27(只認 v1/v2)的簽章者必須是鏈的根(舊 debug 金鑰),
#      否則舊機器上的使用者升不上去。
#
# 前置條件缺席時**一律中止**,不是略過。這個專案已經被「安靜跳過的檢查」
# 咬過好幾次(發布關卡的升級測試曾因順序寫反被判略過,報出一片全綠)。
SIGNING_DIR="${RIME_SIGNING_DIR:-$HOME/rime-signing}"
LINEAGE_REF="$SIGNING_DIR/signing-lineage.bin"
APKSIGNER="$(dirname "$AAPT")/apksigner"

[ -x "$APKSIGNER" ] || die "找不到 apksigner($APKSIGNER),無法驗證簽章。拒絕發布。"
[ -f "$LINEAGE_REF" ] || die "找不到輪替證明 $LINEAGE_REF,無法確認這份 APK 簽得對不對。
拒絕發布 —— 沒有這個檔就無法分辨「正確簽章」與「隨便一把 debug 金鑰」。"

# 取出一條鏈的所有 SHA-256(依 Signer #1、#2… 的順序)
lineage_chain() {
  local out
  out="$("$APKSIGNER" lineage --in "$1" --print-certs 2>/dev/null || true)"
  printf '%s\n' "$out" \
    | sed -n 's/^Signer #[0-9]* in lineage certificate SHA-256 digest: //p'
}

# 取出某個 SDK 區間下實際生效的簽章者
signer_in_range() {
  local out digest
  out="$("$APKSIGNER" verify --min-sdk-version "$2" --max-sdk-version "$3" \
          --print-certs "$1" 2>/dev/null || true)"
  digest="$(printf '%s\n' "$out" \
            | sed -n 's/^Signer #1 certificate SHA-256 digest: //p')"
  # 只取第一行。不用 head,避免 SIGPIPE 配上 pipefail 變成間歇性失敗。
  printf '%s' "${digest%%$'\n'*}"
}

REF_CHAIN="$(lineage_chain "$LINEAGE_REF")"
[ -n "$REF_CHAIN" ] || die "讀不出 $LINEAGE_REF 的憑證鏈"
EXPECT_ROOT="$(printf '%s' "$REF_CHAIN" | sed -n '1p')"
EXPECT_CURRENT="$(printf '%s\n' "$REF_CHAIN" | tail -1)"

APK_CHAIN="$(lineage_chain "$ROOT/release/$NAME")"

echo
echo "=== 簽章檢查 ==="
SIGFAIL=0

if [ "$APK_CHAIN" != "$REF_CHAIN" ]; then
  echo "  [失敗] APK 內嵌的輪替鏈與 $LINEAGE_REF 不符" >&2
  echo "    預期:" >&2; printf '%s\n' "$REF_CHAIN" | sed 's/^/      /' >&2
  echo "    實得:" >&2
  if [ -z "$APK_CHAIN" ]; then
    echo "      (沒有輪替鏈 —— 這份 APK 不是用正式金鑰加 lineage 簽的)" >&2
  else
    printf '%s\n' "$APK_CHAIN" | sed 's/^/      /' >&2
  fi
  SIGFAIL=1
else
  echo "  [OK] 輪替鏈與 $LINEAGE_REF 相符（$(printf '%s\n' "$REF_CHAIN" | wc -l | tr -d ' ') 把金鑰）"
fi

GOT_CURRENT="$(signer_in_range "$ROOT/release/$NAME" 28 35)"
if [ "$GOT_CURRENT" = "$EXPECT_CURRENT" ]; then
  echo "  [OK] API 28+ 的簽章者 = ${EXPECT_CURRENT:0:16}…（目前的正式金鑰）"
else
  echo "  [失敗] API 28+ 的簽章者是 '${GOT_CURRENT:-無}',預期 $EXPECT_CURRENT" >&2
  SIGFAIL=1
fi

GOT_ROOT="$(signer_in_range "$ROOT/release/$NAME" 26 27)"
if [ "$GOT_ROOT" = "$EXPECT_ROOT" ]; then
  echo "  [OK] API 26–27 的簽章者 = ${EXPECT_ROOT:0:16}…（鏈的根，舊機器升得上去）"
else
  echo "  [失敗] API 26–27 的簽章者是 '${GOT_ROOT:-無}',預期 $EXPECT_ROOT" >&2
  SIGFAIL=1
fi

if [ "$SIGFAIL" -ne 0 ]; then
  echo >&2
  echo "這份 APK 的簽章與正式金鑰不符。發出去的話,**所有既有使用者都會裝不上來**" >&2
  echo "(INSTALL_FAILED_UPDATE_INCOMPATIBLE),只能移除重裝、失去自訂詞庫。" >&2
  echo >&2
  echo "最可能的原因:這份 APK 是在沒有 $SIGNING_DIR 的機器上建的(例如 CI runner)," >&2
  echo "build.gradle.kts 會退回 Android 預設的 debug 金鑰並只印一行警告。" >&2
  echo "請在有簽章金鑰的機器上重建再發。" >&2
  exit 1
fi

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

# --check-only 到這裡就結束:簽章鏈、API 28+ 與 26–27 的簽章者、
# 以及 versionCode 單調性都已經查過了,而這四項就是「發出去會不會害既有
# 使用者裝不上來」的全部。剩下的是上傳,那需要 R2 憑證,而且必須是人主動要的。
if [ "$CHECK_ONLY" -eq 1 ]; then
  echo
  echo "=== --check-only:關卡通過,沒有上傳任何東西 ==="
  echo "  這份 APK 的簽章與正式金鑰同鏈,versionCode $VERSION_CODE 可以覆蓋線上版本。"
  echo "  要真的發布請手動觸發 publish(不帶 --check-only)。"
  exit 0
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
# xxd 來自 vim-common,不是每台機器都有(CI runner 就不保證)。
# 缺了它時 MAGIC 會是空字串,而空字串 != 504b0304 → 這一關會報一個
# 看起來像「R2 傳回壞檔」的假失敗。所以退回 od(coreutils,一定有)。
if command -v xxd >/dev/null 2>&1; then
  MAGIC="$(curl -sS -r 0-3 "$BASE_URL/$REMOTE_SUBDIR/$NAME" | xxd -p 2>/dev/null || true)"
else
  MAGIC="$(curl -sS -r 0-3 "$BASE_URL/$REMOTE_SUBDIR/$NAME" | od -An -tx1 -v 2>/dev/null | tr -d ' \n' || true)"
fi
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
