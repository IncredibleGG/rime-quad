#!/usr/bin/env bash
#
# package_core.sh — 把驗證過的 macOS 核心層打包成可散布的 tar.gz
#
# ⚠ 這不是可以安裝的輸入法。沒有 .app、沒有 IMKit、沒有候選窗 ——
#   那些是下一輪。這裡打包的是「核心層」:librime 靜態庫、標頭、rime_console。
#
# 為什麼值得留下來:
#   Ubuntu 建置機編不了 macOS,CI 是唯一的建置管道。下一輪寫 Swift 綁定時,
#   有這包就不必再從原始碼重編(約 4 分鐘),而且拿到的是**這次真的打出
#   「你好」的那一份**二進位,不是另外編的一份。
#
# 產出: apple/build/dist/<name>.tar.gz
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PREFIX="${ROOT}/apple/build/prefix"
CONSOLE="${ROOT}/apple/build/rime_console"

[ -d "${PREFIX}/lib" ] || { echo "錯誤: 找不到 ${PREFIX}/lib，先跑 build_macos.sh" >&2; exit 1; }
[ -x "${CONSOLE}" ]    || { echo "錯誤: 找不到 ${CONSOLE}" >&2; exit 1; }

cd "${ROOT}"
SHA="$(git rev-parse --short HEAD)"
FULLSHA="$(git rev-parse HEAD)"
STAMP="$(date -u +%Y%m%d-%H%M)"
NAME="rime-macos-core-${STAMP}-${SHA}"
DIST="${ROOT}/apple/build/dist/${NAME}"

rm -rf "${DIST}"
mkdir -p "${DIST}/lib" "${DIST}/bin"

cp "${PREFIX}"/lib/*.a "${DIST}/lib/"
cp -R "${PREFIX}/include" "${DIST}/include"
cp "${CONSOLE}" "${DIST}/bin/"
strip -S "${DIST}/bin/rime_console" 2>/dev/null || true

CMAKE_V="$(cmake --version | head -1 | awk '{print $3}')"
CLANG_V="$(clang++ --version | head -1)"
MACOS_V="$(sw_vers -productVersion 2>/dev/null || echo unknown)"
ARCH="$(uname -m)"

# manifest:比照 Android 端 third_party/prebuilt/manifest.json 的紀律。
# 少了這份,日後沒人分得出這包是哪個 librime、哪個工具鏈編出來的。
cat > "${DIST}/manifest.json" << JSON
{
  "name": "${NAME}",
  "built_by": ".github/workflows/macos.yml + apple/scripts/build_macos.sh",
  "repo_commit": "${FULLSHA}",
  "librime_version": "1.17.0",
  "librime_commit": "1d0df6e40cdcac17a986adc65e4668ae84ae0ada",
  "arch": "${ARCH}",
  "macos": "${MACOS_V}",
  "cmake": "${CMAKE_V}",
  "clang": "${CLANG_V}",
  "lua_plugin_commit": "ec52e48ea18f11af37717a01c337f853215cf70b",
  "lua_version": "5.4.8",
  "sandbox_patch": "patches/librime-lua@sandbox.patch",
  "link_order": ["librime.a", "libopencc.a", "libmarisa.a", "libleveldb.a", "libyaml-cpp.a", "libglog.a"],
  "link_order_note": "librime 依賴 opencc/glog/yaml-cpp/leveldb/marisa;opencc 依賴 marisa,故 marisa 必須排在 opencc 之後。",
  "verified": ["nihao -> 你好 (luna_pinyin_tw)", "su3cl3 -> 你好 (bopomofo_tw)"],
  "not_included": "無 .app / IMKit / 候選窗 —— 這是核心層,不是可安裝的輸入法"
}
JSON

cat > "${DIST}/README.txt" << TXT
${NAME}

這是 LuminaKey 的 macOS **核心層**產物,不是可以安裝的輸入法。
沒有 .app、沒有候選窗、沒有 IMKit —— 那些是下一輪。

內容:
  lib/              librime 與 5 個依賴的靜態庫(${ARCH})
  include/          標頭(含 librime 私有標頭)
  bin/rime_console  不經 UI 直接驅動 librime 的驗證工具

自己驗一次(需要 core/data/shared 與 core/data/user,由 scripts/collect_data.sh 產生):
  ./bin/rime_console <shared> <user> nihao  1 luna_pinyin_tw
  ./bin/rime_console <shared> <user> su3cl3 1 bopomofo_tw
兩者的最後一行都應該是:  >>> COMMIT: "你好"

連結順序不可更動,見 manifest.json 的 link_order。
librime-lua 已套用 patches/librime-lua@sandbox.patch(第三方方案的沙盒)。
TXT

tar czf "${ROOT}/apple/build/dist/${NAME}.tar.gz" -C "${ROOT}/apple/build/dist" "${NAME}"
echo "${NAME}" > "${ROOT}/apple/build/dist/NAME.txt"
ls -lh "${ROOT}/apple/build/dist/${NAME}.tar.gz"
echo "打包完成: ${NAME}.tar.gz"
