//
//  KeyRemap.swift — 換鍵：把「按下 a 實際上是 b」變成一張 keysym 的置換表
//
//  ── 桌面端的換鍵到底換掉什麼？這是本檔存在的理由 ────────────────────────
//
//  行動端是自繪的軟鍵盤，換鍵 = 把佈局上那兩顆鍵的**格子**對調，畫面跟著變
//  （見 android/.../keyboard/KeyRemap.kt）。桌面端沒有軟鍵盤，使用者按的是
//  實體鍵盤上印著 a 的那一顆，畫面上沒有任何東西可以跟著變。所以桌面端必須
//  在兩個語義裡選一個，而且兩者在**組字**時的行為完全不同：
//
//    (A) 換掉**送進引擎的 keysym**：收到 a，送 s 給 librime。
//    (B) 換掉**上屏的字**：照常送 a 給 librime，等它吐出結果再把字換掉。
//
//  **本專案選 (A)。** 理由不是偏好，是 (B) 在組字的時候根本沒有東西可以換：
//
//   1. 打拼音時按下 a **不上屏任何東西**，它只讓正在打的拼音從 "" 變成 "a"。
//      (B) 這時無事可做；等到使用者選完字、上屏的是「愛」，那個字與字母 a
//      之間已經沒有任何對應關係可言。於是 (B) 只有在英文模式下才有作用 ——
//      一個「一半的時間會生效」的設定，比沒有這個設定更難理解。
//   2. 使用者看得到的東西會說謊：他按 a、期待 b，正在打的拼音卻寫著 a，
//      選字窗給的也是 a 開頭的候選。(A) 之下「正在打的拼音、候選、打出來的字」
//      三者一致，(B) 之下三者互相矛盾。
//   3. 使用者要的九宮格「1 跟 2 對調」本來就是要讓拼音串跟著變 ——
//      那正是 (B) 定義上做不到的事。
//
//  (A) 的代價是**拼音方案吃到的是換過的碼**。這件事安全，是因為下面那條規則。
//
//  ── 硬規則：換鍵必須是 keysym 上的置換（bijection）─────────────────────
//
//  Android 那一份在檔頭寫了一整段「絕不動送出什麼」，因為九宮格那八顆鍵送的
//  `A/D/G/J/M/P/T/W` 是 `t9_pinyin` 的 `speller/alphabet` 契約，把某一顆改成
//  送別的字母，librime 會老老實實拿錯的碼去查 prism，而鍵面上完全看不出原因。
//
//  桌面端**改了送出的 keysym**，看起來像違反那條規則，其實不是：Android 的
//  不變式真正守住的是「套用前後是**同一批**鍵，只有順序不同」——也就是
//  **置換**。桌面端守住同一條不變式，只是換了個座標系：
//
//      實體鍵盤上每一顆鍵仍然送得出去，只是換了一顆鍵送。
//
//  所以 `speller/alphabet` 需要的每一個字母**都還打得出來**，方案一個字都不會
//  少。反過來說，任意的多對一映射（a→b 而且 s→b）是**禁止**的：那會讓某個
//  字母從此打不出來，使用者會得到一個「有些字永遠選不到」的輸入法，而畫面上
//  完全正常。[KeyRemapTable] 的建構子因此會拒絕非置換的表。
//
//  ── 與方案、英文模式、修飾鍵的關係 ────────────────────────────────────
//
//  · **方案**吃到的是換過的 keysym（這就是 (A) 的定義）。
//  · **英文模式**照樣換。換鍵是「這顆鍵是哪一顆」，不是「中文時才成立的規則」；
//    同一顆鍵按中文模式與英文模式做兩件事，是使用者最難自己查出來的一種狀態。
//  · **Shift** 跟著走：Shift+a 得到 S（不是 s，也不是 A）。作法見
//    [DesktopRemap.compile] —— 大小寫是**兩個不同的 keysym**，各自查表，
//    所以 Caps Lock 也自動正確，一行特例都不必寫。
//  · **Command / Control 一律不換**，見 [KeyRemapTable.apply]。
//
//  ── 這一檔是純的 ──────────────────────────────────────────────────────
//  不碰 AppKit、不碰檔案、不碰 librime。輸入是一份 [RemapDocument]，輸出是一張
//  表與一串「給使用者看的說明」。「使用者能不能把自己鎖死」因此在單元測試裡
//  答得完（KeyRemapTests），不必開一台 Mac。
//

import Foundation

// MARK: - 四端共用的資料模型

