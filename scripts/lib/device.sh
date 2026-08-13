# shellcheck shell=bash
#
# device.sh — **唯一**的裝置選擇入口。fail-closed:寧可不跑,也不要跑在別人的機器上。
#
# ── 為什麼需要這一支 ────────────────────────────────────────────────────
# 每一支裝置端腳本原本各自寫著
#
#     SERIAL="${RIME_SERIAL:-${ANDROID_SERIAL:-emulator-${RIME_EMU_PORT:-5554}}}"
#
# 或者「抓 adb devices 的第一台」。這台建置機上長期有三到四台模擬器在跑,
# `adb devices` 以 port 升冪列出,所以**兩種寫法都會落在 emulator-5554 上**。
#
# 猜錯的代價不是「腳本失敗」,是:
#
#   · `pm clear` 清掉**別條線**正在用的那台上的 app 資料(實測發生過一次);
#   · `ime set` 改掉那台的系統預設輸入法;
#   · 量到的是那台上的**舊 APK** —— 於是整輪綠燈與紅燈都是別人的,
#     而 artifact 裡沒有任何一個檔案記著序號,事後看不出來。
#
# ── ⛔ 這裡沒有預設 port,而且不會有 ──────────────────────────────────
# `docs/emulator.md` 曾經寫著「同時只跑一台模擬器時 serial 固定是
# emulator-5554」。那句話從寫下的那天起就不成立,而它正是所有腳本把 5554
# 當預設的正當性來源。**port 不是身分**,實測:
#
#     -avd rime_test    -port 5554   (Aug  8)
#     -avd lumina_test  (無 -port)   (Aug 11) → 落在 5556
#     -avd lumina_test2 -port 5558   (Aug 13)
#
# 任何一台重開,port 就會換人。身分是 AVD 名字,問法:
#
#     adb -s <serial> emu avd name
#     adb -s <serial> shell getprop ro.boot.qemu.avd_name
#
# 用法
# ─────────────────────────────────────────────────────────────────────
#     . "$HERE/lib/device.sh"
#     # 有 --serial 的腳本(閘要看得到命令列指名):
#     rs_select_device "$ADB" "$SERIAL_FROM_FLAG" || exit 2
#     SERIAL="$RS_SERIAL"
#     # 沒有 --serial 的腳本:
#     SERIAL="$(rs_pick_serial "$ADB")" || exit 2
#     ...
#     rs_assert_destructive_ok "$ADB" "$SERIAL" "pm clear / ime set" || exit 2
#     rs_write_device_stamp "$ADB" "$SERIAL" "$OUT_DIR/device.txt" "$APK" "$IME_PKG"
#
# ── ⚠ sha256 能證什麼、不能證什麼 ────────────────────────────────────
# 本專案的 debug APK **不是 reproducible build**:同一份原始碼重建兩次,
# `apk_sha256` 不一樣(簽章時間戳、`BuildConfig` 的建置時間、dex 合併順序、
# `versionCode` 由 `scripts/ci_version_code.sh` 依當下時間算)。所以這個雜湊
#
#   · **證得了**:「這一輪腳本量的就是這一份檔案」——
#     artifact 裡的截圖與這個雜湊對得起來,事後覆核得動。
#   · **證不了**:「我這一份與你報告裡那一份是同一份原始碼」。
#     兩個人拿同一個 commit 各自建,雜湊必定不同 —— **不要拿它比對別人的報告**,
#     那會得出「版本不一樣」這個假結論。要比原始碼就比 commit。
#
# 所以這一支同時記 `pkg_version` / `pkg_version_code` / `pkg_last_update`:
# 那三個在「量的是不是我剛裝上去的那一份」這個問題上比雜湊好用。

