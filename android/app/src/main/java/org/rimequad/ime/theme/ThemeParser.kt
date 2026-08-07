package org.rimequad.ime.theme

/** 規範 §8 欄位表所列的內建預設值。這是「欄位缺失 → 採預設值」的唯一來源。 */
object ThemeDefaults {

    val METRICS = Metrics(
        cornerRadius = 8f, borderWidth = 0f, padding = 6f, spacing = 4f, elevation = 2f
    )

    val FONT_STACK = FontStack(
        family = listOf("\$system"), weight = 400, italic = false, scriptFallback = emptyMap()
    )

    /** §8.4 規定必須存在的具名字體堆疊。 */
    val REQUIRED_FONT_NAMES = listOf("ui", "candidate", "label", "comment", "key", "preedit")

    /** §8.8.1 規定必須存在的具名 key style。 */
    val REQUIRED_KEY_STYLES = listOf("default", "modifier", "action", "space", "accent")

    private fun item(icon: String?, from: LabelSource, verb: ActionVerb, raw: String, vararg args: String) =
        ToolbarItem(icon, "", from, KeyAction(verb, args.toList(), raw))

    /** §8.6.6.1 的規範性預設工具列。`items` 缺席時必須產生這一份。 */
    val TOOLBAR_ITEMS: List<ToolbarItem> = listOf(
        item("globe", LabelSource.NONE, ActionVerb.SCHEMA_PICKER, "schema:picker"),
        item(null, LabelSource.INPUT_MODE, ActionVerb.TOGGLE_OPTION, "toggle:ascii_mode", "ascii_mode"),
        item(null, LabelSource.VARIANT, ActionVerb.TOGGLE_OPTION, "toggle:simplification", "simplification"),
        item("emoji", LabelSource.NONE, ActionVerb.EMOJI, "emoji"),
        item("settings", LabelSource.NONE, ActionVerb.SETTINGS, "settings"),
        item("keyboard_hide", LabelSource.NONE, ActionVerb.HIDE_KEYBOARD, "hide_keyboard")
    )

    /**
     * §8.6.6.1 的必備項：不論主題怎麼寫，使用者都必須能觸達這兩個。
     * `schema:picker` 是方案切換的唯一入口，`settings` 是修好其他一切問題的入口;
     * 允許主題刪掉它們,等於允許主題把使用者鎖死在一個他無法離開的方案裡。
     */
    val REQUIRED_TOOLBAR_ITEMS: List<ToolbarItem> = listOf(
        TOOLBAR_ITEMS[0],
        TOOLBAR_ITEMS[4]
    )

    val TOOLBAR = Toolbar(show = true, items = TOOLBAR_ITEMS)

    val KEY_STYLE = KeyStyle(
        background = 0xFFFFFFFF.toInt(),
        pressedBackground = 0xFFFFFFFF.toInt(),
        foreground = 0xFF000000.toInt(),
        pressedForeground = 0xFF000000.toInt(),
        activeBackground = 0xFFFFFFFF.toInt(),
        activeForeground = 0xFF000000.toInt(),
        cornerRadius = 8f,
        borderWidth = 0f,
        borderColor = ColorSpec.TRANSPARENT,
        elevation = 2f,
        font = "key",
        labelSize = 22f,
        hintSize = 10f,
        hintColor = scaleAlpha(0xFF000000.toInt(), 0.6f),
        hintPosition = HintPosition.TOP_RIGHT,
        iconSize = 22f
    )

    fun keyStyle(m: Metrics): KeyStyle = KEY_STYLE.copy(
        cornerRadius = m.cornerRadius, borderWidth = m.borderWidth, elevation = m.elevation
    )

