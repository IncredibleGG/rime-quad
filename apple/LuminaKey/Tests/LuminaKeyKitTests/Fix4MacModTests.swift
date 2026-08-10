import XCTest
@testable import LuminaKeyKit

// ─────────────────────────────────────────────────────────────────────────
// (1) 組字中按 Caps Lock 會清掉組字 —— 上一輪自己做出來的迴歸
// ─────────────────────────────────────────────────────────────────────────

final class ModifierGateTests: XCTestCase {

    /// **這一條就是缺陷本身。**
    ///
    /// 因果鏈(每一段都查得到出處):
    ///   1. `recognizedEvents(_:)` 宣告要收 `flagsChanged` 之後,IMKit 會把
    ///      **每一顆**修飾鍵的 flagsChanged 都送過來。
    ///   2. 舊版 `ModifierTracker` 把每一次 Caps Lock 的 flagsChanged 都當成
    ///      「按下」(那顆鍵的旗標代表**燈亮著**,不是鍵按著)。
    ///   3. 隨附的 `core/data/shared/default.yaml`(上游 rime-prelude)有
    ///      `ascii_composer/switch_key: { Caps_Lock: clear }`。
    ///   4. librime 的 `AsciiComposer::ProcessCapsLock()` 在 `!release` 時呼叫
    ///      `SwitchAsciiMode(..., kAsciiModeSwitchClear)`,而它對正在組字的
    ///      context 做的事是 `ctx->Clear()`
    ///      (third_party/librime/src/rime/gear/ascii_composer.cc)。
    ///
    /// 結果:打到一半按 Caps Lock,剛打的半句話沒了。使用者按 Caps Lock
    /// 是要打大寫,不是要丟掉組字。
    func testCapsLockNeverReachesTheEngine() {
        XCTAssertFalse(ModifierGate.shouldForward(keyCode: 57, flags: [.capsLock]),
                       "亮燈那一次")
        XCTAssertFalse(ModifierGate.shouldForward(keyCode: 57, flags: []),
                       "熄燈那一次")
        var t = ModifierTracker()
        XCTAssertNil(t.transition(keyCode: 57, flags: [.capsLock]),
                     "Caps Lock 不可以變成送進 librime 的按鍵事件")
        XCTAssertNil(t.transition(keyCode: 57, flags: []))
    }

    /// 只有真的會觸發切換的那一顆進得去。其餘的修飾鍵一概不碰 ——
    /// 送不進去的東西不會有行為,而白跑一趟引擎的東西會。
    func testOnlyTheSwitchKeyIsForwarded() {
        // ⌘L ⌘R ⌥L ⌥R ⌃L ⌃R Caps
        for code in [UInt16(55), 54, 58, 61, 59, 62, 57] {
            XCTAssertFalse(ModifierGate.shouldForward(keyCode: code, flags: []),
                           "keyCode \(code) 不該進 librime")
        }
        XCTAssertTrue(ModifierGate.shouldForward(keyCode: 56, flags: [.shift]), "Shift_L")
        XCTAssertTrue(ModifierGate.shouldForward(keyCode: 60, flags: [.shift]), "Shift_R")
        XCTAssertEqual(ModifierGate.switchKeyCodes, [56, 60])
    }

    /// ⌘⇧S 之類的宿主快捷鍵不得送進引擎。
    /// `process()` 早就有這條護欄(「Command 組合鍵一律讓宿主處理」),
    /// 而 `processFlags()` 以前沒有 —— 同一個 app 對同一顆鍵有兩種立場。
    func testHostShortcutCombinationsAreNotForwarded() {
        XCTAssertFalse(ModifierGate.shouldForward(keyCode: 56, flags: [.shift, .command]))
        XCTAssertFalse(ModifierGate.shouldForward(keyCode: 56, flags: [.shift, .control]))
        XCTAssertFalse(ModifierGate.shouldForward(keyCode: 56, flags: [.shift, .option]))
        XCTAssertFalse(ModifierGate.shouldForward(keyCode: 60, flags: [.shift, .command]))
    }

