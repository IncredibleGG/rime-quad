// windows/common/hotkey_policy.h — 中英切換的鍵盤快捷鍵(Ctrl+空白鍵)
//
// ══ 使用者原話 ═══════════════════════════════════════════════════
//
// 「ctrl+ 空格沒辦法切中英文 這個應該是所有輸入法的基本配置」。
//
// 他是對的:微軟拼音、搜狗、Gboard、macOS 內建的注音與拼音,全部是這個鍵。
// 在這一輪之前,Windows 端切中英的入口只有懸浮狀態列的第一格、系統匣、
// 與設定視窗 —— 全部要**放開鍵盤去點滑鼠**,而中英切換發生在句子中間。
//
// ══ ⚠ 為什麼不是低階鍵盤 hook ═════════════════════════════════════
//
// `WH_KEYBOARD_LL` 會看到使用者在**每一個程式裡的每一次按鍵**,包含密碼
// 欄位。我們要求別人相信這支輸入法不偷看,那就不能裝一個看得到全部按鍵
// 的東西 —— 這條裁決寫在 service/status_bar.h 的檔頭與 docs/coordination.md
// 的 Windows 回報裡,不因為「只想攔一顆鍵」而鬆動。
//
// TSF 有正規的做法:`ITfKeystrokeMgr::PreserveKey`。它只在**我們自己的
// 文字服務被啟用時**生效,而且是 TSF 幫我們比對、只把命中的那一顆
// 經由 `OnPreservedKey` 交回來 —— 我們看不到任何其他按鍵。
//
// ══ ⚠ 為什麼不走 OnTestKeyDown ════════════════════════════════════
//
// `common/key_eat_policy.h` 記著這個專案吃過的虧:舊的 `OnTestKeyDown`
// 宣稱「每一個映得出 keysym 的鍵都是我的」,結果吃掉了 Ctrl+C / Ctrl+V、
// 退格、英文字母與 F 系列鍵。現在的規則是**帶 Ctrl / Alt / Win 的組合鍵
// 一律 `kHostOnly`,永遠不吃**。
//
// 要在那裡開一個 Ctrl+空白鍵的洞,就得在那張真值表上加一條例外,
// 而那張表正是「不要再吃掉別人的鍵」這件事的唯一防線。走 PreserveKey
// 則**完全不動它** —— TSF 在 key event sink 之前就把那一顆挑走了,
// key_eat_policy 一個字都不用改,Ctrl+C 的路徑一點都沒被碰到。
//
// ══ 這個檔案在回答什麼 ═══════════════════════════════════════════
//
// 「這一顆鍵是不是中英切換」在兩個進程裡各要問一次:
//
//   · 瘦 DLL:`OnPreservedKey` 收到 GUID 之後,把它翻成一顆按鍵送給服務;
//   · 服務  :收到那顆按鍵之後,決定是「切中英」還是「交給 librime」。
//
// 兩邊各寫一份 `keysym == ' ' && mods == Control` 就是兩份真相,
// 而漂移的樣子是「按下去沒有反應」或者更糟的「空白鍵被吃掉了」。
// 所以判斷只有這一份,而且是純函式 —— 在 Ubuntu 上有一張逐鍵的真值表
// (tests/test_hotkey_policy.cc),證明**只有**那一個組合命中。
//
// ══ Shift 單擊切中英(工單 #89,這一輪做了)═══════════════════════
//
// 微軟拼音的預設是「按一下 Shift 切中英」,而使用者問到了業界慣例。
//
// 舊的註解寫著「本輪刻意沒做,理由是**驗不到**:沒有辦法驗證 TSF 到底
// 是不是分得出『Shift 單獨按放』與『拿它打大寫字母』」。**那個理由沒有了**
// —— 上一輪量到 key event sink 收得到純修飾鍵(CI run 31511075812 /
// sha ca97498),於是分辨這件事根本不必問 TSF:它是一個吃事件流的
// 純函式狀態機,住在 common/shift_tap.h,在 Ubuntu 上有一張逐事件的真值表。
//
// ⚠ 所以走的**不是** `TF_PRESERVEDKEY` + `TF_MOD_ON_KEYUP`。那條路會讓
//   TSF 攔下**每一次** Shift 放開,而「宿主收不到 Shift 的 key-up」的症狀是
//   使用者眼裡的「Shift 卡住了」—— 這個專案的規矩是壞掉的鍵比缺功能嚴重。
//   偵測全部在 key event sink 裡做,`*pfEaten` 一律 FALSE,一顆鍵都不吃。
//
// 這個檔案在這件事上只負責一格:**輕點被偵測到之後,線路上長什麼樣**。
//
#ifndef RIMEWIN_COMMON_HOTKEY_POLICY_H_
#define RIMEWIN_COMMON_HOTKEY_POLICY_H_

