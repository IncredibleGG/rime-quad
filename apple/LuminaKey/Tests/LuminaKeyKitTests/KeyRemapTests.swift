import XCTest
@testable import LuminaKeyKit

/// 換鍵。
///
/// 這一組要證明的三件事，每一件都是「壞掉的時候畫面完全正常」的那一類：
///
///  1. **按下 a 送進引擎的真的是 s**（不是上屏之後才換 —— 那在組字時無事可做）。
///  2. **每一個字母都還打得出來**：換鍵是置換，不是任意映射。破了這一條，
///     方案的 `speller/alphabet` 會少一個字母，使用者只會發現「有些字選不到」。
///  3. **手機上調好的鍵位不會在電腦上消失**，而電腦也不會把手機的東西寫掉。
final class KeyRemapTests: XCTestCase {

    // ─────────────────────────── 小工具 ───────────────────────────

    private func doc(_ json: String) -> RemapDocument {
        RemapDocument.decode(Data(json.utf8))
    }

    /// 只有一則 `qwerty` 交換的檔案（兩層都寫，桌面端寫出來就是這個形狀）。
    private func swapped(_ a: String, _ b: String) -> RemapDocument {
        doc("""
        {"format_version": 1, "layouts": [
          {"id": "qwerty", "ops": [
            {"op": "swap", "layer": "lower", "a": "\(a)", "b": "\(b)"},
            {"op": "swap", "layer": "upper", "a": "\(a)", "b": "\(b)"}
          ]}
        ]}
        """)
    }

    private func stroke(_ c: Character, _ mods: RSModifier = []) -> KeyStroke {
        KeyStroke(keysym: Int32(c.unicodeScalars.first!.value), modifiers: mods)
    }

    // ─────────────────────── 1. 換的是送進引擎的碼 ───────────────────────

    /// 使用者的原話:「按下 a 實際上是 b」。
    func testPressingAProducesSGoingIntoTheEngine() {
        let t = DesktopRemap.compile(swapped("a", "s")).table
        XCTAssertEqual(t.apply(to: stroke("a")).keysym, Int32(UInt8(ascii: "s")))
        XCTAssertEqual(t.apply(to: stroke("s")).keysym, Int32(UInt8(ascii: "a")))
    }

    /// 沒被換到的鍵一個位元都不能動 —— 包括修飾鍵狀態。
    func testUntouchedKeysPassThroughUnchanged() {
        let t = DesktopRemap.compile(swapped("a", "s")).table
        let before = stroke("q", [.shift, .alt])
        XCTAssertEqual(t.apply(to: before), before)
    }

    /// Shift+a 要得到 S(大寫的 s)。大小寫是**兩個不同的 keysym**,
    /// 所以這件事不需要任何「先還原成小寫再查表」的特例。
    func testShiftFollowsTheKey() {
        let t = DesktopRemap.compile(swapped("a", "s")).table
        let out = t.apply(to: stroke("A", [.shift]))
        XCTAssertEqual(out.keysym, Int32(UInt8(ascii: "S")))
        XCTAssertEqual(out.modifiers, RSModifier.shift.rawValue, "修飾鍵不該被動到")
    }

    /// Caps Lock 亮著的時候 macOS 給的也是大寫的 keysym,所以同一張表就對了。
    /// **這一條是「不必寫特例」的證據** —— 少了它,日後有人「順手」加一段
    /// Caps Lock 的判斷時,沒有東西會攔住他。
    func testCapsLockNeedsNoSpecialCase() {
        let t = DesktopRemap.compile(swapped("a", "s")).table
        XCTAssertEqual(t.apply(to: stroke("A", [.caps])).keysym, Int32(UInt8(ascii: "S")))
    }

