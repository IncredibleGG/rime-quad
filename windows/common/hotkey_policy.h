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
// ══ Shift 單擊切中英呢 ═══════════════════════════════════════════
//
// 微軟拼音的預設是「按一下 Shift 切中英」,而使用者問到了業界慣例。
// 技術上做得到而且**不需要 hook**:`TF_PRESERVEDKEY` 的 `uModifiers`
// 有 `TF_MOD_ON_KEYUP`,配 `VK_SHIFT` 就是「Shift 放開時」。
//
// **本輪刻意沒做**,理由是驗不到:那顆鍵的正確行為是「Shift 單獨按下
// 再放開才算,拿它打大寫字母時不算」,而這台機器上沒有辦法驗證 TSF 到底
// 是不是這樣分辨的。猜錯的後果是使用者每打一個大寫字母就切一次中英 ——
// 比沒有這個功能糟得多。已寫進 docs/coordination.md,要做就要連同
// 一顆「關掉它」的開關與真機驗證一起做。
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

}  // namespace rimewin

#endif  // RIMEWIN_COMMON_HOTKEY_POLICY_H_
