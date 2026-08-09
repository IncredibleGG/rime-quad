//
//  KeyMapper.swift — NSEvent → X11 keysym（core/include/rime_shell.h「按鍵輸入」一節）
//
//  ── 這一檔要解決的問題 ─────────────────────────────────────────────────
//  librime 吃的是 X11 keysym，不是字元也不是平台鍵碼。而**映射受使用者的實體
//  鍵盤佈局影響**：Dvorak 使用者按下 QWERTY 的 `V` 鍵位（keyCode 9），系統送出
//  的字是 `k`；法文 AZERTY 的 keyCode 12 是 `a` 不是 `q`。
//
//  所以這裡**沒有一張 keyCode → 字母的常數表**，而且刻意做不出來：
//
//    · **可列印的鍵**一律走 `charactersIgnoringModifiers`。那是 macOS 依
//      **當前啟用的鍵盤配置**（TIS input source）算出來的字，Dvorak、AZERTY、
//      德文 QWERTZ 全部自動正確，我們一行佈局特例都不必寫。
//    · **只有功能位置固定的鍵**才查 keyCode 表：Return / Tab / Escape / 方向鍵 /
//      F1–F20 / 數字鍵台 / 修飾鍵。這些鍵的**位置語義**與佈局無關 ——
//      Dvorak 的 Return 還是 Return。
//
//  判準很簡單：**如果換一套鍵盤配置這顆鍵會產生不同的字，它就不可以進表。**
//  `PhysicalKeys.table` 的每一項都通過這個判準。
//
//  ── 為什麼不自己寫死 keysym 數值 ────────────────────────────────────────
//  表裡放的是**名稱**（"BackSpace"、"KP_Enter"），值由門面層的
//  `rs_keysym_by_name()` 查 librime 自己那一份權威表。手抄一份數值表會腐爛，
//  而且抄錯了看起來完全正常（keysym 是個數字，沒有型別能擋）。
//  `apple/scripts/verify_keysyms.sh` 會拿這張表的每一個名稱去問 librime，
//  有任何一個查不到就讓 CI 紅。
//
//  ⚠ 注意 librime 查不到時回傳 XK_VoidSymbol(0xffffff)，那是個看起來很像有效
//    keysym 的值；門面層已正規化成 **0**（見 rime_shell.h）。
//

import Foundation

/// rs_modifier（core/include/rime_shell.h）。
///
/// ⚠ 這**不是** librime 的 X11 遮罩。門面層 `to_rime_mask()` 負責換算，
///   那邊 `kSuperMask` 是 `1 << 26` 而不是 `1 << 6`。前端絕對不要自己換。
public struct RSModifier: OptionSet, Sendable {
    public let rawValue: UInt32
    public init(rawValue: UInt32) { self.rawValue = rawValue }

    public static let shift   = RSModifier(rawValue: 1 << 0)
    public static let control = RSModifier(rawValue: 1 << 1)
    public static let alt     = RSModifier(rawValue: 1 << 2)
    public static let super_  = RSModifier(rawValue: 1 << 3)
    public static let caps    = RSModifier(rawValue: 1 << 4)
    public static let release = RSModifier(rawValue: 1 << 5)
}

/// `NSEvent.modifierFlags` 裡我們在意的位元，抄成純資料以便單元測試。
/// 值與 `NSEvent.ModifierFlags` 的 rawValue 相同。
public struct MacModifierFlags: OptionSet, Sendable {
    public let rawValue: UInt
    public init(rawValue: UInt) { self.rawValue = rawValue }

    public static let capsLock = MacModifierFlags(rawValue: 1 << 16)
    public static let shift    = MacModifierFlags(rawValue: 1 << 17)
    public static let control  = MacModifierFlags(rawValue: 1 << 18)
    public static let option   = MacModifierFlags(rawValue: 1 << 19)
    public static let command  = MacModifierFlags(rawValue: 1 << 20)
    public static let function = MacModifierFlags(rawValue: 1 << 23)
}