    fun candidateStyle(m: Metrics): CandidateStyle = CandidateStyle(
        orientation = Orientation.HORIZONTAL,
        label = LabelStyle(
            show = true, format = "{label}", size = 12f,
            color = 0xFF808080.toInt(), highlightColor = 0xFF808080.toInt()
        ),
        text = TextStyle(size = 20f, color = 0xFF000000.toInt(), highlightColor = 0xFF000000.toInt()),
        comment = CommentStyle(
            show = true, position = CommentPosition.AFTER, size = 12f,
            color = 0xFF808080.toInt(), highlightColor = 0xFF808080.toInt()
        ),
        item = ItemStyle(
            paddingH = m.padding, paddingV = m.padding, spacing = m.spacing,
            cornerRadius = m.cornerRadius, minWidth = 0f,
            background = ColorSpec.TRANSPARENT,
            highlightBackground = 0xFF3060C0.toInt(),
            borderWidth = 0f, borderColor = ColorSpec.TRANSPARENT,
            highlightBorderWidth = 0f, highlightBorderColor = ColorSpec.TRANSPARENT
        ),
        separator = SeparatorStyle(show = false, color = 0xFF808080.toInt(), width = 1f),
        pageIndicator = PageIndicatorStyle(
            show = true, kind = PageIndicatorKind.ARROWS,
            color = 0xFF808080.toInt(), disabledColor = 0xFF808080.toInt(), size = 14f
        )
    )
}

internal fun scaleAlpha(argb: Int, f: Float): Int {
    var a = ((((argb ushr 24) and 0xFF).toFloat()) * f + 0.5f).toInt()
    if (a < 0) a = 0
    if (a > 255) a = 255
    return (a shl 24) or (argb and 0x00FFFFFF)
}

internal inline fun <reified E : Enum<E>> Cursor.enumValue(default: E): E {
    val all = enumValues<E>()
    val names = all.map { it.name.lowercase() }
    val chosen = this.enumOf(names, default.name.lowercase())
    for (e in all) if (e.name.lowercase() == chosen) return e
    return default
}

/**
 * 主題綁定層（規範 §7.4 的第 6–9 步）。
 * 輸入是已完成繼承合併與平台覆寫的節點樹，輸出是不可變的 [Theme]。
 */
object ThemeParser {

    private val DOC_KEYS = setOf(
        "format", "id", "revision", "name", "description", "author", "license",
        "appearance", "counterpart", "inherits", "min_client",
        "palette", "typography", "metrics", "candidates", "preedit",
        "keyboard", "motion", "feedback", "platform_overrides"
    )

    private val TYPOGRAPHY_KEYS = setOf(
        "respect_system_font_scale", "font_scale_min", "font_scale_max", "fonts", "assets"
    )
    private val FONT_STACK_KEYS = setOf("family", "weight", "italic", "script_fallback")
    private val FONT_ASSET_KEYS = setOf("family", "file", "weight", "italic")
    private val METRICS_KEYS = setOf("corner_radius", "border_width", "padding", "spacing", "elevation")

    private val CANDIDATE_STYLE_KEYS =
        setOf("orientation", "label", "text", "comment", "item", "separator", "page_indicator")
    private val CANDIDATES_KEYS = CANDIDATE_STYLE_KEYS + setOf("bar", "window")
    private val BAR_KEYS = CANDIDATE_STYLE_KEYS + setOf(
        "height", "background", "border_top_width", "border_top_color", "max_visible",
        "scroll", "expand_button", "show_preedit_inline", "empty_shows_toolbar", "toolbar"
    )
    private val TOOLBAR_KEYS = setOf("show", "items")
    private val TOOLBAR_ITEM_KEYS = setOf("icon", "label", "label_from", "tap")
    private val WINDOW_KEYS = CANDIDATE_STYLE_KEYS + setOf(
        "background", "corner_radius", "padding", "border_width", "border_color",
        "min_width", "max_width", "placement", "offset_x", "offset_y",
        "follow_caret", "backdrop", "opacity", "shadow"
    )
    private val LABEL_KEYS = setOf("show", "format", "size", "color", "highlight_color")
    private val TEXT_KEYS = setOf("size", "color", "highlight_color")
    private val COMMENT_KEYS = setOf("show", "position", "size", "color", "highlight_color")
    private val ITEM_KEYS = setOf(
        "padding_h", "padding_v", "spacing", "corner_radius", "min_width",
        "background", "highlight_background", "border_width", "border_color",
        "highlight_border_width", "highlight_border_color"
    )
    private val SEPARATOR_KEYS = setOf("show", "color", "width")
    private val PAGE_INDICATOR_KEYS = setOf("show", "style", "color", "disabled_color", "size")
    private val SHADOW_KEYS = setOf("show", "radius", "offset_x", "offset_y", "color")
    private val EXPAND_BUTTON_KEYS = setOf("show", "color", "size")

