//
//  InputModeSwitch.swift — 中／英切換：按哪一顆鍵，以及「現在是什麼」怎麼講
//
//  ── 為什麼不是 ⌃Space ──────────────────────────────────────────────────
//  Windows 端使用者要的是 Ctrl+Space，**但那個組合在 macOS 上不是我們的**：
//  系統的「選取上一個輸入來源」預設就綁在 ⌃Space（系統設定 › 鍵盤 ›
//  鍵盤快速鍵 › 輸入來源）。輸入法搶得到它的唯一辦法是註冊全域熱鍵，
//  而那會讓使用者按下去之後**得到兩個行為**，且他不會知道是我們幹的。
//  照抄 Windows 的鍵在這一端是主動製造衝突。
//
//  macOS 上真正的慣例有兩個，兩個都不是 ⌃Space：
//
//    · **輕點 Shift** —— RIME 自己的 macOS 前端（Squirrel／鼠鬚管）預設就是它，
//      而我們隨附的 `core/data/shared/default.yaml` 早就寫著
//      `ascii_composer/switch_key: { Shift_L: inline_ascii, Shift_R: commit_text }`。
//      也就是說**引擎那一半本來就在等這顆鍵**，缺的只有把事件送給它。
//    · **Caps Lock** —— 那是系統層的（系統設定 › 鍵盤 ›「使用大寫鎖定鍵切換⋯」），
//      由 macOS 在輸入來源之間切，不是輸入法自己實作的東西。
//
//  所以這一端採用**輕點 Shift**，並且只實作這一顆：多綁幾顆鍵不會讓人更好找，
//  只會讓「我到底按到了什麼」變成另一個問題。
//
//  ⚠ 真正的缺陷不在鍵的選擇，而在事件根本沒送到：見 `IMKRecognizedEvents`。
//

import Foundation

/// IMKit 只送「你宣告要收」的事件（`IMKStateSetting.recognizedEvents(_:)`）。
///
/// ⚠ **沒有宣告時的預設值是「只有 keyDown」。** 修飾鍵在 macOS 上不產生
///   keyDown／keyUp，只產生 `flagsChanged` —— 所以不宣告 flagsChanged 的話，
///   `ModifierTracker`、`processFlags()`、以及 librime 的 `ascii_composer`
///   這一整條路徑**一次都不會被走到**，而症狀是「輕點 Shift 什麼都沒發生」：
///   沒有錯誤訊息、沒有 log、程式碼看起來完全正確。
///
/// 值是 `NSEvent.EventTypeMask` 的位元位置（= `NSEvent.EventType` 的 rawValue）：
/// `keyDown` = 10、`flagsChanged` = 12。這裡刻意寫成純數字而不 import AppKit ——
/// 這個 package 不含 AppKit（見 Package.swift 的檔頭）。
/// **抄錯的風險由 `--self-check` 擋**：它在真的有 AppKit 的地方拿
/// `NSEvent.EventTypeMask` 對一次，對不上就紅。
public enum IMKRecognizedEvents {
    public static let keyDown: UInt64 = 1 << 10
    public static let flagsChanged: UInt64 = 1 << 12

    /// `LuminaKeyInputController.recognizedEvents(_:)` 要回傳的值。
    public static let mask: UInt64 = keyDown | flagsChanged

    /// IMKit 在我們什麼都不說時給的預設。留著它是為了讓測試問得出
    /// 「我們有沒有真的比預設多要一種事件」，而不是只比對一個常數。
    public static let imkitDefault: UInt64 = keyDown

    public static func includesFlagsChanged(_ mask: UInt64) -> Bool {
        mask & flagsChanged != 0
    }
}

// ───────────────────────────────────────────────────────────────────────
//  哪些修飾鍵的 flagsChanged 該送進 librime
// ───────────────────────────────────────────────────────────────────────

