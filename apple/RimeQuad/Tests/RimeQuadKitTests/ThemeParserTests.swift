import XCTest
@testable import RimeQuadKit

final class ThemeParserTests: XCTestCase {

    private let minimal = """
    format: rime-theme/1
    id: minimal
    appearance: light
    """

    // ───────────────── 附錄 A：最小可用主題 ─────────────────

    func testMinimalThemeLoadsWithNoDiagnostics() {
        let r = loadTheme("minimal", ["minimal": minimal])
        XCTAssertTrue(r.isSuccess)
        XCTAssertEqual(r.diagnostics.count, 0, "最小主題不該產生任何診斷：\(r.diagnostics.map(\.developerMessage))")
        let t = r.value!
        XCTAssertEqual(t.metrics.padding, 6)
        XCTAssertEqual(t.candidates.text.size, 20)
        XCTAssertEqual(t.window.maxWidth, 640)
        XCTAssertEqual(t.window.lines, 1, "§8.6.7.1：lines 預設 1（＝v1 的單行行為）")
        XCTAssertEqual(t.window.placement, .auto)
        XCTAssertFalse(t.statusBar.show, "§8.12：狀態列預設不顯示")
        XCTAssertEqual(t.accessibility.announceCandidates, .full)
    }

    /// §8.6.4：`item.*` 的預設值取自 `metrics`，不是字面預設。
    func testItemDefaultsComeFromMetrics() {
        let r = loadTheme("m", ["m": """
        format: rime-theme/1
        id: m
        metrics:
          padding: 11
          spacing: 3
        """])
        let t = r.value!
        XCTAssertEqual(t.candidates.item.paddingH, 11)
        XCTAssertEqual(t.candidates.item.spacing, 3)
        XCTAssertEqual(t.window.columnGap, 3, "§8.6.7.1：column_gap 預設 = item.spacing")
    }

    // ───────────────── §6.3 可回復錯誤 ─────────────────

    /// §10 檢核第 4 條。
    func testBadColourYieldsExactlyOneWarningAndKeepsTheDefault() {
        let r = loadTheme("m", ["m": """
        format: rime-theme/1
        id: m
        candidates:
          window:
            background: "#ZZZ"
        """])
        XCTAssertEqual(r.warnings.count, 1)
        XCTAssertEqual(r.warnings[0].code, .badColor)
        XCTAssertEqual(r.warnings[0].path, "candidates.window.background")
        XCTAssertEqual(r.value!.window.background, RGBA.hex(0xFFFFFFFF))
    }

    /// §10 檢核第 5 條（在**消費該區塊**的平台上）。
    func testUnknownFieldYieldsExactlyOneWarning() {
        let r = loadTheme("m", ["m": """
        format: rime-theme/1
        id: m
        candidates:
          window:
            blahblah: 1
        """])
        XCTAssertEqual(r.warnings.count, 1)
        XCTAssertEqual(r.warnings[0].code, .unknownField)
        XCTAssertTrue(r.isSuccess)
    }

    /// §10 檢核第 4b 條：夾制**必須**發出診斷。靜默夾制是不合規的。
    func testClampReportsExactlyOneWarning() {
        let r = loadTheme("m", ["m": """
        format: rime-theme/1
        id: m
        candidates:
          window:
            opacity: 4.0
        """])
        XCTAssertEqual(r.warnings.count, 1)
        XCTAssertEqual(r.warnings[0].code, .outOfRange)
        XCTAssertEqual(r.value!.window.opacity, 1.0)
    }

    func testBadEnumFallsBackAndWarns() {
        let r = loadTheme("m", ["m": """
        format: rime-theme/1
        id: m
        candidates:
          orientation: diagonal
        """])
        XCTAssertEqual(r.warnings.map(\.code), [.badEnum])
        XCTAssertEqual(r.value!.candidates.orientation, .horizontal)
    }

    func testMissingFieldIsSilent() {
        let r = loadTheme("m", ["m": "format: rime-theme/1\nid: m\ncandidates: {}\n"])
        XCTAssertEqual(r.diagnostics.count, 0)
    }

