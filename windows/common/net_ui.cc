#include "net_ui.h"

#include <ctime>
#include <cwchar>

namespace rimewin {
namespace {

// 欄與欄之間的標點。⚠ 一律寫成 `\u` 逃脫,不寫真的字元。
//
//   這幾個(U+00B7 中點、U+FF1A 全形冒號、U+FF08/FF09 全形括號)目前
//   都**不在** tools/cjk_literal_scan.py 掃的區間裡,所以寫成字元也不會
//   被 W7 判違規 —— 但那是「現在剛好沒事」,不是規則。那支腳本的區間
//   哪天補上全形形式,這個檔案就會變成違規,而它其實一個介面文案都沒有。
//   逃脫寫法讓本檔的原始碼保持純 ASCII,那個依賴就不存在了。
constexpr const wchar_t* kSep = L"  \u00B7  ";
// 原因與它的附註(例如套件名)之間。
constexpr const wchar_t* kLabelSep = L"\uFF1A";
constexpr const wchar_t* kParenOpen = L"\uFF08";
constexpr const wchar_t* kParenClose = L"\uFF09";

// 向下取整的除法。⚠ C++ 的 / 對負數是向零取整,而時間戳算日期時
// 那個差別就是「1969 年的那一天整個算錯」。紀錄裡不該有負的時間戳,
// 但格式化不可以是崩潰或亂數的來源。
int64_t FloorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
  return q;
}

// 天數(自 1970-01-01)→ 年月日。Howard Hinnant 的 civil_from_days,
// 對 1970 以前也成立。
void CivilFromDays(int64_t z, int* year, int* month, int* day) {
  z += 719468;
  const int64_t era = FloorDiv(z, 146097);
  const int64_t doe = z - era * 146097;                            // [0,146096]
  const int64_t yoe =
      (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;       // [0,399]
  const int64_t y = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);     // [0,365]
  const int64_t mp = (5 * doy + 2) / 153;                          // [0,11]
  const int64_t d = doy - (153 * mp + 2) / 5 + 1;                  // [1,31]
  const int64_t m = mp < 10 ? mp + 3 : mp - 9;                     // [1,12]
  *year = static_cast<int>(y + (m <= 2 ? 1 : 0));
  *month = static_cast<int>(m);
  *day = static_cast<int>(d);
}

std::wstring Pad(int v, int width) {
  std::wstring s = std::to_wstring(v < 0 ? -v : v);
  while (static_cast<int>(s.size()) < width) s.insert(s.begin(), L'0');
  if (v < 0) s.insert(s.begin(), L'-');
  return s;
}

// 上限保護:host 來自轉址目標,是對方控制得了的字串。net_policy.h 已經
// 在寫進紀錄時夾過長度,這裡再夾一次 —— 讀進來的紀錄檔可能是手改過的。
std::wstring Utf8ToWideSafe(const std::string& s, size_t max_chars) {
  // ⚠ 這裡**不做**真正的 UTF-8 解碼:common/ 不 include windows.h,而
  //   紀錄裡的主機名與附註都是 ASCII 為主。非 ASCII 位元組原樣升位,
  //   結果可能不好看,但**不會是崩潰,也不會是截斷到一半的多位元組**。
  //   真正需要漂亮顯示的欄位(原因、結果)全部來自 catalog。
  std::wstring out;
  for (unsigned char c : s) {
    if (out.size() >= max_chars) break;
    // 換行與 tab 一律換成空白:紀錄是一列一筆,一個換行就能在畫面上
    // 偽造出多一筆看起來無害的連線。
    out.push_back((c == '\n' || c == '\r' || c == '\t')
                      ? L' '
                      : static_cast<wchar_t>(c));
  }
  return out;
}

}  // namespace

// ── 開關 ────────────────────────────────────────────────────────

UiString NetSwitchSummary(bool enabled) {
  return enabled ? UiString::kNetworkOnSummary : UiString::kNetworkOffSummary;
}

