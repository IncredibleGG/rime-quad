#!/usr/bin/env bash
#
# audit_offline.sh — 「離線為預設」的守門腳本
#
# ═══════════════════════════════════════════════════════════════════════════
#  為什麼要有這一支
# ═══════════════════════════════════════════════════════════════════════════
#
# 這個專案的定位是「無審查、安全、經得起審計」。支撐這句話的東西有一個共同
# 的弱點:**它們現在是對的,只是因為還沒有人把它們改壞。**
#
#   · 全 app 只有 net/NetworkGate.kt 碰得到 java.net
#   · 沒有任何 crash reporter(無 Firebase / Sentry / ACRA / Bugly / 友盟)
#   · 沒有 WebView
#   · 原生層沒有 socket / curl / getaddrinfo
#   · allowBackup=false,詞庫不會被系統偷偷同步到 Google Drive
#   · 送出的 User-Agent 不含這個 app 的名字
#   · 編進去的 Lua 直譯器移除了 os.execute / io.popen / package.loadlib
#
# 哪天有人為了除錯加一個 crash reporter、為了顯示說明頁塞一個 WebView、
# 或是把 librime-lua 升級後忘了重新套沙盒 patch,上面每一句話都會變成假的,
# **而且不會有任何人注意到** —— 沒有測試會紅,app 照常運作,只有使用者
# 在不知情的狀況下被多送出去一些東西。
#
# 所以這支腳本檢查的不是「功能對不對」,而是「我們對外講的話還算不算數」。
# 任何一項不過就 exit 1。它被接在 scripts/release_check.sh 的第 0 關,
# 發布前一定會跑到。
#
# ═══════════════════════════════════════════════════════════════════════════
#  allowBackup 的實測方法(第 5 項的由來,寫在這裡免得下次有人重新發明)
# ═══════════════════════════════════════════════════════════════════════════
#
# Android 的自動備份是**系統元件**代勞的,不經過我們的行程,所以
# NetworkGate 攔不到、連網紀錄也記不到。要驗它只能從外面看:
#
#   adb shell bmgr enable true
#   adb shell bmgr transport com.android.localtransport/.LocalTransport
#   adb shell monkey -p org.rimequad.ime -c android.intent.category.LAUNCHER 1
#     # ⚠ 一定要先啟動 app。被 force-stop 過的 app 會被判定
#     #   "Backup is not allowed",那是**假的通過**,不是我們的設定生效。
#   adb shell bmgr backupnow org.rimequad.ime
#   adb shell cat /data/data/com.android.localtransport/files/1/_full/org.rimequad.ime > b.tar
#   tar tvf b.tar
#
# allowBackup=true 時,實測(2026-08-08,emulator-5578,Android 35)tar 裡有:
#   apps/org.rimequad.ime/f/rime/user/luna_pinyin.userdb/*   使用者詞庫
#   apps/org.rimequad.ime/f/rime/user/installation.yaml      跨重裝穩定的 UUID
#   apps/org.rimequad.ime/f/rime/user/user.yaml              何時用過哪個方案
#   apps/org.rimequad.ime/f/rime/user/rimequad-store.json    裝了哪些第三方方案
#   apps/org.rimequad.ime/f/net/                             連網紀錄本身
# allowBackup=false 之後,同樣的指令回的是 "Backup is not allowed"。
#
# ═══════════════════════════════════════════════════════════════════════════
#  已知的未竟事項(不是這支腳本檢查得到的,但要有人記著)
# ═══════════════════════════════════════════════════════════════════════════
#
#   · 關掉 allowBackup 的代價是換手機時詞庫不會跟過去。誠實的替代方案是
#     **使用者自己匯出/匯入**(主動、看得見、去處由使用者決定)。
#     那個功能目前**還沒有實作**,在做出來之前,換機的使用者會失去詞庫。
#   · Lua 沙盒保留了 io.*(方案要讀自己的資料檔)。它開不了 socket,
#     但讀寫得到 app 私有目錄。這是相容性取捨,不是完整隔離。
#
# 用法:
#   ./audit_offline.sh            # 全部檢查
#   ./audit_offline.sh --verbose  # additionally 印出每一項比對到的內容
#
set -uo pipefail   # 刻意不用 -e:要跑完全部檢查再一次回報,而不是第一項就中斷

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERBOSE=0
[ "${1:-}" = "--verbose" ] && VERBOSE=1