    /// §4.7：palette 遞迴解析；查不到的 `$ref` 讓欄位退回預設值 + WARNING。
    func testPaletteReferences() {
        let r = loadTheme("m", ["m": """
        format: rime-theme/1
        id: m
        palette:
          base: "#112233"
          accent: "$base"
          faded: "$accent@50%"
        candidates:
          item:
            highlight_background: "$faded"
            background: "$nope"
        """])
        let t = r.value!
        XCTAssertEqual(t.palette["accent"], RGBA.hex(0x112233FF))
        XCTAssertEqual(t.palette["faded"], RGBA.hex(0x11223380))
        XCTAssertEqual(t.candidates.item.highlightBackground, RGBA.hex(0x11223380))
        XCTAssertEqual(t.candidates.item.background, RGBA.transparent, "退回欄位預設值")
        XCTAssertEqual(r.warnings.map(\.code), [.badColor])
    }

    func testPaletteSelfReferenceIsDropped() {
        let r = loadTheme("m", ["m": """
        format: rime-theme/1
        id: m
        palette:
          a: "$a"
        """])
        XCTAssertEqual(r.warnings.map(\.code), [.paletteSelfReference])
        XCTAssertNil(r.value!.palette["a"])
    }

    // ───────────────── §7 繼承與合併 ─────────────────

    func testInheritanceMergesMapsAndReplacesSequences() {
        let parent = """
        format: rime-theme/1
        id: parent
        typography:
          fonts:
            candidate:
              family: ["A", "B"]
              script_fallback:
                hant: ["PA"]
                hans: ["PB"]
        candidates:
          item:
            padding_h: 10
            corner_radius: 4
        """
        let child = """
        format: rime-theme/1
        id: child
        inherits: parent
        typography:
          fonts:
            candidate:
              family: ["C"]
              script_fallback:
                hant: ["CA"]
        candidates:
          item:
            corner_radius: 14
        """
        let r = loadTheme("child", ["parent": parent, "child": child])
        let t = r.value!
        XCTAssertEqual(t.candidates.item.paddingH, 10, "映射逐 key 合併")
        XCTAssertEqual(t.candidates.item.cornerRadius, 14, "子勝出")
        XCTAssertEqual(t.typography.fonts["candidate"]!.family, ["C"], "序列整體取代")
        let fb = t.typography.fonts["candidate"]!.scriptFallback
        XCTAssertEqual(fb["hant"], ["CA"])
        XCTAssertEqual(fb["hans"], ["PB"], "script_fallback 是映射，逐 key 合併")
        XCTAssertEqual(t.ancestry, ["parent", "child"])
    }

    /// §7.2 最容易做錯的一格：顯式 null 回到**規範預設值**，不是父值。
    func testExplicitNullDeletesToSpecDefaultNotParentValue() {
        let parent = """
        format: rime-theme/1
        id: parent
        candidates:
          item:
            border_width: 1
            highlight_background: "#123456"
        """
        let child = """
        format: rime-theme/1
        id: child
        inherits: parent
        candidates:
          item:
            border_width: ~
            highlight_background: null
        """
        let t = loadTheme("child", ["parent": parent, "child": child]).value!
        XCTAssertEqual(t.candidates.item.borderWidth, 0)
        XCTAssertEqual(t.candidates.item.highlightBackground, RGBA.hex(0x3060C0FF))
    }

    /// §7.3：`revision` / `id` / `format` 不參與繼承。
    func testOwnKeysAreNotInherited() {
        let parent = "format: rime-theme/1\nid: parent\nrevision: 9\nauthor: P\n"
        let child = "format: rime-theme/1\nid: child\ninherits: parent\n"
        let t = loadTheme("child", ["parent": parent, "child": child]).value!
        XCTAssertEqual(t.revision, 1, "revision 取自最終文件本身")
        XCTAssertEqual(t.author, "P", "author 會繼承")
    }