/// 一次按鍵事件，抽成純資料 —— AppKit 的 NSEvent 建不出來也測不動。
public struct MacKeyEvent: Sendable {
    public var keyCode: UInt16
    public var charactersIgnoringModifiers: String
    public var flags: MacModifierFlags
    public var isKeyUp: Bool

    public init(keyCode: UInt16, charactersIgnoringModifiers: String,
                flags: MacModifierFlags = [], isKeyUp: Bool = false) {
        self.keyCode = keyCode
        self.charactersIgnoringModifiers = charactersIgnoringModifiers
        self.flags = flags
        self.isKeyUp = isKeyUp
    }
}

public struct KeyStroke: Equatable, Sendable {
    public let keysym: Int32
    public let modifiers: UInt32

    public init(keysym: Int32, modifiers: RSModifier) {
        self.keysym = keysym
        self.modifiers = modifiers.rawValue
    }
}

/// 由名稱查 keysym。App 端接 `rs_keysym_by_name`；測試端接固定表。
public protocol KeysymResolver {
    func keysym(named: String) -> Int32
}

public enum PhysicalKeys {

    /// keyCode → X11 keysym 名稱。
    ///
    /// **收錄判準：換一套鍵盤配置之後這顆鍵仍然產生同一件事。**
    /// 字母、數字、標點一律不在此表（它們走 charactersIgnoringModifiers）。
    public static let table: [UInt16: String] = [
        36: "Return",
        48: "Tab",
        49: "space",
        51: "BackSpace",       // macOS 的 Delete 鍵＝向左刪除
        53: "Escape",
        114: "Insert",         // Help / Insert
        115: "Home",
        116: "Page_Up",
        117: "Delete",         // Forward Delete（fn+Delete）
        119: "End",
        121: "Page_Down",
        123: "Left",
        124: "Right",
        125: "Down",
        126: "Up",

        // 功能鍵
        122: "F1", 120: "F2", 99: "F3", 118: "F4", 96: "F5", 97: "F6",
        98: "F7", 100: "F8", 101: "F9", 109: "F10", 103: "F11", 111: "F12",
        105: "F13", 107: "F14", 113: "F15", 106: "F16", 64: "F17",
        79: "F18", 80: "F19", 90: "F20",

        // 數字鍵台。位置固定，但字元會隨區域設定變（小數點在部分歐洲配置是逗號），
        // 所以**必須**走位置而不是字元 —— 這是本表存在的第二個理由。
        82: "KP_0", 83: "KP_1", 84: "KP_2", 85: "KP_3", 86: "KP_4",
        87: "KP_5", 88: "KP_6", 89: "KP_7", 91: "KP_8", 92: "KP_9",
        65: "KP_Decimal", 67: "KP_Multiply", 69: "KP_Add", 75: "KP_Divide",
        76: "KP_Enter", 78: "KP_Subtract", 81: "KP_Equal", 71: "Clear",

        // 修飾鍵本身。librime 的「按一下 Shift 切中英」靠的是它們的 **release**
        // 事件，所以 flagsChanged 也必須送進引擎（見 ModifierTracker）。
        56: "Shift_L", 60: "Shift_R",
        59: "Control_L", 62: "Control_R",
        58: "Alt_L", 61: "Alt_R",
        55: "Super_L", 54: "Super_R",
        57: "Caps_Lock",
    ]

    /// 修飾鍵的 keyCode → 它在 `modifierFlags` 裡對應的旗標。
    public static let modifierKeyFlag: [UInt16: MacModifierFlags] = [
        56: .shift, 60: .shift,
        59: .control, 62: .control,
        58: .option, 61: .option,
        55: .command, 54: .command,
        57: .capsLock,
    ]
}

public struct KeyMapper {

    private let resolver: KeysymResolver

    public init(resolver: KeysymResolver) { self.resolver = resolver }

    /// X11 對「沒有專屬 keysym 的 Unicode 字元」的慣例：碼位 + 0x01000000。
    public static let unicodeKeysymBase: Int32 = 0x0100_0000

