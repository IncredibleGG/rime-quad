//
//  ThemeParser.swift — 主題綁定層（docs/theme-format.md §7.4 的第 6–9 步）
//
//  輸入是已完成繼承合併與平台覆寫的節點樹，輸出是不可變的 Theme。
//
//  ⚠ 桌面端**不**綁定 `keyboard`、`feedback`、`candidates.bar` 三個區塊
//    （§1.1 的形態分野）。它們仍列在 KNOWN 集合裡，所以不會刷出
//    「unknown field」噪音，但區塊**內部**的診斷不會產生 ——
//    這正是 §10 檢核第 9 條需要按作用域切開的原因（見規範 §10 的說明）。
//
//  ⚠ `candidates.syllables`（§8.6.6.3）**不在**那三個裡面：它被完整綁定，
//    只是不渲染。§8.6.6.3.5 第 1 點明文要求桌面端這麼做，理由是欄位層級的
//    診斷屬於第 9 條的共用作用域 —— 少解析一個區塊，第 9 條就在那裡破掉。
//

import Foundation

public enum ThemeParser {

    static let docKeys: Set<String> = [
        "format", "id", "revision", "name", "description", "author", "license",
        "appearance", "counterpart", "inherits", "min_client",
        "palette", "typography", "metrics", "candidates", "preedit",
        "status_bar", "accessibility",
        "keyboard", "motion", "feedback", "platform_overrides",
    ]

    static let typographyKeys: Set<String> =
        ["respect_system_font_scale", "font_scale_min", "font_scale_max", "fonts", "assets"]
    static let fontStackKeys: Set<String> = ["family", "weight", "italic", "script_fallback"]
    static let fontAssetKeys: Set<String> = ["family", "file", "weight", "italic"]
    static let metricsKeys: Set<String> =
        ["corner_radius", "border_width", "padding", "spacing", "elevation"]

    static let candidateStyleKeys: Set<String> =
        ["orientation", "label", "text", "comment", "item", "separator", "page_indicator"]
    static let candidatesKeys: Set<String> =
        candidateStyleKeys.union(["bar", "window", "syllables"])
    /// §8.6.6.3。`orientation` **不是**欄位（由 `placement` 推導），別加進來。
    static let syllablesKeys: Set<String> = ["placement", "trigger", "max_items", "height"]
    static let windowKeys: Set<String> = candidateStyleKeys.union([
        "background", "corner_radius", "padding", "border_width", "border_color",
        "min_width", "max_width", "placement", "offset_x", "offset_y",
        "follow_caret", "backdrop", "opacity", "shadow",
        // §8.6.7.1（本輪新增）
        "lines", "equal_columns", "column_gap", "row_gap", "max_height",
        "item_align", "overflow",
    ])
    static let labelKeys: Set<String> = ["show", "format", "size", "color", "highlight_color"]
    static let textKeys: Set<String> = ["size", "color", "highlight_color"]
    static let commentKeys: Set<String> = ["show", "position", "size", "color", "highlight_color"]
    static let itemKeys: Set<String> = [
        "padding_h", "padding_v", "spacing", "corner_radius", "min_width",
        "background", "highlight_background", "border_width", "border_color",
        "highlight_border_width", "highlight_border_color",
    ]
    static let separatorKeys: Set<String> = ["show", "color", "width"]
    static let pageIndicatorKeys: Set<String> = ["show", "style", "color", "disabled_color", "size"]
    static let shadowKeys: Set<String> = ["show", "radius", "offset_x", "offset_y", "color"]
    static let preeditKeys: Set<String> = [
        "show", "size", "color", "background", "padding_h", "padding_v",
        "corner_radius", "caret", "selection",
    ]
    static let caretKeys: Set<String> = ["show", "color", "width", "blink"]
    static let selectionKeys: Set<String> = ["color", "text_color"]
    static let statusBarKeys: Set<String> = [
        "show", "position", "background", "height", "padding_h", "padding_v",
        "spacing", "arrangement", "size", "color", "active_color", "separator", "items",
    ]
    static let statusItemKeys: Set<String> = ["source", "text", "tap"]
    static let accessibilityKeys: Set<String> = [
        "announce_candidates", "candidate_announcement", "announce_input_mode", "announce_page",
    ]
    static let motionKeys: Set<String> = [
        "enabled", "respect_reduce_motion", "curve", "key_press_ms", "key_release_ms",
        "candidate_change_ms", "popup_ms", "window_show_ms",
    ]

