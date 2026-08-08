//
//  InputModeBinding.swift — 輸入模式 ↔ 方案 ↔ 簡繁
//
//  ── 這個檔案為什麼存在 ──────────────────────────────────────────────────
//  真機回報:使用者選了 `…RimeQuad.Hans`(簡體)輸入模式,打 `hao le` 得到
//  「好了 / 好嘞 / 好 / 號 / 浩」——「號」是繁體字。也就是**輸入模式選了簡體,
//  載入的卻是繁體方案,而且沒有任何地方看得出來**。
//
//  這正是本專案最會出事的那一類缺陷:候選窗出現了、打得出字、選字上屏正常,
//  自動化全綠 —— 錯的只有「打出來是哪一種字」,而那要懂中文的人盯著看才發現。
//
//  所以這一層是**純邏輯**:給定 input source ID、使用者設定、已啟用的方案清單,
//  算出「該用哪個方案、simplification 開關該是什麼」。不碰 AppKit、不碰 librime,
//  有單元測試。AppSources 那一側只負責把 TIS 給的字串餵進來、把結果送出去。
//
//  ⚠ 優先順序寫在 docs/settings-model.md §4,四端共用。改這裡就要改那裡。
//

import Foundation

// MARK: - 字集

/// 一個輸入模式、或一個方案,**傾向**產生哪一種漢字。
public enum ScriptVariant: String, Sendable, CaseIterable {
    /// 繁體(含台灣、香港字形)。
    case hant
    /// 簡體。
    case hans
    /// 說不出來。**不是**「兩種都行」,是「沒有資訊」——
    /// 兩者的處置不同:沒有資訊時不該覆寫使用者已經選好的東西。
    case unspecified

    public var opposite: ScriptVariant? {
        switch self {
        case .hant: return .hans
        case .hans: return .hant
        case .unspecified: return nil
        }
    }
}

// MARK: - 輸入模式

/// macOS 的一個輸入模式(Info.plist 的 `tsInputModeListKey` 底下一筆)。
///
/// Windows 端的等價物是 TSF 的一份 language profile(langid + GUID);
/// 兩端的差別只在字串長什麼樣,底下的判定規則是同一條,所以放在同一個型別裡。
public enum InputModeBinding {

    /// 本 app 註冊的兩個輸入模式的後綴。**必須**與 Info.plist 的
    /// `tsInputModeListKey` 的鍵、以及 `.lproj/InfoPlist.strings` 的鍵一致。
    /// 三處對不上的症狀各不相同(選不到 / 顯示成 bundle id / 選錯方案),
    /// 所以 verify_app_bundle.sh 把三者釘在一起。
    public static let hantSuffix = ".Hant"
    public static let hansSuffix = ".Hans"

    /// 從 TIS 給的 input source ID 判斷字集。
    ///
    /// ⚠ 用**後綴**比對而不是完全相等:IMKit 在不同的呼叫點給的字串不完全一樣
    /// (有時是 `ComponentInputModeDict` 的鍵、有時是 `TISInputSourceID`),
    /// 而本 app 兩者刻意設成同一個值。日後若有第三個模式,這裡加一條就好。
    ///
    /// 認不出來時回 `.unspecified` 而**不是**預設繁體 —— 猜錯的代價是使用者
    /// 打出他不要的字,而且完全不知道為什麼。寧可什麼都不做。
    public static func script(forInputSourceID id: String?) -> ScriptVariant {
        guard let id, !id.isEmpty else { return .unspecified }
        if id.hasSuffix(hansSuffix) { return .hans }
        if id.hasSuffix(hantSuffix) { return .hant }
        // 有些宿主會把 modeString 傳成完整的 TISInputSourceID,也有傳鍵名的。
        // 兩者本 app 相同,但保險起見再看一次「是否包含」。
        if id.contains(hansSuffix + ".") || id.contains(hansSuffix) { return .hans }
        if id.contains(hantSuffix + ".") || id.contains(hantSuffix) { return .hant }
        return .unspecified
    }
}

// MARK: - 方案的字集

