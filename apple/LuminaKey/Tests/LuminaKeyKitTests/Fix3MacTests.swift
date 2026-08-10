import XCTest
@testable import LuminaKeyKit

// ─────────────────────────────────────────────────────────────────────────
// M-1 中／英切換的快捷鍵
// ─────────────────────────────────────────────────────────────────────────

final class InputModeSwitchTests: XCTestCase {

    /// IMKit 預設**只**送 keyDown。修飾鍵在 macOS 上不產生 keyDown，
    /// 所以不多要 `flagsChanged` 的話，輕點 Shift 這條路整條不存在。
    func testRecognizedMaskMustAskForFlagsChanged() {
        XCTAssertTrue(IMKRecognizedEvents.includesFlagsChanged(IMKRecognizedEvents.mask),
                      "沒有 flagsChanged 就收不到修飾鍵 —— 中英切換不會有任何反應")
        XCTAssertFalse(IMKRecognizedEvents.includesFlagsChanged(IMKRecognizedEvents.imkitDefault),
                       "這一條在描述 IMKit 的預設值；它要是也含 flagsChanged，上一條就沒有意義")
        XCTAssertNotEqual(IMKRecognizedEvents.mask, IMKRecognizedEvents.imkitDefault,
                          "我們必須比 IMKit 的預設多要一種事件")
        // keyDown 不可以被順手拿掉 —— 那會讓一個字都打不出來。
        XCTAssertTrue(IMKRecognizedEvents.mask & IMKRecognizedEvents.keyDown != 0)
    }

    /// 位元位置就是 `NSEvent.EventType` 的 rawValue（keyDown=10、flagsChanged=12）。
    /// 這裡只釘住數值；與 AppKit 真的對得上由 `--self-check` 在有 AppKit 的地方驗。
    func testMaskBitPositions() {
        XCTAssertEqual(IMKRecognizedEvents.keyDown, 1 << 10)
        XCTAssertEqual(IMKRecognizedEvents.flagsChanged, 1 << 12)
    }

    /// 送得進引擎的必須是 `Shift_L` 的**按下與放開兩個事件**：
    /// librime 的 ascii_composer 偵測的是 release，而 release 必須帶著
    /// 該修飾鍵自己的位元（X11 語義：這顆鍵放開了，鍵還是那顆）。
    func testShiftTapProducesPressAndReleaseWithTheShiftBit() {
        let mapper = KeyMapper(resolver: FixtureKeysyms())
        var tracker = ModifierTracker()

        let down = tracker.transition(keyCode: 56, flags: [.shift])
        XCTAssertNotNil(down)
        XCTAssertEqual(down?.isKeyUp, false)
        let downStroke = mapper.stroke(for: MacKeyEvent(keyCode: 56,
                                                        charactersIgnoringModifiers: "",
                                                        flags: down!.flags,
                                                        isKeyUp: down!.isKeyUp))
        XCTAssertEqual(downStroke?.keysym, 0xFFE1, "Shift_L")
        XCTAssertEqual(downStroke!.modifiers & RSModifier.release.rawValue, 0)

        let up = tracker.transition(keyCode: 56, flags: [])
        XCTAssertNotNil(up)
        XCTAssertEqual(up?.isKeyUp, true)
        XCTAssertTrue(up!.flags.contains(.shift),
                      "放開時仍要帶著 shift 位元，否則 librime 認不出是哪一顆")
        let upStroke = mapper.stroke(for: MacKeyEvent(keyCode: 56,
                                                      charactersIgnoringModifiers: "",
                                                      flags: up!.flags,
                                                      isKeyUp: up!.isKeyUp))
        XCTAssertEqual(upStroke?.keysym, 0xFFE1)
        XCTAssertNotEqual(upStroke!.modifiers & RSModifier.release.rawValue, 0)
    }