    /// ⚠ **這一條擋的是不可逆的損失。** 把 a 換成 s 的使用者按 ⌘A 想的是全選,
    /// 換過去就變成儲存 —— 而他絕對不會把那件事聯想到鍵盤設定。
    func testCommandAndControlAreNeverRemapped() {
        let t = DesktopRemap.compile(swapped("a", "s")).table
        XCTAssertEqual(t.apply(to: stroke("a", [.super_])).keysym, Int32(UInt8(ascii: "a")))
        XCTAssertEqual(t.apply(to: stroke("a", [.control])).keysym, Int32(UInt8(ascii: "a")))
        XCTAssertEqual(t.apply(to: stroke("A", [.super_, .shift])).keysym,
                       Int32(UInt8(ascii: "A")))
    }

    /// Option 與 Shift 是打字的一部分,照換。放行的邊界只有 ⌘ 與 ⌃。
    func testOptionAndShiftAreStillRemapped() {
        let t = DesktopRemap.compile(swapped("a", "s")).table
        XCTAssertEqual(t.apply(to: stroke("a", [.alt])).keysym, Int32(UInt8(ascii: "s")))
    }

    /// 放開鍵(release)也要換,否則引擎收到的是「按下 s、放開 a」。
    func testKeyUpIsRemappedToo() {
        let t = DesktopRemap.compile(swapped("a", "s")).table
        XCTAssertEqual(t.apply(to: stroke("a", [.release])).keysym, Int32(UInt8(ascii: "s")))
    }

    // ─────────────────── 2. 每一個字母都還打得出來 ───────────────────

    /// 多對一會讓某個字母從此打不出來。**建構子必須拒絕。**
    func testTableRefusesManyToOne() {
        let a = Int32(UInt8(ascii: "a")), b = Int32(UInt8(ascii: "b"))
        let s = Int32(UInt8(ascii: "s"))
        XCTAssertNil(KeyRemapTable(checking: [a: s, b: s]), "兩顆鍵送同一個字母應該被拒絕")
        XCTAssertNil(KeyRemapTable(checking: [a: s]), "s 被搬走卻沒有人補位,應該被拒絕")
        XCTAssertNotNil(KeyRemapTable(checking: [a: s, s: a]))
    }

    /// 編出來的表一定是置換:值域與定義域必須是同一批 keysym。
    func testCompiledTableIsAlwaysAPermutation() {
        let d = doc("""
        {"layouts": [{"id": "qwerty", "ops": [
          {"op": "swap", "layer": "lower", "a": "a", "b": "s"},
          {"op": "swap", "layer": "lower", "a": "a", "b": "d"},
          {"op": "swap", "layer": "upper", "a": "q", "b": "p"}
        ]}]}
        """)
        let map = DesktopRemap.compile(d).table.map
        XCTAssertFalse(map.isEmpty)
        XCTAssertEqual(Set(map.keys), Set(map.values))
    }

    // ─────────────── 3. 與 Android 的行為必須一模一樣 ───────────────

    /// Android 的 `swap` 是「找出這兩顆鍵**現在**在哪兩個格子再對調」,
    /// 所以連續兩則操作會組成一個環,不是兩組獨立的對調。
    /// 桌面端算錯的話,同一份檔案在手機與電腦上的行為會不一樣,而且沒有跡象。
    func testSequentialSwapsComposeLikeAndroid() {
        let d = doc("""
        {"layouts": [{"id": "qwerty", "ops": [
          {"op": "swap", "layer": "lower", "a": "a", "b": "s"},
          {"op": "swap", "layer": "lower", "a": "a", "b": "d"}
        ]}]}
        """)
        let t = DesktopRemap.compile(d).table
        // 按 a → s、按 s → d、按 d → a
        XCTAssertEqual(t.apply(to: stroke("a")).keysym, Int32(UInt8(ascii: "s")))
        XCTAssertEqual(t.apply(to: stroke("s")).keysym, Int32(UInt8(ascii: "d")))
        XCTAssertEqual(t.apply(to: stroke("d")).keysym, Int32(UInt8(ascii: "a")))
    }

