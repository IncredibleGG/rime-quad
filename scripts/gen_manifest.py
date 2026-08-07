#!/usr/bin/env python3
"""產生 third_party/prebuilt/manifest.json。由 scripts/build_native.sh 呼叫。"""

import hashlib
import json
import os
import re
import subprocess
import sys
from datetime import datetime, timezone

RIME_ROOT = os.environ["RIME_ROOT"]
PREBUILT_ROOT = os.environ["PREBUILT_ROOT"]
LIBRIME_SRC = os.environ["LIBRIME_SRC"]
BOOST_ROOT_DIR = os.environ["BOOST_ROOT_DIR"]

TRIPLES = {
    "arm64-v8a": "aarch64-linux-android",
    "x86_64": "x86_64-linux-android",
    "armeabi-v7a": "armv7a-linux-androideabi",
    "x86": "i686-linux-android",
}
MACHINES = {
    "arm64-v8a": "AArch64",
    "x86_64": "Advanced Micro Devices X86-64",
    "armeabi-v7a": "ARM",
    "x86": "Intel 80386",
}


def sh(cmd, cwd=None):
    try:
        return subprocess.check_output(cmd, cwd=cwd, text=True,
                                       stderr=subprocess.DEVNULL).strip()
    except Exception:
        return None


def rel(path):
    return os.path.relpath(path, RIME_ROOT)


def librime_version():
    with open(os.path.join(LIBRIME_SRC, "CMakeLists.txt"), encoding="utf-8") as f:
        m = re.search(r"set\(rime_version\s+([0-9.]+)\)", f.read())
    return m.group(1) if m else "unknown"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def plugins_info():
    """librime 的內建外掛。目前只有 librime-lua。

    重點:lua 外掛(含 Lua 直譯器)是以 object 檔的形式**併進 librime.a**的
    (BUILD_MERGED_PLUGINS=ON),沒有另外產生 liblua.a / librime-lua.a,
    所以 link_order 完全不變 —— 下游不需要多連任何一個檔案。
    """
    if os.environ.get("ENABLE_LUA", "1") != "1":
        return {"lua": {"enabled": False,
                        "note": "以 scripts/build_native.sh --no-lua 建置,"
                                "lua_translator/lua_filter 不可用。"}}
    src = os.environ["LIBRIME_LUA_SRC"]
    return {
        "lua": {
            "enabled": True,
            "upstream": os.environ["LIBRIME_LUA_REPO"],
            "commit": os.environ["LIBRIME_LUA_COMMIT"],
            "license": "BSD-3-Clause",
            "source_dir": rel(src),
            "mounted_at": rel(os.path.join(LIBRIME_SRC, "plugins", "lua"))
                          + "(symlink)",
            "interpreter": {
                "name": "Lua",
                "version": os.environ["LUA_VERSION"],
                "license": "MIT",
                "source": "librime-lua 的 thirdparty 分支(上游 action-install.sh 的做法)",
                "commit": os.environ["LIBRIME_LUA_TP_COMMIT"],
                "source_dir": rel(os.path.join(src, "thirdparty", "lua5.4")),
                "note": "不找系統 lua5.4/luajit(交叉編譯到 Android 找不到),"
                        "把 Lua 原始碼一起編進外掛的 object 檔。",
            },
            "provides": ["lua_translator", "lua_filter", "lua_segmentor",
                         "lua_processor"],
            "packaging": "BUILD_MERGED_PLUGINS=ON:外掛與 Lua 直譯器的 object 檔"
                         "直接併入 librime.a,不另外產生靜態庫,連結順序不變。",
            "registration": "上層 CMakeLists 依 rime_plugins_modules 定義 "
                            "RIME_EXTRA_MODULES=(lua),rime_api.cc 因此會呼叫 "
                            "rime_require_module_lua();librime.a 內同時存在該符號的"
                            "定義(T)與引用(U),build_native.sh 的 verify_lua() 會檢查。",
            "runtime_data": "librime-lua 會把 package.path 設成 "
                            "<user>/lua/?.lua;<user>/lua/?/init.lua;"
                            "<shared>/lua/?.lua;<shared>/lua/?/init.lua,"
                            "並載入 <user>/rime.lua(不存在則 <shared>/rime.lua)。"
                            "方案套件必須保留 lua/ 子目錄結構與 rime.lua。",
        }
    }


link_order = os.environ["LINK_ORDER"].split()
system_libs = os.environ["SYSTEM_LIBS"].split()
abis = os.environ["ABIS"].split()

manifest_path = os.path.join(PREBUILT_ROOT, "manifest.json")
existing = {}
if os.path.exists(manifest_path):
    try:
        with open(manifest_path, encoding="utf-8") as f:
            existing = json.load(f)
    except Exception:
        existing = {}

abi_entries = dict(existing.get("abis", {}))