    /// 「現在是什麼就顯示什麼」：那一行只講**一個**狀態。
    func testMenuTitleNamesOnlyTheCurrentMode() {
        for lang in [UiLanguage.zhHant, .zhHans, .en] {
            let chinese = InputModeSwitch.menuTitle(isAsciiMode: false, lang: lang)
            let english = InputModeSwitch.menuTitle(isAsciiMode: true, lang: lang)
            XCTAssertNotEqual(chinese, english, "\(lang) 兩種狀態的文字必須不同")

            let cjkName = InputModeSwitch.currentModeName(isAsciiMode: false)[lang]
            let latinName = InputModeSwitch.currentModeName(isAsciiMode: true)[lang]
            XCTAssertNotEqual(cjkName, latinName)

            XCTAssertTrue(chinese.contains(cjkName), "\(lang)：中文模式那一行要說「\(cjkName)」")
            XCTAssertFalse(chinese.contains(latinName),
                           "\(lang)：不得把另一態也寫上去 —— 那就是「兩個都畫出來讓他猜」")
            XCTAssertTrue(english.contains(latinName))
            XCTAssertFalse(english.contains(cjkName))

            XCTAssertTrue(chinese.contains(InputModeSwitch.switchKeyLabel),
                          "要告訴使用者按哪一顆鍵，否則他找不到這個功能")
        }
    }

    /// ⌃Space 在 macOS 是系統的「選取上一個輸入來源」。照抄 Windows 會撞車，
    /// 所以這一端的切換鍵**不是**它。
    func testSwitchKeyIsShiftNotControlSpace() {
        XCTAssertEqual(InputModeSwitch.switchKeyLabel, "Shift")
    }
}

// ─────────────────────────────────────────────────────────────────────────
// M-2 狀態列不得把兩態並排
// ─────────────────────────────────────────────────────────────────────────

final class DesktopStatusBarTests: XCTestCase {

    private func status(ascii: Bool) -> EngineStatus {
        var s = EngineStatus()
        s.isAsciiMode = ascii
        s.schemaName = "注音"          // 刻意不含「中」也不含「En」
        s.schemaId = "bopomofo_tw"
        return s
    }

    private func rendered(_ items: [StatusItem], ascii: Bool) -> String {
        items.map {
            StatusFace.plainText(StatusFace.segments(for: $0, status: status(ascii: ascii),
                                                     pageNo: 0, isLastPage: true))
        }.joined(separator: " ")
    }

    /// 桌面狀態列是**顯示**不是按鍵，所以只畫現在那一態。
    func testDefaultItemsShowOnlyTheCurrentMode() {
        XCTAssertTrue(StatusBar.defaultItems.contains { $0.source == .inputMode })
        XCTAssertFalse(StatusBar.defaultItems.contains { $0.source == .inputModePair },
                       "兩態並排會讓使用者要猜現在是哪一個 —— 真機回報過")
    }

    /// 行為面的那一條：任何一種狀態下，畫面上都**只會**出現一個模式字樣。
    func testRenderedStatusBarNeverShowsBothModesAtOnce() {
        let cjkOnly = rendered(StatusBar.defaultItems, ascii: false)
        XCTAssertTrue(cjkOnly.contains(StatusFace.cjk))
        XCTAssertFalse(cjkOnly.contains(StatusFace.latin), "中文模式底下不該出現「En」")

        let latinOnly = rendered(StatusBar.defaultItems, ascii: true)
        XCTAssertTrue(latinOnly.contains(StatusFace.latin))
        XCTAssertFalse(latinOnly.contains(StatusFace.cjk), "英文模式底下不該出現「中」")
    }

    /// `input_mode_pair` 本身**不可以被刪掉**：行動端的佈局按鍵在用它
    /// （core/layouts/ 底下有 20 幾處），刪了那些主題會解析出 unknown。
    func testPairIsStillSupportedForMobileKeys() {
        let pair = StatusFace.segments(for: StatusItem(source: .inputModePair),
                                       status: status(ascii: false), pageNo: 0, isLastPage: true)
        XCTAssertEqual(pair.map(\.text), ["中", "/", "En"])
    }
}

final class AppearanceOverridesTests: XCTestCase {

    private func theme(statusBarShows: Bool) -> Theme {
        var t = Theme(id: "default-light")
        t.statusBar.show = statusBarShows
        return t
    }

    /// 這一顆開關在畫面上一直都在，但沒有任何地方讀它 ——
    /// 「切得動、存得起來、什麼都不會發生」。
    func testShowStatusBarOverridesTheTheme() {
        var s = LuminaKeySettings()

        s.showStatusBar = .on
        XCTAssertTrue(AppearanceOverrides.apply(theme(statusBarShows: false), settings: s)
                        .statusBar.show, "使用者說要顯示，主題不得否決")

        s.showStatusBar = .off
        XCTAssertFalse(AppearanceOverrides.apply(theme(statusBarShows: true), settings: s)
                        .statusBar.show)

        s.showStatusBar = .followTheme
        XCTAssertTrue(AppearanceOverrides.apply(theme(statusBarShows: true), settings: s)
                        .statusBar.show)
        XCTAssertFalse(AppearanceOverrides.apply(theme(statusBarShows: false), settings: s)
                        .statusBar.show)
    }

