import XCTest
@testable import LuminaKeyKit

final class KeyMapperTests: XCTestCase {

    private let mapper = KeyMapper(resolver: FixtureKeysyms())

    private func stroke(_ keyCode: UInt16, _ chars: String,
                        _ flags: MacModifierFlags = [], up: Bool = false) -> KeyStroke? {
        mapper.stroke(for: MacKeyEvent(keyCode: keyCode,
                                       charactersIgnoringModifiers: chars,
                                       flags: flags, isKeyUp: up))
    }

    // ─────────── 佈局無關性：這一組是本檔存在的理由 ───────────

    /// keyCode 9 在 QWERTY 上是 `v`，在 Dvorak 上是 `k`。
    /// **同一個 keyCode 必須產生不同的 keysym** —— 若某天有人「順手」加了一張
    /// keyCode → 字母的常數表，這一條就會紅。
    func testSameKeyCodeDifferentLayoutsGiveDifferentKeysyms() {
        let qwerty = stroke(9, "v")
        let dvorak = stroke(9, "k")
        XCTAssertEqual(qwerty?.keysym, 0x76)      // 'v'
        XCTAssertEqual(dvorak?.keysym, 0x6B)      // 'k'
        XCTAssertNotEqual(qwerty?.keysym, dvorak?.keysym)
    }

    /// AZERTY：keyCode 12 是 `a` 而不是 `q`；keyCode 0 是 `q` 而不是 `a`。
    func testAzertyIsHandledWithoutAnySpecialCase() {
        XCTAssertEqual(stroke(12, "a")?.keysym, 0x61)
        XCTAssertEqual(stroke(0, "q")?.keysym, 0x71)
    }

    /// 反過來：**位置固定**的鍵不看字元。Dvorak 的 Return 還是 Return。
    func testPhysicalKeysIgnoreCharacters() {
        XCTAssertEqual(stroke(36, "")?.keysym, 0xFF0D)
        XCTAssertEqual(stroke(36, "x")?.keysym, 0xFF0D, "Return 不因為字元不同而改變")
        XCTAssertEqual(stroke(51, "")?.keysym, 0xFF08)   // BackSpace
        XCTAssertEqual(stroke(53, "")?.keysym, 0xFF1B)   // Escape
        XCTAssertEqual(stroke(49, "")?.keysym, 0x20)     // space
        XCTAssertEqual(stroke(123, "")?.keysym, 0xFF51)  // Left
    }

    /// 數字鍵台走位置：部分歐洲配置的小數點鍵送出的是逗號，
    /// 走字元會變成 `,`，走位置才會是 KP_Decimal。
    func testKeypadUsesPositionNotCharacter() {
        XCTAssertEqual(stroke(65, ",")?.keysym, 0xFFAE)
        XCTAssertEqual(stroke(82, "0")?.keysym, 0xFFB0)
        XCTAssertEqual(stroke(76, "\r")?.keysym, 0xFF8D)
    }

    // ─────────── 修飾鍵 ───────────

    func testModifierBitsAreRSModifierNotX11() {
        let s = stroke(0, "a", [.shift, .control, .option, .command, .capsLock])!
        let expected = RSModifier([.shift, .control, .alt, .super_, .caps]).rawValue
        XCTAssertEqual(s.modifiers, expected)
        // 若有人把 kSuperMask 的 1<<26 抄到前端來，這個值會是 67108879 之類的東西。
        XCTAssertLessThan(s.modifiers, 64, "rs_modifier 只有 6 個位元；X11 遮罩是門面層的事")
    }

    func testReleaseBit() {
        XCTAssertEqual(stroke(0, "a", [], up: true)!.modifiers, RSModifier.release.rawValue)
    }

    /// Shift 已經反映在 charactersIgnoringModifiers 上（`2` → `@`），
    /// 但 shift 位元仍必須送出 —— librime 兩者都要。
    func testShiftedPunctuationCarriesBothCharacterAndBit() {
        let s = stroke(19, "@", [.shift])!
        XCTAssertEqual(s.keysym, 0x40)
        XCTAssertEqual(s.modifiers, RSModifier.shift.rawValue)
    }

    func testUnicodeFallsBackToX11Convention() {
        // 沒有專屬 keysym 的字元：碼位 + 0x01000000。
        XCTAssertEqual(KeyMapper.keysym(forScalar: "你".unicodeScalars.first!),
                       Int32(0x0100_0000 + 0x4F60))
        // Latin-1 直接就是碼位。
        XCTAssertEqual(KeyMapper.keysym(forScalar: "é".unicodeScalars.first!), 0xE9)
    }

    func testUnmappableKeyReturnsNil() {
        XCTAssertNil(stroke(999, ""), "沒有字元又不在位置表裡 → 不得吞掉，交回宿主")
    }

    // ─────────── flagsChanged ───────────

    /// librime 的「輕點 Shift 切中英」偵測的是 **release**。
    /// 少了這個狀態機，那個所有 RIME 使用者都習慣的操作就不存在。
    func testModifierTrackerReportsPressThenRelease() {
        var t = ModifierTracker()
        let down = t.transition(keyCode: 56, flags: [.shift])
        XCTAssertEqual(down?.isKeyUp, false)
        let up = t.transition(keyCode: 56, flags: [])
        XCTAssertEqual(up?.isKeyUp, true)
        XCTAssertTrue(up!.flags.contains(.shift),
                      "放開時仍要帶著那顆鍵自己的位元，否則 librime 認不出是哪顆鍵放開")
    }

    func testModifierTrackerIgnoresNonModifierKeyCodes() {
        var t = ModifierTracker()
        XCTAssertNil(t.transition(keyCode: 0, flags: [.shift]))
    }

    func testModifierTrackerNoDuplicateEvents() {
        var t = ModifierTracker()
        _ = t.transition(keyCode: 56, flags: [.shift])
        XCTAssertNil(t.transition(keyCode: 56, flags: [.shift]), "狀態沒變就沒有事件")
    }

    /// ⚠ **這一條這一輪反過來了,而且它就是覆核者實測到的迴歸。**
    ///
    /// 舊版把每一次 Caps Lock 的 flagsChanged 都當成「按下」送進 librime,
    /// 而隨附 `default.yaml` 的 `ascii_composer/switch_key: { Caps_Lock: clear }`
    /// 會讓 `AsciiComposer` 對正在組字的 context 做 `ctx->Clear()` ——
    /// **組字打到一半按 Caps Lock,剛打的半句話就沒了。**
    /// 現在它一顆都不放行,理由與 librime 那一半的出處見 `ModifierGate`。
    func testCapsLockNeverProducesAnEvent() {
        var t = ModifierTracker()
        XCTAssertNil(t.transition(keyCode: 57, flags: [.capsLock]), "亮燈那一次")
        XCTAssertNil(t.transition(keyCode: 57, flags: []), "熄燈那一次")
    }

    func testEveryModifierKeyCodeHasAName() {
        for code in PhysicalKeys.modifierKeyFlag.keys {
            XCTAssertNotNil(PhysicalKeys.table[code], "keyCode \(code) 有旗標卻沒有 keysym 名稱")
        }
    }
}