for abi in abis:
    out = os.path.join(PREBUILT_ROOT, abi)
    lib_dir = os.path.join(out, "lib")
    if not os.path.isdir(lib_dir):
        continue
    libs = []
    ordered = [n for n in link_order if os.path.exists(os.path.join(lib_dir, n))]
    extra = sorted(n for n in os.listdir(lib_dir)
                   if n.endswith(".a") and n not in ordered)
    for name in ordered + extra:
        p = os.path.join(lib_dir, name)
        libs.append({
            "file": name,
            "size_bytes": os.path.getsize(p),
            "size_human": f"{os.path.getsize(p) / (1 << 20):.2f} MiB",
            "sha256": sha256(p),
        })
    abi_entries[abi] = {
        "api_level": int(os.environ["ANDROID_API"]),
        "triple": TRIPLES.get(abi),
        "clang_target": f"{TRIPLES.get(abi)}{os.environ['ANDROID_API']}",
        "elf_machine": MACHINES.get(abi),
        "include_dir": rel(os.path.join(out, "include")),
        "lib_dir": rel(lib_dir),
        "libs": libs,
        "total_size_bytes": sum(x["size_bytes"] for x in libs),
    }

manifest = {
    "schema_version": 1,
    "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "generated_by": "scripts/build_native.sh",
    "librime": {
        "version": librime_version(),
        "commit": sh(["git", "rev-parse", "HEAD"], cwd=LIBRIME_SRC),
        "source_dir": rel(LIBRIME_SRC),
    },
    "toolchain": {
        "ndk_version": os.environ["NDK_VERSION"],
        "ndk_path": os.environ["ANDROID_NDK"],
        "cmake_version": os.environ["CMAKE_VER"],
        "cmake_note": "必須用 Android SDK 內附的 cmake 3.22.1;CMake 4.x 已移除 FindBoost 模組。",
        "stl": os.environ["ANDROID_STL_VALUE"],
        "build_type": os.environ["BUILD_TYPE"],
        "debug_info_stripped": os.environ.get("KEEP_DEBUG", "0") != "1",
        "debug_info_note": "NDK 預設帶 -g,未剝離的 librime.a 約 118 MiB。"
                           "匯出時已套用 llvm-strip --strip-debug(保留所有連結符號)。"
                           "需要完整 DWARF 時用 scripts/build_native.sh --keep-debug 重建。",
    },
    "plugins": plugins_info(),
    "boost": {
        "version": os.environ["BOOST_VERSION"],
        "header_only": True,
        "include_dir": rel(BOOST_ROOT_DIR),
        "abs_include_dir": BOOST_ROOT_DIR,
        "note": "CMAKE_SYSTEM_NAME=Android 時 librime 走 find_package(Boost 1.77.0) "
                "不帶 COMPONENTS,只需要 headers,不需要編譯任何 Boost 二進位。"
                "下游若要編譯 librime 的私有標頭,需要把這個路徑加進 include path。",
    },
    # 靜態庫的順序會影響連結:被依賴者必須排在依賴者之後。
    "link_order": link_order,
    "link_order_note": "librime.a 依賴 opencc/glog/yaml-cpp/leveldb/marisa;"
                       "libopencc.a 依賴 libmarisa.a,故 marisa 必須排在 opencc 之後。"
                       "librime-lua 外掛與 Lua 5.4 直譯器已併入 librime.a,"
                       "不需要額外的 liblua.a,順序與加入 lua 之前完全相同。",
    "system_libs": system_libs,
    "compile_definitions": {
        "public_c_api": [],
        "public_c_api_note": "只用 rime_api.h 的 C API 時不需要任何額外 define。",
        "internal_cxx_headers": [
            "RIME_VERSION=\"%s\"" % librime_version(),
            "YAML_CPP_STATIC_DEFINE",
            "Opencc_BUILT_AS_STATIC",
            "GLOG_EXPORT=",
            "GLOG_NO_EXPORT=",
            "GLOG_DEPRECATED=__attribute__((deprecated))",
            "BOOST_DLL_USE_STD_FS",
        ],
        "internal_cxx_headers_note": "若下游要 #include librime 的 C++ 私有標頭"
                                     "(third_party/prebuilt/<abi>/include/rime/…),"
                                     "必須帶上這些 define 才能和 .a 的 ABI 一致。",
    },
    "cmake_flags": {
        "librime": [
            "BUILD_SHARED_LIBS=OFF", "BUILD_STATIC=ON", "BUILD_TEST=OFF",
            "BUILD_SAMPLE=OFF", "BUILD_DATA=OFF", "BUILD_SEPARATE_LIBS=OFF",
            "BUILD_MERGED_PLUGINS=%s" % (
                "ON" if os.environ.get("ENABLE_LUA", "1") == "1" else "OFF"),
            "ENABLE_EXTERNAL_PLUGINS=OFF",
            "ENABLE_LOGGING=ON", "ENABLE_THREADING=ON", "ENABLE_TIMESTAMP=ON",
            "INSTALL_PRIVATE_HEADERS=ON",
        ],
    },
    "runtime_notes": [
        "opencc 的 .ocd2 詞典是執行期資源,本階段不處理;"
        "APK 需自行打包 opencc 的 share/opencc/*.json 與 *.ocd2,"
        "並讓 RimeTraits.shared_data_dir 底下的 opencc 目錄可以找到它們。",
        "librime 的 shared_data_dir / user_data_dir / log_dir 必須是 app 可寫的路徑。",
        "glog 已編入(ENABLE_LOGGING=ON),log 會寫到 RimeTraits.log_dir。",
    ],
    "abis": abi_entries,
}

os.makedirs(PREBUILT_ROOT, exist_ok=True)
with open(manifest_path, "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2, ensure_ascii=False)
    f.write("\n")
print(f"wrote {manifest_path}")
