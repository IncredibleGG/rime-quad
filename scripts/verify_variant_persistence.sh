#!/usr/bin/env bash
#
# verify_variant_persistence.sh — 簡繁到底有沒有真的送到引擎(而且留得住)
#
# 使用者實機回報:設定裡選了簡體,狀態列那一格畫「简」,而打出來是繁體。
# 這支腳本把那條缺陷的**引擎側事實**釘在一個自動化跑得到的地方。
#
# ── 為什麼在 Android 模擬器上跑 ──────────────────────────────────
#
# 缺陷本身在 Windows 端,而 windows/service/ 在這台開發機上編不起來
# (只有 GitHub Actions 的 windows-latest 編得動,一輪十幾分鐘)。
# 但這條缺陷的根**不在 Windows**,在 librime 與 core/src/rime_shell.cc:
#
#   · 換方案會把 switches 重設回方案宣告的值
#     (ConcreteEngine::InitializeOptions,只設有 `reset:` 的那些)
#   · rs_set_option 對一個**不存在**的選項不會失敗,會原樣記下、原樣回讀
#   · 本專案打包的方案通通沒有 `simplification`,用的是一組互斥的 radio
#
# 這三件事在模擬器上用純 librime + rime_shell 驗得到,不必經過任何
# Windows API,而且是分鐘級的。
#
# ── ⚠ 這支腳本**證不到**的事 ────────────────────────────────────
#
# 它跑不到 windows/service/engine.cc。它證的是「裸 select_schema 會洗掉
# 簡繁」與「選完立刻重套就留得住」這兩個引擎側事實;engine.cc 走的是
# 哪一種,由 windows/audit_single_source.sh 在**原始碼層面**守
# (rs_select_schema 只能有一個裸呼叫點,而且必須在 SelectAndApply 裡)。
# 兩件事合起來才是完整的守門,單獨任何一件都不是。
#
# ── ⚠ 每次都用全新的 user 目錄 ──────────────────────────────────
#
# shared/default.yaml 的 `switcher/save_options` 列著 zh_hant / zh_hans /
# zh_hant_tw(以及 simplification)。librime 會記住它們,**同一個行程裡
# 後來建的 session 會把它們吃回去**。用既有的 user 目錄跑,基準那一段
# 看到的就不是「剛載入」的樣子,而是上一次跑剩下的狀態。
#
# 用法:
#   RIME_SERIAL=emulator-5558 scripts/verify_variant_persistence.sh
#   scripts/verify_variant_persistence.sh --serial emulator-5558
#
# ⚠ 不要 adb root。這支腳本只用 /data/local/tmp,不需要。
#
# ⛔ **這一支從前把 `SERIAL` 寫死成 `emulator-5558`**,不讀 RIME_SERIAL、
#    不走 `rs_pick_serial`、也沒有過 `rs_assert_destructive_ok` —— 而它第 106
#    行是 `adb -s "$SERIAL" shell rm -rf …`。這台機器上長期有三到四台模擬器,
#    5558 哪天換成別條線的,這一支就會安靜地刪別人的目錄。
#    上一輪宣稱「全樹再 grep 一次」而漏掉了它,所以現在有一支**跑得起來的**
#    守門在掃:`scripts/verify_device_hygiene.sh`。
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
# shellcheck source=lib/device.sh
. "$HERE/lib/device.sh"
SDK="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
ADB="$SDK/platform-tools/adb"
NDK_DIR="${ANDROID_NDK_HOME:-}"
SERIAL=""
ABI="x86_64"
DEV_ROOT="/data/local/tmp/rime"
KEEP=0

while [ $# -gt 0 ]; do
  case "$1" in
    --serial) SERIAL="$2"; shift 2 ;;
    --abi) ABI="$2"; shift 2 ;;
    --keep) KEEP=1; shift ;;
    -h|--help) sed -n '2,45p' "$0"; exit 0 ;;
    *) echo "不認得的參數:$1" >&2; exit 2 ;;
  esac
done

[ -x "${ADB}" ] || { echo "找不到 adb:${ADB}" >&2; exit 2; }
# ⛔ `--serial` 也要算「指名」。閘從前只看環境變數,於是這一行帶 `--serial`
#   就必死(RC=2,訊息說「是自動選來的」而那台正是命令列指名的)。
#   `rs_select_device` 把來源(flag / env / auto)一起記下來給閘看。
rs_select_device "${ADB}" "${SERIAL}" || exit 2
SERIAL="${RS_SERIAL}"

if [ -z "${NDK_DIR}" ]; then
  NDK_DIR="$(ls -d "${SDK}"/ndk/* 2>/dev/null | sort -V | tail -1 || true)"
fi
[ -n "${NDK_DIR}" ] || { echo "找不到 NDK(設 ANDROID_NDK_HOME)" >&2; exit 1; }

PREBUILT="${ROOT}/third_party/prebuilt/${ABI}"
if [ ! -f "${PREBUILT}/lib/librime.a" ]; then
  echo "缺 ${PREBUILT}/lib/librime.a —— 先跑 scripts/build_native.sh" >&2
  exit 1
fi

CXX="${NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/bin/${ABI}-linux-android24-clang++"
[ -x "${CXX}" ] || { echo "找不到 ${CXX}" >&2; exit 1; }