    /// 其餘欄位一律不動 —— 這一層只接一項，而那件事要說出來。
    func testOnlyTheDeclaredFieldIsTouched() {
        var s = LuminaKeySettings()
        s.showStatusBar = .on
        s.candidateScale = .extraLarge
        s.candidateOrientation = .vertical
        s.showCandidateLabels = .off

        var t = Theme(id: "default-light")
        t.window.style.orientation = .horizontal
        t.window.style.label.show = true
        t.window.style.text.size = 17

        let out = AppearanceOverrides.apply(t, settings: s)
        XCTAssertEqual(out.window.style.orientation, .horizontal,
                       "candidateOrientation 還沒接線，這裡不得偷偷生效")
        XCTAssertTrue(out.window.style.label.show)
        XCTAssertEqual(out.window.style.text.size, 17,
                       "candidateScale 還沒接線")
    }

    /// 已接與未接的欄位清單是**紀錄**，不是註解。動它的人要一起改這裡。
    func testWiringInventoryIsRecorded() {
        XCTAssertEqual(AppearanceOverrides.wiredFields, ["showStatusBar"])
        XCTAssertEqual(AppearanceOverrides.unwiredFields,
                       ["candidateScale", "candidateOrientation", "showCandidateLabels"])
        XCTAssertTrue(Set(AppearanceOverrides.wiredFields)
                        .isDisjoint(with: Set(AppearanceOverrides.unwiredFields)))
    }
}

// ─────────────────────────────────────────────────────────────────────────
// M-3 「啟用的方案」清單
// ─────────────────────────────────────────────────────────────────────────

final class EffectiveSchemaListTests: XCTestCase {

    /// 上游 rime-prelude 的 default.yaml 長這樣（節錄，含我們沒有打包的方案）。
    private let upstreamDefault = """
    # Rime default settings
    config_version: '0.50'

    schema_list:
      - schema: luna_pinyin
      - schema: bopomofo
      - schema: cangjie5
      - schema: quick5
      - schema: stroke
      - schema: terra_pinyin

    switcher:
      caption: 〔方案選單〕
      hotkeys:
        - Control+grave
        - F4

    menu:
      page_size: 5
    """

    /// `scripts/collect_data.sh` 產生、隨 .app 附上的那一份。
    private let ourCustom = """
    # 由 scripts/collect_data.sh 產生。
    patch:
      schema_list:
        - schema: luna_pinyin_tw    # 拼音（臺灣字形）
        - schema: bopomofo_tw       # 注音（臺灣字形）
        - schema: luna_pinyin
        - schema: t9_pinyin
    """

    func testReadsTopLevelSchemaListFromDefaultYaml() {
        XCTAssertEqual(SchemaListReader.readBaseSchemaList(text: upstreamDefault),
                       ["luna_pinyin", "bopomofo", "cangjie5", "quick5", "stroke", "terra_pinyin"])
    }

    /// `switcher:` 底下也有 `- Control+grave` 這種清單項。頂層鍵一換就要停，
    /// 否則熱鍵會被當成方案名混進清單裡。
    func testStopsAtTheNextTopLevelKey() {
        let ids = SchemaListReader.readBaseSchemaList(text: upstreamDefault)
        XCTAssertFalse(ids.contains { $0.contains("grave") || $0 == "F4" })
        XCTAssertEqual(ids.count, 6)
    }

    /// **這一條就是缺陷本身。** 沒有 `default.custom.yaml` 時（全新安裝就是這樣），
    /// 舊的實作回傳空陣列 → 設定畫面一列都沒有勾、繁簡綁定也挑不出方案。
    func testFallsBackToTheBaseListWhenThereIsNoCustomFile() {
        let r = SchemaListReader.resolve(customText: "", baseText: upstreamDefault)
        XCTAssertEqual(r.source, .base)
        XCTAssertFalse(r.ids.isEmpty, "引擎有方案，清單就不可以是空的")
        XCTAssertEqual(r.ids.first, "luna_pinyin")
    }