    private val PREEDIT_KEYS = setOf(
        "show", "size", "color", "background", "padding_h", "padding_v",
        "corner_radius", "caret", "selection"
    )
    private val CARET_KEYS = setOf("show", "color", "width", "blink")
    private val SELECTION_KEYS = setOf("color", "text_color")

    private val KEYBOARD_KEYS = setOf(
        "background", "key_aspect", "key_height", "reference_grid", "row_height",
        "max_screen_ratio", "padding",
        "row_spacing", "key_spacing", "honor_bottom_inset", "key_styles",
        "popup", "press_preview",
        // 已被取代，仍列為「已知」以免產生 unknown field 噪音；見 §8.8.0.2
        "height"
    )
    private val KEY_HEIGHT_KEYS = setOf("min", "max")
    private val ROW_HEIGHT_KEYS = setOf("min", "max")
    private val REFERENCE_GRID_KEYS = setOf("units", "rows")
    private val MAX_SCREEN_RATIO_KEYS = setOf("portrait", "landscape")
    private val PADDING_KEYS = setOf("left", "top", "right", "bottom")
    private val KEY_STYLE_KEYS = setOf(
        "background", "pressed_background", "foreground", "pressed_foreground",
        "active_background", "active_foreground", "corner_radius", "border_width",
        "border_color", "elevation", "font", "label_size", "hint_size", "hint_color",
        "hint_position", "icon_size"
    )
    private val POPUP_KEYS = setOf(
        "show", "background", "foreground", "highlight_background", "highlight_foreground",
        "corner_radius", "item_size", "item_padding", "elevation", "max_columns"
    )
    private val PRESS_PREVIEW_KEYS = setOf(
        "show", "background", "foreground", "size", "corner_radius", "elevation"
    )
    private val MOTION_KEYS = setOf(
        "enabled", "respect_reduce_motion", "curve", "key_press_ms", "key_release_ms",
        "candidate_change_ms", "popup_ms", "window_show_ms"
    )
    private val FEEDBACK_KEYS = setOf("haptic", "haptic_strength", "sound", "sound_volume")

    private val KNOWN_SCRIPTS = setOf("latn", "hant", "hans", "jpan", "kore", "bopo", "zyyy")

    fun bind(root: YamlNode.Mapping, id: String, ancestry: List<String>, ctx: ParseContext): Theme {
        val c = Cursor.root(root, ctx)
        c.warnUnknownKeys(DOC_KEYS)

        // 步驟 7：palette 必須先解析，之後所有 color 欄位才能解析 $ref。
        ctx.palette = PaletteResolver.resolve(c.child("palette").asMapping())

        val metrics = parseMetrics(c.mapping("metrics"))
        val typography = parseTypography(c.mapping("typography"))

        val candidatesCursor = c.mapping("candidates")
        candidatesCursor.warnUnknownKeys(CANDIDATES_KEYS)
        val shared = parseCandidateStyle(candidatesCursor, ThemeDefaults.candidateStyle(metrics))

        val barCursor = candidatesCursor.mapping("bar")
        barCursor.warnUnknownKeys(BAR_KEYS)
        val bar = parseBar(barCursor, shared, ctx)

        val windowCursor = candidatesCursor.mapping("window")
        windowCursor.warnUnknownKeys(WINDOW_KEYS)
        val window = parseWindow(windowCursor, shared, metrics)

        return Theme(
            id = id,
            revision = c.child("revision").int(1, 1, Int.MAX_VALUE),
            name = c.child("name").localized(),
            description = c.child("description").localized(),
            author = c.child("author").string(""),
            license = c.child("license").string(""),
            appearance = c.child("appearance").enumValue(Appearance.LIGHT),
            counterpart = c.child("counterpart").stringOrNull(),
            ancestry = ancestry,
            palette = ctx.palette,
            typography = typography,
            metrics = metrics,
            candidates = Candidates(shared, bar, window),
            preedit = parsePreedit(c.mapping("preedit")),
            keyboard = parseKeyboard(c.mapping("keyboard"), metrics),
            motion = parseMotion(c.mapping("motion")),
            feedback = parseFeedback(c.mapping("feedback"))
        )
    }

    // ───────────────────────── metrics / typography ─────────────────────────

