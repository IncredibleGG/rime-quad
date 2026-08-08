#!/usr/bin/env bash
#
# ci_version_code.sh — 算出這次建置該用的 versionCode,印在 stdout。
#
# ── 為什麼需要它 ────────────────────────────────────────────────────────
# android/app/build.gradle.kts 從 **HEAD commit 的時間**推導 versionCode,
# 格式 `yyMMddHH`(精確到小時)。那個設計是為了「同一個 commit 在任何機器
# 建都得到同一個號碼」,很好 —— 但它有一個已知限制:**同一小時內的兩個
# commit 拿到同一個號碼**。
#
# 手動側載的節奏撞不到,CI 一定撞得到:CI 就是為了讓人一小時內推好幾次。
# 而撞到的後果不是「版本號長得一樣」而已 —— publish_apk.sh 的單調性護欄
# 會擋下發布(versionCode 不大於線上的),於是一小時內第二次發版直接失敗。
# 剛才就撞到了。
#
# ── 解法 ────────────────────────────────────────────────────────────────
# 不改 gradle 的推導(那是 android/ 的檔案,而且那個設計本身沒錯),
# 改在 CI 用 `-Prime.versionCode=<n>` 覆寫 —— 那個覆寫入口 build.gradle.kts
# 本來就留好了。取的值是:
#
#     max(HEAD commit 時間推導值, 線上已發布的 version_code + 1)
#
# 為什麼不是「加上 run number」:run number 是單調的,但它與時間戳沒有
# 共同的量綱,把兩者相加(或相乘)遲早produce 出一個比舊版小的數字,
# 而那個失敗會在使用者按下「更新」時才出現。
# 直接以**已經發出去的那個號碼**為基準往上加一,是唯一保證單調的作法,
# 而且自我修復:不管之前發生過什麼,下一個號碼永遠比線上的大。
#
# 上限不成問題:versionCode 是 32 位有號整數(上限 2147483647),
# 而 `yyMMddHH` 到 2099 年最大也只有 99123123。一小時內要發到溢位得發一億次。
#
# 用法:
#   ./ci_version_code.sh                 # 用預設的線上 version.json
#   ./ci_version_code.sh --dir rime/test # 以測試路徑的線上版本為基準
#   ./ci_version_code.sh --base 26080807 # 明確指定推導基準(測試用)
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE_URL="${RIME_BASE_URL:-https://pub-d6a54d2e5f5947e2b0b23fb8e27ce0a5.r2.dev}"
SUBDIR="rime"
BASE=""

while [ $# -gt 0 ]; do
  case "$1" in
    --dir)  SUBDIR="$2"; shift 2 ;;
    --base) BASE="$2"; shift 2 ;;
    -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
    *) echo "未知參數: $1" >&2; exit 2 ;;
  esac
done

die() { echo "ci_version_code: $*" >&2; exit 1; }

if [ -z "$BASE" ]; then
  BASE="$(cd "$ROOT" && TZ=UTC git log -1 --format=%cd --date=format-local:%y%m%d%H 2>/dev/null || true)"
fi
case "$BASE" in
  [0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]) ;;
  *) die "推導不出 commit 時間版本號(得到 '$BASE')。沒有 git?" ;;
esac
BASE=$((10#$BASE))

# 線上那一份是唯一的基準:使用者手上裝的就是它。讀不到就只用 commit 推導值
# —— 但要說出來,因為那代表單調性這一次沒有被檢查到。
PUBLISHED="$(curl -sS --max-time 20 "$BASE_URL/$SUBDIR/version.json" 2>/dev/null \
  | sed -n 's/.*"version_code"[[:space:]]*:[[:space:]]*\([0-9]*\).*/\1/p' | head -1 || true)"

CODE="$BASE"
if [ -n "$PUBLISHED" ]; then
  if [ "$BASE" -le "$PUBLISHED" ]; then
    CODE=$((PUBLISHED + 1))
    echo "ci_version_code: commit 推導值 $BASE 不大於線上的 $PUBLISHED(同一小時內的第二次建置)," \
         "改用 $CODE" >&2
  else
    echo "ci_version_code: commit 推導值 $BASE > 線上的 $PUBLISHED,直接用" >&2
  fi
else
  echo "ci_version_code: [警告] 讀不到 $BASE_URL/$SUBDIR/version.json," \
       "這次沒有對線上版本做單調性檢查" >&2
fi

[ "$CODE" -lt 2147483647 ] || die "versionCode $CODE 溢位"
printf '%s\n' "$CODE"