    func testPlatformOverrides() {
        let doc = """
        format: rime-theme/1
        id: m
        candidates:
          window:
            max_width: 400
        platform_overrides:
          macos:
            candidates:
              window:
                max_width: 520
          plan9:
            candidates:
              window:
                max_width: 1
        """
        XCTAssertEqual(loadTheme("m", ["m": doc], platform: .macos).value!.window.maxWidth, 520)
        XCTAssertEqual(loadTheme("m", ["m": doc], platform: .windows).value!.window.maxWidth, 400)
        XCTAssertEqual(loadTheme("m", ["m": doc], platform: .macos).diagnostics.count, 0,
                       "§7.4：未知平台鍵必須被忽略且不產生 WARNING")
    }

    func testNestedPlatformOverridesWarn() {
        let r = loadTheme("m", ["m": """
        format: rime-theme/1
        id: m
        platform_overrides:
          macos:
            platform_overrides:
              windows:
                author: x
        """])
        XCTAssertEqual(r.warnings.map(\.code), [.nestedPlatformOverrides])
    }

    // ───────────────── §6.2 致命錯誤 ─────────────────

    /// §10 檢核第 3 條。
    func testEmptyDocumentIsFatalF3() {
        let r = loadTheme("m", ["m": "{}"])
        XCTAssertFalse(r.isSuccess)
        XCTAssertEqual(r.errors.map(\.code), [.fatalFormatMissing])
    }

    func testWrongKindIsFatal() {
        let r = loadTheme("m", ["m": "format: rime-layout/1\nid: m\n"])
        XCTAssertEqual(r.errors.map(\.code), [.fatalFormatKindMismatch])
    }

    func testFutureMajorIsFatal() {
        let r = loadTheme("m", ["m": "format: rime-theme/2\nid: m\n"])
        XCTAssertEqual(r.errors.map(\.code), [.fatalFormatMajorUnsupported])
    }

    func testIdMismatchIsFatal() {
        let r = loadTheme("m", ["m": "format: rime-theme/1\nid: other\n"])
        XCTAssertEqual(r.errors.map(\.code), [.fatalIdMismatch])
    }

    func testUppercaseIdIsFatal() {
        let r = loadTheme("M", ["M": "format: rime-theme/1\nid: M\n"])
        XCTAssertEqual(r.errors.map(\.code), [.fatalIdInvalid])
    }

    func testMissingParentIsFatalF5() {
        let r = loadTheme("child", ["child": "format: rime-theme/1\nid: child\ninherits: ghost\n"])
        XCTAssertEqual(r.errors.map(\.code), [.fatalParentNotFound])
    }

    /// §10 檢核第 8 條：指向自己不得無限遞迴。
    func testSelfInheritIsFatalF6() {
        let r = loadTheme("m", ["m": "format: rime-theme/1\nid: m\ninherits: m\n"])
        XCTAssertEqual(r.errors.map(\.code), [.fatalInheritsCycle])
    }

    func testMinClientTooNewIsFatalF7() {
        let r = loadTheme("m", ["m": "format: rime-theme/1\nid: m\nmin_client: \"99.0.0\"\n"])
        XCTAssertEqual(r.errors.map(\.code), [.fatalMinClient])
    }

    func testYamlSyntaxErrorIsFatalF1() {
        let r = loadTheme("m", ["m": "format: rime-theme/1\nid: m\nx:\n\ty: 1\n"])
        XCTAssertEqual(r.errors.map(\.code), [.fatalYamlSyntax])
    }

    // ───────────────── 本輪新增的欄位 ─────────────────

    func testWindowGridFields() {
        let t = loadTheme("m", ["m": """
        format: rime-theme/1
        id: m
        candidates:
          window:
            lines: 3
            equal_columns: false
            column_gap: 12
            row_gap: 5
            max_height: 300
            item_align: center
            overflow: clip
        """]).value!
        XCTAssertEqual(t.window.lines, 3)
        XCTAssertFalse(t.window.equalColumns)
        XCTAssertEqual(t.window.columnGap, 12)
        XCTAssertEqual(t.window.rowGap, 5)
        XCTAssertEqual(t.window.maxHeight, 300)
        XCTAssertEqual(t.window.itemAlign, .center)
        XCTAssertEqual(t.window.overflow, .clip)
    }

