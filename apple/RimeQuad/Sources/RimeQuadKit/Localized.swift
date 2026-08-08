//
//  Localized.swift — 介面文字
//
//  ── 為什麼不是 .strings 檔 ──────────────────────────────────────────────
//  因為漏翻譯這件事**必須編不過**,而不是變成一句英文出現在中文介面裡。
//  `NSLocalizedString` 找不到鍵時回傳鍵本身,那是執行期才看得到的缺陷,
//  而且看起來只是「有一句沒翻到」,不像壞掉,所以沒有人會回報。
//
//  `T(_:_:_:)` 三個參數都是必填,少一個就編不過。整份介面的翻譯完整性
//  因此是**編譯期**的性質,不需要任何測試去追。
//
//  ⚠ 唯一的例外是「進階」頁那一塊給我們自己看的診斷資訊,它永遠是英文
//  (與 Android 端同一個決定,理由見 SettingsCatalog 的 diagnosticsNote)。
//

import Foundation

/// 三種語言。順序刻意是**繁 → 簡 → 英**:這個專案的第一批使用者用繁體,
/// 而寫程式的人一眼要看到的是他正在改的那一句。
public struct T: Equatable, Sendable {
    public let hant: String
    public let hans: String
    public let en: String

    public init(_ hant: String, _ hans: String, _ en: String) {
        self.hant = hant
        self.hans = hans
        self.en = en
    }

    public func callAsFunction(_ lang: UiLanguage) -> String { self[lang] }

    public subscript(lang: UiLanguage) -> String {
        switch lang {
        case .zhHant: return hant
        case .zhHans: return hans
        case .en: return en
        case .system: return self[L10n.systemLanguage()]
        }
    }

    /// 帶參數的版本。`{0}` `{1}` … 依序取代。
    ///
    /// 用位置參數而不是 `%@`:三種語言的語序不同,`%@` 一旦超過一個
    /// 就沒有辦法在中文裡調換順序而不改格式字串。
    public func format(_ lang: UiLanguage, _ args: String...) -> String {
        var s = self[lang]
        for (i, a) in args.enumerated() {
            s = s.replacingOccurrences(of: "{\(i)}", with: a)
        }
        return s
    }
}

public enum L10n {

    /// 系統語言 → 我們支援的三種之一。
    ///
    /// 認不出來一律回英文,**不是**回繁體。理由:看不懂中文的人看到中文
    /// 完全沒有辦法自救;看得懂中文的人看到英文至少還能對照。
    public static func systemLanguage() -> UiLanguage {
        for id in Locale.preferredLanguages {
            let l = id.lowercased()
            guard l.hasPrefix("zh") else {
                if l.hasPrefix("en") { return .en }
                continue
            }
            if l.contains("hans") || l.contains("-cn") || l.contains("-sg") { return .zhHans }
            if l.contains("hant") || l.contains("-tw") || l.contains("-hk") || l.contains("-mo") {
                return .zhHant
            }
            // 只有 "zh" 沒有其他資訊 —— 依 BCP 47 的慣例,裸 zh 指的是簡體。
            return .zhHans
        }
        return .en
    }

    /// 使用者選的語言(`.system` 時解析成實際那一種)。
    public static func resolve(_ pref: UiLanguage) -> UiLanguage {
        pref == .system ? systemLanguage() : pref
    }

    public static let languageNames: [(UiLanguage, T)] = [
        (.system, T("跟著系統", "跟随系统", "Follow system")),
        (.zhHant, T("繁體中文", "繁体中文", "Traditional Chinese")),
        (.zhHans, T("簡體中文", "简体中文", "Simplified Chinese")),
        (.en, T("English", "English", "English")),
    ]
}