/// 一則換鍵操作。**與 Android 的 `RemapOp` 同構**，欄位名一字不差
/// （見 `android/.../keyboard/UserLayoutStore.kt` 的 `LayoutRemapJson`）。
///
/// 全部以鍵的 `id` 定位，不用索引：佈局檔會被市集更新，上游在一列中間插一顆鍵
/// 就會讓所有索引位移一格，使用者「第 2 顆與第 3 顆對調」會靜靜變成另外兩顆鍵
/// 對調，而他從沒改過設定。
public enum RemapOp: Equatable, Sendable {
    /// 兩顆鍵互換所在的格子。
    case swap(layer: String, a: String, b: String)
    /// 把 `key` 移到 `before` 現在所在的格子。
    ///
    /// ⚠ **桌面端不消費這一種。** 它的意義是「在那一列裡往前插」，而桌面端
    /// 手上沒有佈局檔、不知道那一列長什麼樣（`core/layouts/` 刻意不隨 .app
    /// 出貨，桌面沒有軟鍵盤）。看到它時整份佈局的換鍵一律不套用，見
    /// [RemapNoticeCode.cannotShowHere] —— 猜一個位置比不做更糟。
    case move(layer: String, key: String, before: String)

    public var layerId: String {
        switch self {
        case .swap(let layer, _, _): return layer
        case .move(let layer, _, _): return layer
        }
    }
}

/// 讀進來的一則操作。`foreign` = **本端看不懂**（未來版本寫的、或別的端寫的）。
///
/// 保留它而不是丟掉，是因為丟掉的後果不對稱：本端少一則自訂只是少一則，
/// 而使用者在**另一台裝置**上調好的鍵位會在他這次存檔時無聲消失。
public enum RemapOpEntry: Equatable, Sendable {
    case known(RemapOp)
    case foreign
}

// MARK: - 檔案（與 Android 同一份）

/// `<user_data_dir>/luminakey-layouts.json` 的內容。
///
/// ── 為什麼把整份原始 JSON 留在身邊 ──────────────────────────────────────
/// 這份檔案是**四端共用**的：手機上調好的鍵位、未來版本加的欄位、別的端寫的
/// 操作，都可能出現在裡面。所以本型別只認得自己要用的那幾個節點，其餘一律
/// 原封不動帶著走 —— 讀進來什麼樣，寫回去就什麼樣。
///
/// ⚠ 這條紀律有一個實際的對照組：Android 端目前的 `LayoutRemapJson.decode`
///   會把不認得的 `op` 直接丟掉，`encode` 再把剩下的寫回去 —— 也就是說
///   舊版手機讀寫一次，就會把新版寫的操作洗掉。已在 `docs/coordination.md` §5
///   回報。桌面端不重複這個錯。
public struct RemapDocument {

    /// 目前的格式版本。**不遞增**：桌面端沒有新增任何欄位，用的就是 v1。
    public static let formatVersion = 1

    /// 整份檔案。`layouts` 以外的鍵、以及每個 layout 物件裡 `id` / `ops` 以外的
    /// 鍵，都只住在這裡，本端不解讀也不修改。
    private var root: [String: Any]

    public init() {
        root = ["format_version": RemapDocument.formatVersion, "layouts": [Any]()]
    }

    private init(root: [String: Any]) { self.root = root }

    /// 解不開一律回空白文件，**不丟例外**。
    ///
    /// 一份壞掉的自訂檔不該讓輸入法打不出字：使用者頂多少了幾則換鍵，
    /// 這比「按鍵全部失效」好得多。壞檔案會在下一次存檔時被整份取代，
    /// 所以這裡刻意不去「修復」它。
    public static func decode(_ data: Data) -> RemapDocument {
        guard let obj = try? JSONSerialization.jsonObject(with: data),
              let dict = obj as? [String: Any] else { return RemapDocument() }
        return RemapDocument(root: dict)
    }

    public func encode() -> Data {
        var out = root
        // 版本欄位一律補上（原本沒有的話）。**不覆寫**已經在裡面的值 ——
        // 那可能是比我們新的版本寫的，降級寫回去等於把新機器上的東西洗掉。
        if out["format_version"] == nil { out["format_version"] = RemapDocument.formatVersion }
        let opts: JSONSerialization.WritingOptions = [.prettyPrinted, .sortedKeys]
        return (try? JSONSerialization.data(withJSONObject: out, options: opts))
            ?? Data("{\n  \"format_version\": 1,\n  \"layouts\": []\n}\n".utf8)
    }

    private var layoutArray: [[String: Any]] {
        (root["layouts"] as? [Any])?.compactMap { $0 as? [String: Any] } ?? []
    }

    public var layoutIds: [String] {
        layoutArray.compactMap { $0["id"] as? String }.filter { !$0.isEmpty }
    }