    private fun parseMetrics(c: Cursor): Metrics {
        c.warnUnknownKeys(METRICS_KEYS)
        val d = ThemeDefaults.METRICS
        return Metrics(
            cornerRadius = c.child("corner_radius").length(d.cornerRadius, 0f, 64f),
            borderWidth = c.child("border_width").length(d.borderWidth, 0f, 8f),
            padding = c.child("padding").length(d.padding, 0f, 64f),
            spacing = c.child("spacing").length(d.spacing, 0f, 64f),
            elevation = c.child("elevation").length(d.elevation, 0f, 32f)
        )
    }

    private fun parseTypography(c: Cursor): Typography {
        c.warnUnknownKeys(TYPOGRAPHY_KEYS)
        val fontsCursor = c.mapping("fonts")
        val fonts = LinkedHashMap<String, FontStack>()
        for (name in fontsCursor.keys()) {
            fonts[name] = parseFontStack(fontsCursor.mapping(name))
        }
        for (name in ThemeDefaults.REQUIRED_FONT_NAMES) {
            if (!fonts.containsKey(name)) fonts[name] = ThemeDefaults.FONT_STACK
        }
        val assets = ArrayList<FontAsset>()
        for (a in c.child("assets").items()) {
            val m = a.asMapping()
            m.warnUnknownKeys(FONT_ASSET_KEYS)
            val family = m.child("family").string("")
            val file = m.child("file").string("")
            if (family.isEmpty() || file.isEmpty()) {
                c.diag.warn(a.path, "font asset needs both 'family' and 'file'; dropped", a.node?.line)
                continue
            }
            if (file.startsWith("/") || file.contains("..")) {
                c.diag.warn(a.path, "font asset path must stay inside the theme package; dropped", a.node?.line)
                continue
            }
            assets.add(
                FontAsset(
                    family = family,
                    file = file,
                    weight = m.child("weight").int(400, 100, 900),
                    italic = m.child("italic").bool(false)
                )
            )
        }
        return Typography(
            respectSystemFontScale = c.child("respect_system_font_scale").bool(true),
            fontScaleMin = c.child("font_scale_min").number(0.85f, 0.5f, 1.0f),
            fontScaleMax = c.child("font_scale_max").number(1.30f, 1.0f, 2.0f),
            fonts = fonts,
            assets = assets
        )
    }

    private fun parseFontStack(c: Cursor): FontStack {
        c.warnUnknownKeys(FONT_STACK_KEYS)
        val fallbackCursor = c.mapping("script_fallback")
        val fallback = LinkedHashMap<String, List<String>>()
        for (script in fallbackCursor.keys()) {
            if (!KNOWN_SCRIPTS.contains(script)) {
                c.diag.warn("${fallbackCursor.path}.$script", "unknown ISO 15924 script tag; ignored")
                continue
            }
            fallback[script] = fallbackCursor.child(script).stringList(emptyList())
        }
        return FontStack(
            family = c.child("family").stringList(ThemeDefaults.FONT_STACK.family),
            weight = roundToHundred(c.child("weight").int(400, 100, 900)),
            italic = c.child("italic").bool(false),
            scriptFallback = fallback
        )
    }

    private fun roundToHundred(v: Int): Int {
        val r = ((v + 50) / 100) * 100
        return clampInt(r, 100, 900)
    }

    // ───────────────────────── candidates ─────────────────────────

    /** `key` 缺席但同區塊的 `sourceKey` 有明確設定時，改用 sourceKey 的解析結果當預設。 */
    private fun derivedColor(c: Cursor, key: String, sourceKey: String, resolvedSource: Int, base: Int): Int {
        val self = c.child(key)
        if (self.exists) return self.color(base)
        return if (c.child(sourceKey).exists) resolvedSource else base
    }

