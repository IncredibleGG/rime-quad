#!/usr/bin/env bash
#
# windows/verify_ime.sh — 經由**真的**具名管道驗證服務進程
#
# 這是本輪 CI 能做到的最強驗證。它走的路是:
#
#   rime_probe → 真的具名管道 → 真的線路格式 → rime_service
#              → rime_shell → librime → 真的詞庫
#
# 也就是 DLL 會走的每一段,除了 TSF 本身。而 probe 用的 IpcClient 就是
# DLL 用的**同一份原始碼** —— 另寫一個測試用戶端的話,驗到的是那一份,
# 不是產品裡的那一份。
#
# ⚠ 它驗不到:TSF 的註冊與組字、候選窗的樣子、VK_* → keysym 那一層
#   (probe 直接送 keysym)。那三件事分別由「只有人做得到」、
#   同上、以及 windows/tests/test_keymap.cc + test_win32_layouts.cc 承擔。
#   詳見 windows/README.md 的「沒有被驗證的部分」。
#
set -euo pipefail

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
ARCH="${ARCH:-x64}"
BUILD_ROOT="${ROOT}/third_party/build/windows-${ARCH}"
BIN="${BUILD_ROOT}/ime/bin"
WORK="${BUILD_ROOT}/ime-verify"

KEYS="nihao"; SELECT=1; EXPECT="你好"; SCHEMA="luna_pinyin_tw"
while [ $# -gt 0 ]; do
  case "$1" in
    --keys)   KEYS="$2"; shift 2 ;;
    --select) SELECT="$2"; shift 2 ;;
    --expect) EXPECT="$2"; shift 2 ;;
    --schema) SCHEMA="$2"; shift 2 ;;
    *) die "未知參數: $1" ;;
  esac
done

command -v cygpath >/dev/null 2>&1 || die "必須在 Git Bash / MSYS2 下執行"
# cygpath -m(正斜線)而不是 -w(反斜線):windows/verify_console.sh 餵給
# rime_console 的就是 -m 的形式,而那條路已經在這個 runner 上驗過很多次。
# 兩支驗證腳本走不同形式的路徑,等於在比較兩件不同的事。
w() { cygpath -m "$1"; }

[ -f "${BIN}/rime_service.exe" ] || die "找不到 ${BIN}/rime_service.exe;先跑 windows/build.sh ime"
[ -f "${BIN}/rime_probe.exe" ]   || die "找不到 ${BIN}/rime_probe.exe"
[ -d "${ROOT}/core/data/shared" ] \
  || die "缺少 core/data/shared。先跑 scripts/fetch_rime_data.sh 與 scripts/collect_data.sh"

rm -rf "${WORK}"
mkdir -p "${WORK}/user"
# 若 verify_console.sh 已經跑過,它的使用者目錄裡有編好的詞庫。
# 沿用可以省掉好幾分鐘的重新編譯 —— 那是 CI 上最貴的一步,
# 而這一支要驗的不是「詞庫編不編得起來」(那由 verify_console.sh 驗)。
if [ -d "${BUILD_ROOT}/verify/user" ]; then
  log "沿用 verify_console.sh 已編好的使用者目錄"
  cp -r "${BUILD_ROOT}/verify/user/." "${WORK}/user/" 2>/dev/null || true
else
  cp -r "${ROOT}/core/data/user/." "${WORK}/user/" 2>/dev/null || true
fi

READY="${WORK}/ready.txt"
SVC_LOG="${WORK}/service.log"

log "啟動 rime_service.exe(--no-ui)"
# --no-ui:CI 上沒有人在看螢幕,而建視窗失敗會把「服務起不來」和
#          「視窗建不起來」混成同一個症狀。
# --quit-after:就算後面哪一步掛了,這支服務也會自己結束,不留殘骸給下一輪。
"${BIN}/rime_service.exe" \
  --no-ui \
  --shared "$(w "${ROOT}/core/data/shared")" \
  --user "$(w "${WORK}/user")" \
  --wait-deploy 900 \
  --ready-file "$(w "${READY}")" \
  --quit-after 600 \
  > "${SVC_LOG}" 2>&1 &
SVC_PID=$!

cleanup() {
  if kill -0 "${SVC_PID}" 2>/dev/null; then
    kill "${SVC_PID}" 2>/dev/null || true
    wait "${SVC_PID}" 2>/dev/null || true
  fi
}
trap cleanup EXIT

log "等待服務就緒(首次部署要編譯詞庫,可能數分鐘)"
for i in $(seq 1 900); do
  [ -f "${READY}" ] && break
  if ! kill -0 "${SVC_PID}" 2>/dev/null; then
    echo "--- service.log ---"; cat "${SVC_LOG}"
    die "服務進程提前結束了"
  fi
  sleep 1
  [ $((i % 30)) -eq 0 ] && log "  ...已等 ${i}s"