    /// 一份佈局的操作，**依原順序**，含本端看不懂的那些（以 `.foreign` 佔位）。
    /// 順序要保留是因為操作是依序套用的，抽掉中間一則會改變後面每一則的結果。
    public func entries(layoutId: String) -> [RemapOpEntry] {
        guard let node = layoutArray.first(where: { $0["id"] as? String == layoutId }),
              let ops = node["ops"] as? [Any] else { return [] }
        return ops.map { raw in
            guard let d = raw as? [String: Any], let kind = d["op"] as? String,
                  let layer = nonEmpty(d["layer"]) else { return .foreign }
            switch kind {
            case "swap":
                guard let a = nonEmpty(d["a"]), let b = nonEmpty(d["b"]) else { return .foreign }
                return .known(.swap(layer: layer, a: a, b: b))
            case "move":
                guard let k = nonEmpty(d["key"]), let before = nonEmpty(d["before"]) else {
                    return .foreign
                }
                return .known(.move(layer: layer, key: k, before: before))
            default:
                return .foreign
            }
        }
    }

    /// 換掉一份佈局的整串操作。**呼叫端必須先確認這份佈局沒有 `.foreign` 操作**
    /// （[DesktopRemap.compile] 的 `editable`），否則就是把別人的東西寫掉。
    public mutating func setOps(layoutId: String, _ ops: [RemapOp]) {
        var arr = (root["layouts"] as? [Any]) ?? []
        let encoded: [Any] = ops.map(RemapDocument.encodeOp)
        if let i = arr.firstIndex(where: { ($0 as? [String: Any])?["id"] as? String == layoutId }) {
            if ops.isEmpty {
                arr.remove(at: i)
            } else {
                // 這個 layout 物件裡別的鍵（未來版本可能加的）留著。
                var node = (arr[i] as? [String: Any]) ?? [:]
                node["id"] = layoutId
                node["ops"] = encoded
                arr[i] = node
            }
        } else if !ops.isEmpty {
            arr.append(["id": layoutId, "ops": encoded])
        }
        root["layouts"] = arr
    }

    /// 拿掉一份佈局的全部換鍵 → 回到那份佈局**當前**的原樣。
    ///
    /// 存的是「操作」而不是「改完的樣子」，所以重置給的是最新的基礎佈局，
    /// 不是使用者當初開始改之前的快照。一個只能回到舊版的重置本身就是陷阱。
    public mutating func removeLayout(_ layoutId: String) { setOps(layoutId: layoutId, []) }

    private static func encodeOp(_ op: RemapOp) -> [String: Any] {
        switch op {
        case .swap(let layer, let a, let b):
            return ["op": "swap", "layer": layer, "a": a, "b": b]
        case .move(let layer, let key, let before):
            return ["op": "move", "layer": layer, "key": key, "before": before]
        }
    }

    private func nonEmpty(_ v: Any?) -> String? {
        guard let s = v as? String, !s.isEmpty else { return nil }
        return s
    }
}

// MARK: - 給使用者看的說明

/// 換鍵可能出現的每一種狀況。**是碼，不是一句組好的字**
/// （與 `docs/theme-format.md` §6.5 的診斷模型同一個做法）。
///
/// 這樣才擋得住「驗證器把內部訊息倒到畫面上」——`docs/ui-design.md` §7.7 明訂
/// 「沒有白話對照的問題碼不准上畫面」，而 `allCases` 讓那條規則測得到。
public enum RemapNoticeCode: String, CaseIterable, Sendable {
    /// 這份佈局裡有本端看不懂的操作（別的裝置或更新版寫的）。
    case cannotShowHere
    /// 操作指到一顆這台鍵盤上沒有的鍵。
    case keyNotOnThisKeyboard
    /// 操作指到一個桌面端不消費的層。
    case layerNotOnThisKeyboard
    /// 換出來的表不是一對一 —— 會有字母從此打不出來。
    case notOneToOne
    /// 一顆鍵不能跟自己對調。
    case sameKey
}

public struct RemapNotice: Equatable, Sendable, Error {
    public let code: RemapNoticeCode
    public let args: [String]

    public init(_ code: RemapNoticeCode, _ args: [String] = []) {
        self.code = code
        self.args = args
    }

    /// 白話結論（一行，放在最前面）。
    public var title: T { RemapCopy.title(code) }
    /// 白話原因 + 現在的狀態。
    public var body: T { RemapCopy.body(code) }

    public func bodyText(_ lang: UiLanguage) -> String {
        RemapCopy.body(code).format(lang, args.first ?? "", args.count > 1 ? args[1] : "")
    }
}

/// 每一個碼的白話對照。少一個碼就編不過（`switch` 是窮舉的），
/// 三種語言少一種也編不過（`T` 的三個參數都是必填）。
public enum RemapCopy {

