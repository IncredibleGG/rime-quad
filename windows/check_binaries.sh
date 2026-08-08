#!/usr/bin/env bash
#
# windows/check_binaries.sh — 對產出的二進位做結構檢查
#
# ── 為什麼需要這支 ─────────────────────────────────────────────
#
# TSF 在 CI 上驗不了(要註冊 COM in-proc server 並有真實的文字輸入情境)。
# 但有幾件事**不需要**真實輸入情境就驗得到,而且它們正好對應到幾個
# 「編得出來但一裝就不能用」的失敗模式:
#
#   1. **匯出的四個進入點齊不齊。** 少一個,regsvr32 會失敗,
#      而失敗訊息不會告訴你少了哪一個。
#   2. **DLL 有沒有意外的相依。** 這支 DLL 被載入到**每一個**接受文字輸入的
#      進程裡。如果它相依 VCRUNTIME140.dll / MSVCP140.dll,那麼在沒裝
#      VC++ 執行檔套件的機器上,它會在載入階段就失敗 —— 而症狀是
#      「輸入法在某些程式裡整個不存在」,錯誤落在宿主那邊,使用者看不到。
#      /MT 就是為了這件事,這道檢查就是為了確認 /MT 真的生效了。
#
# 用法(Git Bash):
#   windows/check_binaries.sh <放二進位的目錄>
#
set -euo pipefail

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

BIN_DIR="${1:-}"
[ -n "${BIN_DIR}" ] || die "用法: windows/check_binaries.sh <目錄>"
[ -d "${BIN_DIR}" ] || die "找不到目錄 ${BIN_DIR}"

command -v cygpath >/dev/null 2>&1 || die "必須在 Git Bash / MSYS2 下執行"