    /// Android 端寫出來的檔案(一層一則,沒有 upper),桌面端必須讀得懂,
    /// 而且要**照實**呈現:沒按 Shift 會換,按著 Shift 不會。
    func testPhoneWrittenSingleLayerIsHonouredAndShownHonestly() {
        let d = doc("""
        {"format_version": 1, "layouts": [
          {"id": "qwerty", "ops": [{"op": "swap", "layer": "lower", "a": "a", "b": "s"}]}
        ]}
        """)
        let c = DesktopRemap.compile(d)
        XCTAssertEqual(c.table.apply(to: stroke("a")).keysym, Int32(UInt8(ascii: "s")))
        XCTAssertEqual(c.table.apply(to: stroke("A", [.shift])).keysym,
                       Int32(UInt8(ascii: "A")), "上層沒有這一則,大寫就不該換")
        XCTAssertEqual(c.cycles.count, 1)
        XCTAssertTrue(c.cycles[0].appliesUnshifted)
        XCTAssertFalse(c.cycles[0].appliesShifted)
    }

    /// 兩層換的是不同的鍵時,畫面上要是兩列,不是把上層硬塞進下層那一列。
    /// 塞進去的話畫面會寫著 a ⇄ s 而按著 Shift 其實是 a ⇄ d —— 一句精確的謊話。
    func testTwoLayersThatDisagreeAreShownAsTwoRows() {
        let d = doc("""
        {"layouts": [{"id": "qwerty", "ops": [
          {"op": "swap", "layer": "lower", "a": "a", "b": "s"},
          {"op": "swap", "layer": "upper", "a": "a", "b": "d"}
        ]}]}
        """)
        let c = DesktopRemap.compile(d)
        XCTAssertEqual(c.cycles.count, 2)
        XCTAssertEqual(c.cycles.filter { $0.appliesUnshifted && !$0.appliesShifted }.count, 1)
        XCTAssertEqual(c.cycles.filter { !$0.appliesUnshifted && $0.appliesShifted }.count, 1)
    }

    /// 兩層一樣時併成一列,而且兩個旗標都是 true。
    func testMatchingLayersCollapseToOneRow() {
        let c = DesktopRemap.compile(swapped("a", "s"))
        XCTAssertEqual(c.cycles.count, 1)
        XCTAssertEqual(c.cycles[0].letters, ["a", "s"])
        XCTAssertTrue(c.cycles[0].appliesUnshifted)
        XCTAssertTrue(c.cycles[0].appliesShifted)
    }

    // ─────────────── 4. 不得把別人的資料弄丟 ───────────────

    /// 手機上的九宮格鍵位、以及未來版本寫的操作,**原封不動**留在檔案裡。
    /// 這一條紅掉的症狀是:使用者在電腦上按一下「對調」,手機上的設定就沒了。
    func testUnknownLayoutsAndOpsSurviveARoundTrip() {
        let d = doc("""
        {"format_version": 1, "future_key": {"anything": 1}, "layouts": [
          {"id": "cn-t9-pinyin", "ops": [{"op": "swap", "layer": "t9", "a": "k1", "b": "k2"}]},
          {"id": "qwerty", "ops": [{"op": "swap", "layer": "lower", "a": "a", "b": "s"}]}
        ]}
        """)
        guard case .success(let next) = DesktopRemap.swapping("q", "w", in: d) else {
            return XCTFail("這份檔案桌面端讀得懂,應該可以再換一組")
        }
        let text = String(decoding: next.encode(), as: UTF8.self)
        XCTAssertTrue(text.contains("cn-t9-pinyin"), "手機的佈局被寫掉了")
        XCTAssertTrue(text.contains("\"k1\""), "手機的鍵位被寫掉了")
        XCTAssertTrue(text.contains("future_key"), "不認得的頂層欄位被寫掉了")
        XCTAssertTrue(next.layoutIds.contains("cn-t9-pinyin"))
    }