PASS=0; FAIL=0
ok()   { echo "  [PASS] $*"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $*" >&2; FAIL=$((FAIL+1)); }
step() { echo; echo "=== $* ==="; }
note() { [ "$VERBOSE" -eq 1 ] && echo "         $*"; return 0; }

# 把命中的行印出來(最多 N 行),讓失敗訊息可以直接動手修而不必再 grep 一次。
show() { printf '%s\n' "$1" | sed -n "1,${2:-10}p" | sed 's/^/         /' >&2; }

SRC="$ROOT/android/app/src"
GATE_REL="main/java/org/rimequad/ime/net/NetworkGate.kt"
MANIFEST="$SRC/main/AndroidManifest.xml"
NETSEC="$SRC/main/res/xml/network_security_config.xml"

# ─────────────────────────────────────────────────────────────────────────────
step "1. 連網出口唯一(java.net 只能出現在 NetworkGate.kt)"
# 這一項就是 NetworkGate.kt 檔頭與 AndroidManifest 註解裡承諾「你自己 grep」
# 的那一條。承諾寫在使用者看得到的地方,就得有東西保證它不會慢慢變成謊話。
#
# 含測試程式碼一起檢查:測試裡開一條真的連線同樣會讓「單一出口」破功。
#
# ⚠ 註解要排除,否則這支腳本永遠是紅的:NetworkGate.kt 的檔頭、
#   NetworkAudit.kt、測試的 KDoc 都**刻意**在文字裡寫了這些 token
#   (「本檔沒有任何 java.net」之類的說明)。一支永遠紅的守門腳本會被
#   當成雜訊直接略過,那比沒有還糟。
#   判準是「整行是註解」(開頭為 // 或 * 或 /* 或 <!--),程式碼行上的
#   行尾註解仍然會被抓到 —— 寧可保守。
NET_TOKENS='java\.net|javax\.net|HttpURLConnection|URLConnection|openConnection|[^a-zA-Z]URL\(|Socket\(|ServerSocket|DatagramSocket|InetAddress|OkHttp|okhttp3|Retrofit|retrofit2|io\.ktor|HttpClient|volley'
strip_comments() { grep -vE '^[^:]*:[0-9]+: *(//|\*|/\*|<!--)' || true; }
HITS="$(grep -rnE "$NET_TOKENS" "$SRC" --include='*.kt' --include='*.java' 2>/dev/null | strip_comments)"
OFFENDERS="$(printf '%s\n' "$HITS" | grep -v "^$SRC/$GATE_REL:" | grep -v '^$' || true)"
if [ -z "$OFFENDERS" ]; then
  N="$(printf '%s\n' "$HITS" | grep -c . || true)"
  ok "只有 NetworkGate.kt 碰得到連網 API(該檔內 $N 處)"
else
  bad "NetworkGate.kt 以外出現了連網 API —— 「全 app 唯一出口」不再成立"
  show "$OFFENDERS" 15
fi

# ─────────────────────────────────────────────────────────────────────────────
step "2. 相依裡沒有 crash reporter / 分析 / 網路函式庫"
# crash reporter 是最容易被「只是為了除錯」加進來的東西,而它送出去的
# stack trace 會夾帶裝置識別碼與使用情境,對這個專案的使用者是實質風險。
# 中國市場常見的統計 SDK 一併列進來 —— 目標使用者正是最不該被它們裝上的人。
DEP_BAD='firebase|crashlytics|sentry|bugsnag|acra|appcenter|instabit|okhttp|retrofit|volley|ktor|apache\.http|google-analytics|play-services-analytics|play-services-measurement|amplitude|mixpanel|matomo|flurry|umeng|bugly|jpush|getui|umsdk|oaid|msa-|tinker|doraemon'
DEP_FILES="$ROOT/android/gradle/libs.versions.toml $ROOT/android/app/build.gradle.kts $ROOT/android/build.gradle.kts $ROOT/android/settings.gradle.kts"
DEP_HITS=""
for f in $DEP_FILES; do
  [ -f "$f" ] || continue
  H="$(grep -inE "$DEP_BAD" "$f" 2>/dev/null | sed "s|^|$(basename "$f"):|" || true)"
  [ -n "$H" ] && DEP_HITS="${DEP_HITS}${H}"$'\n'
done
DEP_HITS="$(printf '%s' "$DEP_HITS" | grep -v '^$' || true)"
if [ -z "$DEP_HITS" ]; then
  ok "相依清單乾淨(無 crash reporter / 分析 / 第三方 HTTP 函式庫)"
else
  bad "相依裡出現了會自己連網或回報的函式庫"
  show "$DEP_HITS" 10
fi

# ─────────────────────────────────────────────────────────────────────────────
step "3. 沒有 WebView"
# WebView 自己就是一個完整的網路堆疊,而且它的連線不經過 NetworkGate,
# 連 network_security_config 以外的行為(第三方 cookie、快取)也難以稽核。
WV_HITS="$(grep -rnE 'WebView|WebSettings|WebViewClient|WebChromeClient|CookieManager' \
            "$SRC" --include='*.kt' --include='*.java' --include='*.xml' 2>/dev/null || true)"
if [ -z "$WV_HITS" ]; then
  ok "原始碼裡沒有任何 WebView"
else
  bad "出現 WebView —— 那是一條繞過 NetworkGate 的完整網路堆疊"
  show "$WV_HITS" 10
fi

# ─────────────────────────────────────────────────────────────────────────────
step "4. 原生層沒有 socket / curl / DNS"
# 原生層是最容易藏東西的地方:Kotlin 那側 grep 得乾乾淨淨,一個 .cc 裡的
# socket() 就足以讓整個定位破功,而且審計的人多半只看 Java/Kotlin。
NATIVE_DIRS=""
[ -d "$ROOT/android/app/src/main/cpp" ] && NATIVE_DIRS="$NATIVE_DIRS $ROOT/android/app/src/main/cpp"
[ -d "$ROOT/core/src" ]                 && NATIVE_DIRS="$NATIVE_DIRS $ROOT/core/src"
[ -d "$ROOT/core/include" ]             && NATIVE_DIRS="$NATIVE_DIRS $ROOT/core/include"
NATIVE_BAD='socket\(|getaddrinfo|gethostbyname|inet_addr|inet_pton|curl_easy|curl/curl\.h|sys/socket\.h|netinet/|arpa/inet\.h|netdb\.h|SSL_new|SSL_CTX'
NAT_HITS="$(grep -rnE "$NATIVE_BAD" $NATIVE_DIRS 2>/dev/null || true)"
if [ -z "$NAT_HITS" ]; then
  ok "我們自己的原生碼沒有網路呼叫($(echo $NATIVE_DIRS | wc -w) 個目錄)"
else
  bad "原生層出現網路呼叫 —— NetworkGate 完全看不到這一條路"
  show "$NAT_HITS" 10
fi

# 進階(有 .so 才做):直接看最終產物的動態符號。這比 grep 原始碼強,
# 因為它連「靜態連進來的第三方函式庫偷偷帶了 socket」都抓得到。
SO="$(find "$ROOT/android/app/build/intermediates" -name 'librime_jni.so' 2>/dev/null | head -1)"
if [ -n "$SO" ] && command -v llvm-readelf >/dev/null 2>&1; then
  SYMS="$(llvm-readelf --dyn-syms "$SO" 2>/dev/null | grep -E ' UND .*(socket|connect|getaddrinfo|gethostbyname)$' || true)"
  if [ -z "$SYMS" ]; then
    ok ".so 的動態符號裡沒有 socket/connect/getaddrinfo($(basename "$SO"))"
  else
    bad ".so 需要網路相關的 libc 符號"
    show "$SYMS" 10
  fi
else
  note "略過 .so 符號檢查(找不到 librime_jni.so 或 llvm-readelf)"
fi

# ─────────────────────────────────────────────────────────────────────────────
step "5. allowBackup 必須是 false"
# 見本檔開頭的實測紀錄:allowBackup=true 時使用者詞庫會被系統同步到
# Google Drive,而且**完全不經過 NetworkGate,連網紀錄一筆都不會有**。
if [ ! -f "$MANIFEST" ]; then
  bad "找不到 AndroidManifest.xml"
elif grep -q 'android:allowBackup="false"' "$MANIFEST"; then
  if grep -q 'tools:replace="android:allowBackup"' "$MANIFEST"; then
    ok "manifest: allowBackup=false,且有 tools:replace(相依函式庫蓋不掉)"
  else
    bad "allowBackup=false 但缺 tools:replace —— manifest 合併時可能被相依蓋掉"
  fi
else
  bad "manifest 的 allowBackup 不是 false:使用者詞庫會被系統同步到雲端備份"
  # 只印真正的屬性行,不要把解釋用的註解一起倒出來 —— 失敗訊息要能直接動手。
  show "$(grep -nE '^[[:space:]]*android:allowBackup=' "$MANIFEST" \
          || echo '(找不到 android:allowBackup 屬性 —— 沒寫就是預設 true)')" 5
fi

# APK 在的話連編出來的結果一起驗 —— 「原始碼寫對」和「使用者手上那份是對的」
# 是兩件事,manifest 合併、build type 覆寫都可能讓它們不一致。
APK="$ROOT/android/app/build/outputs/apk/debug/app-debug.apk"
AAPT="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}/build-tools/35.0.0/aapt2"
if [ -f "$APK" ] && [ -x "$AAPT" ]; then
  X="$("$AAPT" dump xmltree --file AndroidManifest.xml "$APK" 2>/dev/null || true)"
  AB="$(printf '%s\n' "$X" | grep -o 'allowBackup([^)]*)=[a-z]*' | head -1)"
  case "$AB" in
    *=false) ok "APK 實際打進去的也是 allowBackup=false" ;;
    "")      bad "APK 的 manifest 裡沒有 allowBackup(等於預設 true)" ;;
    *)       bad "APK 實際打進去的是 $AB" ;;
  esac
