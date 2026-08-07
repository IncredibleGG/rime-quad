#!/usr/bin/env bash
#
# run_console_test.sh — 在模擬器上跑 librime 端到端測試（不經 Android UI）
#
# 這支測試把「librime + schema 資料 + rime_shell 門面」和「JNI + Compose UI」
# 分開驗證。跑通代表核心與資料是好的，之後 APK 若打不出字，問題必在 UI 那一側。
#
# 用法:
#   ./run_console_test.sh                       # 跑預設的拼音與注音兩組
#   ./run_console_test.sh --keys nihao --select 1
#   ./run_console_test.sh --keys su3cl3 --schema bopomofo_tw --expect 你好
#   ./run_console_test.sh --skip-push           # 資料已在裝置上，只重推執行檔
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
ADB="$SDK/platform-tools/adb"
NDK="$SDK/ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin"
ABI=x86_64            # 模擬器是 x86_64
DEV_DIR=/data/local/tmp/rime

KEYS=""; SELECT=1; SCHEMA=""; EXPECT=""; SKIP_PUSH=0
while [ $# -gt 0 ]; do
  case "$1" in
    --keys)      KEYS="$2"; shift 2 ;;
    --select)    SELECT="$2"; shift 2 ;;
    --schema)    SCHEMA="$2"; shift 2 ;;
    --expect)    EXPECT="$2"; shift 2 ;;
    --skip-push) SKIP_PUSH=1; shift ;;
    -h|--help)   sed -n '2,15p' "$0"; exit 0 ;;
    *) echo "未知參數: $1" >&2; exit 2 ;;
  esac
done

die() { echo "錯誤: $*" >&2; exit 1; }

[ -d "$ROOT/core/data/shared" ] || die "缺少 core/data/shared，先跑 scripts/collect_data.sh"
[ -f "$ROOT/third_party/prebuilt/$ABI/lib/librime.a" ] || \
  die "缺少 $ABI 的 librime.a，先跑 scripts/build_native.sh $ABI"

# ---------------------------------------------------------------- 編譯 ---
echo "=== 編譯 rime_console ($ABI) ==="
BIN="$ROOT/third_party/build/rime_console.$ABI"
mkdir -p "$(dirname "$BIN")"
L="$ROOT/third_party/prebuilt/$ABI/lib"
# 連結順序不可更動，見 third_party/prebuilt/manifest.json
"$NDK/clang++" --target=${ABI}-linux-android21 -std=c++17 -O2 \
  -I "$ROOT/core/include" -I "$ROOT/third_party/prebuilt/$ABI/include" \
  "$ROOT/tools/rime_console.cc" "$ROOT/core/src/rime_shell.cc" \
  -static-libstdc++ \
  "$L/librime.a" "$L/libopencc.a" "$L/libmarisa.a" \
  "$L/libleveldb.a" "$L/libyaml-cpp.a" "$L/libglog.a" \
  -llog -lm -o "$BIN"
echo "  $(stat -c%s "$BIN") bytes"

# ---------------------------------------------------------------- 裝置 ---
"$ROOT/scripts/emu.sh" status >/dev/null 2>&1 || "$ROOT/scripts/emu.sh" start
SERIAL="$("$ADB" devices | awk '/emulator-[0-9]+\tdevice/{print $1; exit}')"
[ -n "$SERIAL" ] || die "找不到模擬器"
echo "=== 裝置 $SERIAL ==="

if [ "$SKIP_PUSH" -eq 0 ]; then
  echo "=== 推送資料（約 13MB，首次較慢）==="
  "$ADB" -s "$SERIAL" shell rm -rf "$DEV_DIR"
  "$ADB" -s "$SERIAL" shell mkdir -p "$DEV_DIR"
  "$ADB" -s "$SERIAL" push "$ROOT/core/data/shared" "$DEV_DIR/shared" | tail -1
  "$ADB" -s "$SERIAL" push "$ROOT/core/data/user"   "$DEV_DIR/user"   | tail -1
fi
"$ADB" -s "$SERIAL" push "$BIN" "$DEV_DIR/rime_console" | tail -1
"$ADB" -s "$SERIAL" shell chmod 755 "$DEV_DIR/rime_console"

# ---------------------------------------------------------------- 執行 ---
run_case() {
  local keys="$1" sel="$2" schema="$3" expect="$4"
  echo
  echo "################################################################"
  echo "# 案例: keys=$keys select=$sel schema=${schema:-預設} expect=${expect:-未指定}"
  echo "################################################################"
  local out
  out="$("$ADB" -s "$SERIAL" shell \
    "cd $DEV_DIR && ./rime_console $DEV_DIR/shared $DEV_DIR/user $keys $sel $schema" 2>&1)"
  # glog 的部署訊息很吵，只在失敗時才全部印出
  echo "$out" | grep -vE '^[WIE][0-9]{8} ' || true

  echo "$out" | grep -q 'COMMIT' || {
    echo "--- 完整輸出（含 glog）---"; echo "$out"
    die "案例失敗：沒有產生 COMMIT。代表組字或選字沒走通。"
  }
  if [ -n "$expect" ]; then
    echo "$out" | grep -q "COMMIT: \"$expect\"" \
      || { echo "--- 完整輸出 ---"; echo "$out"; die "案例失敗：commit 內容不是預期的「$expect」"; }
    echo "  ✓ commit == 「$expect」"
  fi
}

if [ -n "$KEYS" ]; then
  run_case "$KEYS" "$SELECT" "$SCHEMA" "$EXPECT"
else
  # 預設跑兩組，涵蓋兩條不同的詞庫路徑：
  #   拼音走 luna_pinyin.dict；注音走 terra_pinyin.dict（外加 stroke 反查）
  run_case nihao  1 luna_pinyin_tw 你好
  run_case su3cl3 1 bopomofo_tw    你好
fi

echo
echo "=== 全部案例通過 ==="