    /// 而那個 fallback 也同時說明了為什麼**範本非裝不可**：
    /// 底層那一份裡沒有我們真正打包的臺灣字形方案。
    func testBaseListDoesNotContainTheSchemasWeActuallyShip() {
        let r = SchemaListReader.resolve(customText: "", baseText: upstreamDefault)
        for shipped in ["luna_pinyin_tw", "bopomofo_tw", "t9_pinyin"] {
            XCTAssertFalse(r.ids.contains(shipped),
                           "\(shipped) 只在隨附的 default.custom.yaml 裡；沒裝範本就等於沒啟用")
        }
        for missing in ["cangjie5", "quick5"] {
            XCTAssertTrue(r.ids.contains(missing),
                          "而底層那一份反過來列了我們沒有打包的 \(missing) —— 部署會報錯")
        }
    }

    func testCustomPatchWinsAndReplacesRatherThanMerges() {
        let r = SchemaListReader.resolve(customText: ourCustom, baseText: upstreamDefault)
        XCTAssertEqual(r.source, .custom)
        XCTAssertEqual(r.ids, ["luna_pinyin_tw", "bopomofo_tw", "luna_pinyin", "t9_pinyin"])
        XCTAssertFalse(r.ids.contains("cangjie5"), "patch 是整段取代，不是附加")
    }

    func testTrulyEmptyIsStillEmpty() {
        XCTAssertEqual(SchemaListReader.resolve(customText: "", baseText: ""), .empty)
        XCTAssertEqual(SchemaListReader.resolve(customText: "patch:\n  menu/page_size: 9\n",
                                                baseText: "").source, .none)
    }

    /// 讀壞掉的檔案不可以拋錯，也不可以把一堆垃圾當成方案。
    func testGarbageInGarbageOut() {
        XCTAssertEqual(SchemaListReader.readBaseSchemaList(text: "schema_list:\n  - \n  - # 註解\n"),
                       [])
        XCTAssertEqual(SchemaListReader.readBaseSchemaList(
            text: "schema_list:\n  - schema: a b c\n  - schema: ok_1\n"), ["ok_1"])
    }

    /// **這一條是 fallback 自己開出來的新失敗面。**
    ///
    /// 上游 `default.yaml` 列了 `cangjie5` / `quick5`,而我們從來沒有打包過
    /// 它們(`core/data/shared` 只有 7 個 `.schema.yaml`)。原樣端出去的後果
    /// 不只是多兩列:`SchemaCatalog.rows` 會把它們畫成「已勾選 + 找不到檔案」,
    /// 而使用者既沒有勾過它們、也沒有辦法修好。
    func testFallbackListsOnlyWhatIsActuallyInstalled() {
        let installed: Set<String> = ["luna_pinyin", "bopomofo", "stroke", "terra_pinyin"]
        let r = SchemaListReader.resolve(customText: "", baseText: upstreamDefault,
                                         installedIds: installed)
        XCTAssertEqual(r.source, .base)
        XCTAssertEqual(r.ids, ["luna_pinyin", "bopomofo", "stroke", "terra_pinyin"],
                       "順序仍然照上游那一份,只是把沒打包的濾掉")
        for missing in ["cangjie5", "quick5"] {
            XCTAssertFalse(r.ids.contains(missing), "\(missing) 沒有打包,不可以出現在清單裡")
        }
        // 一個都不在磁碟上 = 真的「一個方案都沒有」,不是六個壞掉的。
        XCTAssertEqual(SchemaListReader.resolve(customText: "", baseText: upstreamDefault,
                                                installedIds: []), .empty)
    }

    /// **使用者自己勾的那一份不過濾。** 檔案不見了是他需要知道的事;
    /// 偷偷拿掉會讓 schema_list 在他不知情的時候變短。
    func testTheUserPatchIsNeverFilteredByWhatIsOnDisk() {
        let r = SchemaListReader.resolve(customText: ourCustom, baseText: upstreamDefault,
                                         installedIds: ["luna_pinyin"])
        XCTAssertEqual(r.source, .custom)
        XCTAssertEqual(r.ids, ["luna_pinyin_tw", "bopomofo_tw", "luna_pinyin", "t9_pinyin"])
    }