# ── 序號的**來源**:誰指名的 ────────────────────────────────────────────
#
# ⛔ 2026-08-13 的實測:`verify_input_matrix.sh --serial emulator-5558` 回 RC=2,
#    訊息是「emulator-5558 是自動選來的,不是你指定的 —— 中止」,而那台正是
#    使用者在命令列上**指名**的。原因是 `rs_assert_destructive_ok` 的閘只看
#    環境變數 `RIME_SERIAL`/`ANDROID_SERIAL`,而 `--serial` 只寫進呼叫端腳本
#    自己的區域變數 —— 閘看不到。七支腳本(verify_ime / verify_input_matrix /
#    verify_layout / verify_selection_digit / verify_syllables / verify_candbar /
#    verify_variant_persistence)因此**帶 `--serial` 保證失敗**,而
#    `verify_variant_persistence.sh` 的用法區塊寫的就是 `--serial emulator-5558`。
#
# 修法:閘要判的是「**有沒有人指名**」,不是「是不是環境變數」。所以序號的
# 來源被記下來:
#
#   flag  命令列 `--serial` 指名的
#   env   RIME_SERIAL / ANDROID_SERIAL 指名的
#   auto  兩者都沒有,場上恰好一台,腳本自己挑的  ← 只有這一種不准做破壞性動作
#
# ⚠ 為什麼要新增 `rs_select_device` 而不是讓 `rs_pick_serial` 設一個全域變數:
#   每一個呼叫端都寫 `SERIAL="$(rs_pick_serial "$ADB")"`,**命令替換跑在子行程
#   裡**,子行程設的變數一出來就沒了。設了也傳不回來的全域變數,長得跟有效的
#   一模一樣 —— 正是本輪在修的那一類。所以改成一個**不經 stdout** 的入口。
RS_SERIAL=""
RS_SERIAL_SOURCE=""

# 選一台裝置,並把**來源**一起記下來。
#
#   rs_select_device <adb> [命令列 --serial 的值]
#
# 成功時設定兩個變數(不印到 stdout,因此**不可以**用命令替換呼叫):
#   RS_SERIAL         選定的序號
#   RS_SERIAL_SOURCE  flag | env | auto
#
# 失敗回 2,訊息在 stderr。
rs_select_device() {
  local adb="${1:?rs_select_device 要 adb 路徑}" flag="${2:-}"
  local env_want="${RIME_SERIAL:-${ANDROID_SERIAL:-}}" listed
  # ⚠ 每一次都先清掉:殘留值 = 上一次的答案冒充這一次的,而閘會放行。
  RS_SERIAL=""; RS_SERIAL_SOURCE=""
  if [ -n "$flag" ]; then
    # 兩邊都指名而且指到不同機器 —— 不猜哪一個才是本意。
    if [ -n "$env_want" ] && [ "$env_want" != "$flag" ]; then
      {
        echo "--serial $flag 與環境變數 RIME_SERIAL/ANDROID_SERIAL=$env_want 指到不同機器 —— 中止。"
        echo "(兩邊各打一台的話,輸出看起來一切正常。)"
      } >&2
      return 2
    fi
    listed="$("$adb" devices 2>/dev/null)"
    if ! printf '%s\n' "$listed" | grep -q "^${flag}	device$"; then
      {
        echo "--serial 指定的 $flag 不在線。目前:"
        printf '%s\n' "$listed" | sed 's/^/    /'
      } >&2
      return 2
    fi
    RS_SERIAL="$flag"; RS_SERIAL_SOURCE="flag"
    echo "[device] $RS_SERIAL(來源:命令列 --serial)" >&2
    return 0
  fi
  RS_SERIAL="$(rs_pick_serial "$adb")" || return 2
  if [ -n "$env_want" ]; then RS_SERIAL_SOURCE="env"; else RS_SERIAL_SOURCE="auto"; fi
  return 0
}