    /// 「全部還原」只清這台電腦顯示得出來的那一份。
    /// 使用者按的是「把我看到的這些還原」,不是「把我看不到的東西也刪掉」。
    func testClearingOnlyTouchesWhatThisComputerShows() {
        let d = doc("""
        {"layouts": [
          {"id": "cn-t9-pinyin", "ops": [{"op": "swap", "layer": "t9", "a": "k1", "b": "k2"}]},
          {"id": "qwerty", "ops": [{"op": "swap", "layer": "lower", "a": "a", "b": "s"}]}
        ]}
        """)
        let after = DesktopRemap.clearing(in: d)
        XCTAssertFalse(after.layoutIds.contains("qwerty"))
        XCTAssertTrue(after.layoutIds.contains("cn-t9-pinyin"), "手機的九宮格不該被一起刪掉")
        XCTAssertTrue(DesktopRemap.compile(after).table.isEmpty)
    }

    /// 看不懂的操作 → **唯讀**,而不是「當作沒看到然後覆蓋掉」。
    func testForeignOpsMakeItReadOnlyInsteadOfBeingDropped() {
        let d = doc("""
        {"layouts": [{"id": "qwerty", "ops": [
          {"op": "rotate_row", "layer": "lower", "row": 1}
        ]}]}
        """)
        let c = DesktopRemap.compile(d)
        XCTAssertFalse(c.editable)
        XCTAssertTrue(c.table.isEmpty, "看不懂就一律不套用")
        XCTAssertEqual(c.notices.first?.code, .cannotShowHere)
        if case .success = DesktopRemap.swapping("a", "s", in: d) {
            XCTFail("唯讀狀態下不該寫得進去")
        }
    }

    /// `move` 需要知道那一列的順序,而桌面端沒有佈局檔。**不准猜。**
    func testMoveIsNotGuessedAt() {
        let d = doc("""
        {"layouts": [{"id": "qwerty", "ops": [
          {"op": "move", "layer": "lower", "key": "a", "before": "s"}
        ]}]}
        """)
        let c = DesktopRemap.compile(d)
        XCTAssertTrue(c.table.isEmpty)
        XCTAssertFalse(c.editable)
    }

    /// 電腦鍵盤上沒有的鍵(九宮格的 k1、符號鍵…)→ 整份不套用,並說原因。
    func testKeysNotOnThisKeyboardStopTheWholeLayout() {
        let d = doc("""
        {"layouts": [{"id": "qwerty", "ops": [
          {"op": "swap", "layer": "lower", "a": "a", "b": "s"},
          {"op": "swap", "layer": "lower", "a": "comma", "b": "enter"}
        ]}]}
        """)
        let c = DesktopRemap.compile(d)
        XCTAssertTrue(c.table.isEmpty, "全有或全無 —— 一半生效比不生效更糟")
        XCTAssertEqual(c.notices.first?.code, .keyNotOnThisKeyboard)
        XCTAssertEqual(c.notices.first?.args.first, "comma")
    }

    /// 桌面端不消費的層 → 同樣是整份不套用。
    func testUnknownLayerStopsTheWholeLayout() {
        let d = doc("""
        {"layouts": [{"id": "qwerty", "ops": [
          {"op": "swap", "layer": "symbols", "a": "a", "b": "s"}
        ]}]}
        """)
        XCTAssertEqual(DesktopRemap.compile(d).notices.first?.code, .layerNotOnThisKeyboard)
    }

    /// 壞掉的檔案不該讓輸入法打不出字 —— 回空白,不丟例外。
    func testBrokenFileDegradesToNoRemapping() {
        let c = DesktopRemap.compile(doc("{ this is not json"))
        XCTAssertTrue(c.table.isEmpty)
        XCTAssertTrue(c.notices.isEmpty)
        XCTAssertTrue(c.editable)
    }