UiString NetSwitchStatus(bool enabled) {
  return enabled ? UiString::kNetworkTurnedOn : UiString::kNetworkTurnedOff;
}

// ── 格式化 ──────────────────────────────────────────────────────

std::wstring FormatNetLogTime(int64_t at_ms, int tz_offset_minutes) {
  const int64_t secs =
      FloorDiv(at_ms, 1000) + static_cast<int64_t>(tz_offset_minutes) * 60;
  const int64_t days = FloorDiv(secs, 86400);
  int64_t rem = secs - days * 86400;
  int y = 0, mo = 0, d = 0;
  CivilFromDays(days, &y, &mo, &d);
  const int hh = static_cast<int>(rem / 3600);
  rem -= static_cast<int64_t>(hh) * 3600;
  const int mi = static_cast<int>(rem / 60);
  const int ss = static_cast<int>(rem - static_cast<int64_t>(mi) * 60);
  std::wstring s = Pad(y, 4);
  s += L'-';
  s += Pad(mo, 2);
  s += L'-';
  s += Pad(d, 2);
  s += L' ';
  s += Pad(hh, 2);
  s += L':';
  s += Pad(mi, 2);
  s += L':';
  s += Pad(ss, 2);
  return s;
}

std::wstring FormatNetBytes(int64_t bytes) {
  if (bytes < 0) bytes = 0;
  constexpr int64_t kKb = 1024;
  constexpr int64_t kMb = 1024 * 1024;
  if (bytes >= kMb) {
    const int64_t whole = bytes / kMb;
    const int64_t tenth = (bytes % kMb) * 10 / kMb;
    return std::to_wstring(whole) + L'.' + std::to_wstring(tenth) + L" MB";
  }
  if (bytes >= kKb) return std::to_wstring(bytes / kKb) + L" KB";
  return std::to_wstring(bytes) + L" B";
}

const wchar_t* NetPurposeUiText(NetPurpose p, UiLang lang) {
  switch (p) {
    case NetPurpose::kStorePackage:
      return UiTextIn(lang, UiString::kNetPurposePackage);
    case NetPurpose::kStoreIndex:
    default:
      return UiTextIn(lang, UiString::kNetPurposeIndex);
  }
}

const wchar_t* NetOutcomeUiText(NetOutcome o, UiLang lang) {
  switch (o) {
    case NetOutcome::kOk:
      return UiTextIn(lang, UiString::kNetOutcomeOk);
    case NetOutcome::kRedirected:
      return UiTextIn(lang, UiString::kNetOutcomeRedirected);
    case NetOutcome::kFailed:
    default:
      return UiTextIn(lang, UiString::kNetOutcomeFailed);
  }
}

int LocalTzOffsetMinutes() {
  // std::mktime 把「本地時間的年月日時分秒」換回 time_t,所以拿同一個
  // 瞬間的 gmtime 結果去餵它,差值就是時區偏移。這是不需要平台 API 的
  // 唯一寫法(common/ 不 include windows.h)。
  const std::time_t now = std::time(nullptr);
  std::tm g{};
  // ⚠ 分支條件是 `_MSC_VER` 而不是 `_WIN32`:mingw 只是拿來做語法檢查的
  //   (windows/syntax_check_mingw.sh),而 mingw 的 gmtime_s 要靠
  //   MINGW_HAS_SECURE_API 才會宣告 —— 用 _WIN32 分支的話,語法檢查會在
  //   一個與產品無關的地方紅掉。plain gmtime 三個平台都有。
#if defined(_MSC_VER)
  if (gmtime_s(&g, &now) != 0) return 0;
#else
  const std::tm* gp = gmtime(&now);
  if (gp == nullptr) return 0;
  g = *gp;
#endif
  g.tm_isdst = -1;
  const std::time_t back = std::mktime(&g);
  if (back == static_cast<std::time_t>(-1)) return 0;
  const double diff = std::difftime(now, back);
  return static_cast<int>(diff / 60.0);
}

// ── 連網紀錄 ────────────────────────────────────────────────────

