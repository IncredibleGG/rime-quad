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
#   ./publish_apk.sh                      # 用預設的 **release** APK
#   ./publish_apk.sh --apk <路徑>
#   ./publish_apk.sh --no-latest          # 只發帶版號的,不動 latest
#   ./publish_apk.sh --allow-dirty        # 工作區未提交也照發(不建議)
#   ./publish_apk.sh --notes "更新說明"   # 寫進 version.json 的 notes
#   ./publish_apk.sh --dir rime/test      # 發到測試路徑(驗證升級流程用)
#   ./publish_apk.sh --allow-downgrade    # 明知 versionCode 較低仍要發
#   ./publish_apk.sh --check-only --apk X # 只跑簽章與單調性關卡,不上傳、不需要 rclone
#   ./publish_apk.sh --page-url <網址>    # 給人看的下載頁,寫進 version.json 的 page_url
#   ./publish_apk.sh --self-check         # 逐條反向測試本檔的關卡,不連網、不需要 APK
#
# --check-only 存在的理由:
#   CI 每次 push 都該回答「這份 APK 的簽章對不對」,但**不該**每次 push 都發布
#   (使用者手上的 rime-latest.apk 被無意間覆蓋是災難)。那道簽章檢查已經寫在
#   這支腳本裡了,再寫第二份遲早會與這一份漂移 —— 兩份檢查不一致時,
#   會過的那一份說了算,於是嚴格的那一份等於不存在。所以是同一支腳本加旗標。
#
# --self-check 存在的理由:
#   本檔的關卡擋的是「發出去之後使用者裝不上來」,而那種事一年撞不到幾次 ——
#   也就是說,關卡本身壞掉的話**不會有人發現**。所以每一條關卡都寫成不碰
#   網路、不碰 APK 的純函式,由 --self-check 餵假清單逐條要求它變紅。
#   反向測試與被測的關卡在同一個檔案裡,是刻意的:抄成第二份就會漂移。
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# ── 為什麼預設是 release 而不是 debug(2026-08-10 改)────────────────────────
# 在此之前這一行指著 app-debug.apk,而 debug 建置的 android:debuggable 是 true。
# 使用者手上因此躺著一份「任何拿得到 adb 或解得開鎖的人都能 `run-as` 讀走
# 詞庫與輸入歷史、還能對輸入法進程掛除錯器」的 APK —— 而輸入法看得到他打的
# 每一個字。這不是效能或體積的取捨,是產品定位(離線為預設、經得起審計)
# 直接被推翻。
#
# 只改這一行不夠:`--apk` 仍然可以指到任何檔案,而 CI 也是用 `--apk` 傳進來的。
# 所以下面另有一道**針對 APK 本身**的關卡(check_debuggable),不管 APK 從哪來,
# debuggable 的一律拒發。
APK="$ROOT/android/app/build/outputs/apk/release/app-release.apk"
BASE_URL="https://pub-d6a54d2e5f5947e2b0b23fb8e27ce0a5.r2.dev"
REMOTE_SUBDIR="rime"                # 只動 rime/ 底下,bucket 內其他路徑屬於別的專案
UPDATE_LATEST=1
ALLOW_DIRTY=0
ALLOW_DOWNGRADE=0
CHECK_ONLY=0
SELF_CHECK=0
NOTES=""
NOTES_SET=0
PAGE_URL="${RIME_PAGE_URL:-}"

while [ $# -gt 0 ]; do
  case "$1" in
    --apk)         APK="$2"; shift 2 ;;
    --no-latest)   UPDATE_LATEST=0; shift ;;
    --allow-dirty) ALLOW_DIRTY=1; shift ;;
    --allow-downgrade) ALLOW_DOWNGRADE=1; shift ;;
    --check-only)  CHECK_ONLY=1; shift ;;
    --self-check)  SELF_CHECK=1; shift ;;
    --notes)       NOTES="$2"; NOTES_SET=1; shift 2 ;;
    --page-url)    PAGE_URL="$2"; shift 2 ;;
    --dir)         REMOTE_SUBDIR="$2"; shift 2 ;;
    -h|--help)     sed -n '2,35p' "$0"; exit 0 ;;
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

# ── product.env ───────────────────────────────────────────────────────────
#
# ⚠ 這一行以前不存在,而下面 version.json 那一段讀 $RS_ANDROID_APP_ID_PREVIOUS
#   來決定要不要寫 replaces_package。沒有人 source 過 product.sh,CI 的
#   workflow 也沒有 export 它 —— 所以那個變數**永遠是空的**,那段程式碼
#   從寫下來的第一天起就是死的,`replaces_package` 一次都沒有被寫出去過。
#   (實測:`env | grep -c '^RS_'` = 0;.github/workflows/build.yml 裡沒有
#   任何 RS_ 的字樣。)這正是「改名那一版沒有 replaces_package」的另一半原因。
. "$ROOT/scripts/lib/product.sh" \
  || die "讀不到 scripts/lib/product.env —— 套件識別碼的唯一來源不見了,拒絕發布"

# ══════════════════════════════════════════════════════════════════════════
#  發布前關卡的純函式
#
#  這一段裡的每一個函式都**不碰網路、不碰 APK、不碰檔案系統**:輸入全部
#  從參數進來,輸出是 stdout 與結束碼。理由只有一個 —— 它們要能被
#  `--self-check` 餵假資料逐條要求變紅。做不到反向測試的關卡,在它自己
#  壞掉的那天會安靜地全綠(這個專案已經被同一件事咬過好幾次)。
# ══════════════════════════════════════════════════════════════════════════

# ── 從一份 version.json 的原文裡取一個**頂層**字串欄位 ────────────────────
#
# ⚠ 刻意用 python 的 json,不用 sed/grep。`notes` 是自由文字,裡面出現
#   `"package": "org.別人"` 完全合法,而 sed 的 `.*"package"` 會抓到它 ——
#   本檔既有的 version_code 抽取就是那個寫法。version_code 是數字、風險小;
#   package 這一關的結論是「要不要擋下發布」,被 notes 裡的一串字騙到的
#   代價是放行一次所有舊使用者都裝不上來的發布。--self-check 有一條專門
#   釘這件事(第 8 條)。
#
# 結束碼:0=取到字串 3=不是合法 JSON 物件 4=沒有這個欄位 5=有但型別不是字串
manifest_str_field() {
  MF_JSON="$1" MF_KEY="$2" python3 -c '
import json, os, sys
try:
    obj = json.loads(os.environ["MF_JSON"])
except Exception:
    sys.exit(3)
if not isinstance(obj, dict):
    sys.exit(3)
key = os.environ["MF_KEY"]
if key not in obj:
    sys.exit(4)
v = obj[key]
if not isinstance(v, str):
    sys.exit(5)
sys.stdout.write(v)
'
}

# 同上,但取整數欄位(version_code)。結束碼的意義一樣。
#
# ⚠ 這裡原本是 `sed -n 's/.*"version_code"…'`。同一個 notes 陷阱:發布說明裡
#   寫一句「修好 "version_code": 99999999 的顯示」,單調性關卡就會拿 99999999
#   當基準,把一次完全正常的發布擋成「降版」,而訊息會叫人去改 versionCode ——
#   完全指錯方向。--self-check 的 B2 釘住這件事。
manifest_num_field() {
  MF_JSON="$1" MF_KEY="$2" python3 -c '
import json, os, sys
try:
    obj = json.loads(os.environ["MF_JSON"])
except Exception:
    sys.exit(3)
if not isinstance(obj, dict):
    sys.exit(3)
key = os.environ["MF_KEY"]
if key not in obj:
    sys.exit(4)
v = obj[key]
if isinstance(v, bool) or not isinstance(v, int):
    sys.exit(5)
sys.stdout.write(str(v))
'
}

# ── 「這串字看起來像不像套件名」──────────────────────────────────────────
#
# 規則逐字照抄 app 端的 PackageIdentity.looksLikePackageName
# (android/…/update/PackageIdentity.kt:71):非空、長度 <= 255、
# 且完全符合 [A-Za-z][A-Za-z0-9_]*(\.[A-Za-z0-9_]+)+。
#
# 兩邊必須用同一套規則。發布端覺得「這是套件名、可以比對」而 app 端覺得
# 「這不像套件名、當成沒有」的話,發布端的結論就是空的 —— 它擋下或放行的
# 依據,使用者手上的 app 根本不會採用。
looks_like_package() {
  LLP="${1:-}" python3 -c '
import os, re, sys
s = os.environ["LLP"]
ok = bool(s.strip()) and len(s) <= 255 and \
     re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z0-9_]+)+", s)
