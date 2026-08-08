# windows/CMakeLists.txt — Windows 端的所有目標
#
# 這裡刻意分成**兩組**,分界線是「要不要 librime」:
#
#   ── 不要 librime(RIME_PREFIX 可以不設)──
#     rime_common  純邏輯:線路格式、按鍵映射、組字政策、候選窗排版
#     rime_tests   單元測試 + 真實鍵盤佈局測試
#     rime_tsf     TSF 的瘦 DLL —— **它本來就不該碰 librime**
#     rime_probe   走真的具名管道驅動服務的驗證用戶端
#
#   ── 要 librime(必須設 RIME_PREFIX)──
#     rime_service 服務進程:rime_shell + librime + 候選窗
#     rime_console 不經 UI 的核心驗證(第一個里程碑)
#
# 分開的理由是**驗證迴圈**:第一組編一次不到一分鐘,不必等 librime 那十幾分鐘
# 的建置與快取。CI 也照這條線切成兩個 job。而這條線同時就是架構本身 ——
# 瘦 DLL 那一側如果哪天需要 RIME_PREFIX 才編得起來,代表有東西跑錯邊了。
#
# librime 與 5 個靜態相依請先用 windows/build.sh 建到 RIME_PREFIX:
#   windows/build.sh deps
#
cmake_minimum_required(VERSION 3.20)

# CMP0091(用 CMAKE_MSVC_RUNTIME_LIBRARY 而非改 CMAKE_*_FLAGS 選 CRT)必須在
# project() 之前就位,否則下面那行只是個沒人讀的變數。
project(rime_windows LANGUAGES CXX)

if(NOT MSVC)
  message(FATAL_ERROR
    "這個目標只支援 MSVC。TSF 實務上無法用 mingw 交叉編譯,而讓核心層與最終的\n"
    "  DLL 用不同編譯器,等於把 ABI 問題留到最難查的那一層才爆。")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# /MT(靜態 CRT)。兩個理由,而第二個是這一輪才變得重要的:
#   1. librime 與 5 個相依全都是 /MT(見 windows/build.sh),混用會是 LNK2038。
#   2. TSF 的 DLL 被載入到**每一個**宿主進程裡。動態 CRT 代表那些進程都得
#      找得到 VCRUNTIME140.dll / MSVCP140.dll —— 少了就是「某些程式裡輸入法
#      整個不存在」,而且錯誤訊息落在宿主那邊,使用者根本看不到。
#      CI 有一道 dumpbin /dependents 在守這件事。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