    /// 而畫面那一端要看得出差別:過濾之後不會再出現「已勾選 + 找不到檔案」。
    func testNoPhantomCheckedRowsAfterTheFallback() {
        let installed = [InstalledSchema(id: "luna_pinyin", name: "朙月拼音",
                                         url: URL(fileURLWithPath: "/x/luna_pinyin.schema.yaml"),
                                         isBuiltin: true)]
        let r = SchemaListReader.resolve(customText: "", baseText: upstreamDefault,
                                         installedIds: ["luna_pinyin"])
        let rows = SchemaCatalog.rows(installed: installed, enabled: r.ids)
        XCTAssertEqual(rows.map(\.id), ["luna_pinyin"])
        XCTAssertTrue(rows.allSatisfy { $0.installed },
                      "不得出現「已勾選但找不到檔案」的列")
    }

    func testFilesOnDisk() throws {
        let tmp = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("lk-eff-\(UUID().uuidString)")
        let user = tmp.appendingPathComponent("user")
        let shared = tmp.appendingPathComponent("shared")
        try FileManager.default.createDirectory(at: user, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: shared, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tmp) }

        try upstreamDefault.write(to: shared.appendingPathComponent("default.yaml"),
                                  atomically: true, encoding: .utf8)

        // 全新安裝：使用者目錄裡什麼都沒有。
        // 隨附目錄裡真的有的那幾個(對照 core/data/shared 的 7 個 .schema.yaml)。
        for id in ["luna_pinyin", "bopomofo", "stroke", "terra_pinyin"] {
            try "schema:\n  schema_id: \(id)\n"
                .write(to: shared.appendingPathComponent("\(id).schema.yaml"),
                       atomically: true, encoding: .utf8)
        }
        var r = SchemaListReader.resolve(userDir: user, sharedDir: shared)
        XCTAssertEqual(r.source, .base)
        XCTAssertEqual(r.ids, ["luna_pinyin", "bopomofo", "stroke", "terra_pinyin"],
                       "退回上游那一份時只列磁碟上真的有的 —— cangjie5 / quick5 沒打包")

        // 裝了範本之後。
        try ourCustom.write(to: user.appendingPathComponent("default.custom.yaml"),
                            atomically: true, encoding: .utf8)
        r = SchemaListReader.resolve(userDir: user, sharedDir: shared)
        XCTAssertEqual(r.source, .custom)
        XCTAssertEqual(r.ids.first, "luna_pinyin_tw")
    }
}

final class UserDataSeedTests: XCTestCase {

    private func sandbox() throws -> (template: URL, user: URL, cleanup: () -> Void) {
        let tmp = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("lk-seed-\(UUID().uuidString)")
        let template = tmp.appendingPathComponent("UserTemplate")
        let user = tmp.appendingPathComponent("user")
        try FileManager.default.createDirectory(at: template, withIntermediateDirectories: true)
        return (template, user, { try? FileManager.default.removeItem(at: tmp) })
    }

    func testSeedsWhenTheUserDirectoryIsEmpty() throws {
        let (template, user, cleanup) = try sandbox()
        defer { cleanup() }
        try "patch:\n  schema_list:\n    - schema: luna_pinyin_tw\n"
            .write(to: template.appendingPathComponent("default.custom.yaml"),
                   atomically: true, encoding: .utf8)

        let o = UserDataSeed.run(templateDir: template, userDir: user)
        XCTAssertEqual(o.copied, ["default.custom.yaml"])
        XCTAssertTrue(o.kept.isEmpty)
        XCTAssertTrue(o.failed.isEmpty)
        XCTAssertFalse(o.templateMissing)

        let ids = RimeConfigPatch.readSchemaList(userDir: user)
        XCTAssertEqual(ids, ["luna_pinyin_tw"], "裝完之後 librime 與設定畫面看到的是同一份")
    }

    /// **絕對不可以覆蓋。** 那個檔案裡是使用者勾選的方案順序與每頁候選數。
    func testNeverOverwritesWhatTheUserAlreadyHas() throws {
        let (template, user, cleanup) = try sandbox()
        defer { cleanup() }
        try "patch:\n  schema_list:\n    - schema: luna_pinyin_tw\n"
            .write(to: template.appendingPathComponent("default.custom.yaml"),
                   atomically: true, encoding: .utf8)
        try FileManager.default.createDirectory(at: user, withIntermediateDirectories: true)
        let mine = "patch:\n  schema_list:\n    - schema: t9_pinyin\n  menu/page_size: 9\n"
        try mine.write(to: user.appendingPathComponent("default.custom.yaml"),
                       atomically: true, encoding: .utf8)

        let o = UserDataSeed.run(templateDir: template, userDir: user)
        XCTAssertEqual(o.copied, [])
        XCTAssertEqual(o.kept, ["default.custom.yaml"])
        XCTAssertEqual(try String(contentsOf: user.appendingPathComponent("default.custom.yaml"),
                                  encoding: .utf8), mine)
        XCTAssertEqual(RimeConfigPatch.readPageSize(userDir: user), 9)
    }

