#include "ui_font.h"

#include <map>
#include <mutex>
#include <string>

#include "../common/ui_dip.h"

namespace rimewin {
namespace {

struct EnumCtx {
  const wchar_t* want;
  bool found;
};

int CALLBACK EnumProc(const LOGFONTW* lf, const TEXTMETRICW*, DWORD, LPARAM p) {
  EnumCtx* ctx = reinterpret_cast<EnumCtx*>(p);
  if (lf && ::lstrcmpiW(lf->lfFaceName, ctx->want) == 0) {
    ctx->found = true;
    return 0;  // 找到就停
  }
  return 1;
}

std::mutex g_exists_mu;
std::map<std::wstring, bool> g_exists;

// §12.8.2 的解析順序。
//
// ⚠ `$system` 的第一順位寫的是 **Segoe UI Variable Text**,不是
//   `Segoe UI Variable`。後者是家族名,含 Display/Text/Small 三種視覺尺寸,
//   `CreateFontIndirectW` 拿家族名會挑到哪一個沒有保證。UI 內文要的是 Text。
//   (規格自承沒有在 Win11 上列舉字體確認過這個確切字串。錯了也不會壞:
//    FontExists 會回 false,然後安靜地退到 Segoe UI —— 退化路徑是對的。)
const wchar_t* const kLatin[] = {L"Segoe UI Variable Text", L"Segoe UI",
                                 L"Tahoma", L"MS Shell Dlg 2", nullptr};
const wchar_t* const kHant[] = {L"Microsoft JhengHei UI", L"Microsoft JhengHei",
                                L"MingLiU", nullptr};
const wchar_t* const kHans[] = {L"Microsoft YaHei UI", L"Microsoft YaHei",
                                L"SimSun", nullptr};
const wchar_t* const kMono[] = {L"Cascadia Mono", L"Consolas", L"Courier New",
                                nullptr};

}  // namespace

bool FontExists(const wchar_t* family) {
  if (!family || !*family) return false;
  const std::wstring key(family);
  {
    std::lock_guard<std::mutex> lock(g_exists_mu);
    auto it = g_exists.find(key);
    if (it != g_exists.end()) return it->second;
  }

  LOGFONTW lf{};
  lf.lfCharSet = DEFAULT_CHARSET;
  ::lstrcpynW(lf.lfFaceName, family, LF_FACESIZE);
  EnumCtx ctx{family, false};
  HDC hdc = ::GetDC(nullptr);
  if (hdc) {
    ::EnumFontFamiliesExW(hdc, &lf, &EnumProc, reinterpret_cast<LPARAM>(&ctx),
                          0);
    ::ReleaseDC(nullptr, hdc);
  }

  std::lock_guard<std::mutex> lock(g_exists_mu);
  g_exists[key] = ctx.found;
  return ctx.found;
}

std::wstring ResolveFamily(FontRole role, Script script) {
  if (role == FontRole::kMono) {
    for (const wchar_t* const* p = kMono; *p; ++p)
      if (FontExists(*p)) return std::wstring(*p);
    return std::wstring();
  }

  // CJK 的 run 先走 script 專屬的堆疊,再退回拉丁堆疊。
  // ⚠ 反過來(先拉丁)的話,中文介面會拿到一個沒有漢字的字體,
  //   然後整片交給 GDI 的 font linking —— 那能顯示,但字形不是我們選的,
  //   而且同一句話裡的中英文會來自兩套設計不同的字體。
  const wchar_t* const* first = nullptr;
  if (script == Script::kHant) first = kHant;
  if (script == Script::kHans) first = kHans;
  if (first) {
    for (const wchar_t* const* p = first; *p; ++p)
      if (FontExists(*p)) return std::wstring(*p);
  }
  for (const wchar_t* const* p = kLatin; *p; ++p)
    if (FontExists(*p)) return std::wstring(*p);

  // 全部不存在 → 留空字串,交給 GDI 依 lfPitchAndFamily 挑。
  return std::wstring();
}

void FontSet::Clear() {
  for (auto& kv : cache_)
    if (kv.second) ::DeleteObject(kv.second);
  cache_.clear();
}

void FontSet::Reset(UINT dpi, Script script) {
  // ⚠ 一定要先 Clear:字型是像素單位的,舊的那一組在新 DPI 下是錯的,
  //   而留著不刪就是 GDI 物件洩漏(症狀:開幾小時之後畫面開始畫不出來)。
  Clear();
  dpi_ = dpi ? dpi : 96;
  script_ = script;
}

HFONT FontSet::Get(int size_dip, bool semibold, FontRole role) {
  const Key k{size_dip, semibold ? FW_SEMIBOLD : FW_NORMAL,
              static_cast<int>(role)};
  auto it = cache_.find(k);
  if (it != cache_.end()) return it->second;

  const std::wstring family = ResolveFamily(role, script_);

  LOGFONTW lf{};
  // §12.3:lfHeight = -MulDiv(size_dip, dpi, 96)。負值 = 字元高度。
  lf.lfHeight = FontHeightFromDip(size_dip, static_cast<int>(dpi_));
  lf.lfWeight = semibold ? FW_SEMIBOLD : FW_NORMAL;
  lf.lfCharSet = DEFAULT_CHARSET;
  lf.lfQuality = CLEARTYPE_QUALITY;
  lf.lfPitchAndFamily = role == FontRole::kMono
                            ? (FIXED_PITCH | FF_MODERN)
                            : (VARIABLE_PITCH | FF_SWISS);
  if (!family.empty())
    ::lstrcpynW(lf.lfFaceName, family.c_str(), LF_FACESIZE);
  // family 為空時 lfFaceName 留空 —— 見標頭:那比硬塞一個名字好。

  HFONT f = ::CreateFontIndirectW(&lf);
  cache_[k] = f;
  return f;
}

}  // namespace rimewin