    private fun parseCandidateStyle(c: Cursor, base: CandidateStyle): CandidateStyle {
        val orientation = c.child("orientation").enumValue(base.orientation)

        val l = c.mapping("label")
        l.warnUnknownKeys(LABEL_KEYS)
        val labelColor = l.child("color").color(base.label.color)
        val label = LabelStyle(
            show = l.child("show").bool(base.label.show),
            format = l.child("format").string(base.label.format),
            size = l.child("size").size(base.label.size),
            color = labelColor,
            highlightColor = derivedColor(l, "highlight_color", "color", labelColor, base.label.highlightColor)
        )

        val t = c.mapping("text")
        t.warnUnknownKeys(TEXT_KEYS)
        val textColor = t.child("color").color(base.text.color)
        val text = TextStyle(
            size = t.child("size").size(base.text.size),
            color = textColor,
            highlightColor = derivedColor(t, "highlight_color", "color", textColor, base.text.highlightColor)
        )

        val m = c.mapping("comment")
        m.warnUnknownKeys(COMMENT_KEYS)
        val commentColor = m.child("color").color(base.comment.color)
        val comment = CommentStyle(
            show = m.child("show").bool(base.comment.show),
            position = m.child("position").enumValue(base.comment.position),
            size = m.child("size").size(base.comment.size),
            color = commentColor,
            highlightColor = derivedColor(m, "highlight_color", "color", commentColor, base.comment.highlightColor)
        )

        val i = c.mapping("item")
        i.warnUnknownKeys(ITEM_KEYS)
        val item = ItemStyle(
            paddingH = i.child("padding_h").length(base.item.paddingH, 0f, 128f),
            paddingV = i.child("padding_v").length(base.item.paddingV, 0f, 128f),
            spacing = i.child("spacing").length(base.item.spacing, 0f, 128f),
            cornerRadius = i.child("corner_radius").length(base.item.cornerRadius, 0f, 64f),
            minWidth = i.child("min_width").length(base.item.minWidth, 0f, 512f),
            background = i.child("background").color(base.item.background),
            highlightBackground = i.child("highlight_background").color(base.item.highlightBackground),
            borderWidth = i.child("border_width").length(base.item.borderWidth, 0f, 8f),
            borderColor = i.child("border_color").color(base.item.borderColor),
            highlightBorderWidth = i.child("highlight_border_width").length(base.item.highlightBorderWidth, 0f, 8f),
            highlightBorderColor = i.child("highlight_border_color").color(base.item.highlightBorderColor)
        )

        val s = c.mapping("separator")
        s.warnUnknownKeys(SEPARATOR_KEYS)
        val separator = SeparatorStyle(
            show = s.child("show").bool(base.separator.show),
            color = s.child("color").color(base.separator.color),
            width = s.child("width").length(base.separator.width, 0f, 8f)
        )

        val p = c.mapping("page_indicator")
        p.warnUnknownKeys(PAGE_INDICATOR_KEYS)
        val pageColor = p.child("color").color(base.pageIndicator.color)
        val pageIndicator = PageIndicatorStyle(
            show = p.child("show").bool(base.pageIndicator.show),
            kind = p.child("style").enumValue(base.pageIndicator.kind),
            color = pageColor,
            disabledColor = derivedColor(p, "disabled_color", "color", pageColor, base.pageIndicator.disabledColor),
            size = p.child("size").size(base.pageIndicator.size)
        )

        return CandidateStyle(orientation, label, text, comment, item, separator, pageIndicator)
    }

    private fun parseBar(c: Cursor, shared: CandidateStyle, ctx: ParseContext): CandidateBar {
        val style = parseCandidateStyle(c, shared)
        val e = c.mapping("expand_button")
        e.warnUnknownKeys(EXPAND_BUTTON_KEYS)
        return CandidateBar(
            toolbar = parseToolbar(c.mapping("toolbar"), ctx),
            style = style,
            height = c.child("height").length(44f, 24f, 96f),
            background = c.child("background").color(0xFFFFFFFF.toInt()),
            borderTopWidth = c.child("border_top_width").length(0f, 0f, 8f),
            borderTopColor = c.child("border_top_color").color(ColorSpec.TRANSPARENT),
            maxVisible = c.child("max_visible").int(0, 0, 128),
            scroll = c.child("scroll").enumValue(ScrollMode.EXPANDABLE),
            expandButton = ExpandButton(
                show = e.child("show").bool(true),
                color = e.child("color").color(style.label.color),
                size = e.child("size").size(18f)
            ),
            showPreeditInline = c.child("show_preedit_inline").bool(true),
            emptyShowsToolbar = c.child("empty_shows_toolbar").bool(true)
        )
    }