done
[ -f "${READY}" ] || { cat "${SVC_LOG}"; die "服務在 900 秒內沒有就緒"; }
log "服務就緒"

set +e
# --schema 不可省。使用者目錄是從 verify_console.sh 沿用來的,而 librime 把
# 「上次選的方案」記在 user.yaml 裡 —— 不指定的話,這個測試驗到的是哪一個
# 方案取決於上一支腳本最後跑了什麼。實測:曾經因此用注音去打 nihao。
#
# ⚠ --attempts 1:「ready 檔存在」必須等於「立刻連得上」。
#   服務端已經把這件事變成真的(pipe_server.cc 的 Start() 會等監聽執行緒
#   回報管道備妥才返回,而 ready 檔是在那之後才寫的;引擎也在開管道之前
#   就預熱過了)。所以第一次就該成功 —— 需要重試才連得上,本身就是缺陷,
#   不可以用「多試幾次總會過」蓋掉。
"${BIN}/rime_probe.exe" --keys "${KEYS}" --select "${SELECT}" \
  --schema "${SCHEMA}" --expect "${EXPECT}" --attempts 1 \
  > "${WORK}/probe.log" 2>&1
rc=$?
set -e

# MSVC 的 CRT 在文字模式下把 \n 寫成 \r\n。留著 CR 的話底下的比對會失敗,
# 而訊息看起來會像「你好 != 你好」。
tr -d '\r' < "${WORK}/probe.log"

if [ "${rc}" -ne 0 ]; then
  echo "--- service.log ---"
  tr -d '\r' < "${SVC_LOG}" | grep -v -E '^[WIEF][0-9]{4,8} ' || true
  die "probe 以 ${rc} 結束"
fi

# 錨定整行精確比對。只用 grep -q "你好" 是不夠的:上屏成「你好嗎」一樣會通過。
if ! grep -qE "^>>> COMMIT: \"${EXPECT}\"$" <(tr -d '\r' < "${WORK}/probe.log"); then
  echo "--- service.log ---"
  tr -d '\r' < "${SVC_LOG}" | grep -v -E '^[WIEF][0-9]{4,8} ' || true
  die "probe 的輸出裡沒有 >>> COMMIT: \"${EXPECT}\""
fi

log "IPC 端到端驗證通過:${KEYS} → 「${EXPECT}」✓"

# ══════════════════════════════════════════════════════════════════
#  Ctrl+空白鍵:中英切換(服務那一半)
# ══════════════════════════════════════════════════════════════════
#
# 使用者回報「ctrl+ 空格沒辦法切中英文 這個應該是所有輸入法的基本配置」。
#
# ⚠ 這裡驗的是**服務那一半**:那顆鍵經由真的管道送進來之後,引擎有沒有
#   真的換模式,而且**行為**跟著換 —— 中文模式下那串字母會被吃掉(組字),
#   英數模式下不會。只驗旗標是不夠的:旗標變了而行為沒變,使用者打出來的
#   仍然是中文。
#
# ⚠ DLL 那一半(TSF 有沒有真的把 Ctrl+空白鍵攔下來)在 verify_tsf.sh。
set +e
"${BIN}/rime_probe.exe" --ascii-toggle --keys "${KEYS}" --schema "${SCHEMA}" \
  --attempts 1 > "${WORK}/ascii-toggle.log" 2>&1
rc=$?
set -e
tr -d '\r' < "${WORK}/ascii-toggle.log"
if [ "${rc}" -ne 0 ]; then
  echo "--- service.log ---"
  tr -d '\r' < "${SVC_LOG}" | grep -v -E '^[WIEF][0-9]{4,8} ' || true
  die "Ctrl+空白鍵的中英切換驗證以 ${rc} 結束"
fi
if ! grep -qE '^>>> ASCII TOGGLE OK$' <(tr -d '\r' < "${WORK}/ascii-toggle.log"); then
  die "ascii-toggle 的輸出裡沒有 >>> ASCII TOGGLE OK"
fi
log "Ctrl+空白鍵切得了中英(而且切得回來)✓"

# ══════════════════════════════════════════════════════════════════
#  「ready 檔存在 = 連得上」—— 反覆啟動,每次都要求第一下就連上
# ══════════════════════════════════════════════════════════════════
#
# ⚠ 這一段是為了一個**間歇性**的缺陷而存在的,所以它要跑很多次。
#
# 症狀:CI 的 install-x64 偶爾紅在「連不上服務或握手失敗」,而服務的日誌
# 乾乾淨淨、`[service] ready` 也印了。而同一個 commit 上一輪是綠的。
#
# 成因是「ready」這個字當時只代表「CreateThread 成功了」:
#   · PipeServer::Start() 建一個管道實例、**關掉**、開一條執行緒去重建,
#     然後在 CreateThread 回來的當下就回 true。那條執行緒可能一步都還沒跑。
#   · 重建時又帶了 FILE_FLAG_FIRST_PIPE_INSTANCE,剛關掉的那個實例只要
#     還沒被系統回收乾淨,這一次就會以 ERROR_ACCESS_DENIED 失敗 ——
#     而舊的監聽迴圈遇到失敗是直接 break,一個字都不印。
#
# 兩者都只在某些排程下才發生,所以**跑一次不算數**。這裡反覆啟動、
# 反覆要求「ready 檔一出現就要第一下連得上」,把那個窗口用次數逼出來。
# 詞庫已經編好了(沿用上面那一輪的使用者目錄),所以每一輪只有幾秒。
RESTARTS="${RESTARTS:-5}"
BIN_W="$(cygpath -w "${BIN}")"