/// ⚠ **這一段修的是上一輪自己做出來的迴歸,而且是真機上按得出來的。**
///
/// `recognizedEvents(_:)` 一旦宣告要收 `flagsChanged`,IMKit 就會把
/// **每一顆**修飾鍵的 flagsChanged 都送過來;而當時的 `processFlags()`
/// 把它們全部餵給 librime。兩個後果,第一個是真的缺陷:
///
///  1. **組字打到一半按 Caps Lock,剛打的半句話會不見。**
///     隨附的 `core/data/shared/default.yaml`(上游 rime-prelude)寫著
///     `ascii_composer/switch_key: { Caps_Lock: clear }`,而
///     `ModifierTracker` 把每一次 Caps Lock 的 flagsChanged 都當成「按下」
///     —— 那顆鍵在 `modifierFlags` 裡的位元代表**燈亮著**,不是鍵按著。
///     librime 那一半照著跑:`AsciiComposer::ProcessCapsLock()` 在
///     `!release` 時呼叫 `SwitchAsciiMode(..., kAsciiModeSwitchClear)`,
///     而它對正在組字的 context 做的事就是 `ctx->Clear()`
///     (third_party/librime/src/rime/gear/ascii_composer.cc)。
///     使用者按 Caps Lock 是要打大寫,不是要丟掉組字。
///
///  2. 沒有組字時,每一次 Shift/⌘/⌥/⌃ 的按下與放開都白跑一趟引擎。
///     ⌘ 尤其不該進去:`process()` 有一條「Command 組合鍵一律讓宿主處理」
///     的護欄,而 `processFlags()` 沒有 —— 同一個 app 對同一顆鍵有兩種立場。
///
/// 所以這裡只放行**真的會觸發切換的那一顆**,其餘一概不碰。
/// 閘門刻意放在 `ModifierTracker.transition` 裡面而不是放在 controller:
/// `AppSources/` 沒有單元測試(`swift test` 不編譯它),把判斷放在那裡
/// 等於放在測不到的地方 —— 那正是這一輪要修的第二件事。
public enum ModifierGate {

    /// 切換鍵的 keyCode:左右 Shift。
    ///
    /// 為什麼不是「`default.yaml` 的 `switch_key` 列了誰就放行誰」:
    /// 那份設定是**上游**的,它同時綁了 `Caps_Lock: clear` 與
    /// `Control_L/R: noop`;照著放行等於把上面第 1 點原封不動再做一次。
    /// 這一端的產品決定是「只實作輕點 Shift」(見本檔開頭),放行清單就該與
    /// 那個決定一致 —— 不一致的話會多出一顆「按了會發生事情、
    /// 但沒有人說得出它是什麼」的鍵。
    public static let switchKeyCodes: Set<UInt16> = [56, 60]

    /// 這些修飾鍵同時按著時,那就不是「輕點切換」而是宿主的快捷鍵。
    ///
    /// librime 自己也是這樣判的(`AsciiComposer::ProcessKeyEvent` 開頭:
    /// shift/ctrl/alt/super 同時超過一個就直接 `kNoop`),擋在這裡只是把
    /// 同一條規則提早一步,順便讓 `process()` 那條 Command 護欄在
    /// flagsChanged 這條路上也成立。
    ///
    /// ⚠ **不含 `capsLock`。** 那個位元代表「大寫鎖定的燈亮著」,不是
    ///   「有人按著一顆鍵」。把它算進來的話,開著大寫鎖定的使用者輕點 Shift
    ///   會完全沒有反應 —— 那是把一個缺陷換成另一個。
    public static let blockingFlags: MacModifierFlags = [.command, .control, .option]

    /// 這一次 flagsChanged 該不該變成送進 librime 的按鍵事件。
    public static func shouldForward(keyCode: UInt16, flags: MacModifierFlags) -> Bool {
        guard switchKeyCodes.contains(keyCode) else { return false }
        return flags.isDisjoint(with: blockingFlags)
    }
}

/// 中／英切換這件事的**文字**面：現在是什麼、按哪一顆鍵。
///
/// ⚠ 這一段的規則只有一條，而且它就是使用者的原話：
///   **「現在是什麼就顯示什麼」，不是兩個都畫出來讓他猜。**
///   所以每一個回傳值都只描述**一個**狀態；把兩態並排的
///   `input_mode_pair` 是給行動端**按鍵**用的（按鍵才有「按了會變成什麼」
///   的歧義），桌面端的狀態列與選單列是**顯示**，不適用。
public enum InputModeSwitch {

    /// 切換鍵在畫面上叫什麼。不在地化 —— 鍵帽上印的就是這個字。
    public static let switchKeyLabel = "Shift"

    /// 現在是什麼。**單一狀態**。
    public static func currentModeName(isAsciiMode: Bool) -> T {
        isAsciiMode
            ? T("英文", "英文", "English")
            : T("中文", "中文", "Chinese")
    }

    /// 選單列（輸入法圖示 › 選單）最上面那一行。
    ///
    /// 為什麼是「目前：中文」而不是「中／En」：這一行是**狀態**，
    /// 它要回答的是「我現在打字會出中文還是英文」。括號裡才是動作。
    public static func menuTitle(isAsciiMode: Bool, lang: UiLanguage) -> String {
        T("目前：{0}（輕點 {1} 切換）",
          "当前：{0}（轻点 {1} 切换）",
          "Currently: {0} (tap {1} to switch)")
            .format(lang, currentModeName(isAsciiMode: isAsciiMode)[lang], switchKeyLabel)
    }
}