    static let knownScripts: Set<String> = ["latn", "hant", "hans", "jpan", "kore", "bopo", "zyyy"]

    /// §8.4：必須存在的具名字體堆疊。
    static let requiredFontNames = ["ui", "candidate", "label", "comment", "key", "preedit"]

    public static func bind(_ root: YamlNode, id: String, ancestry: [String],
                            ctx: ParseContext) -> Theme {
        let c = Cursor.root(root, ctx: ctx)
        c.warnUnknownKeys(docKeys)

        // 第 7 步：palette 必須先解析，之後所有 color 欄位才能解析 $ref。
        ctx.palette = PaletteResolver.resolve(c.child("palette").asMapping())

        var theme = Theme(id: id)
        theme.ancestry = ancestry
        theme.palette = ctx.palette
        theme.revision = c.child("revision").int(1, min: 1)
        theme.name = c.child("name").localized()
        theme.description = c.child("description").localized()
        theme.author = c.child("author").string("")
        theme.license = c.child("license").string("")
        theme.appearance = Appearance(rawValue:
            c.child("appearance").enumOf(["light", "dark"], "light")) ?? .light
        theme.counterpart = c.child("counterpart").stringOrNil()

        theme.metrics = parseMetrics(c.mapping("metrics"))
        theme.typography = parseTypography(c.mapping("typography"))

        let candidatesCursor = c.mapping("candidates")
        candidatesCursor.warnUnknownKeys(candidatesKeys)
        let shared = parseCandidateStyle(candidatesCursor, base: CandidateStyle.defaults(theme.metrics))
        theme.candidates = shared

        let windowCursor = candidatesCursor.mapping("window")
        windowCursor.warnUnknownKeys(windowKeys)
        theme.window = parseWindow(windowCursor, shared: shared, m: theme.metrics)

        theme.syllables = parseSyllables(candidatesCursor.mapping("syllables"))

        theme.preedit = parsePreedit(c.mapping("preedit"), m: theme.metrics)
        theme.statusBar = parseStatusBar(c.mapping("status_bar"), m: theme.metrics, ctx: ctx)
        theme.accessibility = parseAccessibility(c.mapping("accessibility"))
        theme.motion = parseMotion(c.mapping("motion"))
        return theme
    }

    // ───────────────────── metrics / typography ─────────────────────

    static func parseMetrics(_ c: Cursor) -> Metrics {
        c.warnUnknownKeys(metricsKeys)
        var m = Metrics()
        m.cornerRadius = c.child("corner_radius").length(m.cornerRadius, min: 0, max: 64)
        m.borderWidth = c.child("border_width").length(m.borderWidth, min: 0, max: 8)
        m.padding = c.child("padding").length(m.padding, min: 0, max: 64)
        m.spacing = c.child("spacing").length(m.spacing, min: 0, max: 64)
        m.elevation = c.child("elevation").length(m.elevation, min: 0, max: 32)
        return m
    }