    public static func title(_ code: RemapNoticeCode) -> T {
        switch code {
        case .cannotShowHere:
            return T("這個鍵盤的換鍵是在別的地方調的",
                     "这个键盘的换键是在别的地方调的",
                     "These key changes were made somewhere else")
        case .keyNotOnThisKeyboard:
            return T("這顆鍵不在你的鍵盤上",
                     "这颗键不在你的键盘上",
                     "That key is not on your keyboard")
        case .layerNotOnThisKeyboard:
            return T("這一組換鍵在電腦上用不到",
                     "这一组换键在电脑上用不到",
                     "These key changes do not apply on a computer")
        case .notOneToOne:
            return T("這樣換會有字母打不出來",
                     "这样换会有字母打不出来",
                     "That change would make a letter impossible to type")
        case .sameKey:
            return T("同一顆鍵不能跟自己對調",
                     "同一颗键不能跟自己对调",
                     "A key cannot swap with itself")
        }
    }

    public static func body(_ code: RemapNoticeCode) -> T {
        switch code {
        case .cannotShowHere:
            return T("是在手機上、或用比較新的版本調的，這台電腦顯示不出來。你打字不受影響，這裡就照原樣放著。要在這台電腦上換鍵的話，先按下面的「還原」。",
                     "是在手机上、或用比较新的版本调的，这台电脑显示不出来。你打字不受影响，这里就照原样放着。要在这台电脑上换键的话，先按下面的「还原」。",
                     "They were made on a phone, or by a newer version, and cannot be shown here. Your typing is unaffected and nothing has been touched. To change keys on this computer, press Restore below first.")
        case .keyNotOnThisKeyboard:
            return T("「{0}」在手機鍵盤上有，在電腦鍵盤上沒有。這裡的換鍵全部保持原樣，你打字不受影響。要在這台電腦上換鍵的話，先按下面的「還原」。",
                     "「{0}」在手机键盘上有，在电脑键盘上没有。这里的换键全部保持原样，你打字不受影响。要在这台电脑上换键的话，先按下面的「还原」。",
                     "“{0}” exists on the phone keyboard but not on a computer keyboard. Nothing has been changed and your typing is unaffected. To change keys on this computer, press Restore below first.")
        case .layerNotOnThisKeyboard:
            return T("電腦鍵盤只有「按著 Shift」和「沒按 Shift」兩種，其他的用不到。這裡的換鍵全部保持原樣。要在這台電腦上換鍵的話，先按下面的「還原」。",
                     "电脑键盘只有「按着 Shift」和「没按 Shift」两种，其他的用不到。这里的换键全部保持原样。要在这台电脑上换键的话，先按下面的「还原」。",
                     "A computer keyboard only has Shift and no-Shift; anything else does not apply here. Nothing has been changed. To change keys on this computer, press Restore below first.")
        case .notOneToOne:
            return T("兩顆鍵要換就是互相換。如果兩顆鍵都變成同一個字母，另一個字母就再也打不出來了。已經幫你保持原樣。",
                     "两颗键要换就是互相换。如果两颗键都变成同一个字母，另一个字母就再也打不出来了。已经帮你保持原样。",
                     "Two keys trade places with each other. If both produced the same letter, the other letter could never be typed again. Nothing has been changed.")
        case .sameKey:
            return T("要對調的是兩顆不同的鍵。再點一顆別的。",
                     "要对调的是两颗不同的键。再点一颗别的。",
                     "Pick two different keys to swap. Tap another one.")
        }
    }
}

// MARK: - 換鍵表

/// keysym → keysym 的置換。**這就是「按下 a 實際上是 b」本身。**
///
/// 為什麼是 keysym 而不是「實體鍵位」：macOS 的 `charactersIgnoringModifiers`
/// 已經替我們把使用者的實體鍵盤配置（Dvorak、AZERTY、德文 QWERTZ）算成字了，
/// 見 KeyMapper 的檔頭。用 keysym 當鍵，Dvorak 使用者的「印著 a 的那顆鍵」
/// 自動就是對的那一顆，我們一行佈局特例都不必寫。
///
/// 順帶解決 Caps Lock：大寫是**另一個 keysym**，各自查表，不必判斷燈亮不亮。
public struct KeyRemapTable: Equatable, Sendable {

    public let map: [Int32: Int32]

    public static let identity = KeyRemapTable(map: [:])

    public var isEmpty: Bool { map.isEmpty }

    /// 建不出非置換的表。回 nil 代表呼叫端給的是多對一 ——
    /// 那會讓某個字母從此打不出來（見檔頭「硬規則」）。
    public init?(checking map: [Int32: Int32]) {
        guard Set(map.values).count == map.count else { return nil }
        // 值域必須被定義域包含，否則「某個 keysym 被搬走了卻沒有人補位」，
        // 那顆鍵送出的東西就永遠消失了。
        guard Set(map.values) == Set(map.keys) else { return nil }
        self.map = map
    }

