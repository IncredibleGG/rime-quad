//
//  CommitPolicy.swift — 「選字」不等於「上屏」
//
//  這是 Android 端用多音節輸入壓測出來、對四端同樣成立的契約
//  （core/include/rime_shell.h 的 rs_commit_composition 註記、
//   docs/coordination.md §4）：
//
//      menu.count > 0                  → 還有段落待選，**不可** commit
//      count == 0 && is_composing      → 轉換完成待確認，呼叫 rs_commit_composition()
//      count == 0 && !is_composing     → 已經結束，什麼都不用做
//
//  拼音方案選字當下就 commit，注音方案選字後仍停在組字狀態 ——
//  **只測拼音永遠不會發現這件事**，所以它必須是一個有名字、被斷言過的決策，
//  而不是散在 InputController 裡的一個 if。
//

import Foundation

/// 決策所需的最小快照。刻意不吃 rs_snapshot 指標 —— 這一層要能在沒有 librime
/// 的情況下被測到。
public struct CompositionState: Sendable {
    public var menuCount: Int
    public var isComposing: Bool

    public init(menuCount: Int, isComposing: Bool) {
        self.menuCount = menuCount
        self.isComposing = isComposing
    }
}

public enum CommitDecision: String, Sendable {
    /// 還有段落待選：繼續顯示候選窗，不上屏。
    case keepComposing
    /// 轉換完成待確認：呼叫 rs_commit_composition()。
    case commit
    /// 已經結束：收起候選窗。
    case idle
}

public enum CommitPolicy {

    public static func decide(_ s: CompositionState) -> CommitDecision {
        if s.menuCount > 0 { return .keepComposing }
        return s.isComposing ? .commit : .idle
    }

    /// 候選窗該不該出現。
    ///
    /// ⚠ 條件是「有候選」**或**「正在組字」，不是只看 `is_composing`：
    ///   注音在選完第一段之後 preedit 已經是選中的字、count 仍大於 0，
    ///   而某些方案在 count 歸零後仍在組字（等使用者按 Enter）。
    ///   任一為真就得看得見，否則使用者會在一個看不見的狀態裡打字。
    public static func shouldShowPanel(_ s: CompositionState) -> Bool {
        s.menuCount > 0 || s.isComposing
    }
}