# ── 找 dumpbin ────────────────────────────────────────────────
#
# 這一段與 windows/build.sh 的 setup_msvc_env 做的是同一件事,而且是**抄過來**
# 的,不是共用的。理由:build.sh 那一段已經在 CI 上驗證過,把它抽成共用檔案
# 等於為了省 30 行去動一段唯一驗證管道要跑十幾分鐘才知道結果的程式碼。
# 兩份漂移的風險由這裡的失敗訊息承擔 —— 這支腳本壞掉不會讓輸入法壞掉。
if ! command -v dumpbin.exe >/dev/null 2>&1; then
  vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
  [ -x "${vswhere}" ] || die "找不到 vswhere,無法定位 dumpbin"
  vsdir="$("${vswhere}" -latest -products '*' \
             -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
             -property installationPath | tr -d '\r')"
  [ -n "${vsdir}" ] || die "vswhere 找不到含 C++ 工具集的 Visual Studio"
  vcvars="$(cygpath -u "${vsdir}")/VC/Auxiliary/Build/vcvars64.bat"
  [ -f "${vcvars}" ] || die "找不到 ${vcvars}"
  tmpbat="$(mktemp -t vcvars.XXXXXX).bat"
  {
    printf '@echo off\r\n'
    printf 'call "%s" >nul\r\n' "$(cygpath -w "${vcvars}")"
    printf 'set\r\n'
  } > "${tmpbat}"
  dump="$(cmd //c "$(cygpath -w "${tmpbat}")")" || die "vcvars64.bat 執行失敗"
  rm -f "${tmpbat}"
  while IFS= read -r line; do
    line="${line%$'\r'}"
    case "${line}" in
      PATH=*|Path=*) export PATH="$(cygpath -p "${line#*=}")" ;;
    esac
  done <<< "${dump}"
  command -v dumpbin.exe >/dev/null 2>&1 || die "載入 vcvars 之後仍然找不到 dumpbin"
fi
log "dumpbin = $(command -v dumpbin.exe)"

fail=0

# ── 1. 匯出 ───────────────────────────────────────────────────
DLL="${BIN_DIR}/rime_tsf.dll"
[ -f "${DLL}" ] || die "找不到 ${DLL}"

exports="$(dumpbin.exe //exports "$(cygpath -w "${DLL}")" | tr -d '\r')"
# 匯出表的資料列長這樣:「  1    0 00001000 DllCanUnloadNow」。
# 只取第 4 欄,且限定前三欄是序號／提示／RVA,免得把標題文字也撈進來。
names="$(echo "${exports}" | awk '$1 ~ /^[0-9]+$/ && $2 ~ /^[0-9A-Fa-f]+$/ && NF >= 4 { print $4 }' | sort)"
expected="$(printf '%s\n' DllCanUnloadNow DllGetClassObject DllRegisterServer DllUnregisterServer | sort)"

echo "  匯出:"
echo "${names}" | sed 's/^/    /'
if [ "${names}" != "${expected}" ]; then
  echo "  !! 匯出的符號與預期不符。預期正好這四個:" >&2
  echo "${expected}" | sed 's/^/     /' >&2
  echo "  (多匯出東西也算不符 —— 這支 DLL 住在別人的進程裡," >&2
  echo "   匯出表就是它對外的全部表面積。)" >&2
  fail=1
else
  log "四個 COM 進入點齊全 ✓"
fi

# ── 2. 相依 ───────────────────────────────────────────────────
#
# 允許清單刻意很短。多出來的東西一律當成錯誤:相依是會悄悄長出來的,
# 而每多一個,就多一種「在某台機器上載入失敗」的可能。
# 要加新的相依請連同「為什麼在每一個宿主進程裡都一定有它」的理由一起加。
# oleaut32.dll 是這一輪新加的:語言列按鈕的 GetText / GetTooltipString
# 要回傳 BSTR,而 BSTR 只能用 SysAllocString 配置(呼叫端會用
# SysFreeString 釋放,配置器必須是同一個)。它是 COM 的一部分,
# 每一個載入了 ole32 的進程裡本來就有。
ALLOWED="kernel32.dll user32.dll advapi32.dll ole32.dll oleaut32.dll"
# 這一組出現就是 /MT 沒生效。後果是在沒裝 VC++ 執行檔套件的機器上,
# DLL 在載入階段就失敗 —— 而使用者看到的是「這個程式裡沒有這個輸入法」。
FORBIDDEN="vcruntime140.dll msvcp140.dll ucrtbase.dll msvcr120.dll msvcrt.dll"

# ── 網路 ──────────────────────────────────────────────────────
#
# ⚠ **這一條守的是產品定位,不是技術細節。**
#
# 這個輸入法的主張是「離線為預設」。使用者要能相信它,而說服他的方式
# **不是** README 上的一句話,是這件事本身可以被外人驗證:
# 拿 dumpbin /dependents 看一眼,兩個二進位都沒有任何網路 DLL。
#
# 這一條對 rime_tsf.dll 尤其重要 —— 那個 DLL 被載入到**每一個**接受文字
# 輸入的進程裡,包括瀏覽器與提權的進程。它有網路能力這件事,
# 光靠讀原始碼是不會有人發現的。
#
# 目前 Windows 端**沒有任何一行程式碼會開連線**(方案市集還沒做,
# windows/common/net_policy.cc 只有判斷邏輯,不含任何網路 API)。
# 哪天要做,這條會紅 —— 那時要做的是:
#   1. 把連線集中在**唯一一個** .cc(服務進程那一側),
#   2. 把 winhttp.dll 加進 rime_service.exe 的例外(**不是** DLL 的),
#   3. 補上開關、fail-closed 與連網紀錄(見 common/net_policy.h)。
# 在那三件事做完之前,這條紅著是對的。
#
# ws2_32.dll 是唯一的例外,而且只給服務進程:leveldb 與 glog 在 Windows 上
# 為了取主機名連結它(見 CMakeLists 的 RIME_SYSTEM_LIBS)。那是 librime
# 的相依,不是我們開的連線 —— 但它**不准**出現在瘦 DLL 上。
NET_DLLS="winhttp.dll wininet.dll urlmon.dll ws2_32.dll ws2_32.dll wsock32.dll dnsapi.dll winsock.dll iphlpapi.dll httpapi.dll"

check_deps() {
  local file="$1"; shift
  local strict="$1"; shift
  [ -f "${file}" ] || { echo "  (略過不存在的 ${file})"; return; }
  local deps
  deps="$(dumpbin.exe //dependents "$(cygpath -w "${file}")" | tr -d '\r' \
          | awk '/Image has the following dependencies/,/Summary/' \
          | grep -oiE '[A-Za-z0-9_.-]+\.dll' | tr 'A-Z' 'a-z' | sort -u)"
  echo "  $(basename "${file}") 的相依:"
  echo "${deps}" | sed 's/^/    /'
  local d
  for d in ${deps}; do
    case " ${FORBIDDEN} " in
      *" ${d} "*)
        echo "  !! ${d} —— /MT 沒有生效。這支二進位在沒裝 VC++ 執行檔套件的" >&2
        echo "     機器上會在載入階段失敗,而症狀是「輸入法在某些程式裡不存在」。" >&2
        fail=1 ;;
    esac
    case "${d}" in
      api-ms-win-crt-*)
        echo "  !! ${d} —— 動態 UCRT。同上,/MT 沒有生效。" >&2
        fail=1 ;;
    esac
    case " ${NET_DLLS} " in
      *" ${d} "*)
        if [ "${strict}" = "strict" ] || [ "${d}" != "ws2_32.dll" ]; then
          echo "  !! ${d} —— 這支二進位有網路能力。" >&2
          echo "     Windows 端目前不連網,而「不連網」這件事是靠這一條" >&2
          echo "     從外面驗證的(見本腳本 NET_DLLS 那一段的說明)。" >&2
          echo "     真的要加連網功能的話,那一段寫了要先做完哪三件事。" >&2
          fail=1
        fi ;;
    esac
    if [ "${strict}" = "strict" ]; then
      case " ${ALLOWED} " in
        *" ${d} "*) ;;
        *)
          case "${d}" in
            api-ms-win-crt-*) ;;  # 上面已經報過
            *)
              echo "  !! ${d} 不在允許清單內。" >&2
              echo "     允許的是:${ALLOWED}" >&2
              echo "     要新增請連同「為什麼每一個宿主進程裡都一定有它」的理由。" >&2
              fail=1 ;;
          esac ;;
      esac
    fi
  done
}

echo
log "檢查 rime_tsf.dll 的相依(嚴格)"
check_deps "${DLL}" strict

echo
log "檢查 rime_service.exe 的相依(只擋 CRT;服務進程的相依不受限)"
check_deps "${BIN_DIR}/rime_service.exe" loose

echo
# rime_ime_setup.exe 由安裝程式在**使用者剛裝好、什麼都還沒設定**的機器上執行。
# 它若相依動態 CRT,症狀是「安裝程式跑到註冊那一步就失敗」,而錯誤訊息會是
# 一個沒有人看得懂的 HRESULT。/MT 同樣不可少。
log "檢查 rime_ime_setup.exe 的相依(只擋 CRT)"
check_deps "${BIN_DIR}/rime_ime_setup.exe" loose

echo
[ "${fail}" -eq 0 ] || die "二進位檢查失敗,見上。"
log "二進位檢查全部通過 ✓"