    // ─────────────── 5. 寫回去的東西 Android 讀得懂 ───────────────

    /// 桌面端按一次「對調」要同時寫兩層,而且欄位名必須是 Android 認得的那幾個。
    func testWhatWeWriteIsWhatAndroidReads() {
        guard case .success(let next) = DesktopRemap.swapping("a", "s", in: RemapDocument()) else {
            return XCTFail("空白檔案上換一組應該會成功")
        }
        let entries = next.entries(layoutId: "qwerty")
        XCTAssertEqual(entries.count, 2)
        XCTAssertTrue(entries.contains(.known(.swap(layer: "lower", a: "a", b: "s"))))
        XCTAssertTrue(entries.contains(.known(.swap(layer: "upper", a: "a", b: "s"))))

        let text = String(decoding: next.encode(), as: UTF8.self)
        for key in ["\"op\"", "\"layer\"", "\"a\"", "\"b\"", "\"layouts\"", "\"format_version\""] {
            XCTAssertTrue(text.contains(key), "少了 Android 端解碼要用的 \(key)")
        }
    }

    /// 拆解出來的操作**重放一次必須得到同一個置換**。
    /// 這是「存操作而不是存結果」這個決定唯一的安全網 —— 拆錯了的症狀是
    /// 使用者換第三組的時候,前兩組莫名其妙變了。
    func testDecompositionReplaysToTheSamePermutation() {
        var d = RemapDocument()
        let moves: [(Character, Character)] = [("a", "s"), ("q", "p"), ("a", "d"), ("z", "q")]
        var expected: [Character: Character] = [:]
        for (p1, p2) in moves {
            guard case .success(let next) = DesktopRemap.swapping(p1, p2, in: d) else {
                return XCTFail("\(p1)/\(p2) 應該換得動")
            }
            d = next
            let k1 = expected[p1] ?? p1
            let k2 = expected[p2] ?? p2
            expected[p1] = k2
            expected[p2] = k1
        }
        let map = DesktopRemap.compile(d).table.map
        for (pos, key) in expected where pos != key {
            XCTAssertEqual(map[Int32(pos.unicodeScalars.first!.value)],
                           Int32(key.unicodeScalars.first!.value),
                           "位置 \(pos) 應該送出 \(key)")
        }
    }

    /// 還原一列不動其他列。
    func testRestoringOneRowLeavesTheOthers() {
        var d = RemapDocument()
        guard case .success(let d1) = DesktopRemap.swapping("a", "s", in: d),
              case .success(let d2) = DesktopRemap.swapping("q", "p", in: d1) else {
            return XCTFail("兩組都該換得動")
        }
        d = d2
        let cycles = DesktopRemap.compile(d).cycles
        XCTAssertEqual(cycles.count, 2)
        guard let target = cycles.first(where: { $0.letters.contains("a") }) else {
            return XCTFail("找不到 a 那一列")
        }
        let after = DesktopRemap.compile(DesktopRemap.restoring(target, in: d))
        XCTAssertEqual(after.cycles.count, 1)
        XCTAssertEqual(after.table.apply(to: stroke("a")).keysym, Int32(UInt8(ascii: "a")))
        XCTAssertEqual(after.table.apply(to: stroke("q")).keysym, Int32(UInt8(ascii: "p")))
    }

