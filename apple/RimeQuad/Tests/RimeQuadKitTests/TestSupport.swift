import Foundation
import XCTest
@testable import RimeQuadKit

/// repo 根目錄。`#filePath` 在 SwiftPM 下永遠是原始碼的絕對路徑，
/// 所以測試不依賴 cwd（`swift test` 的 cwd 會隨呼叫方式改變）。
enum Repo {
    static let root: URL = {
        var u = URL(fileURLWithPath: #filePath)
        // .../apple/RimeQuad/Tests/RimeQuadKitTests/TestSupport.swift
        for _ in 0..<5 { u.deleteLastPathComponent() }
        return u
    }()

    static var themesDir: URL { root.appendingPathComponent("core/themes") }
    static var specFile: URL { root.appendingPathComponent("docs/theme-format.md") }

    static func themeIds() throws -> [String] {
        let names = try FileManager.default.contentsOfDirectory(atPath: themesDir.path)
        return names.filter { $0.hasSuffix(".yaml") }
            .map { String($0.dropLast(5)) }
            .sorted()
    }
}

/// 測試專用的 keysym 查表。
///
/// ⚠ 這份表**不是**權威 —— 權威是 librime。它只是為了讓純邏輯測試不必連
///   librime。真正保證表裡的名稱在 librime 查得到的是
///   `apple/scripts/verify_keysyms.sh`（在 CI 上用真的 rs_keysym_by_name 問一遍）。
struct FixtureKeysyms: KeysymResolver {
    static let known: [String: Int32] = [
        "BackSpace": 0xFF08, "Tab": 0xFF09, "Return": 0xFF0D, "Escape": 0xFF1B,
        "Delete": 0xFFFF, "Insert": 0xFF63, "Clear": 0xFF0B,
        "Home": 0xFF50, "Left": 0xFF51, "Up": 0xFF52, "Right": 0xFF53, "Down": 0xFF54,
        "Page_Up": 0xFF55, "Page_Down": 0xFF56, "End": 0xFF57,
        "space": 0x0020,
        "F1": 0xFFBE, "F2": 0xFFBF, "F3": 0xFFC0, "F4": 0xFFC1, "F5": 0xFFC2,
        "F6": 0xFFC3, "F7": 0xFFC4, "F8": 0xFFC5, "F9": 0xFFC6, "F10": 0xFFC7,
        "F11": 0xFFC8, "F12": 0xFFC9, "F13": 0xFFCA, "F14": 0xFFCB, "F15": 0xFFCC,
        "F16": 0xFFCD, "F17": 0xFFCE, "F18": 0xFFCF, "F19": 0xFFD0, "F20": 0xFFD1,
        "KP_0": 0xFFB0, "KP_1": 0xFFB1, "KP_2": 0xFFB2, "KP_3": 0xFFB3, "KP_4": 0xFFB4,
        "KP_5": 0xFFB5, "KP_6": 0xFFB6, "KP_7": 0xFFB7, "KP_8": 0xFFB8, "KP_9": 0xFFB9,
        "KP_Decimal": 0xFFAE, "KP_Multiply": 0xFFAA, "KP_Add": 0xFFAB,
        "KP_Divide": 0xFFAF, "KP_Enter": 0xFF8D, "KP_Subtract": 0xFFAD, "KP_Equal": 0xFFBD,
        "Shift_L": 0xFFE1, "Shift_R": 0xFFE2, "Control_L": 0xFFE3, "Control_R": 0xFFE4,
        "Alt_L": 0xFFE9, "Alt_R": 0xFFEA, "Super_L": 0xFFEB, "Super_R": 0xFFEC,
        "Caps_Lock": 0xFFE5,
    ]

    func keysym(named: String) -> Int32 { FixtureKeysyms.known[named] ?? 0 }
}

func loadTheme(_ id: String, _ docs: [String: String],
               platform: Platform = .macos) -> LoadResult<Theme> {
    ThemeLoader.load(id: id, source: MapDocumentSource(docs), platform: platform)
}

/// 只取「身分」來比對，避免測試綁死在訊息文字上（訊息已經不是規範的一部分）。
func identities(_ r: LoadResult<Theme>) -> [String] { r.diagnostics.map(\.identity) }

func codes(_ r: LoadResult<Theme>) -> [DiagnosticCode] { r.diagnostics.map(\.code) }
