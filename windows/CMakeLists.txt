# windows/CMakeLists.txt — Windows 端「不經 UI 的核心驗證」目標
#
# 這裡只建一個東西:rime_console.exe = 四端共用的門面 core/src/rime_shell.cc
# 加上直接驅動它的 tools/rime_console.cc。
#
# **沒有** TSF DLL、沒有 COM、沒有候選窗。那些是下一輪。核心層沒綠之前做 UI,
# 出問題分不出是引擎、資料、還是 UI 的錯。
#
# librime 與 5 個靜態相依請先用 windows/build.sh 建到 RIME_PREFIX:
#   windows/build.sh deps
#
cmake_minimum_required(VERSION 3.20)

# CMP0091(用 CMAKE_MSVC_RUNTIME_LIBRARY 而非改 CMAKE_*_FLAGS 選 CRT)必須在
# project() 之前就位,否則下面那行只是個沒人讀的變數。
project(rime_windows_core LANGUAGES CXX)

if(NOT MSVC)
  message(FATAL_ERROR
    "這個目標只支援 MSVC。TSF 實務上無法用 mingw 交叉編譯,而讓核心層與最終的\n"
    "  DLL 用不同編譯器,等於把 ABI 問題留到最難查的那一層才爆。")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# /MT。librime 與 5 個相依全都是 /MT(見 windows/build.sh 的說明),混用會是
# LNK2038,而它指的檔案通常不是真正出錯的那個。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

if(NOT RIME_PREFIX)
  message(FATAL_ERROR "請以 -DRIME_PREFIX=<windows/build.sh 的 prefix> 設定")
endif()
if(NOT EXISTS "${RIME_PREFIX}/include/rime_api.h")
  message(FATAL_ERROR "${RIME_PREFIX} 裡沒有 rime_api.h;先跑 windows/build.sh deps")
endif()

get_filename_component(RIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# ── 靜態庫 ────────────────────────────────────────────────────────────────
#
# 檔名不寫死:glog 與 yaml-cpp 在不同版本/組態下會叫 glog.lib / glogd.lib /
# yaml-cpp.lib / libyaml-cppmt.lib。NAMES 清單抄自 librime 自己的
# cmake/Find*.cmake,免得日後換版本時得回來猜。
# NO_DEFAULT_PATH 是重點:只准用我們自己編的那一份,不准撿到 runner 上別人裝的。
set(CMAKE_FIND_LIBRARY_SUFFIXES .lib)
function(rime_find_lib var)
  find_library(${var}
    NAMES ${ARGN}
    PATHS "${RIME_PREFIX}/lib"
    NO_DEFAULT_PATH
    REQUIRED)
endfunction()

rime_find_lib(RIME_LIB      rime librime)
rime_find_lib(OPENCC_LIB    opencc)
rime_find_lib(MARISA_LIB    marisa libmarisa)
rime_find_lib(LEVELDB_LIB   leveldb libleveldb)
rime_find_lib(YAMLCPP_LIB   yaml-cpp libyaml-cppmt yaml-cppd libyaml-cppmtd)
rime_find_lib(GLOG_LIB      glog glogd libglog)

# 連結順序:被依賴者排在依賴者之後,與 Android 的
# third_party/prebuilt/manifest.json 記載的同一份順序。
#   librime -> opencc, glog, yaml-cpp, leveldb, marisa
#   opencc  -> marisa
# MSVC 的連結器對靜態庫順序比 GNU ld 寬容,但把順序寫對是零成本的,
# 而且讓兩端的「連結順序」是同一件可以互相對照的事。
set(RIME_STATIC_LIBS
  "${RIME_LIB}"
  "${OPENCC_LIB}"
  "${MARISA_LIB}"
  "${LEVELDB_LIB}"
  "${YAMLCPP_LIB}"
  "${GLOG_LIB}")

# 系統庫:
#   dbghelp — glog 0.7 的 stacktrace 用得到,而 glog 的 CMake 忘了自己帶上
#             (librime 的 src/CMakeLists.txt 為 WIN32 補了同一條)。
#   ws2_32  — leveldb / glog 在 Windows 上的網路與主機名相關呼叫。
#   bcrypt  — boost::uuid 在 MSVC 上取亂數用。Boost 有 auto-link pragma 會自己
#             帶上,這裡明寫一次,免得哪天 BOOST_ALL_NO_LIB 被開起來就斷。
set(RIME_SYSTEM_LIBS dbghelp ws2_32 bcrypt)

# ── 目標 ──────────────────────────────────────────────────────────────────
add_executable(rime_console
  "${RIME_ROOT}/tools/rime_console.cc"
  "${RIME_ROOT}/core/src/rime_shell.cc")

target_include_directories(rime_console PRIVATE
  "${RIME_ROOT}/core/include"
  "${RIME_PREFIX}/include")

target_link_libraries(rime_console PRIVATE
  ${RIME_STATIC_LIBS}
  ${RIME_SYSTEM_LIBS})

# /utf-8 不是可選的。
#
# MSVC 預設用系統 ANSI 代碼頁解讀「沒有 BOM 的原始碼」,也用它當執行字元集。
# rime_console.cc 與 rime_shell.cc 兩支都有中文字面值(輸出訊息、rs_last_error
# 的錯誤字串),少了這個旗標,那些字串在編譯階段就已經被解錯 —— 印出來是亂碼,
# 看起來像引擎壞了,實際上引擎好得很。這正是 docs/handoff-windows.md 點名的坑。
#
# (主控台那一端的代碼頁由 windows/verify_console.sh 處理:輸出一律導進檔案,
#  拿到的是原始 UTF-8 位元組,不經過主控台的代碼頁轉換。)
target_compile_options(rime_console PRIVATE /utf-8 /EHsc)

# 多組態產生器預設會多塞一層 Release/。固定輸出位置,讓驗證腳本不必猜。
set_target_properties(rime_console PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/bin")

message(STATUS "rime    : ${RIME_LIB}")
message(STATUS "opencc  : ${OPENCC_LIB}")
message(STATUS "marisa  : ${MARISA_LIB}")
message(STATUS "leveldb : ${LEVELDB_LIB}")
message(STATUS "yaml-cpp: ${YAMLCPP_LIB}")
message(STATUS "glog    : ${GLOG_LIB}")
