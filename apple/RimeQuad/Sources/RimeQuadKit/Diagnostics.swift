//
//  Diagnostics.swift — 診斷模型（docs/theme-format.md §6）
//
//  ⚠ 這一版採用的是 **code + args**，不是自由文字。
//
//  規範原本把 `message` 定義成自由文字，而 §10 檢核第 9 條又要求「同一份壞檔案，
//  四端報一樣多則」。這兩件事在要上畫面的那一刻互相矛盾：訊息一旦在地化，
//  四端就沒有任何可以互相比對的東西了（Android 端提出，見 coordination.md §5）。
//
//  所以診斷的身分是 `(severity, code, path)`，`args` 是填進在地化樣板的參數，
//  `developerMessage` 只是給開發者看的英文回退，**不參與比對、不上使用者畫面**。
//
//  桌面端是第一個實作這個模型的端；規範 §6.5 的碼表與這裡的 `DiagnosticCode`
//  必須逐項對得上（`ThemeSpecTests.testEveryDiagnosticCodeIsInTheSpec` 會檢查）。
//

import Foundation

/// 診斷嚴重度。ERROR 只用於 §6.2 的致命錯誤清單。
public enum Severity: String, Sendable, CaseIterable {
    case info = "INFO"
    case warning = "WARNING"
    case error = "ERROR"
}

/// 穩定的診斷代碼。**字面值是規範的一部分**，不可為了好看而更名。
public enum DiagnosticCode: String, Sendable, CaseIterable {

    // ── 致命（§6.2 的 F1–F10 與文件層級） ──────────────────────────
    case fatalYamlSyntax            = "fatal.yaml_syntax"            // F1  args: [detail]
    case fatalRootNotMapping        = "fatal.root_not_mapping"       // F2  args: []
    case fatalFormatMissing         = "fatal.format_missing"         // F3  args: [id]
    case fatalFormatMalformed       = "fatal.format_malformed"       // F3  args: [tag, id]
    case fatalFormatKindMismatch    = "fatal.format_kind_mismatch"   // F3  args: [actual, expected, id]
    case fatalFormatMajorUnsupported = "fatal.format_major_unsupported" // F3 args: [id, kind, major, supported]
    case fatalIdMissing             = "fatal.id_missing"             // F4  args: [name]
    case fatalIdInvalid             = "fatal.id_invalid"             // F4  args: [id]
    case fatalIdMismatch            = "fatal.id_mismatch"            // F4  args: [id, name]
    case fatalDocumentNotFound      = "fatal.document_not_found"     //     args: [id]
    case fatalParentNotFound        = "fatal.parent_not_found"       // F5  args: [id]
    case fatalInheritsCycle         = "fatal.inherits_cycle"         // F6  args: [chain]
    case fatalInheritsTooDeep       = "fatal.inherits_too_deep"      // F6  args: [max]
    case fatalMinClient             = "fatal.min_client"             // F7  args: [required, running]
    case fatalLayersMissing         = "fatal.layers_missing"         // F8  args: []      （僅行動端會產生）
    case fatalDefaultLayerUnknown   = "fatal.default_layer_unknown"  // F9  args: [layer]
    case fatalAlphaLayerUnknown     = "fatal.alpha_layer_unknown"    // F9  args: [layer]
    case fatalLayerEmpty            = "fatal.layer_empty"            // F10 args: [layer]

    // ── 可回復（§6.3） ────────────────────────────────────────────
    case unknownField               = "unknown_field"                // args: [field] 或 [field, suggestion]
    case duplicateKey               = "duplicate_key"                // args: [key]
    case typeMismatch               = "type_mismatch"                // args: [expected, found]
    case badBool                    = "bad_bool"                     // args: [value]
    case badNumber                  = "bad_number"                   // args: [value]
    case outOfRange                 = "out_of_range"                 // args: [value, min, max, clamped]
    case badEnum                    = "bad_enum"                     // args: [value, allowed, default]
    case badColor                   = "bad_color"                    // args: [value]
    case paletteNotScalar           = "palette_not_scalar"           // args: [name]
    case paletteBadColor            = "palette_bad_color"            // args: [name, value]
    case paletteUnresolvedRef       = "palette_unresolved_ref"       // args: [name, ref]
    case paletteSelfReference       = "palette_self_reference"       // args: [name]
    case paletteCycleOrTooDeep      = "palette_cycle_or_too_deep"    // args: [name]
    case entryDropped               = "entry_dropped"                // args: []
    case assetIncomplete            = "asset_incomplete"             // args: []
    case assetPathEscape            = "asset_path_escape"            // args: [file]
    case unknownScriptTag           = "unknown_script_tag"           // args: [tag]
    case unknownIcon                = "unknown_icon"                 // args: [icon]
    case unknownAction              = "unknown_action"               // args: [raw]
    case badActionArgument          = "bad_action_argument"          // args: [raw]
    case toolbarItemNoTap           = "toolbar_item_no_tap"          // args: []
    case statusItemNoSource         = "status_item_no_source"        // args: []
    case nestedPlatformOverrides    = "nested_platform_overrides"    // args: []