    /** §8.6.6.1。`items` 缺席 → 規範性預設清單；被刪掉的必備項會被補回 + INFO。 */
    private fun parseToolbar(c: Cursor, ctx: ParseContext): Toolbar {
        c.warnUnknownKeys(TOOLBAR_KEYS)
        val show = c.child("show").bool(ThemeDefaults.TOOLBAR.show)
        val itemsCursor = c.child("items")
        if (!itemsCursor.exists) return Toolbar(show, ThemeDefaults.TOOLBAR_ITEMS)

        val diag = ctx.diagnostics
        val items = ArrayList<ToolbarItem>()
        for (entry in itemsCursor.items()) {
            val m = entry.asMapping()
            m.warnUnknownKeys(TOOLBAR_ITEM_KEYS)
            val tapCursor = m.child("tap")
            val raw = tapCursor.stringOrNull()
            if (raw == null) {
                diag.warn(entry.path, "toolbar item needs a 'tap' action; dropped", entry.node?.line)
                continue
            }
            val action = Actions.parse(raw, tapCursor.path, diag, tapCursor.node?.line) ?: continue
            val icon = m.child("icon").stringOrNull()
            if (icon != null && !LayoutParser.isKnownIcon(icon)) {
                diag.warn("${m.path}.icon", "unknown icon '$icon'; falling back to the label", m.node?.line)
            }
            items.add(
                ToolbarItem(
                    icon = icon,
                    label = m.child("label").string(""),
                    labelFrom = m.child("label_from").enumValue(LabelSource.NONE),
                    tap = action
                )
            )
        }
        return Toolbar(show, ensureRequiredToolbarItems(items, diag, c.path))
    }

    private fun ensureRequiredToolbarItems(
        items: List<ToolbarItem>,
        diag: Diagnostics,
        path: String
    ): List<ToolbarItem> {
        val out = ArrayList(items)
        for (required in ThemeDefaults.REQUIRED_TOOLBAR_ITEMS) {
            if (out.any { it.tap.verb == required.tap.verb }) continue
            out.add(required)
            diag.info(
                if (path.isEmpty()) "candidates.bar.toolbar" else path,
                "the toolbar must expose '${required.tap.raw}'; the required item was added back"
            )
        }
        return out
    }

    private fun parseWindow(c: Cursor, shared: CandidateStyle, m: Metrics): CandidateWindow {
        val style = parseCandidateStyle(c, shared)
        val sh = c.mapping("shadow")
        sh.warnUnknownKeys(SHADOW_KEYS)
        return CandidateWindow(
            style = style,
            background = c.child("background").color(0xFFFFFFFF.toInt()),
            cornerRadius = c.child("corner_radius").length(m.cornerRadius, 0f, 64f),
            padding = c.child("padding").length(m.padding, 0f, 64f),
            borderWidth = c.child("border_width").length(m.borderWidth, 0f, 8f),
            borderColor = c.child("border_color").color(ColorSpec.TRANSPARENT),
            minWidth = c.child("min_width").length(0f, 0f, 2048f),
            maxWidth = c.child("max_width").length(640f, 0f, 4096f),
            placement = c.child("placement").enumValue(Placement.AUTO),
            offsetX = c.child("offset_x").length(0f, -64f, 64f),
            offsetY = c.child("offset_y").length(6f, -64f, 64f),
            followCaret = c.child("follow_caret").bool(true),
            backdrop = c.child("backdrop").enumValue(Backdrop.NONE),
            opacity = c.child("opacity").ratio(1.0f),
            shadow = Shadow(
                show = sh.child("show").bool(true),
                radius = sh.child("radius").length(18f, 0f, 64f),
                offsetX = sh.child("offset_x").length(0f, -64f, 64f),
                offsetY = sh.child("offset_y").length(4f, -64f, 64f),
                color = sh.child("color").color(0x40000000)
            )
        )
    }

    // ───────────────────────── preedit ─────────────────────────