    static func parseTypography(_ c: Cursor) -> Typography {
        c.warnUnknownKeys(typographyKeys)
        var t = Typography()
        let fontsCursor = c.mapping("fonts")
        for name in fontsCursor.keys() {
            t.fonts[name] = parseFontStack(fontsCursor.mapping(name))
            t.fontOrder.append(name)
        }
        for name in requiredFontNames where t.fonts[name] == nil {
            t.fonts[name] = FontStack()
            t.fontOrder.append(name)
        }
        for a in c.child("assets").items() {
            let m = a.asMapping()
            m.warnUnknownKeys(fontAssetKeys)
            let family = m.child("family").string("")
            let file = m.child("file").string("")
            if family.isEmpty || file.isEmpty {
                c.diag.add(.assetIncomplete, [], path: a.path, line: a.node?.line)
                continue
            }
            if file.hasPrefix("/") || file.contains("..") {
                c.diag.add(.assetPathEscape, [file], path: a.path, line: a.node?.line)
                continue
            }
            t.assets.append(FontAsset(family: family, file: file,
                                      weight: m.child("weight").int(400, min: 100, max: 900),
                                      italic: m.child("italic").bool(false)))
        }
        t.respectSystemFontScale = c.child("respect_system_font_scale").bool(true)
        t.fontScaleMin = c.child("font_scale_min").number(0.85, min: 0.5, max: 1.0)
        t.fontScaleMax = c.child("font_scale_max").number(1.30, min: 1.0, max: 2.0)
        return t
    }

    static func parseFontStack(_ c: Cursor) -> FontStack {
        c.warnUnknownKeys(fontStackKeys)
        var f = FontStack()
        let fb = c.mapping("script_fallback")
        for script in fb.keys() {
            guard knownScripts.contains(script) else {
                c.diag.add(.unknownScriptTag, [script], path: "\(fb.path).\(script)")
                continue
            }
            f.scriptFallback[script] = fb.child(script).stringList([])
        }
        f.family = c.child("family").stringList(["$system"])
        f.weight = roundToHundred(c.child("weight").int(400, min: 100, max: 900))
        f.italic = c.child("italic").bool(false)
        return f
    }

    static func roundToHundred(_ v: Int) -> Int { clamp(((v + 50) / 100) * 100, 100, 900) }

    // ───────────────────── candidates ─────────────────────

    /// `key` 缺席但同區塊的 `sourceKey` 有明確設定時，改用 sourceKey 的解析結果當預設。
    /// （規範裡「`highlight_color` 預設 = `color`」的那一格。）
    static func derivedColor(_ c: Cursor, _ key: String, _ sourceKey: String,
                             _ resolvedSource: RGBA, _ base: RGBA) -> RGBA {
        let self_ = c.child(key)
        if self_.exists { return self_.color(base) }
        return c.child(sourceKey).exists ? resolvedSource : base
    }

