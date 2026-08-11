// windows/common/key_eat_policy.cc — 見 key_eat_policy.h 的檔頭
//
// 不 include windows.h。keysym 常數與 common/keymap.cc 同源(X11),
// 這裡重列一份是刻意的:keymap.cc 的那一份在匿名 namespace 裡,
// 而把它公開出去等於讓「keysym 表」多一個對外的面。兩份都是抄自 X11 的
// keysymdef.h,值不會變 —— 而且 tests/test_key_eat_policy.cc 會拿
// LayoutIndependentKeysym() 的**實際輸出**來對帳,所以抄錯會被抓到。

#include "key_eat_policy.h"

namespace rimewin {
namespace {

// X11 keysymdef.h。
constexpr int32_t kBackSpace = 0xFF08, kTab = 0xFF09, kClear = 0xFF0B,
                  kReturn = 0xFF0D, kPause = 0xFF13, kScrollLock = 0xFF14,
                  kEscape = 0xFF1B, kHome = 0xFF50, kLeft = 0xFF51,
                  kUp = 0xFF52, kRight = 0xFF53, kDown = 0xFF54,
                  kPageUp = 0xFF55, kPageDown = 0xFF56, kEnd = 0xFF57,
                  kBegin = 0xFF58, kSelect = 0xFF60, kPrint = 0xFF61,
                  kExecute = 0xFF62, kInsert = 0xFF63, kMenu = 0xFF67,
                  kHelp = 0xFF6A, kNumLock = 0xFF7F, kKpEnter = 0xFF8D,
                  kKpHome = 0xFF95, kKpLeft = 0xFF96, kKpUp = 0xFF97,
                  kKpRight = 0xFF98, kKpDown = 0xFF99, kKpPrior = 0xFF9A,
                  kKpNext = 0xFF9B, kKpEnd = 0xFF9C, kKpBegin = 0xFF9D,
                  kKpInsert = 0xFF9E, kKpDelete = 0xFF9F,
                  kKpMultiply = 0xFFAA, kKpDivide = 0xFFAF, kKp0 = 0xFFB0,
                  kKp9 = 0xFFB9, kF1 = 0xFFBE, kF24 = 0xFFD5,
                  kShiftL = 0xFFE1, kSuperR = 0xFFEC, kDelete = 0xFFFF;

// keymap.h 的 Mod 位元。這裡不 include keymap.h 是為了讓這個檔案能單獨編譯,
// 值必須一致 —— tests/test_key_eat_policy.cc 有一條斷言在對帳。
constexpr uint32_t kCtrlBit = 1u << 1;
constexpr uint32_t kAltBit = 1u << 2;
constexpr uint32_t kSuperBit = 1u << 3;

// 這個 keysym 代不代表一個真的可以寫進文件的字元。
//
// X11 的規則:0x20–0x7E 與 0xA0–0xFF 的 keysym 就是 Unicode 碼點本身
// (Latin-1 區段刻意對齊),其餘的 Unicode 一律是 0x01000000 | 碼點。
// 見 common/keymap.h 的 KeysymFromChar。
bool IsCharacterKeysym(int32_t k) {
  if (k >= 0x20 && k <= 0x7E) return true;
  if (k >= 0xA0 && k <= 0xFF) return true;
  if (k >= 0x01000000 && k <= 0x0110FFFF) return true;
  return false;
}

}  // namespace

KeyKind ClassifyKeyKind(int32_t keysym, uint32_t modifiers) {
  if (keysym == 0) return KeyKind::kUnmappable;

  // ⚠ Ctrl / Alt / Win 一律先攔掉,**在任何其他判斷之前**。
  //
  //   Ctrl+C 的 keysym 就是 'c',Alt+F4 的就是 F4。舊規則會把 Ctrl+C 當成
  //   一個字元鍵吃掉,然後引擎不處理,那顆鍵就消失了 —— 使用者複製不了東西。
  //   (Shift 與 CapsLock 不在這裡:它們是字元的一部分,大寫 A 仍然是字元。)
  if (modifiers & (kCtrlBit | kAltBit | kSuperBit)) return KeyKind::kHostOnly;

  switch (keysym) {
    // ── 編輯組字串 ────────────────────────────────────────────
    case kBackSpace:
    case kDelete:
    case kKpDelete:
      return KeyKind::kEditing;

    // ── 移動與翻頁 ────────────────────────────────────────────
    case kLeft:
    case kRight:
    case kUp:
    case kDown:
    case kHome:
    case kEnd:
    case kPageUp:
    case kPageDown:
    case kBegin:
    case kKpLeft:
    case kKpRight:
    case kKpUp:
    case kKpDown:
    case kKpHome:
    case kKpEnd:
    case kKpPrior:
    case kKpNext:
    case kKpBegin:
      return KeyKind::kNavigation;

    // ── 結束組字 ──────────────────────────────────────────────
    case kReturn:
    case kKpEnter:
    case kEscape:
    case kTab:
      return KeyKind::kFinish;

    // ── 明擺著不是我們的 ──────────────────────────────────────
    //
    // Insert / Clear / Pause / ScrollLock / NumLock / PrintScreen / Menu /
    // Help / Select / Execute:這些鍵我們一件事都沒有實作,而
    // 「沒有實作的就不要吃掉」是本檔的規則。
    case kInsert:
    case kKpInsert:
    case kClear:
    case kPause:
    case kScrollLock:
    case kNumLock:
    case kPrint:
    case kExecute:
    case kSelect:
    case kMenu:
    case kHelp:
      return KeyKind::kHostOnly;

    default:
      break;
  }

  // F1–F24。
  if (keysym >= kF1 && keysym <= kF24) return KeyKind::kHostOnly;
  // 修飾鍵本身(Shift_L … Super_R)。吃掉它們會讓使用者按不了任何組合鍵。
  //
  // ⚠ 這裡原本寫「TSF 本來就不太會把它們交過來,但交過來時…」。**「不太會」
  //   那半句已經量掉了:TSF 會交過來。** 出處 CI run 31511075812
  //   (sha ca97498)logic-x64 的「真的經過 TSF」,量法見
  //   windows/tests/tsf_host_main.cc 的 MeasureShiftDelivery(送一次左 Shift,
  //   數 trace 檔多了幾行)。量到 SHIFT_TRACE_LINES=1,而那一行是
  //   OnTestKeyDown 自己寫的:vk=0x10 scan=0x2A keysym=0xFFE1 族=host-only
  //   吃掉=0 —— 也就是**這一格的判斷當場被實測驗證了一次**。
  //   (tsf/text_service.cc 的 OnTestKeyUp 曾經寫著相反的話,已照實測改寫。)
  //
  // ⚠ 「輕點 Shift 切中英」要做的話**不是**改這一格的分類:分類仍然是不吃
  //   (三支 sink 的 *eaten 實測都是 0),偵測輕點是 sink 那一層的狀態機。
  if (keysym >= kShiftL && keysym <= kSuperR) return KeyKind::kHostOnly;

  // 數字鍵台的數字與運算符號。**是字元鍵** —— 使用者按數字鍵台的 3
  // 期待的是一個「3」,而在組字中它是選第三個候選。兩者都由 kCharacter
  // 那條路處理(引擎不吃就自己補),所以這裡歸成字元。
  if (keysym >= kKp0 && keysym <= kKp9) return KeyKind::kCharacter;
  if (keysym >= kKpMultiply && keysym <= kKpDivide) return KeyKind::kCharacter;

  if (IsCharacterKeysym(keysym)) return KeyKind::kCharacter;

  // 走到這裡 = 一個我們沒有分類過的 keysym。**不吃。**
  //
  // ⚠ 預設值的方向是這一格最重要的一行。預設「吃」的話,日後 keymap 多支援
  //   一顆鍵,那顆鍵就會安靜地變成黑洞;預設「不吃」的話,最壞的情況是
  //   那顆鍵進不了引擎 —— 使用者看得到、查得出來、而且不會弄壞別的東西。
  return KeyKind::kHostOnly;
}

bool ShouldEatKey(KeyKind kind, bool composing) {
  switch (kind) {
    case KeyKind::kUnmappable:
    case KeyKind::kHostOnly:
      return false;
    case KeyKind::kCharacter:
      // 組字中與不組字都吃。理由見檔頭:哪些字元會起頭是方案決定的,
      // 而 DLL 不知道方案(注音的 alphabet 含數字)。
      return true;
    case KeyKind::kEditing:
    case KeyKind::kNavigation:
    case KeyKind::kFinish:
      // ⚠ 這一行就是使用者回報的那個 bug 的修法。
      //   沒有組字時,退格是宿主的鍵。
      return composing;
  }
  return false;
}

bool ShouldSelfInsert(KeyKind kind) { return kind == KeyKind::kCharacter; }

char32_t CharForSelfInsert(int32_t keysym) {
  if (keysym >= kKp0 && keysym <= kKp9)
    return static_cast<char32_t>('0' + (keysym - kKp0));
  switch (keysym) {
    case kKpMultiply: return U'*';
    case 0xFFAB:      return U'+';  // XK_KP_Add
    case 0xFFAC:      return U',';  // XK_KP_Separator
    case 0xFFAD:      return U'-';  // XK_KP_Subtract
    case 0xFFAE:      return U'.';  // XK_KP_Decimal
    case kKpDivide:   return U'/';
    default:          break;
  }
  if (keysym >= 0x20 && keysym <= 0x7E) return static_cast<char32_t>(keysym);
  if (keysym >= 0xA0 && keysym <= 0xFF) return static_cast<char32_t>(keysym);
  if (keysym >= 0x01000000 && keysym <= 0x0110FFFF)
    return static_cast<char32_t>(keysym - 0x01000000);
  return 0;
}

const char* KeyKindTag(KeyKind kind) {
  switch (kind) {
    case KeyKind::kUnmappable: return "unmappable";
    case KeyKind::kCharacter:  return "character";
    case KeyKind::kEditing:    return "editing";
    case KeyKind::kNavigation: return "navigation";
    case KeyKind::kFinish:     return "finish";
    case KeyKind::kHostOnly:   return "host-only";
  }
  return "?";
}

}  // namespace rimewin
