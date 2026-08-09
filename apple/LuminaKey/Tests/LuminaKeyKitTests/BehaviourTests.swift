import XCTest
@testable import LuminaKeyKit

final class PlaceholderTests: XCTestCase {

    func testCandidateLabelPlaceholders() {
        XCTAssertEqual(Placeholders.candidateLabel(format: "{label}", label: "①", indexOnPage: 0), "①")
        XCTAssertEqual(Placeholders.candidateLabel(format: "{index}. ", label: "1", indexOnPage: 4), "5. ")
        XCTAssertEqual(Placeholders.candidateLabel(format: "{index0}", label: "1", indexOnPage: 4), "4")
    }

    /// §8.6.1：**未知佔位符必須原樣保留**（不得丟棄，也不得報錯）。
    /// 這是「新增佔位符不算破壞性變更」的前提。
    func testUnknownPlaceholderIsPreservedVerbatim() {
        XCTAssertEqual(
            Placeholders.candidateLabel(format: "{label}{tone}", label: "1", indexOnPage: 0),
            "1{tone}")
    }

    func testUnclosedBraceIsPreserved() {
        XCTAssertEqual(Placeholders.expand("a{b", [:]), "a{b")
    }

    func testAnnouncementCollapsesEmptyFields() {
        XCTAssertEqual(
            Placeholders.announcement(format: "{label} {text} {comment}",
                                      label: "1", text: "你好", comment: ""),
            "1 你好",
            "空欄位不得留下多餘空白 —— VoiceOver 會把它念成停頓")
    }
}

final class CommitPolicyTests: XCTestCase {

    /// 這三條是 Android 用多音節輸入壓測出來的，四端共用。
    func testTheThreeCases() {
        XCTAssertEqual(CommitPolicy.decide(CompositionState(menuCount: 4, isComposing: true)),
                       .keepComposing)
        XCTAssertEqual(CommitPolicy.decide(CompositionState(menuCount: 0, isComposing: true)),
                       .commit)
        XCTAssertEqual(CommitPolicy.decide(CompositionState(menuCount: 0, isComposing: false)),
                       .idle)
    }

    /// 注音選了只覆蓋第一音節的「你」之後，preedit 是「你ㄏㄠˇ」且 count=4。
    /// 此時 commit 會吃掉後半段 —— 這正是「只測拼音永遠不會發現」的那一格。
    func testPartialSelectionMustNotCommit() {
        XCTAssertNotEqual(CommitPolicy.decide(CompositionState(menuCount: 4, isComposing: true)),
                          .commit)
    }

    func testPanelVisibility() {
        XCTAssertTrue(CommitPolicy.shouldShowPanel(CompositionState(menuCount: 5, isComposing: false)))
        XCTAssertTrue(CommitPolicy.shouldShowPanel(CompositionState(menuCount: 0, isComposing: true)))
        XCTAssertFalse(CommitPolicy.shouldShowPanel(CompositionState(menuCount: 0, isComposing: false)))
    }
}

final class StatusFaceTests: XCTestCase {

    private func status(ascii: Bool = false, simplified: Bool = false,
                        fullShape: Bool = false, name: String = "注音") -> EngineStatus {
        var s = EngineStatus()
        s.isAsciiMode = ascii
        s.isSimplified = simplified
        s.isFullShape = fullShape
        s.schemaName = name
        return s
    }

    private func face(_ source: StatusSource, _ st: EngineStatus,
                      page: Int = 0, last: Bool = true) -> [FaceSegment] {
        StatusFace.segments(for: StatusItem(source: source), status: st,
                            pageNo: page, isLastPage: last)
    }

    /// `input_mode_pair` 同時畫出兩態，並強調當前那一態。
    /// 只寫一個「中」有兩種讀法（「現在是中文」／「按了會變中文」），
    /// 它們指向相反的操作 —— 真機回報過。
    func testInputModePairShowsBothStates() {
        let cjk = face(.inputModePair, status(ascii: false))
        XCTAssertEqual(cjk.map(\.text), ["中", "/", "En"])
        XCTAssertEqual(cjk.map(\.emphasised), [true, false, false])

        let latin = face(.inputModePair, status(ascii: true))
        XCTAssertEqual(latin.map(\.emphasised), [false, false, true])
        XCTAssertEqual(StatusFace.plainText(latin), "中/En")
    }