    static func parseCandidateStyle(_ c: Cursor, base: CandidateStyle) -> CandidateStyle {
        var s = base
        s.orientation = Orientation(rawValue:
            c.child("orientation").enumOf(["horizontal", "vertical"],
                                          base.orientation.rawValue)) ?? base.orientation

        let l = c.mapping("label")
        l.warnUnknownKeys(labelKeys)
        let labelColor = l.child("color").color(base.label.color)
        s.label.show = l.child("show").bool(base.label.show)
        s.label.format = l.child("format").string(base.label.format)
        s.label.size = l.child("size").size(base.label.size)
        s.label.color = labelColor
        s.label.highlightColor = derivedColor(l, "highlight_color", "color",
                                              labelColor, base.label.highlightColor)

        let t = c.mapping("text")
        t.warnUnknownKeys(textKeys)
        let textColor = t.child("color").color(base.text.color)
        s.text.size = t.child("size").size(base.text.size)
        s.text.color = textColor
        s.text.highlightColor = derivedColor(t, "highlight_color", "color",
                                             textColor, base.text.highlightColor)

        let m = c.mapping("comment")
        m.warnUnknownKeys(commentKeys)
        let commentColor = m.child("color").color(base.comment.color)
        s.comment.show = m.child("show").bool(base.comment.show)
        s.comment.position = CommentPosition(rawValue:
            m.child("position").enumOf(["after", "below", "hidden"],
                                       base.comment.position.rawValue)) ?? base.comment.position
        s.comment.size = m.child("size").size(base.comment.size)
        s.comment.color = commentColor
        s.comment.highlightColor = derivedColor(m, "highlight_color", "color",
                                                commentColor, base.comment.highlightColor)

        let i = c.mapping("item")
        i.warnUnknownKeys(itemKeys)
        s.item.paddingH = i.child("padding_h").length(base.item.paddingH, min: 0, max: 128)
        s.item.paddingV = i.child("padding_v").length(base.item.paddingV, min: 0, max: 128)
        s.item.spacing = i.child("spacing").length(base.item.spacing, min: 0, max: 128)
        s.item.cornerRadius = i.child("corner_radius").length(base.item.cornerRadius, min: 0, max: 64)
        s.item.minWidth = i.child("min_width").length(base.item.minWidth, min: 0, max: 512)
        s.item.background = i.child("background").color(base.item.background)
        s.item.highlightBackground = i.child("highlight_background").color(base.item.highlightBackground)
        s.item.borderWidth = i.child("border_width").length(base.item.borderWidth, min: 0, max: 8)
        s.item.borderColor = i.child("border_color").color(base.item.borderColor)
        s.item.highlightBorderWidth = i.child("highlight_border_width")
            .length(base.item.highlightBorderWidth, min: 0, max: 8)
        s.item.highlightBorderColor = i.child("highlight_border_color")
            .color(base.item.highlightBorderColor)

        let sep = c.mapping("separator")
        sep.warnUnknownKeys(separatorKeys)
        s.separator.show = sep.child("show").bool(base.separator.show)
        s.separator.color = sep.child("color").color(base.separator.color)
        s.separator.width = sep.child("width").length(base.separator.width, min: 0, max: 8)

        let p = c.mapping("page_indicator")
        p.warnUnknownKeys(pageIndicatorKeys)
        let pageColor = p.child("color").color(base.pageIndicator.color)
        s.pageIndicator.show = p.child("show").bool(base.pageIndicator.show)
        s.pageIndicator.kind = PageIndicatorKind(rawValue:
            p.child("style").enumOf(["arrows", "dots", "text", "none"],
                                    base.pageIndicator.kind.rawValue)) ?? base.pageIndicator.kind
        s.pageIndicator.color = pageColor
        s.pageIndicator.disabledColor = derivedColor(p, "disabled_color", "color",
                                                     pageColor, base.pageIndicator.disabledColor)
        s.pageIndicator.size = p.child("size").size(base.pageIndicator.size)
        return s
    }

    /// §8.6.6.3 逐音節消歧欄。**桌面端解析它但不渲染它**（§8.6.6.3.5）。
    ///
    /// 為什麼要解析一個不畫的東西：§8.6.6.3.5 第 1 點。欄位層級的診斷屬於
    /// §10 第 9 條的共用作用域，把整個區塊當成「已知但不進入」（`candidates.bar`
    /// 的做法）會讓 `placemnt:` 這種拼字錯誤在四端報出不同的則數。
    ///
    /// ⚠ 這裡**不得**產生 `feature_unsupported`：`placement` 的預設值是
    ///   `keyboard_slot`，所以每一份主題都會命中 —— 每次載入刷一則 INFO，
    ///   而主題作者沒有做錯任何事（§8.6.6.3.5 第 3 點、§9.5.1 第二條紀律）。
    ///
    /// ⚠ 也**不得**在這裡跑 §8.6.6.3.3 的退化規則：D1/D3/D4 的輸入是佈局與
    ///   當前方案，桌面端連 `core/layouts/` 都不消費，算不出來也不該假裝算得出來。
    static func parseSyllables(_ c: Cursor) -> SyllableBar {
        c.warnUnknownKeys(syllablesKeys)
        var s = SyllableBar()
        s.placement = SyllablePlacement(rawValue:
            c.child("placement").enumOf(["none", "above_candidates", "keyboard_slot"],
                                        s.placement.rawValue)) ?? s.placement
        s.trigger = SyllableTrigger(rawValue:
            c.child("trigger").enumOf(["while_composing", "on_demand"],
                                      s.trigger.rawValue)) ?? s.trigger
        s.maxItems = c.child("max_items").int(0, min: 0, max: 32)
        s.height = c.child("height").length(40, min: 24, max: 96)
        return s
    }

