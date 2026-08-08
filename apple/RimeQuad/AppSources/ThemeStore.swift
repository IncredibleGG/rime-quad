//
//  ThemeStore.swift — 主題的載入與外觀跟隨
//
//  §8.2 的執行期外觀選擇是規範性的：
//    1. 使用者明確指定某個主題 id → 直接用它，**不做**深淺切換。
//    2. 使用者選「跟隨系統」→ 取當前主題的 appearance；與系統不符且
//       counterpart 可載入 → 改用 counterpart。
//    3. counterpart 載入失敗 → 沿用當前主題 + WARNING（**不是**致命錯誤）。
//
//  §6.1 的另一半同樣重要：載入失敗時**必須**退回上一個成功載入的主題；
//  若無，退回內建 default-light / default-dark。**不得顯示空白介面。**
//

import AppKit

final class ThemeStore {

    private let source: DocumentSource
    private(set) var current: Theme
    private(set) var lastDiagnostics: [Diagnostic] = []

    /// 使用者選的主題 id；nil = 跟隨系統（在 light/dark 這一對之間切）。
    var preferredId: String?
    var followSystemAppearance = true

    init(searchPaths: [URL]) {
        source = DirectoryDocumentSource(searchPaths)
        // 退無可退時的內建主題：一份完全靠規範預設值的空主題。
        // 它保證「畫得出來」，即使使用者的 themes 目錄整個壞掉。
        current = Theme(id: ThemeStore.builtinId(dark: false))
        _ = reload()
    }

    static func builtinId(dark: Bool) -> String { dark ? "default-dark" : "default-light" }

    /// §4.9 的查詢演算法吃的是 BCP-47（`zh-Hant-TW`），而 `Locale.identifier`
    /// 在 macOS 上是 POSIX 風格的 `zh_Hant_TW`。不換的話每一次查詢都會落到
    /// 「文件順序中的第一筆」，主題名稱在中文系統上會顯示英文。
    static func bcp47Locale() -> String {
        Locale.current.identifier.replacingOccurrences(of: "_", with: "-")
    }

    static func systemIsDark() -> Bool {
        // 刻意用 NSApplication.shared 而不是 NSApp：後者在 NSApplication 尚未
        // 被建立時是 nil，而 ThemeStore 可能比 applicationDidFinishLaunching 早跑。
        NSApplication.shared.effectiveAppearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua
    }

    /// 回傳 true 表示主題有變。
    @discardableResult
    func reload() -> Bool {
        let dark = ThemeStore.systemIsDark()
        let wantId = preferredId ?? ThemeStore.builtinId(dark: dark)
        let result = ThemeLoader.load(id: wantId, source: source, platform: .macos,
                                      locale: ThemeStore.bcp47Locale())
        lastDiagnostics = result.diagnostics
        guard var theme = result.value else {
            // §6.1：退回上一個成功的主題，不得顯示空白。
            NSLog("RimeQuad: 主題 \(wantId) 載入失敗，沿用 \(current.id)：\(result.errors.map(\.developerMessage))")
            return false
        }

        // §8.2 第 2、3 步。
        if followSystemAppearance,
           theme.appearance != (dark ? .dark : .light),
           let cp = theme.counterpart {
            let other = ThemeLoader.load(id: cp, source: source, platform: .macos,
                                         locale: ThemeStore.bcp47Locale())
            if let t = other.value {
                theme = t
                lastDiagnostics += other.diagnostics
            } else {
                NSLog("RimeQuad: counterpart \(cp) 載不起來，沿用 \(theme.id)")
            }
        }

        let changed = theme.id != current.id
        current = theme
        return changed
    }
}
