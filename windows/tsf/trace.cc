#include "trace.h"

// 診斷記錄檔住在 %LOCALAPPDATA%\<資料夾名>\diagnostics\ 底下,而<資料夾名>
// 與 %APPDATA% 底下那個是**同一個**。原本這裡抄了一份字面值,於是產品
// 改名時有兩個地方要改。見 winshared/winutil.h 的 RimeUserDataFolderName()。
#include "../winshared/winutil.h"

#include <stdarg.h>
#include <stdio.h>

namespace rimewin {
namespace {

// 一整個檔案的上限。超過就從頭來過(不做輪替 —— 兩個檔案在診斷時只會讓
// 「最近的錯誤在哪一個裡面」變成一個新問題)。
// 1 MiB 大約是兩萬行,遠多於任何一次診斷需要的量。
constexpr LONGLONG kMaxFileBytes = 1024 * 1024;

// **這一個進程**最多寫多少行。防的是一種很實際的失敗:某個宿主每秒切換
// 焦點好幾次(瀏覽器分頁),於是記錄檔被那一種事件塞滿,而真正有用的
// 前幾行被上面的上限捲掉了。
constexpr int kMaxLinesPerProcess = 400;

SRWLOCK g_lock = SRWLOCK_INIT;
int g_lines = 0;
// 0 = 還沒決定, 1 = 開著, 2 = 關掉
LONG g_state = 0;
wchar_t g_path[MAX_PATH] = {0};
char g_host[64] = {0};

void ComputeHostName() {
  wchar_t path[MAX_PATH] = {0};
  const DWORD n = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
  const wchar_t* base = path;
  for (DWORD i = 0; i < n; ++i)
    if (path[i] == L'\\' || path[i] == L'/') base = path + i + 1;
  // 只取檔名,而且只轉 ASCII。宿主的路徑可能含中文,而這裡不值得為了
  // 一個診斷欄位把 MultiByteToWideChar 那一整套(以及它的失敗模式)拉進來。
  int j = 0;
  for (; base[j] != 0 && j < static_cast<int>(sizeof(g_host)) - 1; ++j) {
    const wchar_t c = base[j];
    g_host[j] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
  }
  g_host[j] = 0;
  if (j == 0) {
    g_host[0] = '?';
    g_host[1] = 0;
  }
}

// 一層一層地建。SHCreateDirectoryEx 在 shell32 裡,而 shell32 不在瘦 DLL 的
// 相依允許清單上(check_binaries.sh)——為了一個診斷目錄多背一個 DLL,
// 等於多一種「在某台機器上載入失敗」的可能。
void EnsureParentDirs(const wchar_t* path) {
  wchar_t buf[MAX_PATH];
  int n = 0;
  while (path[n] != 0 && n < MAX_PATH - 1) {
    buf[n] = path[n];
    ++n;
  }
  buf[n] = 0;
  for (int i = 3; i < n; ++i) {  // 從 "C:\" 之後開始
    if (buf[i] != L'\\' && buf[i] != L'/') continue;
    const wchar_t saved = buf[i];
    buf[i] = 0;
    ::CreateDirectoryW(buf, nullptr);
    buf[i] = saved;
  }
}

// 回傳 true = 記錄是開著的,g_path 已備妥。
bool EnsureReady() {
  if (g_state == 1) return true;
  if (g_state == 2) return false;
  ComputeHostName();
  if (!TraceFilePath(g_path, MAX_PATH)) {
    g_state = 2;
    return false;
  }
  EnsureParentDirs(g_path);
  g_state = 1;
  return true;
}

}  // namespace

bool TraceFilePath(wchar_t* out, size_t out_len) {
  if (!out || out_len < 8) return false;
  out[0] = 0;

  wchar_t env[MAX_PATH] = {0};
  const DWORD n = ::GetEnvironmentVariableW(L"RIME_TSF_TRACE", env, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    // ⚠ 只認**完整**的 "0" / "off"(不分大小寫)當「關掉」。
    //   不要寫成「開頭是 o 就當 off」—— 那會把 `O:\logs\rime.log` 這種
    //   完全合法的路徑當成關閉指令,而症狀是「我明明設了記錄檔,卻什麼都沒有」。
    const bool off =
        (env[0] == L'0' && env[1] == 0) ||
        ((env[0] == L'o' || env[0] == L'O') &&
         (env[1] == L'f' || env[1] == L'F') &&
         (env[2] == L'f' || env[2] == L'F') && env[3] == 0);
    if (off) return false;
    for (size_t i = 0; i < out_len; ++i) {
      out[i] = env[i];
      if (env[i] == 0) return true;
    }
    return false;  // 放不下
  }

  wchar_t local[MAX_PATH] = {0};
  const DWORD ln = ::GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
  if (ln == 0 || ln >= MAX_PATH) return false;

  // 手動接字串。這裡不可以用 std::wstring —— 這段程式碼會在 DllMain 的
  // 載入器鎖底下跑一次,那裡不該配置堆積。
  //
  // ⚠ 資料夾名向 winshared 要,**不要在這裡再抄一份**。抄一份的代價不是
  //   多打幾個字,是「改名時漏掉這一處」——而漏掉不會編譯失敗、不會報錯,
  //   只會讓記錄寫進一個叫舊名字的資料夾,然後 doctor 說它找不到記錄檔。
  //   RimeUserDataFolderName() 回傳靜態字串,不配置堆積。
  const wchar_t* const folder = RimeUserDataFolderName();
  static const wchar_t kHead[] = L"\\";
  static const wchar_t kTail[] = L"\\diagnostics\\tsf.log";
  size_t i = 0;
  for (; i < out_len - 1 && local[i] != 0; ++i) out[i] = local[i];
  for (size_t k = 0; kHead[k] != 0; ++k) {
    if (i >= out_len - 1) return false;
    out[i++] = kHead[k];
  }
  for (size_t k = 0; folder[k] != 0; ++k) {
    if (i >= out_len - 1) return false;
    out[i++] = folder[k];
  }
  for (size_t k = 0; kTail[k] != 0; ++k) {
    if (i >= out_len - 1) return false;
    out[i++] = kTail[k];
  }
  out[i] = 0;
  return true;
}

const char* TraceHostName() {
  if (g_host[0] == 0) ComputeHostName();
  return g_host;
}

void Trace(const char* fmt, ...) {
  // 先把訊息格式化好,再進鎖。格式化是這裡最慢的一步,而鎖保護的只有
  // 「同一個進程裡兩條執行緒同時寫」——宿主可以有幾十條執行緒。
  char msg[900];
  va_list ap;
  va_start(ap, fmt);
  const int mn = ::vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  if (mn < 0) return;

  SYSTEMTIME st;
  ::GetLocalTime(&st);

  char line[1024];
  const int n = ::snprintf(
      line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu %s | %s\r\n",
      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
      st.wMilliseconds, static_cast<unsigned long>(::GetCurrentProcessId()),
      TraceHostName(), msg);
  if (n <= 0) return;
  const DWORD len = static_cast<DWORD>(n < static_cast<int>(sizeof(line))
                                           ? n
                                           : static_cast<int>(sizeof(line)) - 1);

  ::AcquireSRWLockExclusive(&g_lock);
  if (g_lines < kMaxLinesPerProcess && EnsureReady()) {
    ++g_lines;
    // 每一行開一次檔案再關掉,不留長壽的控制代碼。
    //
    // ⚠ 這是刻意的取捨。留著控制代碼會快一點,但那代表**每一個接受文字
    //   輸入的進程**都一直握著同一個檔案 —— 瀏覽器可以開好幾天。
    //   要寫的行數本來就是有上限的(見上面兩個常數),省下的時間沒有價值,
    //   而「使用者刪不掉自己的記錄檔」是實實在在的困擾。
    //   FILE_SHARE_* 三個都給:別的宿主同時在寫,而使用者可能正在看它。
    HANDLE h = ::CreateFileW(
        g_path, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      LARGE_INTEGER size{};
      if (::GetFileSizeEx(h, &size) && size.QuadPart > kMaxFileBytes) {
        ::CloseHandle(h);
        // 從頭來過。舊的內容是**更舊的**故障,而診斷要看的是最近這一次。
        h = ::CreateFileW(g_path, GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      }
    }
    if (h != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      ::WriteFile(h, line, len, &written, nullptr);
      ::CloseHandle(h);
    }
    // 失敗一律安靜:診斷壞掉不該讓輸入法跟著壞掉。
  }
  ::ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace rimewin