    /// 還原一個三環要**整環一起**回去,不能只拉回一顆 ——
    /// 只拉一顆會留下「d 沒有人給、s 沒有人收」的半途狀態,而那不是置換。
    func testRestoringATripleCycleRestoresAllThreeKeys() {
        let d = doc("""
        {"layouts": [{"id": "qwerty", "ops": [
          {"op": "swap", "layer": "lower", "a": "a", "b": "s"},
          {"op": "swap", "layer": "lower", "a": "a", "b": "d"}
        ]}]}
        """)
        let c = DesktopRemap.compile(d)
        XCTAssertEqual(c.cycles.count, 1)
        XCTAssertEqual(c.cycles[0].letters.count, 3)
        let restored = DesktopRemap.restoring(c.cycles[0], in: d)
        let after = DesktopRemap.compile(restored)
        XCTAssertTrue(after.table.isEmpty)
        XCTAssertTrue(after.cycles.isEmpty)
        // ⚠ 上面兩條**不足以**抓到「只還原一顆」:剩下的半途狀態不是置換,
        //   編譯時會退回原樣,於是表與列都是空的,看起來跟還原成功一模一樣。
        //   真正的判準是「沒有任何一句話要對使用者說」。
        XCTAssertTrue(after.notices.isEmpty, "還原之後不該還有話要說:\(after.notices)")
        XCTAssertTrue(after.editable, "還原之後應該可以繼續換")
        XCTAssertTrue(restored.entries(layoutId: "qwerty").isEmpty, "操作應該被清乾淨")
    }

    /// 同一顆鍵不能跟自己對調,而且要說得出為什麼。
    func testSelfSwapIsRefusedWithAReason() {
        guard case .failure(let n) = DesktopRemap.swapping("a", "a", in: RemapDocument()) else {
            return XCTFail("應該被擋下來")
        }
        XCTAssertEqual(n.code, .sameKey)
    }

    /// 界線之外的鍵(數字、標點)不接受 —— 而且是明著拒絕,不是靜靜不做事。
    func testNonLetterKeysAreRefused() {
        guard case .failure(let n) = DesktopRemap.swapping("1", "2", in: RemapDocument()) else {
            return XCTFail("應該被擋下來")
        }
        XCTAssertEqual(n.code, .keyNotOnThisKeyboard)
        XCTAssertEqual(DesktopRemap.letters.count, 26)
    }

    // ─────────────── 6. 每一種狀況都有一句白話 ───────────────

    /// `docs/ui-design.md` §7.7:**沒有白話對照的問題碼不准上畫面。**
    /// 新增一個碼而忘了寫白話,這一條會紅。
    func testEveryNoticeCodeHasPlainLanguageInAllThreeLanguages() {
        XCTAssertFalse(RemapNoticeCode.allCases.isEmpty)
        for code in RemapNoticeCode.allCases {
            for lang in [UiLanguage.zhHant, .zhHans, .en] {
                let title = RemapCopy.title(code)[lang].trimmingCharacters(in: .whitespaces)
                let body = RemapCopy.body(code)[lang].trimmingCharacters(in: .whitespaces)
                XCTAssertFalse(title.isEmpty, "\(code.rawValue) 的白話結論在 \(lang.rawValue) 是空的")
                XCTAssertFalse(body.isEmpty, "\(code.rawValue) 的白話說明在 \(lang.rawValue) 是空的")
            }
        }
    }

    /// 白話裡不准出現實作的詞。與 SettingsCatalogTests 同一條規則,
    /// 只是那一條掃不到這裡 —— 這些字串不在目錄裡。
    func testNoticesDoNotLeakImplementationWords() {
        // ⚠ 刻意**不**禁 "swap":那是英文裡的白話動詞,正是 §6.2 詞彙表要求的
        //   講法(remap → 換鍵,動作說成「把兩顆鍵對調」)。禁掉它會逼出一句
        //   更彆扭的英文,那與這條檢查要達成的目的相反。
        let banned = ["keysym", "layout", "layer", "remap", "librime", "JSON", "yaml",
                      "qwerty", "bijection", "置換", "佈局", "映射"]
        for code in RemapNoticeCode.allCases {
            for lang in [UiLanguage.zhHant, .zhHans, .en] {
                let text = RemapCopy.title(code)[lang] + " " + RemapCopy.body(code)[lang]
                for word in banned {
                    XCTAssertFalse(text.contains(word),
                                   "\(code.rawValue) 在 \(lang.rawValue) 出現了實作的詞:\(word)")
                }
            }
        }
    }