    private init(map: [Int32: Int32]) { self.map = map }

    /// ⚠ **Command 或 Control 按著的時候一律不換。**
    ///
    /// 那些是宿主 app 的快捷鍵，不是輸入。把 a 換成 s 的使用者按 ⌘A 想的是
    /// 「全選」，換過去就變成「儲存」——一個他絕對不會聯想到鍵盤設定的後果，
    /// 而且可能是不可逆的。Shift / Caps / Option 照常換：那些是打字的一部分。
    public func apply(to stroke: KeyStroke) -> KeyStroke {
        let mods = RSModifier(rawValue: stroke.modifiers)
        // 兩個修飾鍵刻意分兩行寫,不用 `||`:變異表的欄位分隔字元是 `|`,
        // 而 `||` 會把那一列拆成六段,於是「這個變異該打紅哪一組」變成一段
        // 程式碼碎片 —— 症狀是守門腳本說「測試裡沒有 KeyRemapTests」,
        // 看起來像測試不見了,其實是分隔字元撞了。
        if mods.contains(.super_) { return stroke }    // ⌘A 是全選,不是儲存
        if mods.contains(.control) { return stroke }   // ⌃` 是 librime 的方案選單
        guard let to = map[stroke.keysym] else { return stroke }
        return KeyStroke(keysym: to, modifiers: mods)
    }

    /// 鍵帽上印著 `c` 的那顆鍵,現在按下去會送出什麼。
    ///
    /// 畫面上要顯示的是**現在會送出什麼**,而不是鍵帽上印什麼 —— 桌面端沒有
    /// 軟鍵盤可以跟著變,所以這是使用者唯一看得到「換過了」的地方。
    public func output(forLetter c: Character) -> Character {
        let from = DesktopRemap.keysym(c, shifted: false)
        guard let to = map[from], to >= 0, let scalar = Unicode.Scalar(UInt32(to)) else { return c }
        return Character(scalar)
    }
}

// MARK: - 桌面端的編譯

/// 使用者在畫面上看到的一組換鍵。
///
/// `letters` 是一個**環**：按下 `letters[i]` 得到 `letters[i+1]`，最後一個回到
/// 第一個。兩顆鍵對調就是長度 2 的環，也就是絕大多數情況。長度 3 以上只會
/// 從別的裝置傳過來，但它顯示得出來就不會變成「畫面上少了一條」。
public struct RemapCycle: Equatable, Sendable {
    public let letters: [Character]
    /// 沒按 Shift 的時候會換。
    public let appliesUnshifted: Bool
    /// 按著 Shift 的時候會換。
    public let appliesShifted: Bool

    public init(letters: [Character], appliesUnshifted: Bool, appliesShifted: Bool) {
        self.letters = letters
        self.appliesUnshifted = appliesUnshifted
        self.appliesShifted = appliesShifted
    }
}

public struct RemapCompilation {
    public let table: KeyRemapTable
    public let cycles: [RemapCycle]
    public let notices: [RemapNotice]
    /// 這台電腦可不可以在這份佈局上再動手。
    ///
    /// **有任何一則 notice 就是 `false`。** 因為本端寫回去的方式是「把整串操作
    /// 重新產生一次」，而它只產生得出自己看得懂的那幾種 —— 在還有東西看不懂
    /// 的時候寫回去，等於把使用者在別的裝置上調的鍵位刪掉，而且沒有訊息。
    /// 這時唯一開放的動作是「全部還原」（那是使用者明確要求的破壞）。
    public let editable: Bool

    public var isEmpty: Bool { cycles.isEmpty }
}

public enum DesktopRemap {

    /// 桌面端消費的佈局 id。
    ///
    /// ── 為什麼是 `qwerty`，而不是另外發明一個「桌面佈局」id ────────────────
    /// 因為使用者要的是同一件事：他在手機的 26 鍵上把 a 跟 s 對調，回到電腦
    /// 前面按 a 也該得到 s。另立一個 id 等於同一個設定有兩個地方可以改，
    /// 而他不會知道為什麼兩台裝置不一樣。
    ///
    /// `core/layouts/qwerty.yaml` 的每一顆字母鍵，`id` 恰好就是它送出的 keysym
    /// 名稱（`{ id: "a", send: { keysym: "a" } }`），上層 `upper` 的 id 也還是
    /// `"a"`（送 `A`）。所以這份對照不需要把佈局檔打包進 .app —— 那正是
    /// `apple/scripts/build_app.sh` 刻意不做的事（桌面沒有軟鍵盤）。
    public static let layoutId = "qwerty"

    /// 沒按 Shift 的那一層。
    public static let unshiftedLayer = "lower"
    /// 按著 Shift 的那一層。
    public static let shiftedLayer = "upper"