else
  note "略過 APK 檢查(還沒編或找不到 aapt2)"
fi

# ─────────────────────────────────────────────────────────────────────────────
step "6. 不允許明文 HTTP(連 loopback 都不留)"
# loopback 例外會讓「同一台裝置上的另一個 app 開本機 server 冒充索引」變得可行。
# 本專案側載發布的是 debug 建置,所以「只在 debug 留著」等於發給每一個使用者。
# XML 註解要先剝掉:這個檔的註解本身就在解釋「不要寫
# cleartextTrafficPermitted=true」,連註解一起 grep 會永遠是紅的。
xml_no_comments() { python3 -c '
import re,sys
sys.stdout.write(re.sub(r"<!--.*?-->", "", open(sys.argv[1], encoding="utf-8").read(), flags=re.S))
' "$1" 2>/dev/null; }
if [ ! -f "$NETSEC" ]; then
  bad "找不到 network_security_config.xml"
elif xml_no_comments "$NETSEC" | grep -q 'cleartextTrafficPermitted="true"'; then
  bad "network_security_config.xml 開了明文例外 —— 發布用的設定不該有"
  show "$(xml_no_comments "$NETSEC" | grep -n 'cleartextTrafficPermitted="true"')" 10
else
  ok "network_security_config.xml 全面禁止明文"
fi
if grep -q 'usesCleartextTraffic="true"' "$MANIFEST" 2>/dev/null; then
  bad "manifest 開了 usesCleartextTraffic=true"
else
  ok "manifest 沒有 usesCleartextTraffic=true"
fi

# ─────────────────────────────────────────────────────────────────────────────
step "7. User-Agent 不自報家門"
# 在有審查的網路環境下,一個 rimequad-android 的 UA 就足以讓使用者被標記為
# 「正在使用這個輸入法」,被動觀察者連解密都不必。
GATE="$SRC/$GATE_REL"
if [ ! -f "$GATE" ]; then
  bad "找不到 NetworkGate.kt"
else
  UA="$(grep -E 'val +USER_AGENT' "$GATE" | head -1 | sed 's/.*= *//' || true)"
  if [ -z "$UA" ]; then
    note "NetworkGate.kt 沒有 USER_AGENT 常數"
    ok "沒有自訂 User-Agent"
  elif printf '%s' "$UA" | grep -qiE 'rime|quad|ime'; then
    bad "User-Agent 含有本專案的名字,等於向網路自報家門: $UA"
  else
    ok "User-Agent 不含專案名稱: $UA"
  fi
  # 順便確認沒有把裝置資訊塞進任何請求標頭
  BUILD_HDR="$(grep -nE 'setRequestProperty\(.*Build\.' "$GATE" || true)"
  if [ -z "$BUILD_HDR" ]; then
    ok "請求標頭沒有夾帶 Build.* 的裝置資訊"
  else
    bad "請求標頭夾帶了裝置資訊"; show "$BUILD_HDR" 5
  fi
fi

# ─────────────────────────────────────────────────────────────────────────────
step "8. Lua 沙盒仍在(第三方方案不能跳出行程)"
# 方案市集下載的是第三方 Lua,而 librime-lua 的 modules.cc 在**模組初始化**時
# 就會 dofile 使用者資料目錄下的 rime.lua —— 使用者根本還沒選到那個方案。
# 沒有沙盒的話,那些腳本可以 os.execute / io.popen / package.loadlib,
# 自己開 socket 完全繞過 NetworkGate。
SANDBOX_PATCH="$ROOT/patches/librime-lua@sandbox.patch"
LUA_CC="$ROOT/third_party/librime-lua/src/lib/lua.cc"
if [ ! -f "$SANDBOX_PATCH" ]; then
  bad "找不到 patches/librime-lua@sandbox.patch —— 沙盒的來源不見了"
else
  ok "沙盒 patch 存在"
fi
if [ ! -f "$LUA_CC" ]; then
  note "third_party/librime-lua 還沒抓下來,略過「已套用」檢查"
else
  # ⚠ 一定要排除被註解掉的行。第一版沒排除,結果把
  #   `-- package.loadlib = nil`(有人把沙盒關掉了)當成「還在」而放行 ——
  #   這個漏洞是靠故意植入違規的演練抓到的,不是靠讀這支腳本。
  #   Lua 的行註解是 --,C++ 那側是 //。
  MISSING=""
  for tok in 'kRimeQuadSandbox' 'os\[k\] = nil' 'package\.loadlib = nil' \
             'io\.popen = nil' 'package\.searchers\[3\] = nil' \
             'debug\[k\] = nil' 'chunkname, "t"'; do
    grep -E "$tok" "$LUA_CC" | grep -qvE '^[[:space:]]*(--|//)' \
      || MISSING="$MISSING ${tok//\\/}"
  done
  if [ -z "$MISSING" ]; then
    ok "librime-lua 的 lua.cc 已套用沙盒"
  else
    bad "lua.cc 的沙盒不完整,缺:$MISSING(升級 librime-lua 後忘了重套 patch?)"
  fi
  # 沙盒必須在 luaL_openlibs 之後、rime.lua 之前生效。順序錯了等於沒做。
  if [ -n "$(grep -n 'luaL_openlibs' "$LUA_CC")" ]; then
    L_OPEN="$(grep -n 'luaL_openlibs' "$LUA_CC" | head -1 | cut -d: -f1)"
    L_SBOX="$(grep -n 'luaL_dostring(L, kRimeQuadSandbox' "$LUA_CC" | head -1 | cut -d: -f1)"
    if [ -n "$L_SBOX" ] && [ "$L_SBOX" -gt "$L_OPEN" ]; then
      ok "沙盒安裝在 luaL_openlibs 之後(第 $L_OPEN -> $L_SBOX 行)"
    else
      bad "沙盒沒有安裝在 luaL_openlibs 之後 —— 順序錯了等於沒做"
    fi
  fi
fi

# ─────────────────────────────────────────────────────────────────────────────
step "9. 沒有多出來的權限"
# 權限清單是使用者第一個會查的東西。這裡把「我們承認的」寫死,
# 多出任何一個都要有人明確決定,而不是某個相依悄悄帶進來。
#
# 只看 <uses-permission>(app 主動要的)。service 上的
# android:permission="android.permission.BIND_INPUT_METHOD" 是**反過來的**:
# 那是規定「只有系統才能綁定我們」的限制,不是我們要到的權限,
# 少了它任何 app 都能綁我們的輸入法服務。把它算成違規是搞反了。
ALLOWED='INTERNET|ACCESS_NETWORK_STATE|REQUEST_INSTALL_PACKAGES'
PERMS="$(grep -E '<uses-permission' "$MANIFEST" 2>/dev/null \
          | grep -oE 'android\.permission\.[A-Z_]+' | sort -u || true)"
EXTRA="$(printf '%s\n' "$PERMS" | grep -vE "$ALLOWED" | grep -v '^$' || true)"
if [ -z "$EXTRA" ]; then
  ok "權限只有 INTERNET / ACCESS_NETWORK_STATE / REQUEST_INSTALL_PACKAGES"
else
  bad "manifest 出現了未列入白名單的權限"
  show "$EXTRA" 10
fi

# ─────────────────────────────────────────────────────────────────────────────
echo
echo "============================================"
echo " 離線稽核:通過 $PASS 項,失敗 $FAIL 項"
echo "============================================"
if [ "$FAIL" -gt 0 ]; then
  echo "「離線為預設、無審查、經得起審計」這句話現在不成立。修好再發布。" >&2
  exit 1
fi
echo "定位仍然站得住。"
