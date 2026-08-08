//
//  ThemeModel.swift — 主題的結果物件（docs/theme-format.md §8）
//
//  ⚠ 桌面端**不**建立 `keyboard` 與 `feedback` 的模型（§1.1：桌面端整段忽略），
//    也完全不碰 core/layouts/。這不是偷懶，是規範裡結構性的分野。
//    「不消費」不等於「可以刪」—— 使用者的行動端設定必須原樣搬運。
//

import Foundation

public enum Appearance: String, Sendable { case light, dark }
public enum Orientation: String, Sendable { case horizontal, vertical }
public enum CommentPosition: String, Sendable { case after, below, hidden }
public enum PageIndicatorKind: String, Sendable { case arrows, dots, text, none }
public enum Placement: String, Sendable { case below, above, auto }
public enum Backdrop: String, Sendable { case none, blur, vibrancy }
/// §8.6.7.1（本輪新增）：格內對齊。
public enum ItemAlign: String, Sendable { case leading, center, trailing }
/// §8.6.7.1（本輪新增）：超出 `max_width` 時的處置。
public enum Overflow: String, Sendable { case shrink, clip }
/// §8.12（本輪新增）：狀態列的排列。
public enum Arrangement: String, Sendable { case leading, center, trailing, spaceBetween = "space_between" }
public enum StatusPosition: String, Sendable { case top, bottom }
/// §8.13（本輪新增）：候選朗讀的詳細程度。
public enum AnnounceLevel: String, Sendable { case full, textOnly = "text_only", none }

public struct Metrics: Sendable {
    public var cornerRadius = 8.0
    public var borderWidth = 0.0
    public var padding = 6.0
    public var spacing = 4.0
    public var elevation = 2.0
}

public struct FontStack: Sendable {
    public var family: [String] = ["$system"]
    public var weight = 400
    public var italic = false
    public var scriptFallback: [String: [String]] = [:]
}

public struct FontAsset: Sendable {
    public var family: String
    public var file: String
    public var weight: Int
    public var italic: Bool
}

public struct Typography: Sendable {
    public var respectSystemFontScale = true
    public var fontScaleMin = 0.85
    public var fontScaleMax = 1.30
    public var fonts: [String: FontStack] = [:]
    public var fontOrder: [String] = []
    public var assets: [FontAsset] = []

    /// §4.4：macOS 的 `system_font_scale` 固定為 1.0（系統無等價設定），
    /// 所以 `effective_scale` 恆為 1.0，而且 AppKit 不會自己再套一次。
    /// 這個函式存在只是為了讓那條規則在程式碼裡有一個名字，不是散落的魔術數。
    public func effectiveScale(systemFontScale: Double = 1.0) -> Double {
        respectSystemFontScale ? clamp(systemFontScale, fontScaleMin, fontScaleMax) : 1.0
    }
}

public struct LabelStyle: Sendable {
    public var show = true
    public var format = "{label}"
    public var size = 12.0
    public var color = RGBA.hex(0x808080FF)
    public var highlightColor = RGBA.hex(0x808080FF)
}

public struct TextStyle: Sendable {
    public var size = 20.0
    public var color = RGBA.hex(0x000000FF)
    public var highlightColor = RGBA.hex(0x000000FF)
}

public struct CommentStyle: Sendable {
    public var show = true
    public var position = CommentPosition.after
    public var size = 12.0
    public var color = RGBA.hex(0x808080FF)
    public var highlightColor = RGBA.hex(0x808080FF)
}

public struct ItemStyle: Sendable {
    public var paddingH = 6.0
    public var paddingV = 6.0
    public var spacing = 4.0
    public var cornerRadius = 8.0
    public var minWidth = 0.0
    public var background = RGBA.transparent
    public var highlightBackground = RGBA.hex(0x3060C0FF)
    public var borderWidth = 0.0
    public var borderColor = RGBA.transparent
    public var highlightBorderWidth = 0.0
    public var highlightBorderColor = RGBA.transparent
}

public struct SeparatorStyle: Sendable {
    public var show = false
    public var color = RGBA.hex(0x808080FF)
    public var width = 1.0
}

public struct PageIndicatorStyle: Sendable {
    public var show = true
    public var kind = PageIndicatorKind.arrows
    public var color = RGBA.hex(0x808080FF)
    public var disabledColor = RGBA.hex(0x808080FF)
    public var size = 14.0
}

/// §8.6.1–8.6.5：`candidates` / `candidates.bar` / `candidates.window` 共用的部分。
public struct CandidateStyle: Sendable {
    public var orientation = Orientation.horizontal
    public var label = LabelStyle()
    public var text = TextStyle()
    public var comment = CommentStyle()
    public var item = ItemStyle()
    public var separator = SeparatorStyle()
    public var pageIndicator = PageIndicatorStyle()

    public static func defaults(_ m: Metrics) -> CandidateStyle {
        var s = CandidateStyle()
        s.item.paddingH = m.padding
        s.item.paddingV = m.padding
        s.item.spacing = m.spacing
        s.item.cornerRadius = m.cornerRadius
        return s
    }
}

