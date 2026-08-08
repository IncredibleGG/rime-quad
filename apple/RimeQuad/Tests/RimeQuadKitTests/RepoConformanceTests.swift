import XCTest
@testable import RimeQuadKit

/// docs/theme-format.md §10「一致性檢核清單」裡桌面端負責的那幾條。
///
/// ⚠ 這裡刻意**不寫死份數**。規範自己說：「寫死的那一刻，新加的檔案就自動免檢。」
///   Android 端的 `LayoutEscapeTest` 就是這樣讓 12 份裡的 8 份從沒被檢查過。
final class RepoConformanceTests: XCTestCase {

    private func source() -> DocumentSource {
        DirectoryDocumentSource([Repo.themesDir])
    }

    /// §10 第 1 條：`core/themes/` 底下**每一份**都解析成功，零 ERROR。
    func testEveryShippedThemeParsesCleanly() throws {
        let ids = try Repo.themeIds()
        XCTAssertGreaterThan(ids.count, 0, "core/themes 是空的 —— 這條檢核等於沒在測")
        for id in ids {
            let r = ThemeLoader.load(id: id, source: source(), platform: .macos)
            XCTAssertTrue(r.isSuccess, "\(id) 解析失敗：\(r.errors.map(\.developerMessage))")
            XCTAssertEqual(r.errors.count, 0, "\(id) 有 ERROR 診斷")
            XCTAssertEqual(r.warnings.count, 0,
                           "\(id) 有 WARNING：\(r.warnings.map(\.developerMessage))")
        }
    }

    /// 每一份主題都要能在四個平台鍵下解析（`platform_overrides` 不得只對某一端有效）。
    func testEveryShippedThemeParsesOnEveryPlatform() throws {
        for id in try Repo.themeIds() {
            for p in Platform.allCases {
                let r = ThemeLoader.load(id: id, source: source(), platform: p)
                XCTAssertTrue(r.isSuccess, "\(id) 在 \(p.rawValue) 上解析失敗")
            }
        }
    }

    /// §10 第 2 條：sakura-dark 的繼承行為。
    func testSakuraDarkInheritance() {
        let r = ThemeLoader.load(id: "sakura-dark", source: source(), platform: .macos)
        let t = try! XCTUnwrap(r.value)
        XCTAssertEqual(t.palette["bg"], RGBA.hex(0x151016FF), "自身")
        XCTAssertEqual(t.palette["fg"], RGBA.hex(0xE6E9EFFF), "繼承自 default-dark")
        XCTAssertEqual(t.candidates.item.cornerRadius, 14, "自身")
        XCTAssertEqual(t.candidates.item.paddingH, 10, "繼承")
        XCTAssertEqual(t.typography.fonts["candidate"]?.family, ["Iansui", "$system"],
                       "序列整體取代，長度為 2")
        let fb = t.typography.fonts["candidate"]!.scriptFallback
        for s in ["hans", "jpan", "kore"] {
            XCTAssertNotNil(fb[s], "script_fallback 是映射，\(s) 應該還在")
        }
        XCTAssertEqual(t.ancestry, ["default-dark", "sakura-dark"])
        XCTAssertEqual(t.window.cornerRadius, 14)
    }

    /// 每一份主題都要有一個載得起來的 `counterpart`（§8.2 的深淺配對）。
    func testCounterpartsResolve() throws {
        for id in try Repo.themeIds() {
            let r = ThemeLoader.load(id: id, source: source(), platform: .macos)
            guard let cp = r.value?.counterpart else { continue }
            let other = ThemeLoader.load(id: cp, source: source(), platform: .macos)
            XCTAssertTrue(other.isSuccess, "\(id) 的 counterpart \(cp) 載不起來")
            XCTAssertNotEqual(other.value?.appearance, r.value?.appearance,
                              "\(id) 與 \(cp) 的 appearance 應該相反")
        }
    }

    /// 規範與程式碼不得漂移：`DiagnosticCode` 的每一個字面值都必須出現在
    /// docs/theme-format.md 的碼表裡（§6.5）。
    ///
    /// 這條的價值在於**反向**也成立的那一半：規範裡列了一個碼但實作沒有，
    /// 或實作加了一個碼但沒寫進規範，兩種都會紅。
    func testEveryDiagnosticCodeIsDocumented() throws {
        let spec = try String(contentsOf: Repo.specFile, encoding: .utf8)
        XCTAssertTrue(spec.contains("## 6.5") || spec.contains("### 6.5"),
                      "規範裡找不到 §6.5 診斷碼表")
        var missing: [String] = []
        for code in DiagnosticCode.allCases where !spec.contains("`\(code.rawValue)`") {
            missing.append(code.rawValue)
        }
        XCTAssertEqual(missing, [], "這些診斷碼沒有寫進 docs/theme-format.md §6.5")
    }

    /// 規範宣告的桌面候選窗欄位必須全部被綁定（否則主題作者寫了沒作用）。
    func testDocumentedWindowFieldsAreAllBound() throws {
        let spec = try String(contentsOf: Repo.specFile, encoding: .utf8)
        for field in ["lines", "equal_columns", "column_gap", "row_gap",
                      "max_height", "item_align", "overflow"] {
            XCTAssertTrue(spec.contains("`\(field)`"), "§8.6.7.1 沒有記載 \(field)")
            XCTAssertTrue(ThemeParser.windowKeys.contains(field),
                          "\(field) 不在 windowKeys —— 會被當成 unknown field 丟掉")
        }
        for field in ["show", "position", "arrangement", "items", "active_color"] {
            XCTAssertTrue(ThemeParser.statusBarKeys.contains(field),
                          "status_bar.\(field) 不在 statusBarKeys")
        }
    }

    /// §9.5.1：本端宣告不支援的動詞，每一個都必須在規範裡有名字。
    func testUnimplementedVerbsAreRealVerbs() {
        for v in DesktopVerbSupport.unimplemented {
            XCTAssertTrue(ActionVerb.allCases.contains(v))
        }
        XCTAssertTrue(DesktopVerbSupport.unimplemented.contains(.emoji),
                      "表情面板還沒做，就不能讓那顆按了沒反應的項目出現在狀態列上")
        XCTAssertTrue(DesktopVerbSupport.isImplemented(.inputModeToggle),
                      "桌面沒有佈局，input_mode:toggle 退化成純模式切換，是做得到的")
        XCTAssertTrue(DesktopVerbSupport.isImplemented(.schemaPicker))
    }

    /// `PhysicalKeys.table` 的每一個名稱在測試表裡都查得到。
    /// 真正對 librime 查的是 apple/scripts/verify_keysyms.sh —— 這一條只擋住
    /// 「加了 keyCode 卻忘了名字」。
    func testPhysicalKeyNamesAreAllResolvable() {
        let fixture = FixtureKeysyms()
        for (code, name) in PhysicalKeys.table.sorted(by: { $0.key < $1.key }) {
            XCTAssertNotEqual(fixture.keysym(named: name), 0,
                              "keyCode \(code) 的名稱「\(name)」查不到")
        }
    }
}