    public static func modifiers(from flags: MacModifierFlags, isKeyUp: Bool) -> RSModifier {
        var m: RSModifier = []
        if flags.contains(.shift) { m.insert(.shift) }
        if flags.contains(.control) { m.insert(.control) }
        if flags.contains(.option) { m.insert(.alt) }
        if flags.contains(.command) { m.insert(.super_) }
        if flags.contains(.capsLock) { m.insert(.caps) }
        if isKeyUp { m.insert(.release) }
        return m
    }

    /// 回傳 nil 代表「這顆鍵無法表示成 keysym」，呼叫端**不得**吞掉它，
    /// 必須讓宿主 app 自己處理（回傳 false）。
    public func stroke(for e: MacKeyEvent) -> KeyStroke? {
        let mods = KeyMapper.modifiers(from: e.flags, isKeyUp: e.isKeyUp)

        // 1. 位置固定的鍵：查表拿名稱，值向 librime 要。
        if let name = PhysicalKeys.table[e.keyCode] {
            let k = resolver.keysym(named: name)
            return k == 0 ? nil : KeyStroke(keysym: k, modifiers: mods)
        }

        // 2. 其餘一律由**當前鍵盤配置**產生的字元決定。這一步就是佈局無關性。
        guard let scalar = e.charactersIgnoringModifiers.unicodeScalars.first else { return nil }
        guard let sym = KeyMapper.keysym(forScalar: scalar) else { return nil }
        return KeyStroke(keysym: sym, modifiers: mods)
    }

    public static func keysym(forScalar s: Unicode.Scalar) -> Int32? {
        let v = s.value
        // Latin-1（含 ASCII 可列印區）：keysym 就是碼位本身。
        if (0x20...0x7E).contains(v) || (0xA0...0xFF).contains(v) { return Int32(v) }
        // 控制字元。charactersIgnoringModifiers 理論上不會給，但實測某些
        // 第三方鍵盤配置會 —— 還原成未按 Control 時的字母，避免送出垃圾 keysym。
        if (0x01...0x1A).contains(v) { return Int32(v + 0x60) }
        if v == 0x7F { return nil }   // DEL：交給 keyCode 表，不猜
        if v < 0x20 { return nil }
        // 其餘 Unicode 走 X11 的 0x01000000 慣例。
        guard v <= 0x10FFFF else { return nil }
        return Int32(bitPattern: UInt32(unicodeKeysymBase) &+ v)
    }
}

/// `flagsChanged` 事件的狀態機。
///
/// macOS 不送修飾鍵的 keyDown / keyUp，只送一個 `flagsChanged`，要靠比對前後的
/// `modifierFlags` 才知道是按下還是放開。librime 的「輕點 Shift 切中英」偵測的是
/// **release**，所以少了這一段，那個所有 RIME 使用者都習慣的操作就不存在。
///
/// 這是純狀態機，沒有 AppKit，可以直接單元測試。
public struct ModifierTracker {
    private var lastFlags: MacModifierFlags = []

    public init() {}

    public mutating func reset() { lastFlags = [] }

    /// 回傳這次 flagsChanged 代表的按下／放開；無法判斷時回傳 nil。
    public mutating func transition(keyCode: UInt16,
                                    flags: MacModifierFlags) -> (isKeyUp: Bool,
                                                                 flags: MacModifierFlags)? {
        defer { lastFlags = flags }
        guard let bit = PhysicalKeys.modifierKeyFlag[keyCode] else { return nil }
        // Caps Lock 是切換鍵：flags 裡的位元代表「燈亮著」而不是「鍵按著」，
        // 所以它的 flagsChanged 一律當按下處理（放開由下一次切換帶出）。
        if bit == .capsLock { return (false, flags) }
        let nowDown = flags.contains(bit)
        let wasDown = lastFlags.contains(bit)
        if nowDown == wasDown { return nil }
        // 放開時，該修飾鍵自己的位元已經從 flags 消失了，但送進 librime 的
        // 事件仍應帶著它 —— X11 的語義是「這顆鍵放開了」，鍵本身還是那顆。
        return (isKeyUp: !nowDown, flags: nowDown ? flags : flags.union(bit))
    }
}