    /// 桌面端換得動的鍵：26 個英文字母。
    ///
    /// ⚠ **這是一條刻意畫出來的界線，不是還沒做完。** 數字與標點在實體鍵盤上
    ///   的「上檔字元」隨鍵盤配置而不同（美式 Shift+2 是 @，英式是 "），要正確
    ///   處理得去問系統目前的鍵盤配置；而 26 個字母的大小寫配對在每一種拉丁
    ///   配置上都成立。使用者要的「按 a 出 b」也正好整個落在這個範圍裡。
    ///   界線之外的鍵**不畫在畫面上**，而不是畫出來按了沒反應。
    public static let letters: [Character] = Array("abcdefghijklmnopqrstuvwxyz")

    /// 把檔案編譯成一張表。**全有或全無**：任何一則操作有問題，整份佈局的
    /// 換鍵都不套用。
    ///
    /// 一半生效是最糟的結果 —— 使用者看到鍵盤變了、但變得跟他要的不一樣，
    /// 而且沒有任何訊息。這一條與 Android 的 `RemapOutcome` 是同一個決定。
    public static func compile(_ doc: RemapDocument) -> RemapCompilation {
        let entries = doc.entries(layoutId: layoutId)
        if entries.isEmpty {
            return RemapCompilation(table: .identity, cycles: [], notices: [], editable: true)
        }
        if entries.contains(.foreign) {
            return RemapCompilation(table: .identity, cycles: [],
                                    notices: [RemapNotice(.cannotShowHere)], editable: false)
        }

        var lower = Permutation()
        var upper = Permutation()
        for case .known(let op) in entries {
            guard case .swap(let layer, let a, let b) = op else {
                // `move` 需要知道那一列的順序，桌面端沒有佈局檔。見 RemapOp.move。
                return RemapCompilation(table: .identity, cycles: [],
                                        notices: [RemapNotice(.cannotShowHere)], editable: false)
            }
            guard let ka = letter(a), let kb = letter(b) else {
                let bad = letter(a) == nil ? a : b
                return RemapCompilation(table: .identity, cycles: [],
                                        notices: [RemapNotice(.keyNotOnThisKeyboard, [bad])],
                                        editable: false)
            }
            if ka == kb {
                return RemapCompilation(table: .identity, cycles: [],
                                        notices: [RemapNotice(.sameKey, [String(ka)])],
                                        editable: false)
            }
            switch layer {
            case unshiftedLayer: lower.swap(ka, kb)
            case shiftedLayer: upper.swap(ka, kb)
            default:
                return RemapCompilation(table: .identity, cycles: [],
                                        notices: [RemapNotice(.layerNotOnThisKeyboard, [layer])],
                                        editable: false)
            }
        }

        var map: [Int32: Int32] = [:]
        for (from, to) in lower.moved { map[keysym(from, shifted: false)] = keysym(to, shifted: false) }
        for (from, to) in upper.moved { map[keysym(from, shifted: true)] = keysym(to, shifted: true) }

        guard let table = KeyRemapTable(checking: map) else {
            // 由 swap 組出來的一定是置換，所以走到這裡代表上面的邏輯壞了，
            // 而不是使用者做錯了。仍然要有一句話可以說 —— 不能讓畫面空白。
            return RemapCompilation(table: .identity, cycles: [],
                                    notices: [RemapNotice(.notOneToOne)], editable: false)
        }
        return RemapCompilation(table: table,
                                cycles: cycles(lower: lower, upper: upper),
                                notices: [], editable: true)
    }

    // ── 使用者按下「對調」時要寫回去的東西 ──────────────────────────────

    /// 把畫面上**位置 `p1`** 與**位置 `p2`** 現在送出的東西對調。
    ///
    /// 參數是「鍵帽上印的字母」，不是「它現在送出什麼」—— 那才是使用者點的
    /// 東西。換過幾次之後兩者會不一樣，這個轉換在這裡做，UI 不必知道。
    public static func swapping(_ p1: Character, _ p2: Character,
                                in doc: RemapDocument) -> Result<RemapDocument, RemapNotice> {
        guard p1 != p2 else { return .failure(RemapNotice(.sameKey, [String(p1)])) }
        guard letters.contains(p1), letters.contains(p2) else {
            let bad = letters.contains(p1) ? p2 : p1
            return .failure(RemapNotice(.keyNotOnThisKeyboard, [String(bad)]))
        }
        let compiled = compile(doc)
        // 把**編譯時真正的那一則**傳出去,而不是一律回「在別的地方調的」。
        // §7.10 要的是「每一種問題都有對應的白話」——回錯一句的話,
        // 那條規則只做了一半:畫面上有話說,但說的不是真正發生的事。
        guard compiled.editable else {
            return .failure(compiled.notices.first ?? RemapNotice(.cannotShowHere))
        }

        var (lower, upper) = permutations(of: doc)
        // 兩層各自套同一個對調。**不是**把上層直接設成下層的副本 ——
        // 手機端一次只寫一層，直接覆蓋會把使用者在手機上只調了小寫的其他鍵
        // 一起改掉，而他沒有要求那件事。
        lower.exchangeAtPositions(p1, p2)
        upper.exchangeAtPositions(p1, p2)
        return .success(write(lower: lower, upper: upper, into: doc))
    }

