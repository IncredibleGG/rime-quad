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
"${BIN}/rime_probe.exe" --keys "${KEYS}" --select "${SELECT}" \
  --schema "${SCHEMA}" --expect "${EXPECT}" \
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