    /// ⚠ 大寫鎖定的**燈**亮著不算「按著一顆鍵」。
    ///   把 capsLock 算進阻擋清單的話,開著大寫鎖定的使用者輕點 Shift
    ///   會完全沒有反應 —— 那是把一個缺陷換成另一個。
    func testCapsLockLightDoesNotBlockTheShiftTap() {
        XCTAssertTrue(ModifierGate.shouldForward(keyCode: 56, flags: [.shift, .capsLock]))
        var t = ModifierTracker()
        XCTAssertEqual(t.transition(keyCode: 56, flags: [.shift, .capsLock])?.isKeyUp, false)
        XCTAssertEqual(t.transition(keyCode: 56, flags: [.capsLock])?.isKeyUp, true)
    }

    /// 不放行的修飾鍵**仍然要更新狀態**。
    /// 少了這一句,「按著 ⌘ 再按 Shift」會把後來的按下算成放開,
    /// 而症狀是切換鍵偶爾反過來 —— 那種缺陷沒有人重現得出來。
    func testUnforwardedModifiersStillUpdateTheTrackerState() {
        var t = ModifierTracker()
        XCTAssertNil(t.transition(keyCode: 55, flags: [.command]), "⌘ 按下")
        XCTAssertNil(t.transition(keyCode: 56, flags: [.command, .shift]),
                     "⌘ 還按著 → Shift 也不放行")
        XCTAssertNil(t.transition(keyCode: 55, flags: [.shift]), "⌘ 放開")
        XCTAssertEqual(t.transition(keyCode: 56, flags: [])?.isKeyUp, true,
                       "Shift 的位元從有變沒有 = 放開")
    }