    static func parseWindow(_ c: Cursor, shared: CandidateStyle, m: Metrics) -> CandidateWindow {
        var w = CandidateWindow()
        w.style = parseCandidateStyle(c, base: shared)
        w.background = c.child("background").color(RGBA.hex(0xFFFFFFFF))
        w.cornerRadius = c.child("corner_radius").length(m.cornerRadius, min: 0, max: 64)
        w.padding = c.child("padding").length(m.padding, min: 0, max: 64)
        w.borderWidth = c.child("border_width").length(m.borderWidth, min: 0, max: 8)
        w.borderColor = c.child("border_color").color(.transparent)
        w.minWidth = c.child("min_width").length(0, min: 0, max: 2048)
        w.maxWidth = c.child("max_width").length(640, min: 0, max: 4096)
        w.placement = Placement(rawValue:
            c.child("placement").enumOf(["below", "above", "auto"], "auto")) ?? .auto
        w.offsetX = c.child("offset_x").length(0, min: -64, max: 64)
        w.offsetY = c.child("offset_y").length(6, min: -64, max: 64)
        w.followCaret = c.child("follow_caret").bool(true)
        w.backdrop = Backdrop(rawValue:
            c.child("backdrop").enumOf(["none", "blur", "vibrancy"], "none")) ?? .none
        w.opacity = c.child("opacity").ratio(1.0)

        let sh = c.mapping("shadow")
        sh.warnUnknownKeys(shadowKeys)
        w.shadow.show = sh.child("show").bool(true)
        w.shadow.radius = sh.child("radius").length(18, min: 0, max: 64)
        w.shadow.offsetX = sh.child("offset_x").length(0, min: -64, max: 64)
        w.shadow.offsetY = sh.child("offset_y").length(4, min: -64, max: 64)
        w.shadow.color = sh.child("color").color(RGBA.hex(0x00000040))

        // §8.6.7.1（本輪新增）
        w.lines = c.child("lines").int(1, min: 0, max: 16)
        w.equalColumns = c.child("equal_columns").bool(true)
        w.columnGap = c.child("column_gap").length(w.style.item.spacing, min: 0, max: 128)
        w.rowGap = c.child("row_gap").length(w.style.item.spacing, min: 0, max: 128)
        w.maxHeight = c.child("max_height").length(0, min: 0, max: 4096)
        w.itemAlign = ItemAlign(rawValue:
            c.child("item_align").enumOf(["leading", "center", "trailing"], "leading")) ?? .leading
        w.overflow = Overflow(rawValue:
            c.child("overflow").enumOf(["shrink", "clip"], "shrink")) ?? .shrink
        return w
    }

    // ───────────────────── preedit / status_bar ─────────────────────

    static func parsePreedit(_ c: Cursor, m: Metrics) -> Preedit {
        c.warnUnknownKeys(preeditKeys)
        var p = Preedit()
        p.color = c.child("color").color(RGBA.hex(0x404040FF))
        p.show = c.child("show").bool(true)
        p.size = c.child("size").size(16)
        p.background = c.child("background").color(.transparent)
        p.paddingH = c.child("padding_h").length(m.padding, min: 0, max: 128)
        p.paddingV = c.child("padding_v").length(m.padding, min: 0, max: 128)
        p.cornerRadius = c.child("corner_radius").length(0, min: 0, max: 64)

        let caret = c.mapping("caret")
        caret.warnUnknownKeys(caretKeys)
        p.caret.show = caret.child("show").bool(true)
        p.caret.color = caret.child("color").color(RGBA.hex(0x3060C0FF))
        p.caret.width = caret.child("width").length(1.5, min: 0, max: 4)
        p.caret.blink = caret.child("blink").bool(true)

        let sel = c.mapping("selection")
        sel.warnUnknownKeys(selectionKeys)
        p.selection.color = sel.child("color").color(RGBA.hex(0x3060C040))
        p.selection.textColor = sel.child("text_color").color(p.color)
        return p
    }