#include <cstdint>

namespace rimewin {

enum class Hotkey {
  // 不是我們的熱鍵。**絕大多數按鍵都落在這裡**,包含每一個 Ctrl 組合。
  kNone,
  // 中英切換(Ctrl+空白鍵)。
  kToggleAsciiMode,
  // 中英切換(輕點 Shift)。與上面同一件事,但**分成兩格**:
  // 輕點 Shift 有一顆使用者關得掉的開關,Ctrl+空白鍵沒有。
  // 壓成同一格的話,關掉開關會連 Ctrl+空白鍵一起關掉。
  kToggleAsciiModeShiftTap,
};

// keysym + 修飾鍵 → 這是哪一個熱鍵。
//
// ⚠ modifiers 用 keymap.h 的 Mod 位元。CapsLock 不算(它不是使用者
//   為了這顆熱鍵按的);**放開事件(kModRelease)一律不算** ——
//   不然按一次會切兩次,而使用者看到的是「按了沒反應」。
Hotkey ClassifyHotkey(int32_t keysym, uint32_t modifiers);

inline bool IsAsciiToggleHotkey(int32_t keysym, uint32_t modifiers) {
  return ClassifyHotkey(keysym, modifiers) == Hotkey::kToggleAsciiMode;
}

// 中英切換熱鍵的**正規形式**:瘦 DLL 在 OnPreservedKey 裡就是送這一組
// 給服務端。⚠ 它與 TSF 那一側註冊的 `{VK_SPACE, TF_MOD_CONTROL}` 是同
// 一顆鍵的兩種寫法;兩邊對不上就是「註冊了一顆永遠不會被認得的鍵」,
// 所以單元測試把這兩個值釘死。
int32_t AsciiToggleKeysym();
uint32_t AsciiToggleModifiers();

// 輕點 Shift 的**正規形式**。瘦 DLL 偵測到一次輕點之後,送的就是這一組。
//
// ⚠ 為什麼是「一顆按鍵」而不是一個新的協議操作:線路格式一個位元都不用動,
//   新舊 DLL 與新舊服務四種組合仍然全部連得起來(protocol.h 檔頭那條規矩)。
//
// ⚠ 為什麼**不能**與 Ctrl+空白鍵送同一組:服務端要分得出來,才有辦法
//   只關掉這一顆。分不出來的話,「關掉輕點 Shift」會把 Ctrl+空白鍵一起關掉,
//   而使用者猜不到那條關聯。
//
// 值是 XK_Shift_L(0xFFE1),修飾鍵 0 —— 也就是「一顆裸的左 Shift」。
// 這一組在正常的按鍵路徑上**永遠不會出現**:common/key_eat_policy.cc 把
// 每一顆修飾鍵歸成 kHostOnly,瘦 DLL 從來不會把它們送進引擎。所以它是
// 一個自然的、不會與真實按鍵撞號的暗號。
//
// ⚠ 左右在這裡分不出來(keymap.cc:157 把泛用的 VK_SHIFT 一律折成
//   XK_Shift_L),而我們也不需要分 —— 兩顆 Shift 做同一件事。
//   真的要「左右不同義」的話,線路上得多帶 scan code,見 shift_tap.h。
int32_t ShiftTapKeysym();
uint32_t ShiftTapModifiers();

// 服務端收到一顆按鍵時,該做哪一件事。
enum class KeyAction {
  // 交給 librime。**絕大多數按鍵都走這裡。**
  kEngine,
  // 切中英。
  kToggleAsciiMode,
  // 什麼都不做。
  //
  // ⚠ 這一格與 kEngine **不是**同一件事。輕點 Shift 被使用者關掉時,
  //   那顆裸的 XK_Shift_L 必須在這裡停下來,不可以「反正關掉了就交給
  //   librime」—— librime 的 ascii_composer 自己也認得 Shift_L,
  //   交過去會去動它的內部狀態,而那不是使用者關掉這顆鍵時想要的。
  kIgnore,
};

// 這是這件事**唯一**的決定處:服務端的 pipe_server 只負責轉達。
//
// ⚠ shift_tap_enabled 從設定來(Settings::ShiftTapToggle())。
//   它只影響輕點 Shift 那一格 —— Ctrl+空白鍵不受它影響。
KeyAction DecideKeyAction(int32_t keysym, uint32_t modifiers,
                          bool shift_tap_enabled);

}  // namespace rimewin

#endif  // RIMEWIN_COMMON_HOTKEY_POLICY_H_