/// 一個可以被選用的方案。
public struct SchemaEntry: Equatable, Sendable {
    public let id: String
    public let name: String
    /// BCP 47。來自方案市集的索引或已安裝紀錄;沒有就是 nil。
    public let languageTag: String?

    public init(id: String, name: String, languageTag: String? = nil) {
        self.id = id
        self.name = name
        self.languageTag = languageTag
    }

    public var script: ScriptVariant { SchemaScript.of(id: id, languageTag: languageTag) }
}

public enum SchemaScript {

    /// 方案傾向哪一種字集。
    ///
    /// 兩層:**語言標籤優先**(來自索引,是作者宣告的,可信),
    /// 沒有標籤才用 id 的命名慣例猜。猜不到就是 `.unspecified` ——
    /// 這時 `resolve` 不會拿字集當理由去換掉使用者選的方案。
    public static func of(id: String, languageTag: String?) -> ScriptVariant {
        if let tag = languageTag, let fromTag = fromLanguageTag(tag) { return fromTag }
        return fromIdConvention(id)
    }

    /// `zh-Hant` / `zh-Hant-TW` / `zh-TW` / `yue-Hant` → `.hant`,依此類推。
    ///
    /// 只看 script subtag 與地區 subtag,不看語言本身:`nan-Hant-TW`(台語)
    /// 也是繁體。地區的對照表刻意只列常見的四個,列不到的回 nil 而不是猜。
    public static func fromLanguageTag(_ tag: String) -> ScriptVariant? {
        let lower = tag.lowercased()
        guard !lower.isEmpty, lower != "und" else { return nil }
        let parts = lower.split(separator: "-").map(String.init)
        for p in parts {
            if p == "hant" { return .hant }
            if p == "hans" { return .hans }
        }
        for p in parts {
            if p == "tw" || p == "hk" || p == "mo" { return .hant }
            if p == "cn" || p == "sg" || p == "my" { return .hans }
        }
        return nil
    }

    /// 命名慣例。**只在沒有語言標籤時才用**,而且只認有把握的幾種。
    ///
    /// ⚠ 不要在這裡加「含 pinyin 就是簡體」這種規則:`luna_pinyin` 的預設輸出
    /// 是繁體,而 `luna_pinyin_tw` 也是拼音。猜錯比不猜更糟,因為它會讓
    /// simplification 開關被設成相反的值。
    public static func fromIdConvention(_ id: String) -> ScriptVariant {
        let lower = id.lowercased()
        for s in ["_tw", "_hk", "_mo", "_trad", "_hant", "-tw", "-hant"] where lower.hasSuffix(s) {
            return .hant
        }
        for s in ["_cn", "_sc", "_simp", "_hans", "-cn", "-hans"] where lower.hasSuffix(s) {
            return .hans
        }
        // 注音本質上是繁體的輸入方式(大千鍵盤、台灣字形)。
        if lower.hasPrefix("bopomofo") { return .hant }
        return .unspecified
    }
}

// MARK: - 解析結果

public struct InputModeResolution: Equatable, Sendable {
    /// 該切到哪個方案。nil = 不要動(現在這個就對了,或沒有東西可選)。
    public let schemaId: String?
    /// librime 的 `simplification` 開關該設成什麼。nil = 不要碰。
    public let simplification: Bool?
    /// 為什麼是這個結果。給診斷畫面與單元測試看的,不上使用者介面。
    public let reason: Reason

    public enum Reason: String, Sendable {
        /// 使用者在設定裡為這個輸入模式指定了方案。
        case pinnedForMode
        /// 使用者在設定裡指定了單一預設方案,且沒有為模式各自指定。
        case pinnedGlobal
        /// 由輸入模式的字集挑出字集相符的方案。
        case matchedByScript
        /// 沒有字集相符的,退回已啟用清單的第一個。
        case firstEnabled
        /// 沒有任何可選的方案。
        case nothingAvailable
        /// 使用者關掉了「跟著輸入模式切換」。
        case followModeDisabled
    }

    public init(schemaId: String?, simplification: Bool?, reason: Reason) {
        self.schemaId = schemaId
        self.simplification = simplification
        self.reason = reason
    }
}