    func testNormativeLiterals() {
        XCTAssertEqual(StatusFace.plainText(face(.inputMode, status(ascii: true))), "En")
        XCTAssertEqual(StatusFace.plainText(face(.inputMode, status(ascii: false))), "中")
        XCTAssertEqual(StatusFace.plainText(face(.variant, status(simplified: true))), "简")
        XCTAssertEqual(StatusFace.plainText(face(.variant, status(simplified: false))), "繁")
        XCTAssertEqual(StatusFace.plainText(face(.shape, status(fullShape: true))), "全")
        XCTAssertEqual(StatusFace.plainText(face(.shape, status(fullShape: false))), "半")
    }

    /// schema 還沒載入完成時 `schema_name` 是空字串 ——
    /// 此時整項略過，**不得**畫出一塊看不出用途的空白。
    func testEmptyStateIsSkippedEntirely() {
        XCTAssertEqual(face(.schemaName, status(name: "")).count, 0)
        XCTAssertEqual(face(.schemaName, status(name: "注音")).count, 1)
    }

    /// 只有一頁時不顯示頁碼 —— 每次組字都掛一個「1」是純粹的噪音。
    func testPageIndicatorHiddenOnSinglePage() {
        XCTAssertEqual(face(.page, status(), page: 0, last: true).count, 0)
        XCTAssertEqual(StatusFace.plainText(face(.page, status(), page: 0, last: false)), "1+")
        XCTAssertEqual(StatusFace.plainText(face(.page, status(), page: 2, last: true)), "3")
    }

    /// §9.5.1：未實作的動詞不得出現在畫面上。
    func testUnimplementedVerbsAreFilteredOut() {
        let items = [
            StatusItem(source: .schemaName, tap: KeyAction(.schemaPicker, raw: "schema:picker")),
            StatusItem(source: .text, text: "☺", tap: KeyAction(.emoji, raw: "emoji")),
            StatusItem(source: .page),
        ]
        let visible = StatusFace.renderable(items)
        XCTAssertEqual(visible.count, 2)
        XCTAssertFalse(visible.contains { $0.tap?.verb == .emoji })
    }
}

final class ActionsTests: XCTestCase {

    private func parse(_ s: String) -> (KeyAction?, [DiagnosticCode]) {
        let d = Diagnostics()
        let a = Actions.parse(s, path: "tap", diag: d, line: nil)
        return (a, d.items.map(\.code))
    }

    func testKnownVerbs() {
        XCTAssertEqual(parse("schema:picker").0?.verb, .schemaPicker)
        XCTAssertEqual(parse("schema:luna_pinyin").0?.verb, .schemaSelect)
        XCTAssertEqual(parse("schema:luna_pinyin").0?.arg, "luna_pinyin")
        XCTAssertEqual(parse("toggle:ascii_mode").0?.arg, "ascii_mode")
        XCTAssertEqual(parse("set:ascii_mode:on").0?.args, ["ascii_mode", "on"])
        XCTAssertEqual(parse("input_mode:toggle").0?.verb, .inputModeToggle)
        XCTAssertEqual(parse("candidate:select:3").0?.args, ["3"])
        XCTAssertEqual(parse("candidate:next_page").0?.verb, .candidateNextPage)
        XCTAssertEqual(parse("clear").0?.verb, .clear)
    }

    func testUnknownVerbWarns() {
        let (a, codes) = parse("teleport:now")
        XCTAssertNil(a)
        XCTAssertEqual(codes, [.unknownAction])
    }

    func testMissingArgumentWarns() {
        XCTAssertEqual(parse("toggle").1, [.badActionArgument])
        XCTAssertEqual(parse("set:ascii_mode").1, [.badActionArgument])
        XCTAssertEqual(parse("set:ascii_mode:maybe").1, [.badActionArgument])
        XCTAssertEqual(parse("candidate:select:x").1, [.badActionArgument])
    }