    static func parseStatusBar(_ c: Cursor, m: Metrics, ctx: ParseContext) -> StatusBar {
        c.warnUnknownKeys(statusBarKeys)
        var s = StatusBar()
        s.show = c.child("show").bool(false)
        s.position = StatusPosition(rawValue:
            c.child("position").enumOf(["top", "bottom"], "bottom")) ?? .bottom
        s.background = c.child("background").color(.transparent)
        s.height = c.child("height").length(0, min: 0, max: 64)
        s.paddingH = c.child("padding_h").length(m.padding, min: 0, max: 64)
        s.paddingV = c.child("padding_v").length(2, min: 0, max: 64)
        s.spacing = c.child("spacing").length(m.spacing, min: 0, max: 64)
        s.arrangement = Arrangement(rawValue:
            c.child("arrangement").enumOf(["leading", "center", "trailing", "space_between"],
                                          "leading")) ?? .leading
        s.size = c.child("size").size(11)
        let color = c.child("color").color(RGBA.hex(0x808080FF))
        s.color = color
        s.activeColor = derivedColor(c, "active_color", "color", color, color)

        let sep = c.mapping("separator")
        sep.warnUnknownKeys(separatorKeys)
        s.separator.show = sep.child("show").bool(false)
        s.separator.color = sep.child("color").color(RGBA.hex(0x808080FF))
        s.separator.width = sep.child("width").length(1, min: 0, max: 8)

        let itemsCursor = c.child("items")
        if !itemsCursor.exists { return s }

        var items: [StatusItem] = []
        for entry in itemsCursor.items() {
            let mm = entry.asMapping()
            mm.warnUnknownKeys(statusItemKeys)
            guard let raw = mm.child("source").stringOrNil(),
                  let source = StatusSource(rawValue: raw) else {
                ctx.diagnostics.add(.statusItemNoSource, [], path: entry.path, line: entry.node?.line)
                continue
            }
            var item = StatusItem(source: source)
            item.text = mm.child("text").string("")
            let tapCursor = mm.child("tap")
            if let tapRaw = tapCursor.stringOrNil() {
                item.tap = Actions.parse(tapRaw, path: tapCursor.path,
                                         diag: ctx.diagnostics, line: tapCursor.node?.line)
            }
            items.append(item)
        }
        s.items = items
        return s
    }

    static func parseAccessibility(_ c: Cursor) -> AccessibilitySpec {
        c.warnUnknownKeys(accessibilityKeys)
        var a = AccessibilitySpec()
        a.announceCandidates = AnnounceLevel(rawValue:
            c.child("announce_candidates").enumOf(["full", "text_only", "none"], "full")) ?? .full
        a.candidateAnnouncement = c.child("candidate_announcement").string(a.candidateAnnouncement)
        a.announceInputMode = c.child("announce_input_mode").bool(true)
        a.announcePage = c.child("announce_page").bool(true)
        return a
    }

    static func parseMotion(_ c: Cursor) -> Motion {
        c.warnUnknownKeys(motionKeys)
        var m = Motion()
        m.enabled = c.child("enabled").bool(true)
        m.respectReduceMotion = c.child("respect_reduce_motion").bool(true)
        m.candidateChangeMs = c.child("candidate_change_ms").int(90, min: 0, max: 5000)
        m.windowShowMs = c.child("window_show_ms").int(90, min: 0, max: 5000)
        return m
    }
}
