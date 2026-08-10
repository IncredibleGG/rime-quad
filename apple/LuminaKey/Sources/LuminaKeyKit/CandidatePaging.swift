//
//  CandidatePaging.swift — 候選窗的翻頁（滾輪／觸控板）
//
//  ── 為什麼桌面端需要這個 ────────────────────────────────────────────────
//  一頁只有 `menu/page_size` 個候選（隨附的 `core/data/shared/default.yaml`
//  訂的是 5），第 6 個字要翻頁才看得到。librime 那一半是通的：
//  `-`／`=`、`,`／`.`、Page_Up／Page_Down 都送得進去（見 `PhysicalKeys.table`
//  與 rime-prelude 的 `key_binder`）。**缺的是畫面上的路**：
//  候選窗沒有翻頁鍵、沒有滾輪、`rs_change_page()` 從來沒有被呼叫過，
//  而狀態列（唯一會顯示頁碼的地方）預設不畫。
//  於是使用者看到五個字就以為只有五個 —— Android 端的原話是
//  「候選詞只有 5 個，下一頁就沒了」。
//
//  ── 為什麼是純函式 ──────────────────────────────────────────────────────
//  「滾多少算一頁」是這一段唯一有邏輯的地方，而 `NSEvent` 在 CI 上造不出來。
//  所以決策抽在這裡，`CandidateView.scrollWheel` 只負責把數字餵進來。
//

import Foundation

public enum PageStep: Equatable, Sendable {
    case none
    case next
    case previous

    /// `rs_change_page(session, backward)` 的參數。`none` 不該走到這裡。
    public var backward: Bool { self == .previous }
}

/// 滾動量 → 翻頁。**有狀態**（觸控板要累積），所以是 struct 而不是 enum。
public struct ScrollPager: Sendable {

    /// 觸控板的累積門檻（點）。太小會變成「手指一放上去就翻三頁」，
    /// 太大會變成「怎麼滾都沒反應」。20 大約是一次自然的輕掃。
    public static let preciseThreshold = 20.0

    private var accumulated = 0.0

    public init() {}

    /// - Parameters:
    ///   - delta: `scrollingDeltaY`（往上滾為正，與 AppKit 相同）。
    ///   - isPrecise: `hasPreciseScrollingDeltas` —— 觸控板／Magic Mouse 為 true，
    ///     傳統滾輪為 false（那時 delta 是「幾格」，一格就該翻一頁）。
    /// - Returns: 這一次事件該翻哪一頁。
    ///
    /// 方向：**往上滾＝上一頁**。這與候選由上往下、由左往右排列一致，
    /// 也與 Squirrel 相同 —— 使用者的手已經學會了。
    public mutating func feed(delta: Double, isPrecise: Bool) -> PageStep {
        guard delta != 0 else { return .none }

        if !isPrecise {
            accumulated = 0
            return delta > 0 ? .previous : .next
        }

        // 方向反轉時把累積歸零：不歸零的話，來回小幅晃動會累積成一次誤翻。
        if accumulated != 0 && (accumulated > 0) != (delta > 0) { accumulated = 0 }
        accumulated += delta
        guard abs(accumulated) >= ScrollPager.preciseThreshold else { return .none }
        let step: PageStep = accumulated > 0 ? .previous : .next
        accumulated = 0
        return step
    }

    /// 候選窗關閉、或換了一次組字時呼叫。留著上一段組字的累積量，
    /// 會讓下一次組字的第一個小滾動直接翻頁。
    public mutating func reset() { accumulated = 0 }
}