// MARK: - 解析

public extension InputModeBinding {

    /// 決定「現在該用哪個方案、simplification 該是什麼」。
    ///
    /// - Parameters:
    ///   - script: 目前輸入模式的字集(由 `script(forInputSourceID:)` 得來)。
    ///   - enabled: **已啟用**的方案,順序就是使用者在設定裡排的順序。
    ///   - pinnedForMode: 使用者為這個字集的模式釘死的方案 id。
    ///   - pinnedGlobal: 使用者釘死的單一方案 id(不分模式)。
    ///   - followMode: 「輸入模式決定簡繁」的總開關。
    ///
    /// ── 優先順序(規範性,docs/settings-model.md §4)──────────────────────
    ///   1. `pinnedForMode` —— 使用者明確為這個模式選的,最高。
    ///   2. `pinnedGlobal` —— 使用者只選了一個方案,兩個模式都用它。
    ///   3. 字集相符的第一個已啟用方案。
    ///   4. 已啟用清單的第一個。
    ///
    /// **1 與 2 都是使用者說的話,所以即使字集不符也照做** —— 這時靠
    /// simplification 開關把字集補齊,而不是偷偷換掉他選的方案。
    static func resolve(script: ScriptVariant,
                        enabled: [SchemaEntry],
                        pinnedForMode: String? = nil,
                        pinnedGlobal: String? = nil,
                        followMode: Bool = true) -> InputModeResolution {

        // 關掉時**連 simplification 都不碰**:使用者要的是「我自己管」,
        // 而不是「方案我自己管、簡繁還是你管」。半套會更難理解。
        guard followMode else {
            return InputModeResolution(schemaId: pinnedForMode ?? pinnedGlobal,
                                       simplification: nil,
                                       reason: .followModeDisabled)
        }

        guard !enabled.isEmpty else {
            return InputModeResolution(schemaId: nil, simplification: nil,
                                       reason: .nothingAvailable)
        }

        let ids = Set(enabled.map(\.id))
        let simp = simplificationOption(for: script)

        if let pin = pinnedForMode, ids.contains(pin) {
            return InputModeResolution(schemaId: pin, simplification: simp,
                                       reason: .pinnedForMode)
        }
        if let pin = pinnedGlobal, ids.contains(pin) {
            return InputModeResolution(schemaId: pin, simplification: simp,
                                       reason: .pinnedGlobal)
        }
        if script != .unspecified,
           let match = enabled.first(where: { $0.script == script }) {
            return InputModeResolution(schemaId: match.id, simplification: simp,
                                       reason: .matchedByScript)
        }
        return InputModeResolution(schemaId: enabled[0].id, simplification: simp,
                                   reason: .firstEnabled)
    }

    /// 輸入模式的字集 → librime 的 `simplification` 開關。
    ///
    /// ── 為什麼「總是設」而不是「只在方案不符時設」 ──────────────────────
    /// 我們沒有可靠的辦法知道某個方案的 `switches` 裡有沒有 `simplification`
    /// (那要解析方案的 YAML,而第三方方案千奇百怪)。但 librime 對一個
    /// 不存在的 switch 呼叫 `rs_set_option` 是安全的 —— 它只是記下一個
    /// 沒有人讀的選項。所以無條件設,少一個猜錯的機會。
    ///
    /// ⚠ **這個開關是單向的。** RIME 的 `simplification` 做的是繁 → 簡的
    /// opencc 轉換,一個本來就輸出簡體的方案不會因為關掉它而變成繁體。
    /// 所以 `.hant` 模式配一個簡體方案時,字集**仍然是簡體** ——
    /// 這時要靠第 3 條(字集相符優先)先挑對方案。挑不到就是挑不到,
    /// 我們不假裝做得到(見 docs/settings-model.md §4 的「做不到的事」)。
    static func simplificationOption(for script: ScriptVariant) -> Bool? {
        switch script {
        case .hans: return true
        case .hant: return false
        case .unspecified: return nil
        }
    }
}