    func testStatusBarDefaultsToTheNormativeList() {
        let t = loadTheme("m", ["m": """
        format: rime-theme/1
        id: m
        status_bar:
          show: true
        """]).value!
        XCTAssertTrue(t.statusBar.show)
        XCTAssertEqual(t.statusBar.items.map(\.source),
                       [.schemaName, .inputModePair, .variant, .page])
        XCTAssertEqual(t.statusBar.items[0].tap?.verb, .schemaPicker)
    }

    func testStatusBarItemsCanBeOverridden() {
        let r = loadTheme("m", ["m": """
        format: rime-theme/1
        id: m
        status_bar:
          show: true
          arrangement: space_between
          items:
            - { source: variant, tap: "toggle:simplification" }
            - { source: text, text: "RimeQuad" }
            - { tap: "settings" }
        """])
        let t = r.value!
        XCTAssertEqual(t.statusBar.arrangement, .spaceBetween)
        XCTAssertEqual(t.statusBar.items.count, 2, "沒有 source 的項目要被丟棄")
        XCTAssertEqual(t.statusBar.items[1].text, "RimeQuad")
        XCTAssertEqual(r.warnings.map(\.code), [.statusItemNoSource])
    }

    func testUnknownActionInStatusBarWarns() {
        let r = loadTheme("m", ["m": """
        format: rime-theme/1
        id: m
        status_bar:
          items:
            - { source: text, text: "x", tap: "teleport" }
        """])
        XCTAssertEqual(r.warnings.map(\.code), [.unknownAction])
        XCTAssertNil(r.value!.statusBar.items[0].tap, "解析不出來的 action 不得留下")
    }

    func testAccessibilityBlock() {
        let t = loadTheme("m", ["m": """
        format: rime-theme/1
        id: m
        accessibility:
          announce_candidates: text_only
          candidate_announcement: "{text}"
          announce_page: false
        """]).value!
        XCTAssertEqual(t.accessibility.announceCandidates, .textOnly)
        XCTAssertEqual(t.accessibility.candidateAnnouncement, "{text}")
        XCTAssertFalse(t.accessibility.announcePage)
    }

    /// §8.4：六個具名字體堆疊必須在缺失時被補齊。
    func testRequiredFontStacksArePresent() {
        let t = loadTheme("minimal", ["minimal": minimal]).value!
        for name in ["ui", "candidate", "label", "comment", "key", "preedit"] {
            XCTAssertNotNil(t.typography.fonts[name], "缺少具名字體堆疊 \(name)")
        }
    }

    func testUnknownScriptTagWarns() {
        let r = loadTheme("m", ["m": """
        format: rime-theme/1
        id: m
        typography:
          fonts:
            candidate:
              script_fallback:
                klingon: ["X"]
        """])
        XCTAssertEqual(r.warnings.map(\.code), [.unknownScriptTag])
    }

    func testFontAssetEscapeIsDropped() {
        let r = loadTheme("m", ["m": """
        format: rime-theme/1
        id: m
        typography:
          assets:
            - { family: "X", file: "../evil.ttf" }
        """])
        XCTAssertEqual(r.warnings.map(\.code), [.assetPathEscape])
        XCTAssertEqual(r.value!.typography.assets.count, 0)
    }

    /// §8.6.1：`highlight_color` 缺席時取自同區塊的 `color`。
    func testHighlightColourDerivesFromColour() {
        let t = loadTheme("m", ["m": """
        format: rime-theme/1
        id: m
        candidates:
          text:
            color: "#102030"
        """]).value!
        XCTAssertEqual(t.candidates.text.highlightColor, RGBA.hex(0x102030FF))
    }
}