sys.exit(0 if ok else 1)
'
}

# ── 關卡:這份 APK 是不是 debuggable ──────────────────────────────────────
#
# 擋的是 2026-08-10 之前一直在發生的事:發出去的是 debug 建置。
# debuggable=true 的後果不是「開發者方便」,是任何拿得到 adb(或把裝置解鎖)
# 的人都能 `run-as <本 app 的套件名>` 把使用者的詞庫與輸入歷史整包讀走,
# 並對輸入法進程掛除錯器 —— 而輸入法看得到使用者打的每一個字。
#
# 為什麼不是「把預設路徑改掉就好」:`--apk` 可以指到任何檔案,CI 也是用它
# 傳進來的。路徑是慣例,慣例攔不住下一次。這一關看的是 **APK 自己說了什麼**。
#
# 兩個獨立的判讀來源,是刻意的:
#   · badging 的 `application-debuggable` 是 aapt2 幫忙歸納的一行;
#   · xmltree 讀的是 manifest 裡真正的 `android:debuggable` 屬性。
# 只看 badging 的話,aapt2 換一版少印那一行,這一關就會從此永遠說「乾淨」,
# 而且輸出和一切正常長得一模一樣。兩邊講的話不一致 = **讀不出來**,
# 那不是通過,是沒查成 —— 所以也擋。
#
#   $1 aapt2 dump badging 的原文
#   $2 aapt2 dump xmltree --file AndroidManifest.xml 的原文
#
# 與本檔 page_url_problem 同一個約定:**永遠 return 0**,有沒有問題看有沒有印字。
# 不用結束碼表達判定,是因為 `p="$(f ...)"` 在 set -e 之下會把非零的結束碼
# 變成當場中止 —— 那會讓 --self-check 在第一個「該紅」的案例上直接死掉。
debuggable_problem() {
  local badging="$1" xmltree="$2" b=0 x=0

  case "$badging" in *application-debuggable*) b=1 ;; esac
  # xmltree 的樣子(build-tools 35.0.0 實測):
  #   A: http://schemas.android.com/apk/res/android:debuggable(0x0101000f)=true
  # 舊版 aapt2 會印成 `(type 0x12)0xffffffff`。兩種都要認得 ——
  # 只認一種的話,換一台機器就等於沒在看。
  case "$xmltree" in
    *"android:debuggable(0x0101000f)=true"*) x=1 ;;
    *"android:debuggable(0x0101000f)=(type 0x12)0xffffffff"*) x=1 ;;
  esac

  # 兩份輸入都得先是「真的有東西」。空字串在下面每一個 case 都不命中,
  # 於是一個讀不出 APK 的 aapt2 會產生一句「不是 debuggable」—— 那是最糟的
  # 一種綠燈。所以先問:manifest 到底讀到了沒有。
  case "$xmltree" in
    *"android:debuggable"*|*"N: android="*|*"E: manifest"*) ;;
    *) printf 'aapt2 讀不出 AndroidManifest.xml(xmltree 輸出裡連 manifest 節點都沒有)—— 這不是「不是 debuggable」,是沒查成'; return 0 ;;
  esac
  case "$badging" in
    *"package: name="*) ;;
    *) printf 'aapt2 dump badging 讀不出這個 APK(輸出裡沒有 package: name=)—— 沒查成,不是通過'; return 0 ;;
  esac

  if [ "$b" -ne "$x" ]; then
    printf 'badging 說 debuggable=%s,xmltree 說 debuggable=%s —— 兩個判讀來源對不上,代表其中一個的解析已經失效。沒查成,不是通過' "$b" "$x"
    return 0
  fi
  if [ "$b" -eq 1 ]; then
    printf '這是 debuggable 的建置(android:debuggable=true)。發出去等於任何拿得到 adb 的人都能 run-as 讀走使用者的詞庫與輸入歷史,並對輸入法進程掛除錯器 —— 而輸入法看得到他打的每一個字。要發的是 release 那一份:cd android && ./gradlew assembleRelease'
    return 0
  fi
  return 0
}

# ── 帶版號的 APK 檔名 ─────────────────────────────────────────────────────
#
# 純函式,字首從外面傳進來 —— 它的唯一合法來源是 product.env 的
# R2_ANDROID_APK_PREFIX。在這裡寫死的話,下一次改名時這支腳本會安靜地
# 繼續產出舊名字(而檔名是**使用者看得到**的東西)。
# --self-check 的 G1 餵一個哨兵字首進來,確認它真的流到輸出。
#
#   $1 字首  $2 時間戳  $3 git sha  $4 -dirty 或空
release_apk_name() { printf '%s%s-%s%s.apk' "$1" "$2" "$3" "$4"; }

# ── 關卡:線上服役中的套件名 vs 這次要發的套件名 ──────────────────────────
#
# 擋的是使用者 2026-08-09 實際撞到的那一則:裝著改名**前**那個 applicationId
# (product.env 的 ANDROID_APP_ID_PREVIOUS)的人按下檢查更新 → 說有新版 →
# 下載 30MB → PackageInstaller 收到套件名不同的 APK 直接
# 拒收 → 畫面上寫「APK 檔案無效或已損毀」,而檔案完全正常。
#
# 為什麼是 PackageInstaller 拒收:UpdateInstaller.kt:96 建 session 時
# `setAppPackageName(app.packageName)`,系統會用它比對 APK 自己宣告的套件名。
#
# 這支腳本原本從頭到尾**沒有任何一處**比對套件名 —— 只比 version_code。
# 於是「改了 applicationId」在發布這一側是完全看不見的。
#
#   $1 線上 version.json 的原文(抓不到就是空字串)
#   $2 這次要發的套件名
#   $3 product.env 的 ANDROID_APP_ID_PREVIOUS(沒宣告就是空字串)
#
# 回傳 0 = 可以繼續,1 = 拒發。
# 這一關到底「查到了什麼」,給結尾的總結用。
# ⚠ 不可以讓總結說「套件名不衝突」而實際上根本沒查得成 —— 一句過度宣告的
#   綠字,比不印還糟:下一個人會以為這件事被守住了。
PKG_VERDICT="(還沒查)"

