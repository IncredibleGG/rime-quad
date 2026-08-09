//
//  InputModeTests.swift — 輸入模式 ↔ 方案 ↔ 簡繁
//
//  這一組測試對應的是真機回報的缺陷:選了簡體輸入模式,打出來卻是繁體字。
//  那一類缺陷的共同點是**畫面完全正常、自動化全過** —— 所以這裡驗的不是
//  「有沒有候選」,而是「挑的是哪一個方案、simplification 被設成什麼」。
//

import XCTest
@testable import LuminaKeyKit

final class InputModeBindingTests: XCTestCase {

    // ── input source id → 字集 ──────────────────────────────

    func testScriptFromInputSourceID() {
        XCTAssertEqual(InputModeBinding.script(
            forInputSourceID: "org.luminakey.inputmethod.LuminaKey.Hans"), .hans)
        XCTAssertEqual(InputModeBinding.script(
            forInputSourceID: "org.luminakey.inputmethod.LuminaKey.Hant"), .hant)
    }

    /// 認不出來時**不可以**猜一個。猜錯的代價是使用者打出他不要的字。
    func testUnknownInputSourceIsUnspecified() {
        XCTAssertEqual(InputModeBinding.script(forInputSourceID: nil), .unspecified)
        XCTAssertEqual(InputModeBinding.script(forInputSourceID: ""), .unspecified)
        XCTAssertEqual(InputModeBinding.script(
            forInputSourceID: "com.apple.keylayout.US"), .unspecified)
    }

    // ── 方案的字集 ─────────────────────────────────────────

    func testLanguageTagBeatsNamingConvention() {
        // 標籤是作者宣告的,可信;命名慣例是猜的。
        XCTAssertEqual(SchemaScript.of(id: "luna_pinyin_tw", languageTag: "zh-Hans"), .hans)
        XCTAssertEqual(SchemaScript.of(id: "whatever", languageTag: "zh-Hant-TW"), .hant)
        XCTAssertEqual(SchemaScript.of(id: "whatever", languageTag: "zh-CN"), .hans)
    }

    func testNamingConventionFallback() {
        XCTAssertEqual(SchemaScript.of(id: "luna_pinyin_tw", languageTag: nil), .hant)
        XCTAssertEqual(SchemaScript.of(id: "bopomofo_tw", languageTag: nil), .hant)
        XCTAssertEqual(SchemaScript.of(id: "bopomofo", languageTag: nil), .hant)
        XCTAssertEqual(SchemaScript.of(id: "pinyin_simp", languageTag: nil), .hans)
    }

    /// ⚠ `luna_pinyin` 的預設輸出是**繁體**,不要因為它是「拼音」就猜簡體。
    /// 猜錯會讓 simplification 開關被設成相反的值。
    func testAmbiguousSchemaStaysUnspecified() {
        XCTAssertEqual(SchemaScript.of(id: "luna_pinyin", languageTag: nil), .unspecified)
        XCTAssertEqual(SchemaScript.of(id: "t9_pinyin", languageTag: nil), .unspecified)
        XCTAssertNil(SchemaScript.fromLanguageTag("und"))
        XCTAssertNil(SchemaScript.fromLanguageTag(""))
    }

    // ── 挑方案 ─────────────────────────────────────────────

    private let enabled = [
        SchemaEntry(id: "luna_pinyin_tw", name: "拼音(繁)", languageTag: "zh-Hant"),
        SchemaEntry(id: "bopomofo_tw", name: "注音", languageTag: "zh-Hant"),
        SchemaEntry(id: "pinyin_simp", name: "拼音(簡)", languageTag: "zh-Hans"),
    ]

    /// 這一條就是真機那個 bug:簡體模式必須挑到簡體方案,
    /// 而且 simplification 必須是 true。
    func testHansModePicksSimplifiedSchema() {
        let r = InputModeBinding.resolve(script: .hans, enabled: enabled)
        XCTAssertEqual(r.schemaId, "pinyin_simp")
        XCTAssertEqual(r.simplification, true)
        XCTAssertEqual(r.reason, .matchedByScript)
    }

    func testHantModePicksTraditionalSchema() {
        let r = InputModeBinding.resolve(script: .hant, enabled: enabled)
        XCTAssertEqual(r.schemaId, "luna_pinyin_tw")
        XCTAssertEqual(r.simplification, false)
        XCTAssertEqual(r.reason, .matchedByScript)
    }