    /// 帶參數的說明要真的把參數填進去,不能留著 `{0}`。
    func testNoticeArgumentsAreSubstituted() {
        let n = RemapNotice(.keyNotOnThisKeyboard, ["comma"])
        for lang in [UiLanguage.zhHant, .zhHans, .en] {
            XCTAssertTrue(n.bodyText(lang).contains("comma"))
            XCTAssertFalse(n.bodyText(lang).contains("{0}"))
        }
    }

    // ─────────────── 7. 存到哪裡、讀不讀得到 ───────────────

    private func tempDir() throws -> URL {
        let u = FileManager.default.temporaryDirectory
            .appendingPathComponent("remap-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: u, withIntermediateDirectories: true)
        return u
    }

    /// 檔名必須與 Android 一字不差,否則兩端各存各的,而且沒有錯誤訊息。
    func testFileNameMatchesAndroid() {
        XCTAssertEqual(KeyRemapStore.fileName, "luminakey-layouts.json")
        XCTAssertEqual(KeyRemapStore.legacyFileName, "rimequad-layouts.json")
    }

    /// 改名前的舊檔讀得到 —— 漏掉這一條的下場是升級之後鍵位全部回到原樣,
    /// 檔案還在磁碟上,只是沒有人再去讀它。
    func testLegacyFileIsPickedUp() throws {
        let dir = try tempDir()
        let legacy = dir.appendingPathComponent(KeyRemapStore.legacyFileName)
        try swapped("a", "s").encode().write(to: legacy)
        let store = KeyRemapStore(userDir: dir)
        XCTAssertEqual(store.compiled.table.apply(to: stroke("a")).keysym,
                       Int32(UInt8(ascii: "s")))
    }

    /// 寫成功之後舊檔要收掉:兩份同時在的話,之後每次讀都得決定信哪一份。
    func testLegacyFileIsRetiredAfterTheFirstWrite() throws {
        let dir = try tempDir()
        let legacy = dir.appendingPathComponent(KeyRemapStore.legacyFileName)
        try swapped("a", "s").encode().write(to: legacy)
        let store = KeyRemapStore(userDir: dir)
        guard case .success(let next) = DesktopRemap.swapping("q", "p", in: store.document) else {
            return XCTFail("應該換得動")
        }
        XCTAssertTrue(store.save(next))
        XCTAssertFalse(FileManager.default.fileExists(atPath: legacy.path))
        XCTAssertTrue(FileManager.default.fileExists(
            atPath: dir.appendingPathComponent(KeyRemapStore.fileName).path))
    }

    /// 存檔之後重新開一個 store 讀得回來(這是「重開之後設定還在」那條)。
    func testRoundTripThroughDisk() throws {
        let dir = try tempDir()
        let store = KeyRemapStore(userDir: dir)
        guard case .success(let next) = DesktopRemap.swapping("a", "s", in: store.document) else {
            return XCTFail("應該換得動")
        }
        XCTAssertTrue(store.save(next))
        let reopened = KeyRemapStore(userDir: dir)
        XCTAssertEqual(reopened.compiled.table.apply(to: stroke("a")).keysym,
                       Int32(UInt8(ascii: "s")))
        XCTAssertTrue(reopened.hasAnythingToRestore)
    }

    /// 沒有檔案時一切照舊 —— 這是絕大多數使用者的狀態,不該有任何例外路徑。
    func testNoFileMeansNoRemapping() throws {
        let store = KeyRemapStore(userDir: try tempDir())
        XCTAssertTrue(store.compiled.table.isEmpty)
        XCTAssertFalse(store.hasAnythingToRestore)
        XCTAssertEqual(store.compiled.table.apply(to: stroke("a")).keysym,
                       Int32(UInt8(ascii: "a")))
    }
}