# 先把上面那支停掉。⚠ 服務有單一實例的 mutex —— 沒停乾淨的話,
# 底下每一輪新起的服務都會判定「已經有一支在跑」然後以 0 靜靜結束,
# 而症狀會是「服務進程提前結束了」,看起來與這一段要驗的東西毫無關聯。
stop_service() {
  "${BIN}/rime_ime_setup.exe" stop-service --dir "${BIN_W}" \
    > "${WORK}/stop.log" 2>&1 || true
}
stop_service
cleanup() { stop_service; }
trap cleanup EXIT

log "反覆重啟 ${RESTARTS} 次,每次都要求「ready 檔一出現就連得上」"
for n in $(seq 1 "${RESTARTS}"); do
  R="${WORK}/ready-${n}.txt"; rm -f "${R}"
  "${BIN}/rime_service.exe" \
    --no-ui \
    --shared "$(w "${ROOT}/core/data/shared")" \
    --user "$(w "${WORK}/user")" \
    --wait-deploy 300 \
    --ready-file "$(w "${R}")" \
    --quit-after 120 \
    > "${WORK}/service-${n}.log" 2>&1 &
  PID=$!
  for i in $(seq 1 300); do
    [ -f "${R}" ] && break
    if ! kill -0 "${PID}" 2>/dev/null; then
      tr -d '\r' < "${WORK}/service-${n}.log" | grep -v -E '^[WIEF][0-9]{4,8} ' || true
      die "第 ${n} 輪:服務進程提前結束了(上一輪沒停乾淨?單一實例的 mutex 還在?)"
    fi
    sleep 1
  done
  [ -f "${R}" ] || { stop_service; die "第 ${n} 輪:服務在 300 秒內沒有就緒"; }

  set +e
  "${BIN}/rime_probe.exe" --connect-only --attempts 1 \
    > "${WORK}/connect-${n}.log" 2>&1
  crc=$?
  set -e
  stop_service
  wait "${PID}" 2>/dev/null || true

  if [ "${crc}" -ne 0 ]; then
    tr -d '\r' < "${WORK}/connect-${n}.log"
    echo "--- service.log(第 ${n} 輪)---"
    tr -d '\r' < "${WORK}/service-${n}.log" | grep -v -E '^[WIEF][0-9]{4,8} ' || true
    die "第 ${n} 輪:ready 檔已經存在,第一次連線卻失敗。
  「ready」的意思必須是「現在就連得上」。上面 probe 的診斷已經指出是
  哪一步失敗(開管道 / 握手 / 建 session),照那一步去查。"
  fi
  printf '  ✓ 第 %s 輪:ready → 立刻連上\n' "${n}"
done

# ── 反向測試:服務不在的時候,上面那道斷言必須紅,而且要說對話 ──────
#
# ⚠ 沒有這一段的話,「連上了」的綠燈不算數:一支永遠回 0 的 probe,
#   或一道其實沒有在連線的檢查,在報表上與真的連上長得一模一樣。
#   這個專案抓過太多次「測試是綠的,因為它沒在測」。
#
# 而且不只要求它非零 —— 還要求它**指到對的那一步**。這一輪修的正是
# 「一句話蓋掉三種不同的失敗」,所以診斷指錯地方等於沒修。
log "反向測試:服務不在時,--connect-only 必須紅在「開管道」那一步"
sleep 2
set +e
"${BIN}/rime_probe.exe" --connect-only --attempts 1 > "${WORK}/connect-none.log" 2>&1
nrc=$?
set -e
tr -d '\r' < "${WORK}/connect-none.log"
[ "${nrc}" -ne 0 ] || die "服務都停了,--connect-only 竟然以 0 結束 —— 這道檢查沒有在檢查"
if ! grep -q '失敗在「開管道」這一步' <(tr -d '\r' < "${WORK}/connect-none.log"); then
  die "服務不在的時候,probe 應該紅在「開管道」那一步並說出 GetLastError。
  它現在講的是別的 —— 診斷指錯方向,和舊版那句「連不上服務或握手失敗」
  一樣沒有用。"
fi
log "反向測試通過:診斷指到「開管道」,而且帶著錯誤碼 ✓"