NetLogView BuildNetLogView(const std::vector<NetLogEntry>& entries, UiLang lang,
                           int tz_offset_minutes) {
  NetLogView v;
  v.count = static_cast<int>(entries.size());
  v.empty = entries.empty();

  if (v.empty) {
    // ⚠ 空的時候**也要有一句話**。一片空白讓人分不出「沒連過」與
    //   「壞掉了」,而「開關從沒開過所以這裡是空的」正是使用者驗證
    //   我們的方式 —— 那句話必須寫在畫面上,不能靠使用者自己推。
    v.summary = UiTextIn(lang, UiString::kNetLogEmptyTitle);
    return v;
  }

  {
    wchar_t buf[160];
    const int n = std::swprintf(buf, sizeof(buf) / sizeof(buf[0]),
                                UiTextIn(lang, UiString::kNetLogCount),
                                v.count);
    v.summary = n > 0 ? std::wstring(buf, static_cast<size_t>(n))
                      : std::wstring(UiTextIn(lang, UiString::kNetLogHeading));
  }

  // 由新到舊。紀錄檔本身是由舊到新(追加寫),但畫面上最有用的那一筆
  // 是最後一次連線。
  v.rows.reserve(entries.size());
  for (size_t i = entries.size(); i > 0; --i) {
    const NetLogEntry& e = entries[i - 1];
    NetLogRow r;
    r.when = FormatNetLogTime(e.at_ms, tz_offset_minutes);
    r.host = Utf8ToWideSafe(e.host, kMaxLogHost);
    r.reason = NetPurposeUiText(e.purpose, lang);
    if (!e.label.empty()) {
      r.reason += kLabelSep;
      r.reason += Utf8ToWideSafe(e.label, kMaxLogLabel);
    }
    r.outcome = NetOutcomeUiText(e.outcome, lang);
    if (e.outcome == NetOutcome::kOk) {
      r.outcome += kParenOpen;
      r.outcome += FormatNetBytes(e.bytes);
      r.outcome += kParenClose;
    } else if (!e.detail.empty()) {
      r.outcome += kParenOpen;
      r.outcome += Utf8ToWideSafe(e.detail, kMaxLogDetail);
      r.outcome += kParenClose;
    }
    r.line = r.when;
    r.line += kSep;
    r.line += r.host;
    r.line += kSep;
    r.line += r.reason;
    r.line += kSep;
    r.line += r.outcome;
    v.rows.push_back(r);
  }
  return v;
}

// ── 檢查更新 ────────────────────────────────────────────────────

UiString UpdateStateText(UpdateCheckState s) {
  switch (s) {
    case UpdateCheckState::kUpToDate:
      return UiString::kUpdateUpToDate;
    case UpdateCheckState::kAvailable:
      return UiString::kUpdateAvailable;
    case UpdateCheckState::kBlocked:
      return UiString::kUpdateNeedsNetwork;
    case UpdateCheckState::kFailed:
      return UiString::kUpdateFailed;
    case UpdateCheckState::kNotWired:
    default:
      return UiString::kUpdateNotWired;
  }
}

UpdateAction DecideUpdateAction(bool network_enabled, bool already_running) {
  // ⚠ 開關**先問**。反過來的話,一次卡住的檢查會讓「開關是關的」
  //   永遠說不出口,而使用者看到的是「請稍候」——那句話在開關關著時
  //   是假的,而且它暗示我們正在連線。
  if (!network_enabled) return UpdateAction::kSwitchIsOff;
  if (already_running) return UpdateAction::kAlreadyRunning;
  return UpdateAction::kStart;
}

UiString UpdateActionText(UpdateAction a) {
  switch (a) {
    case UpdateAction::kSwitchIsOff:
      return UiString::kUpdateNeedsNetwork;
    case UpdateAction::kAlreadyRunning:
    case UpdateAction::kStart:
    default:
      return UiString::kUpdateChecking;
  }
}

}  // namespace rimewin