    /// 沒有字集相符的方案時仍然要選一個,而且 simplification 要補上 ——
    /// 這是「只有繁體方案,但使用者選了簡體輸入來源」的情形,
    /// 靠 RIME 的簡繁轉換開關把字集補齊。
    func testFallsBackToFirstEnabledAndStillSetsSimplification() {
        let onlyTraditional = [enabled[0], enabled[1]]
        let r = InputModeBinding.resolve(script: .hans, enabled: onlyTraditional)
        XCTAssertEqual(r.schemaId, "luna_pinyin_tw")
        XCTAssertEqual(r.simplification, true)
        XCTAssertEqual(r.reason, .firstEnabled)
    }

    func testPinnedForModeWins() {
        let r = InputModeBinding.resolve(script: .hans, enabled: enabled,
                                         pinnedForMode: "bopomofo_tw",
                                         pinnedGlobal: "luna_pinyin_tw")
        XCTAssertEqual(r.schemaId, "bopomofo_tw")
        XCTAssertEqual(r.reason, .pinnedForMode)
        // 使用者選的方案不換掉,但字集仍然照輸入模式補。
        XCTAssertEqual(r.simplification, true)
    }

    func testPinnedGlobalUsedWhenNoModePin() {
        let r = InputModeBinding.resolve(script: .hant, enabled: enabled,
                                         pinnedGlobal: "pinyin_simp")
        XCTAssertEqual(r.schemaId, "pinyin_simp")
        XCTAssertEqual(r.reason, .pinnedGlobal)
    }

    /// 釘的方案已經被停用了 → 忽略它,不要選一個清單上沒有的東西。
    func testPinnedButNotEnabledIsIgnored() {
        let r = InputModeBinding.resolve(script: .hant, enabled: enabled,
                                         pinnedForMode: "cangjie5")
        XCTAssertEqual(r.schemaId, "luna_pinyin_tw")
        XCTAssertEqual(r.reason, .matchedByScript)
    }

    func testNothingEnabled() {
        let r = InputModeBinding.resolve(script: .hans, enabled: [])
        XCTAssertNil(r.schemaId)
        XCTAssertNil(r.simplification)
        XCTAssertEqual(r.reason, .nothingAvailable)
    }

    /// 關掉「跟著輸入來源」時**連 simplification 都不碰**。
    /// 半套(方案不跟、簡繁還是跟)比完全不做更難理解。
    func testFollowModeDisabledTouchesNothing() {
        let r = InputModeBinding.resolve(script: .hans, enabled: enabled,
                                         pinnedGlobal: "bopomofo_tw", followMode: false)
        XCTAssertEqual(r.schemaId, "bopomofo_tw")
        XCTAssertNil(r.simplification)
        XCTAssertEqual(r.reason, .followModeDisabled)
    }
}

final class SessionOptionsTests: XCTestCase {

    func testVariantFollowsInputModeByDefault() {
        let s = LuminaKeySettings()
        XCTAssertEqual(SessionOptions.resolve(settings: s, inputModeScript: .hans)["simplification"],
                       true)
        XCTAssertEqual(SessionOptions.resolve(settings: s, inputModeScript: .hant)["simplification"],
                       false)
    }

    /// 使用者在「文字」頁明確選了 → 他說了算,輸入模式不再有話語權。
    func testExplicitVariantOverridesInputMode() {
        var s = LuminaKeySettings()
        s.variant = .traditional
        XCTAssertEqual(SessionOptions.resolve(settings: s, inputModeScript: .hans)["simplification"],
                       false)
        s.variant = .simplified
        XCTAssertEqual(SessionOptions.resolve(settings: s, inputModeScript: .hant)["simplification"],
                       true)
    }

    func testUnknownInputModeLeavesSimplificationAlone() {
        let s = LuminaKeySettings()
        XCTAssertNil(SessionOptions.resolve(settings: s, inputModeScript: .unspecified)["simplification"])
    }

    /// 「跟著方案」**不是**「設成 false」。無條件設 false 會讓這個選項
    /// 變成「一律中文標點」,而使用者選的是不干預。
    func testFollowSchemaMeansDoNotTouch() {
        let s = LuminaKeySettings()
        let opts = SessionOptions.resolve(settings: s, inputModeScript: .hant)
        XCTAssertNil(opts["ascii_punct"])
        XCTAssertNil(opts["full_shape"])
    }

    func testPunctuationAndShape() {
        var s = LuminaKeySettings()
        s.punctuation = .half
        s.shape = .fullShape
        let opts = SessionOptions.resolve(settings: s, inputModeScript: .unspecified)
        XCTAssertEqual(opts["ascii_punct"], true)
        XCTAssertEqual(opts["full_shape"], true)
    }
}