public struct Shadow: Sendable {
    public var show = true
    public var radius = 18.0
    public var offsetX = 0.0
    public var offsetY = 4.0
    public var color = RGBA.hex(0x00000040)
}

/// §8.6.7 桌面候選窗。`lines` 之後的欄位是本輪新增的 §8.6.7.1。
public struct CandidateWindow: Sendable {
    public var style = CandidateStyle()
    public var background = RGBA.hex(0xFFFFFFFF)
    public var cornerRadius = 8.0
    public var padding = 6.0
    public var borderWidth = 0.0
    public var borderColor = RGBA.transparent
    public var minWidth = 0.0
    public var maxWidth = 640.0
    public var placement = Placement.auto
    public var offsetX = 0.0
    public var offsetY = 6.0
    public var followCaret = true
    public var backdrop = Backdrop.none
    public var opacity = 1.0
    public var shadow = Shadow()

    // ── §8.6.7.1 多行／表格排版 ────────────────────────────────
    /// 次要軸上的行數。`1` = v1 的單行行為；`0` = 自動（在 `max_width` 內塞得下多少排多少）。
    public var lines = 1
    public var equalColumns = true
    public var columnGap = 4.0
    public var rowGap = 4.0
    public var maxHeight = 0.0
    public var itemAlign = ItemAlign.leading
    public var overflow = Overflow.shrink
}

/// §8.12（本輪新增）狀態列項目的來源。
public enum StatusSource: String, Sendable {
    case schemaName = "schema_name"
    case schemaId = "schema_id"
    case inputMode = "input_mode"
    case inputModePair = "input_mode_pair"
    case shape
    case variant
    case page
    case text
}

public struct StatusItem: Sendable {
    public var source: StatusSource
    public var text: String = ""
    public var tap: KeyAction?

    public init(source: StatusSource, text: String = "", tap: KeyAction? = nil) {
        self.source = source
        self.text = text
        self.tap = tap
    }
}

/// §8.12（本輪新增）桌面候選窗的狀態列。
public struct StatusBar: Sendable {
    public var show = false
    public var position = StatusPosition.bottom
    public var background = RGBA.transparent
    public var height = 0.0
    public var paddingH = 6.0
    public var paddingV = 2.0
    public var spacing = 4.0
    public var arrangement = Arrangement.leading
    public var size = 11.0
    public var color = RGBA.hex(0x808080FF)
    public var activeColor = RGBA.hex(0x808080FF)
    public var separator = SeparatorStyle()
    public var items: [StatusItem] = StatusBar.defaultItems

    /// §8.12 的規範性預設清單。`items` 缺席時必須產生這一份。
    public static let defaultItems: [StatusItem] = [
        StatusItem(source: .schemaName, tap: KeyAction(.schemaPicker, raw: "schema:picker")),
        StatusItem(source: .inputModePair, tap: KeyAction(.inputModeToggle, raw: "input_mode:toggle")),
        StatusItem(source: .variant, tap: KeyAction(.toggleOption, ["simplification"],
                                                    raw: "toggle:simplification")),
        StatusItem(source: .page),
    ]
}

public struct Caret: Sendable {
    public var show = true
    public var color = RGBA.hex(0x3060C0FF)
    public var width = 1.5
    public var blink = true
}

public struct SelectionStyle: Sendable {
    public var color = RGBA.hex(0x3060C040)
    public var textColor = RGBA.hex(0x404040FF)
}

public struct Preedit: Sendable {
    public var show = true
    public var size = 16.0
    public var color = RGBA.hex(0x404040FF)
    public var background = RGBA.transparent
    public var paddingH = 6.0
    public var paddingV = 6.0
    public var cornerRadius = 0.0
    public var caret = Caret()
    public var selection = SelectionStyle()
}

/// §8.13（本輪新增）無障礙朗讀。
public struct AccessibilitySpec: Sendable {
    public var announceCandidates = AnnounceLevel.full
    public var candidateAnnouncement = "{label} {text} {comment}"
    public var announceInputMode = true
    public var announcePage = true
}

public struct Motion: Sendable {
    public var enabled = true
    public var respectReduceMotion = true
    public var candidateChangeMs = 90
    public var windowShowMs = 90
}

public struct Theme: Sendable {
    public var id: String
    public var revision = 1
    public var name = LocalizedString.empty
    public var description = LocalizedString.empty
    public var author = ""
    public var license = ""
    public var appearance = Appearance.light
    public var counterpart: String?
    public var ancestry: [String] = []
    public var palette: [String: RGBA] = [:]
    public var typography = Typography()
    public var metrics = Metrics()
    public var candidates = CandidateStyle()
    public var window = CandidateWindow()
    public var statusBar = StatusBar()
    public var preedit = Preedit()
    public var accessibility = AccessibilitySpec()
    public var motion = Motion()

    public init(id: String) { self.id = id }
}