    private fun parsePreedit(c: Cursor): Preedit {
        c.warnUnknownKeys(PREEDIT_KEYS)
        val color = c.child("color").color(0xFF404040.toInt())
        val caret = c.mapping("caret")
        caret.warnUnknownKeys(CARET_KEYS)
        val sel = c.mapping("selection")
        sel.warnUnknownKeys(SELECTION_KEYS)
        return Preedit(
            show = c.child("show").bool(true),
            size = c.child("size").size(16f),
            color = color,
            background = c.child("background").color(ColorSpec.TRANSPARENT),
            paddingH = c.child("padding_h").length(ThemeDefaults.METRICS.padding, 0f, 128f),
            paddingV = c.child("padding_v").length(ThemeDefaults.METRICS.padding, 0f, 128f),
            cornerRadius = c.child("corner_radius").length(0f, 0f, 64f),
            caret = Caret(
                show = caret.child("show").bool(true),
                color = caret.child("color").color(0xFF3060C0.toInt()),
                width = caret.child("width").length(1.5f, 0f, 4f),
                blink = caret.child("blink").bool(true)
            ),
            selection = SelectionStyle(
                color = sel.child("color").color(0x403060C0),
                textColor = sel.child("text_color").color(color)
            )
        )
    }

    // ───────────────────────── keyboard ─────────────────────────

    private fun parseKeyboard(c: Cursor, m: Metrics): Keyboard {
        c.warnUnknownKeys(KEYBOARD_KEYS)

        // §8.8.0.2：舊的 height 區塊已被取代，忽略但要讓使用者知道。
        if (c.child("height").exists) {
            c.diag.info(
                "keyboard.height",
                "the screen-ratio height model was replaced by key_aspect / key_height / " +
                    "max_screen_ratio (spec 8.8); this block is ignored",
                c.child("height").node?.line
            )
        }
        val keyHeight = c.mapping("key_height")
        keyHeight.warnUnknownKeys(KEY_HEIGHT_KEYS)
        val rowHeight = c.mapping("row_height")
        rowHeight.warnUnknownKeys(ROW_HEIGHT_KEYS)
        val refGrid = c.mapping("reference_grid")
        refGrid.warnUnknownKeys(REFERENCE_GRID_KEYS)
        val ratios = c.mapping("max_screen_ratio")
        ratios.warnUnknownKeys(MAX_SCREEN_RATIO_KEYS)
        val geometry = KeyGeometry(
            aspect = c.child("key_aspect").number(1.28f, 0.6f, 2.5f),
            keyHeightMin = keyHeight.child("min").length(40f, 20f, 200f),
            keyHeightMax = keyHeight.child("max").length(56f, 20f, 200f),
            referenceUnits = refGrid.child("units").number(10f, 4f, 20f),
            referenceRows = refGrid.child("rows").number(4f, 1f, 8f),
            rowHeightMin = rowHeight.child("min").length(32f, 16f, 200f),
            rowHeightMax = rowHeight.child("max").length(96f, 16f, 200f),
            maxScreenRatioPortrait = ratios.child("portrait").ratio(0.45f, 0.2f, 0.8f),
            maxScreenRatioLandscape = ratios.child("landscape").ratio(0.62f, 0.2f, 0.9f)
        )

        val pad = c.mapping("padding")
        pad.warnUnknownKeys(PADDING_KEYS)

        val stylesCursor = c.mapping("key_styles")
        val styles = LinkedHashMap<String, KeyStyle>()
        val styleDefault = ThemeDefaults.keyStyle(m)
        for (name in stylesCursor.keys()) {
            styles[name] = parseKeyStyle(stylesCursor.mapping(name), styleDefault)
        }
        for (name in ThemeDefaults.REQUIRED_KEY_STYLES) {
            if (!styles.containsKey(name)) styles[name] = styleDefault
        }

        val popupCursor = c.mapping("popup")
        popupCursor.warnUnknownKeys(POPUP_KEYS)
        val popup = PopupStyle(
            show = popupCursor.child("show").bool(true),
            background = popupCursor.child("background").color(0xFFFFFFFF.toInt()),
            foreground = popupCursor.child("foreground").color(0xFF000000.toInt()),
            highlightBackground = popupCursor.child("highlight_background").color(0xFF3060C0.toInt()),
            highlightForeground = popupCursor.child("highlight_foreground").color(0xFFFFFFFF.toInt()),
            cornerRadius = popupCursor.child("corner_radius").length(m.cornerRadius, 0f, 64f),
            itemSize = popupCursor.child("item_size").size(22f),
            itemPadding = popupCursor.child("item_padding").length(10f, 0f, 64f),
            elevation = popupCursor.child("elevation").length(6f, 0f, 32f),
            maxColumns = popupCursor.child("max_columns").int(6, 1, 12)
        )

        val ppCursor = c.mapping("press_preview")
        ppCursor.warnUnknownKeys(PRESS_PREVIEW_KEYS)
        val pressPreview = PressPreviewStyle(
            show = ppCursor.child("show").bool(true),
            background = ppCursor.child("background").color(popup.background),
            foreground = ppCursor.child("foreground").color(popup.foreground),
            size = ppCursor.child("size").size(28f),
            cornerRadius = ppCursor.child("corner_radius").length(popup.cornerRadius, 0f, 64f),
            elevation = ppCursor.child("elevation").length(6f, 0f, 32f)
        )

        return Keyboard(
            background = c.child("background").color(0xFFD0D0D0.toInt()),
            geometry = geometry,
            padding = EdgeInsets(
                left = pad.child("left").length(5f, 0f, 64f),
                top = pad.child("top").length(4f, 0f, 64f),
                right = pad.child("right").length(5f, 0f, 64f),
                bottom = pad.child("bottom").length(4f, 0f, 64f)
            ),
            rowSpacing = c.child("row_spacing").length(12f, 0f, 32f),
            keySpacing = c.child("key_spacing").length(6f, 0f, 32f),
            honorBottomInset = c.child("honor_bottom_inset").bool(true),
            keyStyles = styles,
            popup = popup,
            pressPreview = pressPreview
        )
    }