# 選一台裝置。成功時把序號印到 stdout(其餘一律到 stderr)。
#
#   RIME_SERIAL / ANDROID_SERIAL 有值 → 只用那一台,不在線就中止
#   兩個都沒有,而且**恰好一台**在線  → 用它,並在 stderr 說出來
#   兩個都沒有,而且不只一台在線      → **不猜**,中止並印出清單
#
# ⚠ 這一支**問不到 `--serial`**(它在呼叫端的區域變數裡)。有 `--serial` 的
#   腳本要走 [rs_select_device];這一支留給沒有那個旗標的腳本。
rs_pick_serial() {
  local adb="${1:?rs_pick_serial 要 adb 路徑}"
  local want="${RIME_SERIAL:-${ANDROID_SERIAL:-}}" listed n
  # ⚠ 不接管線:`set -o pipefail` 之下上游提前結束會變成 141,
  #   於是「有裝置」被判成「指令失敗」。
  listed="$("$adb" devices 2>/dev/null)"
  if [ -n "$want" ]; then
    if printf '%s\n' "$listed" | grep -q "^${want}	device$"; then
      printf '%s\n' "$want"
      return 0
    fi
    {
      echo "指定的 $want 不在線。目前:"
      printf '%s\n' "$listed" | sed 's/^/    /'
    } >&2
    return 2
  fi
  n="$(printf '%s\n' "$listed" | grep -cE '	device$')"
  if [ "$n" -eq 1 ]; then
    printf '%s\n' "$listed" | awk '/\tdevice$/{print $1; exit}'
    echo "[device] 只有一台裝置在線,自動選用。要指定請設 RIME_SERIAL。" >&2
    return 0
  fi
  {
    echo "這台機器上有 $n 台裝置在線,**不猜** —— 猜錯會 pm clear 別人的模擬器,"
    echo "而且量到的是別人的 APK。請顯式指定:"
    printf '%s\n' "$listed" | sed 's/^/    /'
    echo "    RIME_SERIAL=emulator-XXXX $0 ..."
  } >&2
  return 2
}

# 這台裝置的**身分**(AVD 名)。問不到就印空字串。
rs_avd_name() {
  local adb="${1:?}" serial="${2:?}" name
  name="$("$adb" -s "$serial" emu avd name 2>/dev/null | head -1 | tr -d '\r')"
  case "$name" in ''|OK|*error*) name="" ;; esac
  [ -n "$name" ] || name="$("$adb" -s "$serial" shell getprop ro.boot.qemu.avd_name 2>/dev/null | tr -d '\r')"
  printf '%s\n' "$name"
}

# 破壞性動作(pm clear / uninstall / ime set / settings put)之前的閘。
#
# 兩道:
#   1. 序號必須是**明著指定**的。自動選來的那一台不准被清資料 ——
#      `release_check.sh` 已經有這個先例(「未指定 RIME_SERIAL,沒有清空 app 資料」)。
#   2. 設了 RIME_AVD_EXPECT 的話,AVD 名字要對得上(port 會換人)。
rs_assert_destructive_ok() {
  local adb="${1:?}" serial="${2:?}" avd source
  shift 2
  # 來源:走過 rs_select_device 的用它記下來的;沒走過的(沒有 --serial 的
  # 腳本)退回只看環境變數 —— 對那些腳本來說「有沒有人指名」就等於「環境
  # 變數有沒有值」。
  source="${RS_SERIAL_SOURCE:-}"
  if [ -n "$source" ] && [ -n "${RS_SERIAL:-}" ] && [ "$RS_SERIAL" != "$serial" ]; then
    # 記下來的那一台與正要被動手的那一台不同 —— 那份「來源」證不了這一台。
    echo "rs_select_device 選的是 $RS_SERIAL,而這裡要動 $serial —— 中止。" >&2
    return 2
  fi
  if [ -z "$source" ]; then
    if [ -n "${RIME_SERIAL:-${ANDROID_SERIAL:-}}" ]; then source="env"; else source="auto"; fi
  fi
  if [ "$source" = "auto" ]; then
    {
      echo "這一關會做破壞性動作($*)。"
      echo "$serial 是自動選來的,不是你指定的 —— 中止。"
      echo "要跑就明著寫:RIME_SERIAL=$serial $0 ...(或 --serial $serial)"
    } >&2
    return 2
  fi
  avd="$(rs_avd_name "$adb" "$serial")"
  if [ -n "${RIME_AVD_EXPECT:-}" ] && [ "$avd" != "$RIME_AVD_EXPECT" ]; then
    echo "$serial 現在是 avd=${avd:-<問不到>},不是你要的 $RIME_AVD_EXPECT —— 中止(port 會換人)。" >&2
    return 2
  fi
  echo "[device] $serial (avd=${avd:-?}, 來源:$source) — 即將:$*" >&2
  return 0
}