get_filename_component(RIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# /utf-8 不是可選的。
#
# MSVC 預設用系統 ANSI 代碼頁解讀「沒有 BOM 的原始碼」,也用它當執行字元集。
# 這個專案的原始碼裡到處都是中文字面值(訊息、註解、測試裡的候選字),
# 少了這個旗標,那些字串在編譯階段就已經被解錯 —— 印出來是亂碼,
# 看起來像引擎壞了,實際上引擎好得很。
add_compile_options(/utf-8 /EHsc /W3)

# UNICODE / _UNICODE。程式碼裡一律呼叫明確的 W 版本 API,所以這兩個巨集
# 影響的其實只有**資源巨集**:少了它們,IDC_ARROW 之類的東西會展開成
# MAKEINTRESOURCEA,傳給 LoadCursorW 就是 C2664。
# NOMINMAX:windows.h 的 min/max 巨集會把 std::max(a, b) 弄壞。
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# 產物位置固定,讓腳本與 CI 不必猜。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(cfg RELEASE DEBUG RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/bin")
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/bin")
endforeach()

# ── 第一組:不需要 librime ────────────────────────────────────────────

# 純邏輯層。**這裡的原始碼不可以 include windows.h** —— 那是刻意的約束,
# 為的是讓 windows/run_logic_tests.sh 能在開發用的 Ubuntu 上用 g++ 編起來、
# 跑起來。Windows 端唯一的建置管道是 CI,一輪十幾分鐘;
# 「推上去看會不會過」不是開發方式。
add_library(rime_common STATIC
  common/protocol.cc
  common/keymap.cc
  common/ime_policy.cc
  common/cand_layout.cc
  common/schema_choice.cc
  common/settings.cc
  common/schema_list_patch.cc
  # net_policy.cc 是離線守門的**判斷**與紀錄格式(Android NetworkGate 的移植)。
  # ⚠ 它不會開任何連線,目前也還沒有東西呼叫它 —— 方案市集還沒做。
  #   放進來是因為它有測試,而且是下一輪的地基。CI 會斷言**兩個二進位
  #   都沒有任何網路相依**,所以「Windows 端不連網」這句話仍然成立而且驗得到。
  common/net_policy.cc)
target_include_directories(rime_common PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/common"
  "${RIME_ROOT}/core/include")

# Windows 專用的一小撮工具(UTF 轉換、SID、管道名)。
# 與 common/ 分開放,免得哪天有人在 common/ 裡順手 include windows.h。
add_library(rime_winshared STATIC winshared/winutil.cc)
target_link_libraries(rime_winshared PUBLIC advapi32)

add_executable(rime_tests
  tests/test_main.cc
  tests/test_protocol.cc
  tests/test_keymap.cc
  tests/test_policy.cc
  tests/test_layout.cc
  tests/test_link_state.cc
  tests/test_schema_choice.cc
  tests/test_settings.cc
  tests/test_net_policy.cc
  tests/test_proto_compat.cc
  tests/test_schema_list_patch.cc
  # 這一支用**真的**鍵盤佈局(LoadKeyboardLayout + ToUnicodeEx),
  # 是 CI 上唯一驗得到「有沒有真的問佈局」的地方。
  tests/test_win32_layouts.cc
  tsf/win32_oracle.cc)
target_link_libraries(rime_tests PRIVATE rime_common user32)

# ── TSF 瘦 DLL ────────────────────────────────────────────────────────
#
# 相依刻意壓到最小。uuid.lib 是為了 TSF 的介面 GUID(IID_ITfThreadMgr、
# CLSID_TF_CategoryMgr、GUID_TFCAT_* …)—— 它是**靜態**庫,只帶常數進來,
# 不會在匯入表裡多一個 DLL。
#
# ⚠ 不要加 msctf.lib:現代的 Windows SDK **沒有這個檔案**(TSF 完全是 COM,
#   沒有要匯入的函式),連上去只會得到 LNK1181: cannot open input file。
#   實測過(CI run #15)。GUID 全部在 uuid.lib 裡。
#
# 真正不可以出現的是 VCRUNTIME140 / MSVCP140 / ucrtbase 那一組 —— 那代表
# /MT 沒生效,而後果是「在某些程式裡輸入法整個不存在」。
# ⚠ lang_bar.cc 是這一輪唯一加進 DLL 的檔案,而且是不得不加的:
#   ITfLangBarItem 是 TSF 向**文字服務**要的介面,語言列的按鈕只能在這裡。
#   它不畫東西、不載字型、不建視窗,按下去只送一則單向 IPC 訊息。
#   設定介面本體在服務進程那一側(service/settings_window.cc)。
add_library(rime_tsf SHARED
  tsf/dll.cc
  tsf/guids.cc
  tsf/registration.cc
  tsf/text_service.cc
  tsf/ipc_client.cc
  tsf/lang_bar.cc
  tsf/win32_oracle.cc
  tsf/rime_tsf.def)
# oleaut32:SysAllocString(語言列按鈕的 BSTR)。它是 COM 的字串配置器,
# 不是新的能力面 —— 而且**沒有任何網路 DLL**,那是 check_binaries.sh 守的。
target_link_libraries(rime_tsf PRIVATE rime_common rime_winshared
                                        kernel32 user32 advapi32 ole32 oleaut32
                                        uuid)
set_target_properties(rime_tsf PROPERTIES OUTPUT_NAME "rime_tsf")

# ── 安裝程式的手腳 ────────────────────────────────────────────────
#
# 註冊 / 反註冊 / 檢查 / 停服務。安裝程式(Inno Setup)呼叫的就是它,
# 而 CI 的 windows/verify_installer.sh 也靠它的 check 動詞做斷言。
#
# 它與 DLL 共用 tsf/registration.cc —— 那是刻意的:兩份實作會漂移,
# 而漂移的症狀是「用安裝程式裝的能用,自己 regsvr32 的不能用」。
# registration_check.cc 則**只**在這裡,不進 DLL:DLL 住在每一個宿主進程裡,
# 沒有理由為了「檢查」多背一份程式碼(見 windows/check_binaries.sh 的允許清單)。
#
# 它也不需要 librime —— 所以它在 logic 那一組就建得出來,快速 job 就驗得到。
add_executable(rime_ime_setup
  setup/setup_main.cc
  tsf/guids.cc
  tsf/registration.cc
  tsf/registration_check.cc)
target_include_directories(rime_ime_setup PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rime_ime_setup PRIVATE rime_winshared
                                             kernel32 user32 advapi32 ole32 uuid)

add_executable(rime_probe
  tests/probe_main.cc
  tsf/ipc_client.cc)
target_link_libraries(rime_probe PRIVATE rime_common rime_winshared)

# ── 第二組:需要 librime ──────────────────────────────────────────────

if(NOT RIME_PREFIX)
  message(STATUS "沒有 RIME_PREFIX:跳過 rime_service 與 rime_console。")
  message(STATUS "  (這是正常的 —— 瘦 DLL 與純邏輯測試本來就不該需要 librime。)")
  return()
endif()

if(NOT EXISTS "${RIME_PREFIX}/include/rime_api.h")
  message(FATAL_ERROR "${RIME_PREFIX} 裡沒有 rime_api.h;先跑 windows/build.sh deps")
endif()

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
  "${RIME_LIB}" "${OPENCC_LIB}" "${MARISA_LIB}"
  "${LEVELDB_LIB}" "${YAMLCPP_LIB}" "${GLOG_LIB}")