check_package_identity() {
  local published="$1" mine="$2" declared="$3"
  local online rc

  if [ -z "$published" ]; then
    PKG_VERDICT="沒查成(線上 version.json 抓不到)"
    echo "  [?] 線上 version.json 抓不到(第一次發布,或連不出去)。"
    echo "      **套件名這一關這一次沒有驗到** —— 不是通過,是沒查。"
    return 0
  fi

  set +e
  online="$(manifest_str_field "$published" package)"
  rc=$?
  set -e

  case "$rc" in
    3)
      PKG_VERDICT="沒查成(線上 version.json 不是合法 JSON)"
      echo "  [!] 線上 version.json 解析不了(不是合法的 JSON 物件)。"
      echo "      那表示**現在每一台裝置的檢查更新都是壞的**。這一次發布會覆蓋它。"
      echo "      套件名這一關沒有驗到。"
      return 0
      ;;
    4)
      _pkg_old_format_warning "$mine" "$declared"
      return 0
      ;;
    5)
      echo "  [!] 線上 version.json 的 package 欄位不是字串。app 端會當成沒有這個欄位"
      echo "      (VersionManifest.kt:172 的 str() 取不到就是 null),所以等同舊格式。"
      _pkg_old_format_warning "$mine" "$declared"
      return 0
      ;;
  esac

  if ! looks_like_package "$online"; then
    echo "  [!] 線上 version.json 的 package 是「$online」,不像套件名。"
    echo "      app 端對這種值的處理是**當成沒有**(不是照收),所以這裡也一樣 ——"
    echo "      一個看起來確定、實際上沒有根據的比對結果,會直接導致擋錯或放錯。"
    _pkg_old_format_warning "$mine" "$declared"
    return 0
  fi

  if [ "$online" = "$mine" ]; then
    PKG_VERDICT="與線上服役中的相同($online),一般升級"
    echo "  線上套件名  : $online(與這次相同,一般升級)"
    if [ -n "$declared" ]; then
      echo "  [i] product.env 仍留著 ANDROID_APP_ID_PREVIOUS=$declared,而線上已經是 $mine"
      echo "      —— 那份一次性宣告可以刪掉了(留著會讓下一次無聲改套件被誤判成已宣告)。"
    fi
    return 0
  fi

  # 到這裡:線上服役中的套件名與這次要發的**不一樣**。
  echo "  線上套件名  : $online" >&2
  echo "  這次的套件名: $mine" >&2
  echo >&2

  if [ -z "$declared" ]; then
    {
      echo "拒絕發布:線上服役中的是 $online,這次要發的是 $mine,而 product.env"
      echo "沒有宣告這次的套件識別碼變更(ANDROID_APP_ID_PREVIOUS)。"
      echo
      echo "現在裝著 $online 的使用者按下「檢查更新」會發生什麼:"
      echo "  1. 讀到這份 version.json,version_code 比較大 → 顯示「有新版本」"
      echo "  2. 下載完整的 APK(約 30MB)"
      echo "  3. 交給 PackageInstaller。UpdateInstaller.kt:96 已經 setAppPackageName($online),"
      echo "     而 APK 宣告的是 $mine → INSTALL_FAILED_INVALID_APK"
      echo "  4. 畫面上寫「APK 檔案無效或已損毀」—— 而檔案完全正常,它剛通過 sha256"
      echo "  5. 使用者重試,每次都一樣,然後放棄"
      echo
      echo "要繼續的話,applicationId 的變更必須是**明文宣告**的:"
      echo "  scripts/lib/product.env 裡寫 ANDROID_APP_ID_PREVIOUS=$online"
      echo "  (以及 ANDROID_APP_ID_CHANGE_REASON=為什麼改)"
      echo "宣告之後這支腳本會把 replaces_package 寫進 version.json,舊版的 app 就會"
      echo "改成顯示搬家卡片(不下載、不給安裝按鈕),而不是讓使用者撞上面那五步。"
      echo
      echo "如果你**沒有**要改套件名,那這份 APK 就是建錯了 —— 不要靠宣告繞過去。"
    } >&2
    return 1
  fi

  if [ "$declared" != "$online" ]; then
    {
      echo "拒絕發布:product.env 宣告的是「取代 $declared」,但線上服役中的是 $online。"
      echo
      echo "這一次會寫進 version.json 的 replaces_package 是 $declared,"
      echo "而裝著 $online 的使用者在那份清單裡找不到自己 —— app 端算出來的"
      echo "「這是不是我們自己改名」會是**否**(UpdateController 的 declared=false),"
      echo "搬家卡片會多印一句「版本資訊的網址可能指到了別的地方」,語氣保守到"
      echo "使用者不會照著做。等於宣告了卻沒有生效。"
      echo
      echo "要嘛把 ANDROID_APP_ID_PREVIOUS 改成 $online,要嘛先確認線上那一份"
      echo "到底是誰發的 —— 兩者只有一個是對的,不要兩個都寫。"
    } >&2
    return 1
  fi

  # 已宣告,而且宣告的正是線上服役中的那一個。放行,但要把後果講完。
  PKG_VERDICT="已宣告的套件識別碼變更 $online → $mine(舊使用者要手動搬家)"
  echo "  [已宣告的套件識別碼變更] $online → $mine"
  echo "  理由:${RS_ANDROID_APP_ID_CHANGE_REASON:-(product.env 沒填 ANDROID_APP_ID_CHANGE_REASON)}"
  echo
  echo "  現在裝著 $online 的使用者會走**遷移路徑**,不是升級:"
  echo "    · version.json 會帶 replaces_package=$declared"
  echo "    · app 端在**下載之前**就判定裝不上去 → 不下載那 30MB、不顯示安裝按鈕"
  echo "    · 改為顯示搬家卡片,五個步驟:匯出詞庫 → 取得新版 → 解除安裝舊版"
  echo "      → 安裝新版 → 匯入詞庫"
  echo "    · **詞典與設定不會自動轉移**,使用者必須自己匯出匯入"
  echo "    · 兩個 app 會並存,舊的要使用者自己移除"
  echo
  echo "  也就是說:這一版對舊使用者而言是「手動搬家」,不是「按一下更新」。"
  echo "  發布說明(notes)是唯一觸及得到他們的欄位 —— 請確認它有講這件事。"
  return 0
}

# 舊格式(線上清單沒有 package 欄位)的說明。case (a),不擋,但要講清楚。
_pkg_old_format_warning() {
  local mine="$1" declared="$2"
  PKG_VERDICT="沒查成(線上 version.json 是沒有 package 欄位的舊格式)"
  echo "  [!] 線上的 version.json **沒有 package 欄位** —— 那是加上這個欄位之前的舊格式。"
  echo "      因此無法從線上清單判斷現在服役中的那一份是哪個套件名,"
  echo "      這一關**沒有辦法驗**(不是通過)。"
  if [ -n "$declared" ] && [ "$declared" != "$mine" ]; then
    echo
    echo "      而 product.env 正宣告著 ANDROID_APP_ID_PREVIOUS=$declared,"
    echo "      也就是說改名這件事還在進行中。若線上那一份確實是 $declared,"
    echo "      那些使用者此刻的處境是:"
    echo "        檢查更新 → 說有新版 → 下載約 30MB → PackageInstaller 因為"
    echo "        setAppPackageName($declared) 與 APK 宣告的 $mine 不符而拒收 →"
    echo "        畫面上寫「APK 檔案無效或已損毀」,而檔案完全正常。"
    echo "      這一次發布會寫出 package=$mine 與 replaces_package=$declared,"
    echo "      舊版的 app 讀到之後就會改成顯示搬家卡片,不再讓他們白下載一次。"
  else
    echo "      這一次發布會寫出 package=$mine,下一次這一關就查得出來。"
  fi
}

# ── 關卡:notes 的預設值能不能當發布說明 ─────────────────────────────────
#
# notes 是 version.json 裡**唯一**會被舊版 app 顯示給使用者看的自由文字 ——
# 也就是我們唯一觸及得到「還沒升級的人」的欄位。原本 --notes 沒給就直接取
# HEAD 的 commit 標題,而這個 repo 的 main 上實際躺著的標題長這樣:
#
#   併入 windows
#   併入 macos
#   規範不准走在實作前面,所以 macOS 一補碼表 Android 就紅了
#
# 前兩個發出去,使用者看到的更新說明就是「併入 windows」。
#
# 這裡不去猜「這句話夠不夠好」(那種模糊的判斷會誤擋,而誤擋的關卡會被關掉),
# 只擋**在定義上不可能是發布說明**的那幾種:合併提交、空的、fixup/squash/WIP、
# revert。剩下的照舊放行,但會印出來說明它是自動取的。
#
#   $1 commit 標題  $2 parent 個數
# 回傳 0 = 可以用,1 = 不可以。
notes_default_usable() {
  local subject="$1" parents="${2:-1}"
  [ -n "$subject" ] || return 1
  [ "$parents" -le 1 ] || return 1
  case "$subject" in
    併入*|Merge\ *|merge\ *)          return 1 ;;
    fixup!*|squash!*|amend!*)          return 1 ;;
    WIP*|wip*|Revert\ *|revert:*)      return 1 ;;
  esac
  return 0
}

# ── 關卡:page_url ────────────────────────────────────────────────────────
#
# 搬家卡片上那顆「開啟下載頁」按鈕開的是 UpdateController 的
# `openUrl = pageUrl ?: downloadUrl`。而 page_url **從來沒有被任何發布腳本
# 寫出來過**,所以它一直退回 downloadUrl —— 也就是 .apk 直連。按下去不是
# 打開一個頁面,是直接開始下載 30MB。
#
# 有問題就把問題印出來(非空字串),沒問題就什麼都不印。
page_url_problem() {
  local u="$1"
  case "$u" in
    http://*|https://*) ;;
    *) printf '只接受 http/https(app 端 VersionManifest.kt:180 也是這樣擋的),收到:%s' "$u"; return 0 ;;
  esac
  case "$u" in
    *[[:space:]]*) printf '含空白字元:%s' "$u"; return 0 ;;
  esac
  # 指到 .apk 就等於沒有設 —— 那正是現在的行為,寫進去只是把謊言變成明文。
  case "$u" in
    *.apk|*.apk\?*|*.apk\#*) printf '指向 .apk 直連,那不是「下載頁」。按鈕的字是「開啟下載頁」,實際會直接下載 30MB:%s' "$u"; return 0 ;;
  esac
  return 0
}