    /// 放行清單裡的每一顆都要有 keysym 名稱,否則它送得進閘門、
    /// 卻在 `KeyMapper.stroke` 那一步變成 nil —— 又一個「什麼都沒發生」。
    func testEverySwitchKeyHasAKeysymName() {
        let fixture = FixtureKeysyms()
        for code in ModifierGate.switchKeyCodes {
            let name = PhysicalKeys.table[code]
            XCTAssertNotNil(name, "keyCode \(code) 沒有 keysym 名稱")
            XCTAssertNotEqual(fixture.keysym(named: name ?? ""), 0, "keyCode \(code)")
            XCTAssertNotNil(PhysicalKeys.modifierKeyFlag[code], "keyCode \(code) 沒有旗標")
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
// (2) 判準自己會不會說「不」
// ─────────────────────────────────────────────────────────────────────────

final class SwiftSourceTests: XCTestCase {

    // 健康的樣子。底下每一組都從它拆一刀。
    private let healthyContext = """
    final class AppContext {
        var effectiveTheme: Theme {
            AppearanceOverrides.apply(themes.current, settings: settings.current)
        }

        func applySchemaForInputMode() -> String? {
            let lists = SchemaListReader.resolve(userDir: userDataDir, sharedDir: sharedDataDir)
            let enabled = lists.ids
            let entries: [SchemaEntry] = enabled.map { byId[$0] }
            let resolution = InputModeBinding.resolve(script: script, enabled: entries)
            return resolution.schemaId
        }
    }
    """

    private let healthyPaging = """
    final class CandidateView: NSView {
        override func scrollWheel(with event: NSEvent) {
            let dy = Double(event.scrollingDeltaY)
            let delta = dy != 0 ? dy : Double(event.scrollingDeltaX)
            let step = pager.feed(delta: delta, isPrecise: event.hasPreciseScrollingDeltas)
            if step != .none { onChangePage?(step) }
        }
    }
    """

    private let healthyController = """
    final class Controller {
        override func activateServer(_ sender: Any!) {
            AppContext.shared.panel.onChangePage = { [weak self] step in
                self?.changePage(step, client: sender)
            }
        }

        private func updatePanel(_ snap: RimeSnapshot, client sender: Any!) {
            panel.show(theme: AppContext.shared.effectiveTheme, snapshot: snap,
                       caretRect: caretRect(client: sender))
        }
    }
    """

    // MARK: - 抹除

    func testCommentsAreBlankedOut() {
        let s = SwiftSource("let a = 1 // AppearanceOverrides.apply(x)\nlet b = 2\n")
        XCTAssertFalse(s.text.contains("AppearanceOverrides"))
        XCTAssertTrue(s.text.contains("let a = 1"))
        XCTAssertTrue(s.text.contains("let b = 2"), "換行必須留著,行號要對得回原檔")
        let block = SwiftSource("/* panel.show(theme: x) */ let c = 3")
        XCTAssertFalse(block.text.contains("panel.show"))
        XCTAssertTrue(block.text.contains("let c = 3"))
    }

    func testStringLiteralsAreBlankedOut() {
        let s = SwiftSource("NSLog(\"panel.onChangePage = nil\")\nlet z = 1\n")
        XCTAssertFalse(s.text.contains("onChangePage"),
                       "字串裡提到一個名字不算接上線")
        XCTAssertTrue(s.text.contains("let z = 1"))
    }

    func testANameThatOnlyAppearsInACommentIsNotACall() {
        let cut = """
        var effectiveTheme: Theme {
            // AppearanceOverrides.apply(themes.current, settings: settings.current)
            themes.current
        }
        """
        let body = SwiftSource(cut).body(of: "var effectiveTheme:") ?? ""
        XCTAssertFalse(body.isEmpty)
        XCTAssertTrue(SwiftSource.arguments(ofCall: "AppearanceOverrides.apply",
                                            in: body).isEmpty,
                      "這正是 grep 會答錯、而這一份必須答對的那一題")
    }

    // MARK: - 切主體

    func testBodyIsCutAtTheMatchingBrace() {
        let body = SwiftSource(healthyPaging).body(of: "override func scrollWheel(") ?? ""
        XCTAssertTrue(SwiftSource.calls("pager.feed", in: body))
        XCTAssertFalse(body.contains("class CandidateView"))
    }

    func testANameInAnotherFunctionIsNotInThisBody() {
        let src = SwiftSource(healthyController)
        let update = src.body(of: "private func updatePanel(") ?? ""
        XCTAssertFalse(SwiftSource.calls("changePage", in: update),
                       "changePage 在 activateServer 裡,不在 updatePanel 裡")
        let activate = src.body(of: "override func activateServer(") ?? ""
        XCTAssertFalse(SwiftSource.calls("panel.show", in: activate))
    }

    // MARK: - 引數

    func testArgumentsAreReadByLabel() {
        let body = SwiftSource(healthyController).body(of: "private func updatePanel(") ?? ""
        let args = SwiftSource.arguments(ofCall: "panel.show", in: body)
        XCTAssertEqual(args["theme"], "AppContext.shared.effectiveTheme")
        XCTAssertEqual(args["snapshot"], "snap")
        XCTAssertNotNil(args["caretRect"], "跨行的引數也要讀得到")
    }

    func testOptionalCallsAreRecognised() {
        let body = SwiftSource(healthyPaging).body(of: "override func scrollWheel(") ?? ""
        XCTAssertEqual(SwiftSource.arguments(ofCall: "onChangePage", in: body)["_0"], "step")
    }

    // MARK: - 值的去向

    func testValuesDerivedFollowsTheChain() {
        let body = SwiftSource(healthyContext).body(of: "func applySchemaForInputMode(") ?? ""
        let derived = SwiftSource.valuesDerived(fromCall: "SchemaListReader.resolve", in: body)
        XCTAssertTrue(derived.contains("lists"))
        XCTAssertTrue(derived.contains("enabled"))
        XCTAssertTrue(derived.contains("entries"), "值要跟著 map 一路傳下去")
        let arg = SwiftSource.arguments(ofCall: "InputModeBinding.resolve", in: body)["enabled"]
        XCTAssertTrue(SwiftSource.anyOf(derived, reaches: arg ?? ""))
    }

    func testValuesDerivedStopsWhenTheChainIsCut() {
        let cut = healthyContext.replacingOccurrences(
            of: "let enabled = lists.ids",
            with: "let enabled: [String] = []")
        let body = SwiftSource(cut).body(of: "func applySchemaForInputMode(") ?? ""
        let derived = SwiftSource.valuesDerived(fromCall: "SchemaListReader.resolve", in: body)
        XCTAssertEqual(derived, ["lists"], "算出來了,但沒有流到任何地方")
        let arg = SwiftSource.arguments(ofCall: "InputModeBinding.resolve", in: body)["enabled"]
        XCTAssertFalse(SwiftSource.anyOf(derived, reaches: arg ?? ""))
    }

    // MARK: - 賦值

    func testAssignedExpressionFindsTheClosure() {
        let body = SwiftSource(healthyController).body(of: "override func activateServer(") ?? ""
        let closure = SwiftSource.assignedExpression(to: "panel.onChangePage", in: body)
        XCTAssertNotNil(closure)
        XCTAssertTrue(SwiftSource.mentions("changePage", in: closure ?? ""))
    }

    func testComparisonIsNotAnAssignment() {
        let body = "if panel.onChangePage == nil { return }\n"
        XCTAssertNil(SwiftSource.assignedExpression(to: "panel.onChangePage", in: body),
                     "比較不是接線")
    }

    // MARK: - 六種「呼叫點刪掉、定義留著」

    /// M-2:兩處接線各拆一刀,兩刀都要被判成沒接。
    func testStatusBarCutsAreRejected() {
        let dataFlowCut = healthyContext.replacingOccurrences(
            of: "AppearanceOverrides.apply(themes.current, settings: settings.current)",
            with: "themes.current")
        let body = SwiftSource(dataFlowCut).body(of: "var effectiveTheme:") ?? ""
        XCTAssertTrue(SwiftSource.arguments(ofCall: "AppearanceOverrides.apply", in: body).isEmpty,
                      "資料流切斷:定義還在,只是沒有人叫它")

        let callSiteCut = healthyController.replacingOccurrences(
            of: "theme: AppContext.shared.effectiveTheme",
            with: "theme: AppContext.shared.themes.current")
        let update = SwiftSource(callSiteCut).body(of: "private func updatePanel(") ?? ""
        let theme = SwiftSource.arguments(ofCall: "panel.show", in: update)["theme"] ?? ""
        XCTAssertFalse(SwiftSource.mentions("effectiveTheme", in: theme),
                       "呼叫點刪掉:effectiveTheme 還定義著,只是沒有人用")
    }

    /// M-3:sharedDir 被拿掉(fallback 永遠找不到 default.yaml),
    /// 以及整條退回只讀使用者 patch。
    func testSchemaListCutsAreRejected() {
        let noShared = healthyContext.replacingOccurrences(
            of: "sharedDir: sharedDataDir", with: "sharedDir: nil")
        var body = SwiftSource(noShared).body(of: "func applySchemaForInputMode(") ?? ""
        var args = SwiftSource.arguments(ofCall: "SchemaListReader.resolve", in: body)
        XCTAssertFalse(SwiftSource.mentions("sharedDataDir", in: args["sharedDir"] ?? ""))

        let reverted = healthyContext.replacingOccurrences(
            of: "SchemaListReader.resolve(userDir: userDataDir, sharedDir: sharedDataDir)",
            with: "RimeConfigPatch.readSchemaList(userDir: userDataDir)")
        body = SwiftSource(reverted).body(of: "func applySchemaForInputMode(") ?? ""
        args = SwiftSource.arguments(ofCall: "SchemaListReader.resolve", in: body)
        XCTAssertTrue(args.isEmpty, "呼叫點刪掉、SchemaListReader 定義留著")
        XCTAssertTrue(SwiftSource.valuesDerived(fromCall: "SchemaListReader.resolve",
                                                in: body).isEmpty)
    }

    /// M-4:算出了要翻哪一頁卻不送出去,以及候選窗的 callback 沒有人接。
    func testPagingCutsAreRejected() {
        let notPublished = healthyPaging.replacingOccurrences(
            of: "if step != .none { onChangePage?(step) }",
            with: "if step != .none { _ = step }")
        let body = SwiftSource(notPublished).body(of: "override func scrollWheel(") ?? ""
        XCTAssertTrue(SwiftSource.calls("pager.feed", in: body), "純函式還是被叫了")
        XCTAssertNil(SwiftSource.arguments(ofCall: "onChangePage", in: body)["_0"],
                     "資料流切斷:算完就丟")

        let unwired = healthyController.replacingOccurrences(
            of: "AppContext.shared.panel.onChangePage = {",
            with: "let pageSink: (PageStep) -> Void = {")
        let activate = SwiftSource(unwired).body(of: "override func activateServer(") ?? ""
        XCTAssertNil(SwiftSource.assignedExpression(to: "panel.onChangePage", in: activate),
                     "呼叫點刪掉:changePage 還定義著,只是沒有人接")
    }
}

// ─────────────────────────────────────────────────────────────────────────
// (2) 三處接線,對**真的那幾份檔案**問一次
// ─────────────────────────────────────────────────────────────────────────

/// M-2:「設定 › 外觀 › 顯示狀態列」→ 候選窗畫出來的主題。
final class StatusBarWiringTests: XCTestCase {

    /// 資料流:`effectiveTheme` 必須真的把設定蓋到主題上。
    func testEffectiveThemeAppliesTheAppearanceOverrides() throws {
        let src = try Repo.appSource("AppContext.swift")
        let body = try XCTUnwrap(src.body(of: "var effectiveTheme:"),
                                 "AppContext 沒有 effectiveTheme 了")
        let args = SwiftSource.arguments(ofCall: "AppearanceOverrides.apply", in: body)
        XCTAssertFalse(args.isEmpty,
                       "effectiveTheme 沒有呼叫 AppearanceOverrides.apply —— 那顆開關又是死鍵")
        XCTAssertTrue(SwiftSource.mentions("themes.current", in: args["_0"] ?? ""),
                      "蓋的對象要是目前的主題")
        XCTAssertTrue(SwiftSource.mentions("settings.current", in: args["settings"] ?? ""),
                      "蓋上去的要是目前的設定,不是預設值")
    }

    /// 呼叫位置:候選窗畫的必須是 `effectiveTheme`,不是 `themes.current`。
    /// 兩者都「有那個字」,差別只在 `panel.show(theme:)` 收到哪一個。
    func testThePanelIsShownWithTheEffectiveTheme() throws {
        let src = try Repo.appSource("LuminaKeyInputController.swift")
        let body = try XCTUnwrap(src.body(of: "func updatePanel("))
        let theme = try XCTUnwrap(SwiftSource.arguments(ofCall: "panel.show", in: body)["theme"],
                                  "updatePanel 沒有呼叫 panel.show(theme:)")
        XCTAssertTrue(SwiftSource.mentions("effectiveTheme", in: theme),
                      "候選窗畫的是 \(theme) —— 設定裡那顆開關又不會有任何事發生")
    }
}

/// M-3:「啟用了哪些方案」要問得到,而且要真的用來挑方案。
final class SchemaListWiringTests: XCTestCase {

    private func applyBody() throws -> String {
        let src = try Repo.appSource("AppContext.swift")
        return try XCTUnwrap(src.body(of: "func applySchemaForInputMode("))
    }

    /// 資料流:fallback 讀的是**隨附的** default.yaml,所以 `sharedDir`
    /// 不可以是 nil —— 使用者目錄裡通常沒有那個檔案。
    func testItAsksForBothDirectories() throws {
        let body = try applyBody()
        let args = SwiftSource.arguments(ofCall: "SchemaListReader.resolve", in: body)
        XCTAssertFalse(args.isEmpty,
                       "applySchemaForInputMode 沒有問 SchemaListReader —— 全新安裝挑不出方案")
        XCTAssertTrue(SwiftSource.mentions("userDataDir", in: args["userDir"] ?? ""))
        XCTAssertTrue(SwiftSource.mentions("sharedDataDir", in: args["sharedDir"] ?? ""),
                      "sharedDir 是 \(args["sharedDir"] ?? "缺") —— fallback 永遠找不到 default.yaml")
    }

    /// 呼叫位置 + 資料流:問到的清單要真的流進 `InputModeBinding.resolve(enabled:)`,
    /// 不是問完就丟。
    func testTheListReachesTheSchemaChooser() throws {
        let body = try applyBody()
        let derived = SwiftSource.valuesDerived(fromCall: "SchemaListReader.resolve", in: body)
        XCTAssertFalse(derived.isEmpty, "SchemaListReader 的結果沒有被繫結到任何名字")
        let arg = try XCTUnwrap(
            SwiftSource.arguments(ofCall: "InputModeBinding.resolve", in: body)["enabled"],
            "沒有把清單交給 InputModeBinding.resolve(enabled:)")
        XCTAssertTrue(SwiftSource.anyOf(derived, reaches: arg),
                      "InputModeBinding.resolve(enabled: \(arg)) 收到的不是 SchemaListReader 算的")
    }
}

/// M-4:滾輪 → 翻頁,兩段接線。
final class PagingWiringTests: XCTestCase {

    /// 候選窗收得到滾輪,而且把 `ScrollPager` 的結論真的送出去。
    func testScrollWheelFeedsThePagerAndPublishesTheResult() throws {
        let src = try Repo.appSource("CandidateView.swift")
        let body = try XCTUnwrap(src.body(of: "override func scrollWheel("),
                                 "候選窗不收滾輪了 —— 翻頁在畫面上又沒有路")
        XCTAssertTrue(SwiftSource.calls("pager.feed", in: body),
                      "沒有問 ScrollPager,那組單元測試就等於沒有在保護任何東西")
        let derived = SwiftSource.valuesDerived(fromCall: "pager.feed", in: body)
        let arg = try XCTUnwrap(SwiftSource.arguments(ofCall: "onChangePage", in: body)["_0"],
                                "算出了要翻哪一頁,卻沒有送出去")
        XCTAssertTrue(SwiftSource.anyOf(derived, reaches: arg),
                      "送出去的 \(arg) 不是 pager 算出來的")
    }

    /// 送出來的事件有人接,而且接到引擎上。
    func testTheControllerWiresThePanelCallback() throws {
        let src = try Repo.appSource("LuminaKeyInputController.swift")
        let activate = try XCTUnwrap(src.body(of: "func activateServer("))
        let closure = try XCTUnwrap(
            SwiftSource.assignedExpression(to: "panel.onChangePage", in: activate),
            "activateServer 沒有接 panel.onChangePage —— 滾輪送出來的事件沒有人收")
        XCTAssertTrue(SwiftSource.mentions("changePage", in: closure),
                      "接了,但接到的不是 changePage")
    }

    /// 而 `changePage` 要真的叫引擎翻頁並把結果送去畫。
    func testChangePageAsksTheEngineAndRedraws() throws {
        let src = try Repo.appSource("LuminaKeyInputController.swift")
        let body = try XCTUnwrap(src.body(of: "func changePage("))
        XCTAssertTrue(SwiftSource.calls("engine.changePage", in: body),
                      "rs_change_page() 又變回從來沒有被呼叫過")
        XCTAssertTrue(SwiftSource.calls("deliver", in: body),
                      "翻了頁卻不重畫,畫面上看不出翻過")
    }
}