# 系統庫:
#   dbghelp — glog 0.7 的 stacktrace 用得到,而 glog 的 CMake 忘了自己帶上
#             (librime 的 src/CMakeLists.txt 為 WIN32 補了同一條)。
#   ws2_32  — leveldb / glog 在 Windows 上的網路與主機名相關呼叫。
#   bcrypt  — boost::uuid 在 MSVC 上取亂數用。Boost 有 auto-link pragma 會自己
#             帶上,這裡明寫一次,免得哪天 BOOST_ALL_NO_LIB 被開起來就斷。
set(RIME_SYSTEM_LIBS dbghelp ws2_32 bcrypt)

add_executable(rime_console
  "${RIME_ROOT}/tools/rime_console.cc"
  "${RIME_ROOT}/core/src/rime_shell.cc")
target_include_directories(rime_console PRIVATE
  "${RIME_ROOT}/core/include" "${RIME_PREFIX}/include")
target_link_libraries(rime_console PRIVATE ${RIME_STATIC_LIBS} ${RIME_SYSTEM_LIBS})

add_executable(rime_service
  service/main.cc
  service/engine.cc
  service/pipe_server.cc
  service/cand_window.cc
  service/settings_store.cc
  service/settings_window.cc
  "${RIME_ROOT}/core/src/rime_shell.cc")
target_include_directories(rime_service PRIVATE "${RIME_PREFIX}/include")
target_link_libraries(rime_service PRIVATE
  rime_common rime_winshared
  ${RIME_STATIC_LIBS} ${RIME_SYSTEM_LIBS}
  # comctl32:設定視窗的分頁控制項。shell32:系統匣圖示與「開啟資料夾」。
  user32 gdi32 shell32 comctl32)
# /MAP:服務進程崩掉時,崩潰過濾器印的是 RVA;要把 RVA 變成函式名就需要這份
# 對照表。CI 會把它一起上傳。這是在沒有偵錯器、沒有符號伺服器的機器上
# 唯一查得動的辦法 —— 而 Windows 端的建置機器正是這種機器。
target_link_options(rime_service PRIVATE /MAP)

message(STATUS "rime    : ${RIME_LIB}")
message(STATUS "opencc  : ${OPENCC_LIB}")
message(STATUS "marisa  : ${MARISA_LIB}")
message(STATUS "leveldb : ${LEVELDB_LIB}")
message(STATUS "yaml-cpp: ${YAMLCPP_LIB}")
message(STATUS "glog    : ${GLOG_LIB}")