    private fun parseKeyStyle(c: Cursor, base: KeyStyle): KeyStyle {
        c.warnUnknownKeys(KEY_STYLE_KEYS)
        val background = c.child("background").color(base.background)
        val foreground = c.child("foreground").color(base.foreground)
        val pressedBackground =
            derivedColor(c, "pressed_background", "background", background, base.pressedBackground)
        val pressedForeground =
            derivedColor(c, "pressed_foreground", "foreground", foreground, base.pressedForeground)
        return KeyStyle(
            background = background,
            pressedBackground = pressedBackground,
            foreground = foreground,
            pressedForeground = pressedForeground,
            activeBackground = derivedColor(
                c, "active_background", "pressed_background", pressedBackground, base.activeBackground
            ),
            activeForeground = derivedColor(
                c, "active_foreground", "pressed_foreground", pressedForeground, base.activeForeground
            ),
            cornerRadius = c.child("corner_radius").length(base.cornerRadius, 0f, 64f),
            borderWidth = c.child("border_width").length(base.borderWidth, 0f, 8f),
            borderColor = c.child("border_color").color(base.borderColor),
            elevation = c.child("elevation").length(base.elevation, 0f, 32f),
            font = c.child("font").string(base.font),
            labelSize = c.child("label_size").size(base.labelSize),
            hintSize = c.child("hint_size").size(base.hintSize),
            hintColor = derivedColor(
                c, "hint_color", "foreground", scaleAlpha(foreground, 0.6f), base.hintColor
            ),
            hintPosition = c.child("hint_position").enumValue(base.hintPosition),
            iconSize = c.child("icon_size").size(base.iconSize)
        )
    }

    // ───────────────────────── motion / feedback ─────────────────────────

    private fun parseMotion(c: Cursor): Motion {
        c.warnUnknownKeys(MOTION_KEYS)
        return Motion(
            enabled = c.child("enabled").bool(true),
            respectReduceMotion = c.child("respect_reduce_motion").bool(true),
            curve = c.child("curve").enumValue(MotionCurve.STANDARD),
            keyPressMs = c.child("key_press_ms").duration(40),
            keyReleaseMs = c.child("key_release_ms").duration(90),
            candidateChangeMs = c.child("candidate_change_ms").duration(120),
            popupMs = c.child("popup_ms").duration(90),
            windowShowMs = c.child("window_show_ms").duration(110)
        )
    }

    private fun parseFeedback(c: Cursor): Feedback {
        c.warnUnknownKeys(FEEDBACK_KEYS)
        return Feedback(
            haptic = c.child("haptic").bool(true),
            hapticStrength = c.child("haptic_strength").enumValue(HapticStrength.MEDIUM),
            sound = c.child("sound").bool(false),
            soundVolume = c.child("sound_volume").ratio(0.3f)
        )
    }
}