# ── version.json 的內容 ──────────────────────────────────────────────────
#
# 抽成函式的理由:這份 JSON 是**真正送到使用者手上**的東西,而它原本是
# 埋在上傳流程中間的一段 heredoc —— 只有真的發布一次才會被執行到,
# 也就是「唯一驗證方式是發一版出去」。抽出來之後 --self-check 就驗得到
# 三件事:合法 JSON、宣告在時 replaces_package 真的有寫出來、
# notes 裡的引號與反斜線不會生出壞掉的 JSON(那份壞 JSON 會被幾百台裝置抓下去)。
#
# 讀的是全域變數而不是十三個位置參數 —— 位置參數排錯一個的後果是靜默地
# 把 sha256 寫進 size,而那種錯誤 heredoc 版本一樣會有,抽成函式沒有變糟。
#
# ⚠ package / replaces_package / page_url 三個**永遠是選用的**。改成必填
#   等於所有舊版本安靜地再也收不到更新,畫面上寫「版本資訊格式錯誤」——
#   比原本的問題更糟。(app 端已用突變測試釘住:改成必填 → 15 條紅。)
render_version_json() {
  local replaces_json="" page_json="" notes_json

  # 只有換套件識別碼的那一次要寫。值取自 product.env 的一次性宣告 ——
  # 有它,升級器才分得出「我們改名了」與「這份清單根本不是我們的」。
  if [ -n "${RS_ANDROID_APP_ID_PREVIOUS:-}" ] \
     && [ "$RS_ANDROID_APP_ID_PREVIOUS" != "$APK_PACKAGE" ]; then
    replaces_json="
  \"replaces_package\": \"$RS_ANDROID_APP_ID_PREVIOUS\","
  fi

  if [ -n "${PAGE_URL:-}" ]; then
    page_json="
  \"page_url\": $(PU="$PAGE_URL" python3 -c 'import json,os;print(json.dumps(os.environ["PU"]))'),"
  fi

  # notes 用 python 轉義,不然說明裡有一個雙引號或反斜線就會產生壞掉的 JSON。
  notes_json="$(NOTES="$NOTES" python3 -c 'import json,os;print(json.dumps(os.environ["NOTES"]))')"

  # version_code 是**唯一**用來判斷新舊的欄位。version_name 只給人看 ——
  # 拿字串比大小遲早比出 "0.10.0" < "0.9.0"。
  cat <<JSON
{
  "version_code": $VERSION_CODE,
  "version_name": "$VERSION_NAME",
  "build_stamp": "$STAMP-$SHA",
  "commit": "$SHA",
  "file": "$NAME",
  "size": $SIZE,
  "sha256": "$SHA256",
  "url": "$BASE_URL/$REMOTE_SUBDIR/$NAME",
  "latest_url": "$BASE_URL/$REMOTE_SUBDIR/$RS_R2_ANDROID_LATEST",
  "package": "$APK_PACKAGE",$replaces_json$page_json
  "notes": $notes_json
}
JSON
}

# ══════════════════════════════════════════════════════════════════════════
#  --self-check:逐條反向測試上面那些關卡
#
#  ⚠ 這裡**一條網路都不能連、一個 APK 都不能讀**。全部餵假清單。
#  ⚠ 每一條同時斷言結束碼**與**訊息裡該出現的字。只斷言結束碼是不夠的:
#    這幾關的價值有一半在它印的那段字 —— 印「拒絕發布」卻不說舊使用者會
#    怎樣的話,下一個人只會找個旗標繞過去。
# ══════════════════════════════════════════════════════════════════════════
SC_N=0; SC_FAIL=0
sc_ok()   { printf '  \033[1;32mok\033[0m   %s\n' "$*"; }
sc_bad()  { printf '\033[1;31m[FAIL]\033[0m %s\n' "$*" >&2; SC_FAIL=$((SC_FAIL+1)); }

# $1 描述  $2 期望結束碼  $3 期望出現的字串(可多個,用 \n 分隔)  $4.. 要跑的命令
sc_case() {
  local desc="$1" want_rc="$2" want_txt="$3"; shift 3
  local out rc missing=""
  SC_N=$((SC_N+1))
  set +e
  out="$("$@" 2>&1)"
  rc=$?
  set -e
  if [ "$rc" -ne "$want_rc" ]; then
    sc_bad "$desc:結束碼是 $rc,預期 $want_rc"
    printf '%s\n' "$out" | sed 's/^/       | /' >&2
    return 0
  fi
  local line
  while IFS= read -r line; do
    [ -n "$line" ] || continue
    case "$out" in *"$line"*) ;; *) missing="$missing
       缺:$line" ;; esac
  done <<EOF
$want_txt
EOF
  if [ -n "$missing" ]; then
    sc_bad "$desc:結束碼對了,但訊息裡少了該說的話$missing"
    printf '%s\n' "$out" | sed 's/^/       | /' >&2
    return 0
  fi
  sc_ok "$desc"
}

run_self_check() {
  echo "=== publish_apk.sh --self-check(不連網、不讀 APK)==="
  echo

  # 2026-08-10 從 R2 實際抓回來的那一份,逐字照貼。它就是使用者撞到的現場:
  # commit 0970777 已經含改名(git merge-base --is-ancestor fe5c78b 0970777 為真),
  # 而清單裡沒有 package 也沒有 replaces_package。
  local REAL_ONLINE
  REAL_ONLINE='{
  "version_code": 26080912,
  "version_name": "0.1.0-dev+26080912.0970777",
  "build_stamp": "20260809-1233-0970777",
  "commit": "0970777",
  "file": "rime-android-debug-20260809-1233-0970777.apk",
  "size": 30620033,
  "sha256": "128903e013ccdcc0c38ae8b7a8a117535316f6461827757ba0c65dd60463c1fa",
  "url": "https://pub-d6a54d2e5f5947e2b0b23fb8e27ce0a5.r2.dev/rime/rime-android-debug-20260809-1233-0970777.apk",
  "latest_url": "https://pub-d6a54d2e5f5947e2b0b23fb8e27ce0a5.r2.dev/rime/rime-latest.apk",
  "notes": "九宮格"
}'
  # ⚠ 底下每一個套件名都從 product.env 取,**不寫死**。
  #   scripts/verify_product_ids.sh 第 3 項會擋(而且它擋得對:寫死的那一份
  #   會在下一次改名時安靜地變成錯的靶,而錯的靶照樣全綠)。
  #   E1/E2 兩條已經釘住「這兩個變數真的等於 product.env 裡的值」。
  # ⚠ 用 ${...:-} 而不是直接展開:沒 source 到 product.sh 時,直接展開會在
  #   set -u 下當場「unbound variable」爆掉 —— 那個訊息看起來像腳本壞了,
  #   不像「product.env 沒被讀進來」。留空字串讓 E1/E2 去指出真正的原因。
  local MINE="${RS_ANDROID_APP_ID:-}"           # 這次要發的
  local PREV="${RS_ANDROID_APP_ID_PREVIOUS:-}"  # 改名前的
  local OTHER="org.example.notours"         # 誰的都不是,用來測「宣告對不上」
  local OLD NEW
  OLD="{\"version_code\":1,\"package\":\"$PREV\",\"notes\":\"x\"}"
  NEW="{\"version_code\":1,\"package\":\"$MINE\",\"notes\":\"x\"}"

  echo "── A. 線上服役中的套件名(check_package_identity)──────────────────"

  sc_case "A1 (a) 線上是**實際線上那一份**(舊格式、沒有 package),而改名宣告還在 → 放行,但要說清楚舊使用者現在的處境" \
    0 "沒有 package 欄位
APK 檔案無效或已損毀
replaces_package=$PREV" \
    check_package_identity "$REAL_ONLINE" "$MINE" "$PREV"

  sc_case "A2 (b) 線上是 $PREV、這次是 $MINE、**沒有宣告** → 拒發" \
    1 "拒絕發布
INSTALL_FAILED_INVALID_APK
APK 檔案無效或已損毀
ANDROID_APP_ID_PREVIOUS=$PREV" \
    check_package_identity "$OLD" "$MINE" ''

  sc_case "A3 (c) 同上但**有宣告**,而且宣告的正是線上那一個 → 放行,並印出遷移路徑" \
    0 '已宣告的套件識別碼變更