    /// §9.5：`syllables:toggle` 必須是**認得的**動詞 —— 不認得的話，同一份含
    /// 這顆動詞的主題在行動端零則、在這一端一則 `unknown_action`，§10 第 9 條失守。
    /// 「認得」不等於「做得到」：它同時在 §9.5.1 的未實作清單裡（§8.6.6.3.5）。
    func testSyllablesToggleIsKnownButUnimplementedOnDesktop() {
        XCTAssertEqual(parse("syllables:toggle").0?.verb, .syllablesToggle)
        XCTAssertEqual(parse("syllables:toggle").1, [], "認得的動詞不得產生診斷")
        XCTAssertEqual(parse("syllables:nope").1, [.badActionArgument])
        XCTAssertFalse(DesktopVerbSupport.isImplemented(.syllablesToggle),
                       "桌面端不渲染消歧欄，這顆開關沒有東西可開（§8.6.6.3.5）")
    }
}

final class LocalizedStringTests: XCTestCase {

    /// §4.9 的查詢演算法。
    func testLookupOrder() {
        let s = LocalizedString(["en": "E", "zh-Hant": "繁", "und": "U"],
                                order: ["en", "zh-Hant", "und"])
        XCTAssertEqual(s.get("zh-Hant"), "繁")
        XCTAssertEqual(s.get("zh-Hant-TW"), "繁", "退到主語言子標籤")
        XCTAssertEqual(s.get("fr"), "E", "退到 en")
        let noEn = LocalizedString(["zh-Hans": "简", "und": "U"], order: ["zh-Hans", "und"])
        XCTAssertEqual(noEn.get("fr"), "U", "再退到 und")
    }
}

final class ColorSpecTests: XCTestCase {

    func testHexForms() {
        XCTAssertEqual(ColorSpec.parseHex("#f0a"), RGBA.hex(0xFF00AAFF), "3 位每位重複一次，alpha 補 ff")
        XCTAssertEqual(ColorSpec.parseHex("#f0a8"), RGBA.hex(0xFF00AA88))
        XCTAssertEqual(ColorSpec.parseHex("#112233"), RGBA.hex(0x112233FF))
        XCTAssertEqual(ColorSpec.parseHex("#11223344"), RGBA.hex(0x11223344))
        XCTAssertNil(ColorSpec.parseHex("#12345"))
        XCTAssertNil(ColorSpec.parseHex("#ZZZ"))
    }

    /// §4.7：`@alpha` 是**相乘**而非覆寫。
    func testAlphaModulationMultiplies() {
        let base = ["a": RGBA.hex(0x11223380)]
        XCTAssertEqual(ColorSpec.resolve("$a@50%", palette: base), RGBA.hex(0x11223340))
        XCTAssertEqual(ColorSpec.resolve("$a@0.5", palette: base), RGBA.hex(0x11223340))
    }

    func testTransparent() {
        XCTAssertEqual(ColorSpec.resolve("transparent", palette: [:]), RGBA.transparent)
        XCTAssertEqual(ColorSpec.resolve("TRANSPARENT", palette: [:]), RGBA.transparent)
    }
}

final class TypographyTests: XCTestCase {

    /// §4.4.1：macOS 的 `system_font_scale` 恆為 1.0，且 AppKit 不會自己再套一次。
    /// 這一條擋的是「縮放被平方」那個四端最常見的錯。
    func testMacosEffectiveScaleIsOne() {
        let t = Typography()
        XCTAssertEqual(t.effectiveScale(systemFontScale: 1.0), 1.0)
    }

    func testScaleIsClampedToTheThemeRange() {
        var t = Typography()
        t.fontScaleMax = 1.30
        XCTAssertEqual(t.effectiveScale(systemFontScale: 2.0), 1.30)
        XCTAssertEqual(t.effectiveScale(systemFontScale: 0.1), t.fontScaleMin)
        t.respectSystemFontScale = false
        XCTAssertEqual(t.effectiveScale(systemFontScale: 2.0), 1.0)
    }
}