    /// 冪等：第二次跑不得再動任何東西。兩個行程各叫一次是常態。
    func testIsIdempotent() throws {
        let (template, user, cleanup) = try sandbox()
        defer { cleanup() }
        try "a\n".write(to: template.appendingPathComponent("default.custom.yaml"),
                        atomically: true, encoding: .utf8)
        XCTAssertEqual(UserDataSeed.run(templateDir: template, userDir: user).copied.count, 1)
        let second = UserDataSeed.run(templateDir: template, userDir: user)
        XCTAssertEqual(second.copied, [])
        XCTAssertEqual(second.kept, ["default.custom.yaml"])
        XCTAssertNil(UserDataSeed.logLine(second), "沒事發生就不要每次啟動都吐一行 log")
    }

    /// 範本不在 = .app 內容不完整。這件事必須說出來，不可以靜靜跳過 ——
    /// 它的後果正是這一輪要修的缺陷。
    func testMissingTemplateIsReported() throws {
        let (template, user, cleanup) = try sandbox()
        defer { cleanup() }
        let o = UserDataSeed.run(templateDir: template, userDir: user)
        XCTAssertTrue(o.templateMissing)
        XCTAssertNotNil(UserDataSeed.logLine(o))
    }

    func testDirectoriesInsideTheTemplateAreIgnored() throws {
        let (template, user, cleanup) = try sandbox()
        defer { cleanup() }
        try FileManager.default.createDirectory(
            at: template.appendingPathComponent("build"), withIntermediateDirectories: true)
        try "a\n".write(to: template.appendingPathComponent("default.custom.yaml"),
                        atomically: true, encoding: .utf8)
        let o = UserDataSeed.run(templateDir: template, userDir: user)
        XCTAssertEqual(o.copied, ["default.custom.yaml"])
        XCTAssertFalse(FileManager.default.fileExists(
            atPath: user.appendingPathComponent("build").path))
    }
}

// ─────────────────────────────────────────────────────────────────────────
// M-4 候選窗的翻頁
// ─────────────────────────────────────────────────────────────────────────

final class ScrollPagerTests: XCTestCase {

    /// 傳統滾輪：一格就是一頁，不累積。
    func testWheelPagesImmediately() {
        var p = ScrollPager()
        XCTAssertEqual(p.feed(delta: -1, isPrecise: false), .next)
        XCTAssertEqual(p.feed(delta: 1, isPrecise: false), .previous)
        XCTAssertEqual(p.feed(delta: 0, isPrecise: false), .none)
    }

    /// 觸控板：要累積到門檻才算一頁，否則手指一放上去就翻好幾頁。
    func testTrackpadNeedsToCrossTheThreshold() {
        var p = ScrollPager()
        let half = ScrollPager.preciseThreshold / 2 - 1
        XCTAssertEqual(p.feed(delta: -half, isPrecise: true), .none)
        XCTAssertEqual(p.feed(delta: -half, isPrecise: true), .none)
        XCTAssertEqual(p.feed(delta: -3, isPrecise: true), .next)
        // 翻過之後歸零，不可以讓下一個微小滾動又翻一頁。
        XCTAssertEqual(p.feed(delta: -1, isPrecise: true), .none)
    }

    /// 來回小幅晃動不該累積成一次誤翻。
    func testDirectionChangeResetsTheAccumulator() {
        var p = ScrollPager()
        for _ in 0..<5 {
            XCTAssertEqual(p.feed(delta: -6, isPrecise: true), .none)
            XCTAssertEqual(p.feed(delta: 6, isPrecise: true), .none)
        }
    }

    func testUpMeansPreviousPage() {
        var p = ScrollPager()
        XCTAssertEqual(p.feed(delta: ScrollPager.preciseThreshold, isPrecise: true), .previous)
        XCTAssertEqual(PageStep.previous.backward, true)
        XCTAssertEqual(PageStep.next.backward, false)
    }

    func testResetClearsTheAccumulator() {
        var p = ScrollPager()
        _ = p.feed(delta: -(ScrollPager.preciseThreshold - 1), isPrecise: true)
        p.reset()
        XCTAssertEqual(p.feed(delta: -1, isPrecise: true), .none)
    }
}