    /// 把一組換鍵還原成原本的樣子。
    ///
    /// 只給 [Permutation.reset] **一顆**鍵當起點,整環由它自己走完 ——
    /// 不是在這裡逐一還原 `cycle.letters`。兩種寫法在正常情況下結果一樣,
    /// 但逐一還原會讓「整環一起回去」這條不變式**沒有任何一個地方在守**:
    /// reset 只還原一顆也照樣通過,而那個 bug 要等到使用者換了一個三環
    /// 再按還原,才會以「有一顆鍵怪怪的」的樣子出現。
    ///
    /// 層是分開的:手機端一次只寫一層,所以同一顆鍵在上下層可能屬於**不同**
    /// 的環。哪一層要動由這一列自己說(`applies*`),不能兩層一起掃。
    public static func restoring(_ cycle: RemapCycle, in doc: RemapDocument) -> RemapDocument {
        guard let anchor = cycle.letters.first else { return doc }
        var (lower, upper) = permutations(of: doc)
        if cycle.appliesUnshifted { lower.reset(anchor) }
        if cycle.appliesShifted { upper.reset(anchor) }
        return write(lower: lower, upper: upper, into: doc)
    }

    /// 全部還原。
    ///
    /// ⚠ **只清掉這台電腦顯示得出來的那一份**（`qwerty`），不掃整個檔案。
    /// 使用者按的是「把我看到的這些還原」；把他看不到的東西（例如手機上的
    /// 九宮格鍵位）一起刪掉，是他沒有辦法預料、也沒有辦法救回的損失。
    public static func clearing(in doc: RemapDocument) -> RemapDocument {
        var next = doc
        next.removeLayout(layoutId)
        return next
    }

    // ── 內部 ────────────────────────────────────────────────────────────

    private static func permutations(of doc: RemapDocument) -> (Permutation, Permutation) {
        var lower = Permutation()
        var upper = Permutation()
        for case .known(.swap(let layer, let a, let b)) in doc.entries(layoutId: layoutId) {
            guard let ka = letter(a), let kb = letter(b), ka != kb else { continue }
            if layer == unshiftedLayer { lower.swap(ka, kb) }
            if layer == shiftedLayer { upper.swap(ka, kb) }
        }
        return (lower, upper)
    }

    private static func write(lower: Permutation, upper: Permutation,
                              into doc: RemapDocument) -> RemapDocument {
        var ops: [RemapOp] = lower.decomposed().map {
            .swap(layer: unshiftedLayer, a: String($0.0), b: String($0.1))
        }
        ops += upper.decomposed().map {
            .swap(layer: shiftedLayer, a: String($0.0), b: String($0.1))
        }
        var next = doc
        next.setOps(layoutId: layoutId, ops)
        return next
    }

    private static func letter(_ id: String) -> Character? {
        guard id.count == 1, let c = id.first, letters.contains(c) else { return nil }
        return c
    }

    /// 字母 → keysym。X11 對 ASCII 可列印字元的 keysym 就是碼位本身
    /// （見 KeyMapper.keysym(forScalar:)），所以這裡不必查表。
    static func keysym(_ c: Character, shifted: Bool) -> Int32 {
        let s = shifted ? Character(c.uppercased()) : c
        return Int32(s.unicodeScalars.first!.value)
    }

    /// 兩層各自算環，再把**一模一樣**的環併成一列。
    ///
    /// 不是「以下層為準、上層用旗標標註」——手機端一次只寫一層，兩層完全可能
    /// 換的是不同的鍵。那時把上層的環硬塞進下層那一列，畫面上會出現一句
    /// 精確的謊話（寫著 a ⇄ s 而按著 Shift 的時候其實是 a ⇄ d）。
    private static func cycles(lower: Permutation, upper: Permutation) -> [RemapCycle] {
        let lowerRings = rings(of: lower)
        let upperRings = rings(of: upper)
        var out: [RemapCycle] = lowerRings.map {
            RemapCycle(letters: $0, appliesUnshifted: true, appliesShifted: upperRings.contains($0))
        }
        out += upperRings.filter { !lowerRings.contains($0) }.map {
            RemapCycle(letters: $0, appliesUnshifted: false, appliesShifted: true)
        }
        return out
    }