遷移路徑
不下載那 30MB
詞典與設定不會自動轉移' \
    check_package_identity "$OLD" "$MINE" "$PREV"

  sc_case "A4 有宣告,但宣告的不是線上服役中的那一個 → 拒發(宣告了卻不會生效)" \
    1 '拒絕發布
declared=false' \
    check_package_identity "$OLD" "$MINE" "$OTHER"

  sc_case "A5 線上與這次相同 → 一般升級,放行" \
    0 '一般升級' \
    check_package_identity "$NEW" "$MINE" ''

  sc_case "A6 線上與這次相同,但宣告還留著 → 放行並提醒那份一次性宣告該刪了" \
    0 '一次性宣告可以刪掉了' \
    check_package_identity "$NEW" "$MINE" "$PREV"

  sc_case "A7 線上清單抓不到 → 不可以宣稱通過,要說「沒查」" \
    0 '沒有驗到' \
    check_package_identity '' "$MINE" "$PREV"

  sc_case "A8 線上的 package 值是數字(型別不對)→ 當成沒有,不可以據此拒發" \
    0 'package 欄位不是字串
等同舊格式' \
    check_package_identity '{"version_code":1,"package":123}' "$MINE" ''

  sc_case "A9 線上的 package 是帶空白的字串 → 同樣當成沒有" \
    0 '不像套件名' \
    check_package_identity "{\"version_code\":1,\"package\":\"${PREV//./ }\"}" "$MINE" ''

  # ⚠ 這一條釘的是「要有 JSON 語意,不可以用 sed/grep 掃字串」。
  #   底下這份清單的**頂層** package 與這次相同(該放行),但後面還有一個
  #   巢狀物件裡也叫 package。`sed -n 's/.*"package"…'` 的 `.*` 是貪婪的,
  #   會抓到**最後**那一個 → 這一關就會擋下一次完全正常的發布。
  #   (試過用 notes 裡藏一個假 package 當靶,但 JSON 會把引號逸出成 \",
  #   sed 的 `":` 反而對不上、誤打誤撞抓對 —— 那種靶分不出兩種實作,不算數。)
  sc_case "A10 巢狀物件裡有同名的 package → 解析器不可以被騙(貪婪的 sed 會)" \
    0 '一般升級' \
    check_package_identity \
      "{\"version_code\":1,\"package\":\"$MINE\",\"meta\":{\"package\":\"$PREV\"}}" \
      "$MINE" ''

  sc_case "A11 線上清單不是合法 JSON → 要說「現在每台裝置的檢查更新都是壞的」,不是靜靜放行" \
    0 '解析不了' \
    check_package_identity '{oops' "$MINE" ''

  echo "── B. 線上 version_code 的抽取(manifest_num_field)────────────────"

  sc_case "B1 真實線上清單讀得出 26080912" \
    0 '26080912' \
    manifest_num_field "$REAL_ONLINE" version_code

  # 同樣的陷阱:巢狀物件裡出現一個更大的 version_code,貪婪的 sed 會抓到它,
  # 於是單調性關卡拿一個不存在的號碼當基準,把一次正常的發布擋成「降版」,
  # 而訊息會叫人去改 versionCode —— 完全指錯方向。
  sc_case "B2 巢狀物件裡有更大的 version_code → 不可以被騙" \
    0 '1' \
    manifest_num_field '{"version_code":1,"meta":{"version_code":99999999}}' version_code

  sc_case "B3 沒有 version_code 欄位 → 非零結束碼(不可以吐空字串當成 0)" \
    4 '' \
    manifest_num_field '{"notes":"x"}' version_code

  echo "── C. notes 的預設值(notes_default_usable)────────────────────────"

  # main 上真的躺著這兩個標題(git log --oneline:35800ed「併入 windows」、
  # 69ac8cc「併入 macos」)。在它們上面跑 publish,使用者看到的更新說明
  # 就是「併入 windows」。
  sc_case "C1 「併入 windows」(main 上真實存在的標題)→ 不可以當發布說明" \
    1 '' notes_default_usable "併入 windows" 1
  sc_case "C2 合併提交(兩個 parent)→ 不可以" \
    1 '' notes_default_usable "看起來很正常的一句話" 2
  sc_case "C3 空標題 → 不可以" \
    1 '' notes_default_usable "" 1
  sc_case "C4 fixup!/WIP → 不可以" \
    1 '' notes_default_usable "WIP 還沒好" 1
  sc_case "C5 一般的中文標題 → 可以(不可以誤擋,誤擋的關卡會被關掉)" \
    0 '' notes_default_usable "九宮格拼音消歧欄改回字母色" 1

  echo "── D. page_url(page_url_problem)──────────────────────────────────"

  sc_page_bad() {
    local p; p="$(page_url_problem "$1")"
    [ -n "$p" ] || return 1
    printf '%s' "$p"
  }
  sc_page_ok() {
    local p; p="$(page_url_problem "$1")"
    [ -z "$p" ] || { printf '不該有問題卻報了:%s' "$p"; return 1; }
    printf 'ok'
  }

  sc_case "D1 .apk 直連 → 要擋(那正是現在按鈕的行為,寫進去只是把謊言變明文)" \
    0 '指向 .apk 直連' \
    sc_page_bad "https://pub-d6a54d2e5f5947e2b0b23fb8e27ce0a5.r2.dev/rime/rime-latest.apk"
  sc_case "D2 非 http(s) → 要擋(app 端也是這樣擋的)" \
    0 '只接受 http/https' \
    sc_page_bad "javascript:alert(1)"
  sc_case "D3 含空白 → 要擋" \
    0 '含空白字元' \
    sc_page_bad "https://example.invalid/a b"
  sc_case "D4 正常的頁面網址 → 放行" \
    0 'ok' \
    sc_page_ok "https://example.invalid/rime/downloads/"

  echo "── E. product.env 真的被讀進來了 ──────────────────────────────────"

  # 這一條釘的是「replaces_package 那段程式碼是死的」那個 bug:
  # publish_apk.sh 原本沒有 source product.sh,所以 RS_ANDROID_APP_ID_PREVIOUS
  # 永遠是空的,`replaces_package` 一次都沒有被寫出來過。
  # 這裡刻意用**另一條路**(awk 直接讀 product.env)取值再比對 ——
  # 兩邊都問 product.sh 的話就沒有第二意見了。
  sc_prodenv() {
    local key="$1" got="$2" want
    want="$(awk -v k="^$key=" '$0 ~ k { sub(/^[^=]*=/, ""); print; exit }' \
              "$ROOT/scripts/lib/product.env")"
    if [ -z "$want" ]; then
      printf 'product.env 裡沒有 %s —— 這一條的比對對象是空的,等於沒驗' "$key"
      return 1
    fi
    if [ "$got" != "$want" ]; then
      printf '%s:product.sh 給的是「%s」,product.env 裡寫的是「%s」' "$key" "$got" "$want"
      return 1
    fi
    printf '%s=%s' "$key" "$got"
  }
  sc_case "E1 RS_ANDROID_APP_ID 有值且與 product.env 逐字相同" \
    0 'ANDROID_APP_ID=' \
    sc_prodenv ANDROID_APP_ID "${RS_ANDROID_APP_ID:-}"
  sc_case "E2 RS_ANDROID_APP_ID_PREVIOUS 有值且與 product.env 逐字相同(沒 source 過的話這裡是空的)" \
    0 'ANDROID_APP_ID_PREVIOUS=' \
    sc_prodenv ANDROID_APP_ID_PREVIOUS "${RS_ANDROID_APP_ID_PREVIOUS:-}"

  echo "── F. 真正送到使用者手上的那份 version.json(render_version_json)──"

  # 這一段以前只有「真的發一版出去」才會被執行到。現在餵假值 render 一次,
  # 用 python 讀回來檢查欄位 —— 不寫檔、不上傳。
  sc_render() {
    local want_declared="$1" want_page="$2"
    local VERSION_CODE=26081000 VERSION_NAME='0.1.0-dev+x' STAMP=20260810-0000 \
          SHA=deadbee NAME=sc-fake-x.apk SIZE=1 SHA256=ff \
          BASE_URL=https://example.invalid REMOTE_SUBDIR=rime \
          APK_PACKAGE="$RS_ANDROID_APP_ID" \
          RS_ANDROID_APP_ID_PREVIOUS="$want_declared" PAGE_URL="$want_page" \
          NOTES='引號 " 反斜線 \ 都要活著'
    render_version_json | WANT_PKG="$APK_PACKAGE" python3 -c '
import json, os, sys
o = json.load(sys.stdin)          # 不是合法 JSON 就在這裡爆
assert o["notes"] == "引號 \" 反斜線 \\ 都要活著", "notes 轉義壞了:%r" % o["notes"]
assert o["package"] == os.environ["WANT_PKG"], o["package"]
print("keys=" + ",".join(sorted(k for k in ("replaces_package", "page_url") if k in o)))
'
  }

  sc_case "F1 有宣告 + 有 page_url → 合法 JSON,而且兩個欄位都真的寫出來了" \
    0 'keys=page_url,replaces_package' \
    sc_render "$PREV" https://example.invalid/rime/downloads/

  sc_case "F2 沒宣告、沒 page_url → 兩個選用欄位都不可以出現(不是寫成空字串)" \
    0 'keys=' \
    sc_render '' ''

  sc_case "F3 宣告的值等於本次套件名(宣告過期了)→ 不寫 replaces_package" \
    0 'keys=' \
    sc_render "$MINE" ''

  echo "── G. debuggable(debuggable_problem)────────────────────────────────"

  # 這一段的靶是 aapt2 的**實際輸出**(build-tools 35.0.0,2026-08-10 量的),
  # 不是我想像中的格式。兩份原文各留一段最小可辨識的片段。
  local BADG_DBG BADG_OK XML_DBG XML_DBG_OLD XML_OK
  BADG_DBG="package: name='org.x' versionCode='1' versionName='1'