OUT="${ROOT}/build/verify-variant"
mkdir -p "${OUT}"

echo "==> 編譯探針(${ABI})"
# 連結順序照 third_party/prebuilt/manifest.json 的 link_order,
# 而且整包夾在 --start-group/--end-group 裡 —— 順序寫錯就整包連不起來。
"${CXX}" -std=c++17 -O1 -static-libstdc++ \
  -I"${ROOT}/core/include" -I"${PREBUILT}/include" \
  "${ROOT}/scripts/variant_probe.cc" "${ROOT}/core/src/rime_shell.cc" \
  -Wl,--start-group \
    "${PREBUILT}/lib/librime.a" "${PREBUILT}/lib/libopencc.a" \
    "${PREBUILT}/lib/libmarisa.a" "${PREBUILT}/lib/libleveldb.a" \
    "${PREBUILT}/lib/libyaml-cpp.a" "${PREBUILT}/lib/libglog.a" \
  -Wl,--end-group -llog -lm -o "${OUT}/variant_probe"

"${ADB}" -s "${SERIAL}" get-state >/dev/null

# ⚠ 這一支會 `rm -rf` 裝置上的目錄(cleanup 那一段),而且會 push 執行檔。
#   自動選來的那一台不准被動 —— 與 run_console_test.sh 同一條理由。
rs_assert_destructive_ok "${ADB}" "${SERIAL}" "rm -rf ${DEV_ROOT}/user_variant_*" || exit 2
rs_write_device_stamp "${ADB}" "${SERIAL}" "${OUT}/device.txt"

if ! "${ADB}" -s "${SERIAL}" shell "[ -d ${DEV_ROOT}/shared ] && echo ok" | grep -q ok; then
  echo "裝置上沒有 ${DEV_ROOT}/shared —— 先跑 scripts/collect_data.sh 並推上去" >&2
  exit 1
fi

# ⚠ 全新的 user 目錄。理由見檔頭(switcher/save_options)。
USER_DIR="${DEV_ROOT}/user_variant_$$"
SOLO_DIR="${DEV_ROOT}/user_variant_solo_$$"
cleanup() {
  [ "${KEEP}" -eq 1 ] && return 0
  "${ADB}" -s "${SERIAL}" shell rm -rf "${USER_DIR}" "${SOLO_DIR}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "==> 推上去(${SERIAL})"
"${ADB}" -s "${SERIAL}" push "${OUT}/variant_probe" /data/local/tmp/variant_probe >/dev/null
"${ADB}" -s "${SERIAL}" shell chmod 755 /data/local/tmp/variant_probe

# ── 側寫一:出貨設定 ────────────────────────────────────────────
#
# default.custom.yaml 要跟過去:上游 default.yaml 列的方案多於本專案實際
# 打包的,少了這個 patch,部署會因為 missing input schema 而以失敗收場。
"${ADB}" -s "${SERIAL}" shell mkdir -p "${USER_DIR}"
"${ADB}" -s "${SERIAL}" shell "cp ${DEV_ROOT}/user/default.custom.yaml ${USER_DIR}/ 2>/dev/null || true"

# ── 側寫二:只有 luna_pinyin ────────────────────────────────────
#
# 情境 0b 要的是**方案自己**的事實(那組 radio 沒有 reset:),而在出貨設定
# 下看不到 —— schema_list 第一項是 luna_pinyin_tw,它把 zh_hant_tw 設成
# 真,而 zh_hant_tw 在 switcher/save_options 裡,會跟著跑到 luna_pinyin 上。
"${ADB}" -s "${SERIAL}" shell mkdir -p "${SOLO_DIR}"
"${ADB}" -s "${SERIAL}" shell "printf 'patch:\n  schema_list:\n    - schema: luna_pinyin\n' > ${SOLO_DIR}/default.custom.yaml"

run_probe() {  # $1=user_dir  $2=標題  $3=額外參數
  local log="$2"
  echo
  echo "==> ${3:-出貨設定}(user=$1)"
  set +e
  "${ADB}" -s "${SERIAL}" shell \
    "/data/local/tmp/variant_probe ${DEV_ROOT}/shared $1 ${4:-}; echo RC=\$?" \
    2>&1 | tee "${log}"
  set -e
  # ⚠ adb shell 的結束碼不可靠(它回的是 adb 自己的),所以判準是探針
  #   自己印出來的那一行。抓不到那一行本身就是失敗 ——
  #   「沒有輸出」不算通過。
  grep -q '^RC=0' "${log}"
}

bad=0
run_probe "${USER_DIR}" "${OUT}/run.log" "出貨設定:基準 + A/B/C" || bad=1
run_probe "${SOLO_DIR}" "${OUT}/run-solo.log" "只有 luna_pinyin:那組 radio 沒有 reset:" solo || bad=1

if [ "${bad}" -ne 0 ]; then
  echo
  echo "!! 簡繁沒有留住,或 rs_status.variant 回報的不是引擎實際套用的字形。" >&2
  echo "   完整輸出:${OUT}/run.log、${OUT}/run-solo.log" >&2
  exit 1
fi

echo
echo "==> 通過。判準只看第 3、4 個候選(逆号/拟好 vs 逆號/擬好)——"
echo "    你好 / 妳好 / 你 在簡繁兩套字集裡長得一樣,拿它們斷言等於沒斷言。"