    // ── INFO（§6.4） ──────────────────────────────────────────────
    case requiredItemRestored       = "required_item_restored"       // args: [action]
    case deprecatedField            = "deprecated_field"             // args: [field]
    case featureUnsupported         = "feature_unsupported"          // args: [field, value]
    case legacyBlockIgnored         = "legacy_block_ignored"         // args: [field]

    /// 規範規定的嚴重度。**碼決定嚴重度**，呼叫端不能自己選 —— 否則
    /// 「四端報一樣多則 WARNING」會因為某一端把同一件事記成 INFO 而失守。
    public var severity: Severity {
        if rawValue.hasPrefix("fatal.") { return .error }
        switch self {
        case .requiredItemRestored, .deprecatedField, .featureUnsupported, .legacyBlockIgnored:
            return .info
        default:
            return .warning
        }
    }
}

/// 一則診斷。
///
/// `path` 是 YAML 路徑（如 `candidates.window.background`）；根層級為空字串。
/// `line` 由讀取層提供，取不到時為 nil。
public struct Diagnostic: Sendable, Equatable {
    public let code: DiagnosticCode
    public let args: [String]
    public let path: String
    public let line: Int?

    public var severity: Severity { code.severity }

    public init(_ code: DiagnosticCode, _ args: [String] = [], path: String, line: Int? = nil) {
        self.code = code
        self.args = args
        self.path = path
        self.line = line
    }

    /// 四端比對用的身分。**不含 args、不含 line、不含訊息**：
    /// args 裡有檔案內容（會隨測試資料改動），line 隨排版變。
    public var identity: String {
        "\(severity.rawValue)|\(code.rawValue)|\(path.isEmpty ? "<document>" : path)"
    }

    /// 給開發者看的英文回退。**不上使用者畫面**，UI 端請用 code 查在地化樣板。
    public var developerMessage: String {
        let a = args.joined(separator: ", ")
        let at = path.isEmpty ? "<document>" : path
        let where_ = line.map { ":\($0)" } ?? ""
        return "[\(severity.rawValue)] \(at)\(where_) \(code.rawValue)(\(a))"
    }
}

/// 診斷累積器。
public final class Diagnostics {
    private(set) public var items: [Diagnostic] = []

    public init() {}

    public func add(_ code: DiagnosticCode, _ args: [String] = [], path: String, line: Int? = nil) {
        items.append(Diagnostic(code, args, path: path, line: line))
    }

    public var hasErrors: Bool { items.contains { $0.severity == .error } }
    public var warningCount: Int { items.filter { $0.severity == .warning }.count }
    public var infoCount: Int { items.filter { $0.severity == .info }.count }
    public var errorCount: Int { items.filter { $0.severity == .error }.count }

    /// §10 檢核第 9 條要比對的東西。
    public var identities: [String] { items.map(\.identity) }
}

/// 載入結果。`value == nil` 代表致命錯誤，呼叫端**必須**退回上一個成功的主題。
public struct LoadResult<T> {
    public let value: T?
    public let diagnostics: [Diagnostic]

    public init(value: T?, diagnostics: [Diagnostic]) {
        self.value = value
        self.diagnostics = diagnostics
    }

    public var isSuccess: Bool { value != nil }
    public var errors: [Diagnostic] { diagnostics.filter { $0.severity == .error } }
    public var warnings: [Diagnostic] { diagnostics.filter { $0.severity == .warning } }
    public var infos: [Diagnostic] { diagnostics.filter { $0.severity == .info } }
}

/// 目標平台。決定 `platform_overrides` 取哪一個分支（§7.4 第 5 步）。
public enum Platform: String, Sendable, CaseIterable {
    case android, ios, macos, windows
}