application-debuggable
application: label='X'"
  BADG_OK="package: name='org.x' versionCode='1' versionName='1'
application: label='X'"
  XML_DBG="E: manifest (line=2)
  E: application (line=20)
    A: http://schemas.android.com/apk/res/android:debuggable(0x0101000f)=true"
  # 舊版 aapt2 印的是原始型別。兩種都要認得 —— 只認一種等於換一台機器就沒在看。
  XML_DBG_OLD="E: manifest (line=2)
  E: application (line=20)
    A: http://schemas.android.com/apk/res/android:debuggable(0x0101000f)=(type 0x12)0xffffffff"
  XML_OK="E: manifest (line=2)
  E: application (line=20)
    A: http://schemas.android.com/apk/res/android:label(0x01010001)=@0x7f120000"

  sc_dbg_bad() {
    local p; p="$(debuggable_problem "$1" "$2")"
    [ -n "$p" ] || return 1
    printf '%s' "$p"
  }
  sc_dbg_ok() {
    local p; p="$(debuggable_problem "$1" "$2")"
    [ -z "$p" ] || { printf '不該有問題卻報了:%s' "$p"; return 1; }
    printf 'ok'
  }

  sc_case "G1 debuggable 的建置 → 要擋,而且要說出後果(run-as 讀走詞庫)" \
    0 'debuggable 的建置
run-as
assembleRelease' \
    sc_dbg_bad "$BADG_DBG" "$XML_DBG"

  sc_case "G2 舊版 aapt2 的 (type 0x12)0xffffffff 寫法也要認得" \
    0 'debuggable 的建置' \
    sc_dbg_bad "$BADG_DBG" "$XML_DBG_OLD"

  sc_case "G3 兩份都乾淨 → 放行(誤擋的關卡會被關掉,那才是真的損失)" \
    0 'ok' \
    sc_dbg_ok "$BADG_OK" "$XML_OK"

  sc_case "G4 兩個來源講的話不一致 → 要擋,而且要講明「沒查成」不是「通過」" \
    0 '對不上
沒查成' \
    sc_dbg_bad "$BADG_DBG" "$XML_OK"

  sc_case "G5 xmltree 空的(aapt2 讀不出 manifest)→ 要擋。空字串必須不等於「乾淨」" \
    0 '沒查成' \
    sc_dbg_bad "$BADG_OK" ""

  sc_case "G6 badging 空的 → 要擋(同上,讀不出來 ≠ 沒問題)" \
    0 '沒查成' \
    sc_dbg_bad "" "$XML_OK"

  echo "── G'. 檔名與 latest 指標都取自 product.env ────────────────────────"

  # 這兩條釘的是「字首/指標被抄回腳本裡」。抄回去之後輸出一模一樣,
  # 差別只有下一次改 product.env 時它不跟著動 —— 那是最難發現的一種。
  # 所以餵哨兵值進去,要求哨兵出現在輸出裡。
  sc_case "G7 release_apk_name 用的是傳進來的字首,不是寫死的" \
    0 'SC-SENTINEL-android-20260810-0000-deadbee.apk' \
    release_apk_name 'SC-SENTINEL-android-' 20260810-0000 deadbee ''

  sc_latest() {
    # 只覆蓋 latest 那一個變數,其餘沿用 sc_render 的假值。
    local RS_R2_ANDROID_LATEST='sc-sentinel-latest.apk'
    local VERSION_CODE=26081000 VERSION_NAME='x' STAMP=20260810-0000 \
          SHA=deadbee NAME=sc-fake-x.apk SIZE=1 SHA256=ff \
          BASE_URL=https://example.invalid REMOTE_SUBDIR=rime \
          APK_PACKAGE="$RS_ANDROID_APP_ID" RS_ANDROID_APP_ID_PREVIOUS='' \
          PAGE_URL='' NOTES='x'
    render_version_json | python3 -c '
import json, sys
print("latest_url=" + json.load(sys.stdin)["latest_url"])
'
  }
  sc_case "G8 version.json 的 latest_url 取自 product.env 的 R2_ANDROID_LATEST" \
    0 'latest_url=https://example.invalid/rime/sc-sentinel-latest.apk' \
    sc_latest

  echo
  # §2-G2:掃描範圍非空。這裡是「條數」—— 少一條就紅,不是「零個違規=通過」。
  # 加新案例時要跟著把這個數字加上去,那是刻意的:少一條的原因通常是有人
  # 在除錯時把某一條註解掉,然後忘了放回來,而全綠會讓他以為沒事。
  if [ "$SC_N" -ne 36 ]; then
    sc_bad "跑了 $SC_N 條,預期 36 條 —— 有案例被繞過或被刪掉了(§2-G2)"
  fi
  echo "=== 共 $SC_N 條,失敗 $SC_FAIL 條 ==="
  [ "$SC_FAIL" -eq 0 ] || return 1
  return 0
}

if [ "$SELF_CHECK" -eq 1 ]; then
  if run_self_check; then exit 0; else exit 1; fi
fi

if [ -n "$PAGE_URL" ]; then
  PAGE_PROBLEM="$(page_url_problem "$PAGE_URL")"
  [ -z "$PAGE_PROBLEM" ] || die "--page-url $PAGE_PROBLEM"
fi

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
# 字首從 product.env 來,不在這裡寫死 —— 見 release_apk_name() 的註解。
NAME="$(release_apk_name "$RS_R2_ANDROID_APK_PREFIX" "$STAMP" "$SHA" "$DIRTY")"

# 上面那一行有沒有真的用到 product.env,在輸出上看不出來(寫死同一串字的
# 結果一模一樣)。所以這裡明著問一次:
#   · 檔名必須以 product.env 的字首開頭 —— 有人把字面值抄回去時,
#     下一次改 product.env 這裡就會紅,而不是安靜地繼續產出舊名字;
#   · 帶版號的那一份不可以撞上 latest 指標 —— 撞上的話,「帶版號的那份
#     不會被覆蓋、那是出問題時的退路」這句話當場不成立。
case "$NAME" in
  "$RS_R2_ANDROID_APK_PREFIX"*) ;;
  *) die "產出的檔名「$NAME」不是以 product.env 的 R2_ANDROID_APK_PREFIX
(「$RS_R2_ANDROID_APK_PREFIX」)開頭 —— 有人把檔名字首寫死在腳本裡了。
檔名是使用者看得到的東西,而寫死的那一份會在下一次改名時安靜地留在舊名字上。" ;;
esac
[ "$NAME" != "$RS_R2_ANDROID_LATEST" ] \
  || die "帶版號的檔名和 latest 指標同名($NAME)——