# 把「這一輪跑在哪一台、量的是哪一份 APK」寫進 artifact。
#
# ⚠ 沒有這一步,「腳本綠了」在事後永遠無法覆核:這台機器上三台 AVD 全是
#   1080×2400 @420dpi,**截圖本身分不出是哪一台**。
# ⚠ 第 5 個參數 `pkg` 是 2026-08-13 補的。從前只有 `apk`,而 `verify_candbar.sh`
#   / `verify_syllables.sh` **不帶 `--apk` 時什麼都不寫** —— 於是三份 gate
#   artifact 有兩份不知道量的是哪一份 APK,正好違反本檔頭自己給的理由。
#   現在改成:量的是**裝置上真的裝著的那一份**(`pm path` → 裝置端 sha256),
#   不是「我打算裝上去的那一份」。兩者不同時,前者才是這一輪的事實。
rs_write_device_stamp() {
  local adb="${1:?}" serial="${2:?}" out="${3:?}" apk="${4:-}" pkg="${5:-}"
  local path sha
  {
    echo "serial=$serial"
    echo "avd=$(rs_avd_name "$adb" "$serial")"
    echo "fingerprint=$("$adb" -s "$serial" shell getprop ro.build.fingerprint 2>/dev/null | tr -d '\r')"
    echo "screen=$("$adb" -s "$serial" shell wm size 2>/dev/null | tr -d '\r' | tail -1)"
    echo "density=$("$adb" -s "$serial" shell wm density 2>/dev/null | tr -d '\r' | tail -1)"
    if [ -n "$apk" ] && [ -f "$apk" ]; then
      echo "apk=$apk"
      echo "apk_sha256=$(sha256sum "$apk" 2>/dev/null | awk '{print $1}')"
    else
      # 沉默地少一行 = 事後看不出「是沒帶還是壞了」。明著寫出來。
      echo "apk=<未指定 --apk>"
      echo "apk_sha256=<未指定 --apk>"
    fi
    if [ -n "$pkg" ]; then
      echo "pkg=$pkg"
      path="$("$adb" -s "$serial" shell pm path "$pkg" 2>/dev/null \
              | tr -d '\r' | sed -n 's/^package://p' | head -1)"
      if [ -n "$path" ]; then
        echo "pkg_apk=$path"
        # ⚠ 在**裝置上**算。host 上那一份可能已經被下一次建置覆蓋掉了,
        #   而這一關要回答的是「剛剛量的是哪一份」。
        sha="$("$adb" -s "$serial" shell sha256sum "$path" 2>/dev/null \
               | tr -d '\r' | awk '{print $1}' | head -1)"
        echo "pkg_apk_sha256=${sha:-<裝置上算不出來>}"
      else
        echo "pkg_apk=<沒裝>"
        echo "pkg_apk_sha256=<沒裝>"
      fi
      "$adb" -s "$serial" shell dumpsys package "$pkg" 2>/dev/null | tr -d '\r' \
        | sed -n 's/^ *versionName=\(.*\)$/pkg_version=\1/p;s/^ *versionCode=\([0-9]*\).*/pkg_version_code=\1/p;s/^ *lastUpdateTime=\(.*\)$/pkg_last_update=\1/p' \
        | sort -u
    fi
    echo "when=$(date -Iseconds)"
  } > "$out"
  echo "[device] 這一輪的裝置與 APK 記在 $out" >&2
}