    /// 一個置換裡所有長度 > 1 的環。
    ///
    /// 依字母順序走，所以每個環一定從它最小的字母開始 —— 這讓環有唯一的寫法，
    /// 上下兩層才比對得起來，畫面上的順序也才是穩定的（換第二組時第一組
    /// 不會莫名其妙跳到下面去）。
    private static func rings(of p: Permutation) -> [[Character]] {
        var out: [[Character]] = []
        var seen = Set<Character>()
        for start in letters where !seen.contains(start) {
            let ring = p.cycle(from: start)
            for c in ring { seen.insert(c) }
            if ring.count > 1 { out.append(ring) }
        }
        return out
    }
}

// MARK: - 置換

/// 「哪一顆鍵現在坐在哪一個位置」。
///
/// ⚠ **這是 Android `applyKeyRemap` 的同一套算法，只是把「格子」換成「鍵帽」。**
/// Android 的 `swap(a, b)` 是「找出 id 為 a 與 b 的鍵**現在**在哪兩個格子，
/// 把那兩個格子的內容對調」。連續兩則操作的結果因此與順序有關：
/// `swap(a,s)` 之後 `swap(a,d)` 得到的是 a→s、s→d、d→a 這個三環，
/// 而不是兩組獨立的對調。桌面端必須算出**一模一樣**的結果，否則同一份檔案
/// 在手機與電腦上的行為會不一樣，而且沒有任何跡象。
struct Permutation {

    /// 位置 → 現在坐在這個位置的鍵。只存動過的。
    private(set) var atPosition: [Character: Character] = [:]
    /// 鍵 → 它現在坐的位置。
    private var positionOf: [Character: Character] = [:]

    /// 動過的位置（位置 → 送出什麼）。
    var moved: [(Character, Character)] {
        atPosition.filter { $0.key != $0.value }.map { ($0.key, $0.value) }
    }

    func key(at position: Character) -> Character { atPosition[position] ?? position }
    func position(of key: Character) -> Character { positionOf[key] ?? key }

    /// Android 的 `RemapOp.Swap`：把**鍵** a 與**鍵** b 所在的位置對調。
    mutating func swap(_ a: Character, _ b: Character) {
        exchangeAtPositions(position(of: a), position(of: b))
    }

    /// 把兩個**位置**上的東西對調。UI 點的是位置，所以這是它要的那一個。
    mutating func exchangeAtPositions(_ p1: Character, _ p2: Character) {
        guard p1 != p2 else { return }
        let ka = key(at: p1)
        let kb = key(at: p2)
        atPosition[p1] = kb
        atPosition[p2] = ka
        positionOf[kb] = p1
        positionOf[ka] = p2
    }

    /// 把某一顆鍵帽以及跟它連在一起的那一環全部放回原位。
    ///
    /// 整環一起還原，不是只還原那一顆：一個環是「a 給 s、s 給 d、d 給 a」，
    /// 只把 a 拉回來會讓剩下的兩顆變成不成立的狀態（d 沒有人給、s 沒有人收）。
    mutating func reset(_ c: Character) {
        // 先把整環抄下來再改 —— 邊走邊改會走到已經被改成原樣的元素上，
        // 環會提早結束，剩下的鍵就留在半途的狀態。
        let ring = cycle(from: c)
        for p in ring {
            atPosition[p] = p
            positionOf[p] = p
        }
    }

    /// 從某個位置出發的環：`[p, key(at:p), key(at:key(at:p)), …]`，回到起點為止。
    /// 沒動過的話回傳只有一個元素的陣列。
    func cycle(from start: Character) -> [Character] {
        var out: [Character] = [start]
        var cur = key(at: start)
        var guardCount = 0
        while cur != start {
            out.append(cur)
            cur = key(at: cur)
            guardCount += 1
            if guardCount > 64 { break }   // 資料壞掉時不要轉到天荒地老
        }
        return out
    }

    /// 拆成一串「兩顆鍵對調」的操作，依序套用會得到這個置換本身。
    ///
    /// 為什麼要拆而不是直接存結果：檔案格式存的是**操作**（四端共用，
    /// Android 只認得 swap / move），存「改完的樣子」會讓上游佈局更新之後
    /// 使用者永遠停在舊版上。作法是從原樣出發，依字母序把每一個位置補到位；
    /// 補第 n 個位置時前 n-1 個已經定案，所以不會被後面的操作弄壞
    /// （證明在 KeyRemapTests：拆完再重放一次，必須逐項相等）。
    func decomposed() -> [(Character, Character)] {
        var state = Permutation()
        var ops: [(Character, Character)] = []
        for p in DesktopRemap.letters {
            let want = key(at: p)
            let have = state.key(at: p)
            if want == have { continue }
            ops.append((have, want))
            state.swap(have, want)
        }
        return ops
    }
}