那會讓每一次發布都覆蓋掉退路。R2_ANDROID_APK_PREFIX 設錯了。"

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
# ⚠ 這三個抽取刻意**不用管線**(原本是 `printf | sed | head -1`)。
#   `set -o pipefail` 下,head -1 讀到就關掉 pipe,上游拿到 SIGPIPE → 整個
#   命令替換非零 → 賦值在 set -e 下當場中止。而且**輸出小的時候完全正常**,
#   badging 長大之後才發作。here-string 不是 pipe,awk 自己 exit,沒有這個問題。
VERSION_CODE="$(awk -F"'" '/versionCode=/ { for (i=1;i<NF;i++) if ($i ~ /versionCode=$/) { print $(i+1); exit } }' <<<"$BADGING")"
VERSION_NAME="$(awk -F"'" '/versionName=/ { for (i=1;i<NF;i++) if ($i ~ /versionName=$/) { print $(i+1); exit } }' <<<"$BADGING")"
[ -n "$VERSION_CODE" ] || die "APK 裡讀不到 versionCode"
[ -n "$VERSION_NAME" ] || die "APK 裡讀不到 versionName"

# 套件名也從**這一份 APK**讀,而且要在關卡跑之前就讀出來 ——
# 原本是等到 version.json 那一段(上傳完之後)才讀,所以沒有任何一關拿得到它。
APK_PACKAGE="$(awk -F"'" '/^package: name=/ { print $2; exit }' <<<"$BADGING")"
[ -n "$APK_PACKAGE" ] || die "從 APK 讀不到套件名(aapt2 dump badging 沒有 package: name=…)"
if [ "$APK_PACKAGE" != "${RS_ANDROID_APP_ID:-}" ]; then
  echo "[!] 這份 APK 的套件名是 $APK_PACKAGE,而 product.env 說 ANDROID_APP_ID=${RS_ANDROID_APP_ID:-(空)}"
  echo "    —— 兩者應該一致。要嘛這份 APK 是舊的,要嘛 applicationId 被改到別的地方去了。"
fi

# ---------------------------------------------------- debuggable ---
# 這一關放在簽章關卡**之前**。理由:簽章關卡問的是「既有使用者裝不裝得上」,
# 這一關問的是「這份東西該不該存在於使用者手上」。後者先回答。
#
# --check-only 也要跑。CI 每次 push 呼叫的就是 --check-only,而「今天建出來的
# 是不是 debuggable」正是每次 push 都該回答的問題 —— 等到真的按下發布才問,
# 中間所有的綠燈都在替一份 debuggable 的 APK 背書。
XMLTREE="$("$AAPT" dump xmltree --file AndroidManifest.xml "$ROOT/release/$NAME" 2>/dev/null || true)"
DBG_PROBLEM="$(debuggable_problem "$BADGING" "$XMLTREE")" || true
if [ -n "$DBG_PROBLEM" ]; then
  die "$DBG_PROBLEM

(這一關是 2026-08-10 加的。在那之前,發給使用者的一直是 app-debug.apk。)"
fi
echo "[OK] 不是 debuggable 的建置(badging 與 manifest 兩個來源一致)"

# ── notes ────────────────────────────────────────────────────────────────
# notes 是 version.json 裡唯一會顯示給使用者看的自由文字,也就是唯一觸及得到
# 「還沒升級的人」的欄位。沒指定就取 commit 標題 —— 但 commit 標題是寫給
# 開發者看的,而 main 上真的躺著「併入 windows」這種標題。
# notes_default_usable() 擋的是在定義上不可能是發布說明的那幾種。
if [ "$NOTES_SET" -eq 0 ] && [ "$CHECK_ONLY" -eq 1 ]; then
  # --check-only 只驗簽章與單調性,不會寫出任何 version.json,所以沒有
  # 「使用者會看到什麼」這件事。CI 每次 push 都跑這一支,而 main 上的
  # HEAD 常常正是「併入 xxx」——擋在這裡等於讓合併提交把 CI 弄紅,
  # 而它擋的那個風險在這條路徑上根本不存在。
  NOTES="(--check-only:不會發布,notes 不適用)"
elif [ "$NOTES_SET" -eq 0 ]; then
  NOTES="$(git log -1 --format=%s 2>/dev/null || true)"
  NOTES_PARENTS="$(git log -1 --format=%p 2>/dev/null | wc -w | tr -d ' ')"
  if ! notes_default_usable "$NOTES" "${NOTES_PARENTS:-1}"; then
    die "沒有給 --notes,而 HEAD 的 commit 標題不能拿來當發布說明:「${NOTES:-(空)}」
(parent 個數 ${NOTES_PARENTS:-?};合併提交、fixup/squash/WIP、revert、空標題都不行)

notes 是 version.json 裡**唯一**會被 app 顯示給使用者看的自由文字 ——
它是我們唯一觸及得到「還沒升級的人」的欄位。發一版「併入 windows」出去,
幾百台裝置的更新說明上就寫著那四個字。

