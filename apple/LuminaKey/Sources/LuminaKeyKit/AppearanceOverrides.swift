//
//  AppearanceOverrides.swift — 把「設定 › 外觀」蓋到主題上
//
//  ── 為什麼需要這一層 ────────────────────────────────────────────────────
//  `docs/settings-model.md` §「外觀」把 `appearance.showStatusBar` 訂成
//  `followTheme | on | off`，也就是**使用者說了算，主題只是預設值**。
//  但這一端一直沒有任何地方讀它：`LuminaKeySettings.showStatusBar` 只被
//  存檔與讀檔碰過，`CandidateView` 讀的是 `theme.statusBar.show`（預設 false，
//  而 `core/themes/` 底下**沒有任何一份**宣告 `status_bar:`）。
//  結果是那顆開關「切得動、存得起來、什麼都不會發生」——
//  正是這個專案抓過四次的那一類鍵。
//
//  ⚠ **這一層刻意只接一項。** 同一頁還有三個同樣沒接線的設定
//  （`candidateScale`、`candidateOrientation`、`showCandidateLabels`），
//  它們不在這一輪的範圍內。把它們寫進 `unwiredFields` 而不是默默留著，
//  是為了讓「還沒接」是一個**查得到、測得到的事實**，
//  而不是下一個人要重新發現一次的東西。
//

import Foundation

public enum AppearanceOverrides {

    /// 這一層真的會處理的設定欄位。
    public static let wiredFields: [String] = ["showStatusBar"]

    /// 畫面上有、但這一層還沒有接的設定欄位。**動它們的人要同時改測試。**
    ///
    /// 為什麼不乾脆一起接：候選字級與橫直排會改變 `CandidateLayout` 的輸入，
    /// 而那一段有自己的一整組排版斷言；標籤開關還牽涉到 `label.format`。
    /// 三件事各自需要自己的驗證，塞進這一輪只會讓四條缺陷都驗不乾淨。
    public static let unwiredFields: [String] =
        ["candidateScale", "candidateOrientation", "showCandidateLabels"]

    /// 回傳套用設定之後的主題。**純函式** —— 不碰檔案、不碰 AppKit。
    public static func apply(_ theme: Theme, settings: LuminaKeySettings) -> Theme {
        var t = theme
        t.statusBar.show = settings.showStatusBar.resolved(themeValue: theme.statusBar.show)
        return t
    }
}