請明確給:  --notes \"這一版改了什麼(寫給使用者看)\"
真的不想寫: --notes \"\"(空字串是允許的,但要是你決定的,不是預設撿到的)"
  fi
  echo "[!] notes 沒有指定,自動取了 HEAD 的 commit 標題。它是寫給開發者看的,"
  echo "    但這一版的使用者會在更新說明上看到它:「$NOTES」"
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
#
# ⚠ apksigner 的輸出格式跟 build-tools 版本走,而**本機與 CI 的 build-tools
#   不會永遠一樣**。實測到的兩種:
#
#     舊(build-tools 35 / 36.1):
#       Signer #1 certificate SHA-256 digest: 444b…
#     新(GitHub runner 上的那一版):
#       V3.0 Signer: certificate SHA-256 digest: 444b…
#       V2 Signer: certificate SHA-256 digest: 6aaa…
#
#   原本只認舊格式,於是 CI 建的 APK 明明**簽得完全正確**(digest 一字不差)
#   卻被判成「沒有簽章者」。一個把好的建置擋下來的檢查,下場通常是被人
#   整個關掉 —— 那才是真正的損失。
#
#   兩種格式的語意不同,要分開處理:
#     · 舊格式的 `Signer #1` 已經是 apksigner 幫你算好的「這個區間實際生效
#       的簽章者」,直接取。
#     · 新格式是把**每一個簽章方案**各印一行。Android 會挑它支援的最高
#       版本,所以有效簽章者是印出來的方案裡版本號最大的那一個
#       (28+ 看到 V2=舊金鑰 與 V3.0=新金鑰,實際生效的是 V3.0)。
#
#   而且抓不到時要把 apksigner 說了什麼原封不動印出來:「讀不出來」與
#   「簽錯了」是完全不同的兩件事,不能都報成同一句 —— 上一版就是這樣,
#   於是排查時完全看不出來問題其實出在解析器。
signer_in_range() {
  local out digest
  out="$("$APKSIGNER" verify --min-sdk-version "$2" --max-sdk-version "$3" \
          --print-certs "$1" 2>&1 || true)"
  digest="$(printf '%s\n' "$out" | python3 -c '
import re, sys
plain, schemes = [], []
for line in sys.stdin:
    m = re.match(r"^Signer (?:\([^)]*\) )?#\d+ certificate SHA-256 digest: (\S+)", line)
    if m:
        plain.append(m.group(1))
        continue
    m = re.match(r"^V(\d+(?:\.\d+)?) Signer: certificate SHA-256 digest: (\S+)", line)
    if m:
        schemes.append((float(m.group(1)), m.group(2)))
if plain:
    print(plain[0])
elif schemes:
    print(max(schemes)[1])
')"
  digest="${digest%%$'\n'*}"
  if [ -z "$digest" ]; then
    {
      echo "  ── apksigner verify --min-sdk-version $2 --max-sdk-version $3 的完整輸出 ──"
      printf '%s\n' "$out" | sed 's/^/    /'
      echo "  ──────────────────────────────────────────────────────────────"
    } >&2
  fi
  printf '%s' "$digest"
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
PUBLISHED_CODE=""
if [ -n "$PUBLISHED_JSON" ]; then
  PUBLISHED_CODE="$(manifest_num_field "$PUBLISHED_JSON" version_code || true)"
fi
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
  echo "  線上版本    : 讀不到(第一次發布,或線上的 version.json 壞了)"
fi

# ------------------------------------------------- 套件識別碼檢查 ---
# 這一關以前完全不存在 —— 上面只比 version_code。於是「改了 applicationId」
# 在發布這一側是隱形的,而它的後果比降版嚴重得多:降版是使用者按了沒反應,
# 換套件名是使用者**下載完 30MB 之後**被告知「APK 檔案無效或已損毀」。
echo
echo "=== 套件識別碼 ==="
echo "  這次的套件名: $APK_PACKAGE"
check_package_identity "$PUBLISHED_JSON" "$APK_PACKAGE" "${RS_ANDROID_APP_ID_PREVIOUS:-}" \
  || die "套件識別碼這一關沒過,沒有上傳任何東西"

# ------------------------------------------------------- 下載頁 ---
# page_url 是搬家卡片上「開啟下載頁」那顆按鈕真正會開的東西 ——
# 沒有它,UpdateController 的 `openUrl = pageUrl ?: downloadUrl` 會退回
# .apk 直連,按鈕的字說「開啟下載頁」而實際行為是直接下載 30MB。
if [ -n "$PAGE_URL" ]; then
  echo "  下載頁      : $PAGE_URL(會寫進 version.json 的 page_url)"
else
  echo
  echo "  [!] 沒有給 --page-url,version.json 不會有 page_url 欄位。"
  echo "      後果:搬家卡片上的「開啟下載頁」按鈕會退回 APK 直連 ——"
  echo "      按下去是直接開始下載 30MB,不是打開一個頁面。功能上走得通"
  echo "      (瀏覽器下載完手動安裝,換套件名也裝得起來),但按鈕的字與行為不符。"
  echo "      這裡刻意**不編一個網址出來**:2026-08-10 實測 R2 上 rime/ 與"
  echo "      rime/downloads/ 都是 404,一個 404 的「下載頁」比退回直連更糟。"
  echo "      真的要做的話,scripts/downloads_server.py 是那個頁面的內容,"
  echo "      但它現在只跑在區網,沒有公開位址。"
fi

# --check-only 到這裡就結束:簽章鏈、API 28+ 與 26–27 的簽章者、
# versionCode 單調性、以及線上服役中的套件名都已經查過了,而這幾項就是
# 「發出去會不會害既有使用者裝不上來」的全部。剩下的是上傳,那需要 R2 憑證,
# 而且必須是人主動要的。
if [ "$CHECK_ONLY" -eq 1 ]; then
  echo
  echo "=== --check-only:關卡通過,沒有上傳任何東西 ==="
  echo "  簽章    : 與正式金鑰同鏈(輪替鏈、API 28+、API 26–27 三項都比對過)"
  if [ -n "$PUBLISHED_CODE" ] && [ "$VERSION_CODE" -le "$PUBLISHED_CODE" ]; then
    echo "  versionCode: $VERSION_CODE **沒有**大於線上的 $PUBLISHED_CODE(你指定了 --allow-downgrade)"
  elif [ -n "$PUBLISHED_CODE" ]; then
    echo "  versionCode: $VERSION_CODE > 線上的 $PUBLISHED_CODE"
  else
    echo "  versionCode: $VERSION_CODE(線上讀不到,沒有比對基準)"
  fi
  # ⚠ 這裡照抄 $PKG_VERDICT,不自己另寫一句。原本寫死「與線上服役中的那一份
  #   不衝突」—— 而線上根本沒有 package 欄位、比不了,那句話是憑空的。
  echo "  套件名  : $APK_PACKAGE —— $PKG_VERDICT"
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
  upload "$ROOT/release/$NAME" "$REMOTE_DIR/$RS_R2_ANDROID_LATEST"
fi

# version.json:app 內檢查更新讀的就是這一份。
#
# version_code 是**唯一**用來判斷新舊的欄位。version_name 只給人看 ——
# 拿字串比大小遲早比出 "0.10.0" < "0.9.0"。
#
# notes 用 python 轉義,不然說明裡有一個雙引號或反斜線就會產生壞掉的 JSON,
# 而那份 JSON 會被幾百台裝置抓下去。
# ── 套件識別碼 ────────────────────────────────────────────────────────
#
# ⚠ 從 **APK 本身**讀,不是從 product.env 讀。理由與 version_code 相同:
#   這份 JSON 描述的是這一個檔案,而不是「我們以為建出來的東西」。
#   設定檔與實際產物不一致時,寫進去的必須是產物那一邊。
#
# 為什麼要有這個欄位:改 applicationId 等於換一個 app,舊版**升不上去**。
# 沒有這個欄位的話,升級器會下載完 28MB、交給系統、然後拿到
#   INSTALL_FAILED_INVALID_APK: specified package X inconsistent with Y
# 而 App 只能把那句話原樣丟給使用者 —— 它甚至會被寫成「APK 檔案無效或已損毀」,
# 而檔案完全正常。使用者實際撞過這一次。
#
# ⚠ **這幾個欄位永遠是選用的。** 改成必填,等於所有舊版本安靜地再也收不到更新,
#   而畫面上寫「版本資訊格式錯誤」—— 比原本的問題更糟。
# $APK_PACKAGE 在版本號那一段就已經從 $BADGING(= release/ 底下那份複本的
# badging)讀出來了,而且套件識別碼那一關已經拿它跟線上服役中的比對過。
# 原本是在這裡才第一次讀 —— 也就是**上傳完之後**,所以沒有任何一關拿得到它。
#
# ⚠ replaces_package 那一段以前是死的:本檔沒有 source lib/product.sh,
#   CI 也沒有 export,所以 $RS_ANDROID_APP_ID_PREVIOUS 永遠是空字串,
#   replaces_package 一次都沒有被寫出去過。source 補在檔案開頭,
#   --self-check 的 E2 釘住那個變數、F1 釘住這個欄位真的有寫出來。
render_version_json > "$ROOT/release/version.json"
python3 -c 'import json,sys;json.load(open(sys.argv[1]))' "$ROOT/release/version.json" \
  || die "產生出來的 version.json 不是合法 JSON,已中止(沒有上傳)"
echo "  version.json:"
sed 's/^/    /' "$ROOT/release/version.json"
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
  verify "$BASE_URL/$REMOTE_SUBDIR/$RS_R2_ANDROID_LATEST" "$SIZE" || FAIL=1
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


# 對外拿回來的 version.json 必須真的含 version_code 與 package ——
# app 沒有 version_code 就判斷不了新舊,沒有 package 就判斷不了「裝不裝得上」,
# 而那兩個失敗都會安靜到沒有人發現(線上那一份沒有 package 就是這樣過了一整輪)。
ONLINE_JSON="$(curl -sS --max-time 20 "$BASE_URL/$REMOTE_SUBDIR/version.json" || true)"
ONLINE_CODE="$(manifest_num_field "$ONLINE_JSON" version_code || true)"
if [ "$ONLINE_CODE" = "$VERSION_CODE" ]; then
  echo "  [OK] 線上 version.json 的 version_code = $ONLINE_CODE"
else
  echo "  [失敗] 線上 version.json 的 version_code 是 '$ONLINE_CODE',預期 $VERSION_CODE"
  FAIL=1
fi

ONLINE_PKG="$(manifest_str_field "$ONLINE_JSON" package || true)"
if [ "$ONLINE_PKG" = "$APK_PACKAGE" ]; then
  echo "  [OK] 線上 version.json 的 package = $ONLINE_PKG"
else
  echo "  [失敗] 線上 version.json 的 package 是 '$ONLINE_PKG',預期 $APK_PACKAGE"
  echo "         —— 下一次發布的套件識別碼關卡會因此查不到基準,而使用者端"
  echo "         也拿不到「這份 APK 裝不裝得上」的事前判斷。"
  FAIL=1
fi

if [ -n "$PAGE_URL" ]; then
  # 一顆按鈕後面接一個 404 比沒有那顆按鈕更糟 —— 使用者會以為是自己做錯了。
  verify "$PAGE_URL" "" || { echo "  [失敗] page_url 取不到,不要把它寫進發布"; FAIL=1; }
fi

[ "$FAIL" -eq 0 ] || die "發布驗證未通過,請勿把網址交給使用者"

echo
echo "=== 發布完成 ==="
echo "  固定版本   : $BASE_URL/$REMOTE_SUBDIR/$NAME"
if [ "$UPDATE_LATEST" -eq 1 ]; then
  echo "  最新版本   : $BASE_URL/$REMOTE_SUBDIR/$RS_R2_ANDROID_LATEST"
fi
echo "  版本資訊   : $BASE_URL/$REMOTE_SUBDIR/version.json"
echo "  versionCode: $VERSION_CODE"
echo "  sha256     : $SHA256"
