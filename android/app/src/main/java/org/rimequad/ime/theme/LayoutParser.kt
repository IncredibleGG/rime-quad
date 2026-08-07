package org.rimequad.ime.theme

/** action 字串的解析（§9.5）。未知 verb → null，呼叫端把該鍵降級為 noop。 */
object Actions {

    fun parse(raw: String, path: String, diag: Diagnostics, line: Int?): KeyAction? {
        val t = raw.trim()
        if (t.isEmpty()) return null
        val parts = t.split(":")
        val verb = parts[0]
        val rest = parts.drop(1)

        fun need(n: Int): Boolean {
            if (rest.size < n || rest.take(n).any { it.isEmpty() }) {
                diag.warn(path, "action '$t' is missing arguments; key becomes noop", line)
                return false
            }
            return true
        }

        return when (verb) {
            "noop" -> KeyAction(ActionVerb.NOOP, emptyList(), t)
            "layer" -> if (need(1)) KeyAction(ActionVerb.LAYER, listOf(rest[0]), t) else null
            "layer_once" -> if (need(1)) KeyAction(ActionVerb.LAYER_ONCE, listOf(rest[0]), t) else null
            "layer_lock" -> if (need(1)) KeyAction(ActionVerb.LAYER_LOCK, listOf(rest[0]), t) else null
            "switch_layout" -> if (need(1)) KeyAction(ActionVerb.SWITCH_LAYOUT, listOf(rest[0]), t) else null
            "toggle" -> if (need(1)) KeyAction(ActionVerb.TOGGLE_OPTION, listOf(rest[0]), t) else null
            "set" -> {
                if (!need(2)) return null
                val v = rest[1].lowercase()
                if (v != "on" && v != "off") {
                    diag.warn(path, "action '$t' expects ':on' or ':off'; key becomes noop", line)
                    return null
                }
                KeyAction(ActionVerb.SET_OPTION, listOf(rest[0], v), t)
            }
            "schema" -> {
                if (!need(1)) return null
                when (rest[0]) {
                    "next" -> KeyAction(ActionVerb.SCHEMA_NEXT, emptyList(), t)
                    "prev" -> KeyAction(ActionVerb.SCHEMA_PREV, emptyList(), t)
                    "picker" -> KeyAction(ActionVerb.SCHEMA_PICKER, emptyList(), t)
                    else -> KeyAction(ActionVerb.SCHEMA_SELECT, listOf(rest[0]), t)
                }
            }
            "candidate" -> {
                if (!need(1)) return null
                when (rest[0]) {
                    "next_page" -> KeyAction(ActionVerb.CANDIDATE_NEXT_PAGE, emptyList(), t)
                    "prev_page" -> KeyAction(ActionVerb.CANDIDATE_PREV_PAGE, emptyList(), t)
                    "next" -> KeyAction(ActionVerb.CANDIDATE_NEXT, emptyList(), t)
                    "prev" -> KeyAction(ActionVerb.CANDIDATE_PREV, emptyList(), t)
                    "select", "delete" -> {
                        if (!need(2)) return null
                        val n = rest[1].toIntOrNull()
                        if (n == null || n < 0) {
                            diag.warn(path, "action '$t' expects a non-negative index; key becomes noop", line)
                            return null
                        }
                        val v = if (rest[0] == "select") ActionVerb.CANDIDATE_SELECT else ActionVerb.CANDIDATE_DELETE
                        KeyAction(v, listOf(n.toString()), t)
                    }
                    else -> {
                        diag.warn(path, "unknown action '$t'; key becomes noop", line)
                        null
                    }
                }
            }
            "cursor" -> {
                if (!need(1)) return null
                when (rest[0]) {
                    "left" -> KeyAction(ActionVerb.CURSOR_LEFT, emptyList(), t)
                    "right" -> KeyAction(ActionVerb.CURSOR_RIGHT, emptyList(), t)
                    "home" -> KeyAction(ActionVerb.CURSOR_HOME, emptyList(), t)
                    "end" -> KeyAction(ActionVerb.CURSOR_END, emptyList(), t)
                    else -> {
                        diag.warn(path, "unknown action '$t'; key becomes noop", line)
                        null
                    }
                }
            }
            "clear" -> KeyAction(ActionVerb.CLEAR, emptyList(), t)
            "hide_keyboard" -> KeyAction(ActionVerb.HIDE_KEYBOARD, emptyList(), t)
            "settings" -> KeyAction(ActionVerb.SETTINGS, emptyList(), t)
            "emoji" -> KeyAction(ActionVerb.EMOJI, emptyList(), t)
            else -> {
                diag.warn(path, "unknown action '$t'; key becomes noop", line)
                null
            }
        }
    }
}

/**
 * 佈局綁定層。輸入是已完成繼承合併與 key_patches 套用的節點樹。
 * 結構性缺陷（F8–F10）由 [LayoutParser.bind] 回傳 null 表示。
 */
object LayoutParser {

    private val DOC_KEYS = setOf(
        "format", "id", "revision", "name", "description", "author", "license",
        "inherits", "min_client", "kind", "targets", "for_schema", "direction",
        "default_layer", "primary", "metrics", "layers", "key_patches"
    )
    private val METRICS_KEYS = setOf("row_spacing", "key_spacing", "height_scale")
    private val LAYER_KEYS = setOf("id", "label", "units", "rows")
    private val ROW_KEYS = setOf("weight", "keys")
    private val KEY_KEYS = setOf(
        "id", "label", "hint", "icon", "label_from", "width", "style", "spacer",
        "active", "repeat", "send", "tap", "double_tap", "long_press", "popup", "swipe"
    )
    private val SUB_KEY_KEYS = setOf("label", "hint", "style", "send", "tap")
    private val SEND_KEYS = setOf("keysym", "text", "modifiers")
    private val POPUP_KEYS = setOf("layout", "columns", "keys")

    private val KNOWN_ICONS = setOf(
        "backspace", "enter", "shift", "shift_lock", "space", "globe", "keyboard_hide",
        "settings", "emoji", "search", "go", "done", "next", "clipboard", "undo", "mic",
        "arrow_left", "arrow_right", "arrow_up", "arrow_down"
    )

    /**
     * §9.6 的語義圖示名是否已知。
     * 工具列項目（§8.6.6.1）與按鍵共用同一份名稱表，所以這裡對外暴露。
     */
    internal fun isKnownIcon(name: String): Boolean = KNOWN_ICONS.contains(name)

    /** 回傳 null 代表致命的結構性錯誤，診斷已寫入 ctx。 */
    fun bind(root: YamlNode.Mapping, id: String, ancestry: List<String>, ctx: ParseContext): KeyboardLayout? {
        val c = Cursor.root(root, ctx)
        val diag = ctx.diagnostics
        c.warnUnknownKeys(DOC_KEYS)

        val layersCursor = c.child("layers")
        val layerNodes = layersCursor.items()
        if (layerNodes.isEmpty()) {
            diag.error("layers", "F8: a layout must declare at least one layer", root.line)
            return null
        }

        // 先收集 layer id，讓 layer:<id> 類 action 能在綁定時就驗證。
        val layerIds = LinkedHashSet<String>()
        for (ln in layerNodes) {
            val lid = ln.asMapping().child("id").stringOrNull()
            if (lid != null) layerIds.add(lid)
        }

        val layers = ArrayList<LayoutLayer>()
        for (ln in layerNodes) {
            val layer = parseLayer(ln.asMapping(), layerIds, ctx) ?: return null
            layers.add(layer)
        }

        val defaultLayer = c.child("default_layer").string(layers[0].id)
        if (!layerIds.contains(defaultLayer)) {
            diag.error("default_layer", "F9: '$defaultLayer' is not a declared layer", root.line)
            return null
        }

        val m = c.mapping("metrics")
        m.warnUnknownKeys(METRICS_KEYS)
        val metrics = LayoutMetrics(
            rowSpacing = if (m.child("row_spacing").isNull) null else m.child("row_spacing").length(6f, 0f, 32f),
            keySpacing = if (m.child("key_spacing").isNull) null else m.child("key_spacing").length(6f, 0f, 32f),
            heightScale = m.child("height_scale").number(1.0f, 0.5f, 2.0f)
        )

        return KeyboardLayout(
            id = id,
            revision = c.child("revision").int(1, 1, Int.MAX_VALUE),
            name = c.child("name").localized(),
            description = c.child("description").localized(),
            author = c.child("author").string(""),
            license = c.child("license").string(""),
            kind = c.child("kind").enumValue(LayoutKind.OTHER),
            targets = c.child("targets").stringList(listOf("android", "ios")),
            forSchema = c.child("for_schema").stringList(listOf("*")),
            direction = c.child("direction").enumValue(Direction.LTR),
            defaultLayer = defaultLayer,
            primary = c.child("primary").bool(false),
            metrics = metrics,
            layers = layers,
            ancestry = ancestry
        )
    }

    private fun parseLayer(c: Cursor, layerIds: Set<String>, ctx: ParseContext): LayoutLayer? {
        val diag = ctx.diagnostics
        c.warnUnknownKeys(LAYER_KEYS)
        val id = c.child("id").stringOrNull()
        if (id == null) {
            diag.error(c.path, "F8: layer is missing 'id'", c.node?.line)
            return null
        }
        val rowNodes = c.child("rows").items()
        if (rowNodes.isEmpty()) {
            diag.error("${c.path}.rows", "F10: layer '$id' has no rows", c.node?.line)
            return null
        }
        val rows = ArrayList<LayoutRow>()
        for (rn in rowNodes) {
            val r = rn.asMapping()
            r.warnUnknownKeys(ROW_KEYS)
            val keyNodes = r.child("keys").items()
            if (keyNodes.isEmpty()) {
                diag.error("${r.path}.keys", "F10: row has no keys", r.node?.line)
                return null
            }
            val keys = ArrayList<LayoutKey>(keyNodes.size)
            for (kn in keyNodes) keys.add(parseKey(kn.asMapping(), layerIds, ctx))
            rows.add(LayoutRow(weight = r.child("weight").number(1.0f, 0.1f, 4.0f), keys = keys))
        }

        val declaredUnits = c.child("units")
        val maxSum = rows.fold(0f) { acc, r -> if (r.widthSum > acc) r.widthSum else acc }
        val units = if (declaredUnits.exists) declaredUnits.number(maxSum, 0.1f, 64f) else maxSum
        for (r in rows) {
            if (kotlin.math.abs(r.widthSum - units) > 0.01f) {
                diag.warn(
                    "${c.path}.rows",
                    "row width sum ${r.widthSum} does not match layer units $units in layer '$id'",
                    c.node?.line
                )
            }
        }

        return LayoutLayer(
            id = id,
            label = c.child("label").localized(),
            units = units,
            rows = rows
        )
    }

    private fun parseKey(c: Cursor, layerIds: Set<String>, ctx: ParseContext): LayoutKey {
        val diag = ctx.diagnostics
        c.warnUnknownKeys(KEY_KEYS)

        val spacer = c.child("spacer").bool(false)
        val width = c.child("width").number(1.0f, 0.1f, 12.0f)
        if (spacer) {
            // §9.6：spacer 的互動欄位一律被忽略，且不產生 WARNING。
            return LayoutKey(
                id = c.child("id").stringOrNull(), label = "", hint = "", icon = null,
                labelFrom = LabelSource.NONE, width = width, style = "default",
                spacer = true, active = false, repeat = false,
                send = null, tap = null, doubleTap = null, longPress = null,
                popup = null, swipe = emptyMap()
            )
        }

        val icon = c.child("icon").stringOrNull()
        if (icon != null && !KNOWN_ICONS.contains(icon)) {
            diag.warn("${c.path}.icon", "unknown icon '$icon'; falling back to the label", c.node?.line)
        }

        var send = parseSend(c.mapping("send"), ctx)
        val tap = parseAction(c, "tap", layerIds, ctx)
        if (tap != null && send != null) {
            diag.warn(c.path, "'send' and 'tap' are mutually exclusive; 'tap' wins", c.node?.line)
            send = null
        }

        val repeat = c.child("repeat").bool(false)
        var longPress = parseAction(c, "long_press", layerIds, ctx)
        if (repeat && longPress != null) {
            diag.warn(c.path, "'repeat' and 'long_press' are mutually exclusive; 'repeat' wins", c.node?.line)
            longPress = null
        }

        val popupCursor = c.mapping("popup")
        val popup = if (popupCursor.exists) parsePopup(popupCursor, layerIds, ctx) else null

        val swipeCursor = c.mapping("swipe")
        val swipe = LinkedHashMap<SwipeDirection, SubKey>()
        for (dirName in swipeCursor.keys()) {
            val dir = SwipeDirection.values().firstOrNull { it.name.lowercase() == dirName.lowercase() }
            if (dir == null) {
                diag.warn("${swipeCursor.path}.$dirName", "unknown swipe direction; ignored")
                continue
            }
            swipe[dir] = parseSubKey(swipeCursor.mapping(dirName), layerIds, ctx)
        }

        return LayoutKey(
            id = c.child("id").stringOrNull(),
            label = c.child("label").string(""),
            hint = c.child("hint").string(""),
            icon = icon,
            labelFrom = c.child("label_from").enumValue(LabelSource.NONE),
            width = width,
            style = c.child("style").string("default"),
            spacer = false,
            active = c.child("active").bool(false),
            repeat = repeat,
            send = send,
            tap = tap,
            doubleTap = parseAction(c, "double_tap", layerIds, ctx),
            longPress = longPress,
            popup = popup,
            swipe = swipe
        )
    }

    private fun parseAction(c: Cursor, key: String, layerIds: Set<String>, ctx: ParseContext): KeyAction? {
        val node = c.child(key)
        val raw = node.stringOrNull() ?: return null
        val a = Actions.parse(raw, node.path, ctx.diagnostics, node.node?.line) ?: return null
        if (a.verb == ActionVerb.LAYER || a.verb == ActionVerb.LAYER_ONCE || a.verb == ActionVerb.LAYER_LOCK) {
            val target = a.arg
            if (target == null || !layerIds.contains(target)) {
                ctx.diagnostics.warn(
                    node.path,
                    "action '$raw' targets a layer that does not exist; key becomes noop",
                    node.node?.line
                )
                return null
            }
        }
        return a
    }

    private fun parseSend(c: Cursor, ctx: ParseContext): SendSpec? {
        if (!c.exists) return null
        c.warnUnknownKeys(SEND_KEYS)
        val diag = ctx.diagnostics
        val keysymName = c.child("keysym").stringOrNull()
        val text = c.child("text").stringOrNull()

        if (keysymName != null && text != null) {
            diag.warn(c.path, "'keysym' and 'text' are mutually exclusive; 'keysym' wins", c.node?.line)
        }
        if (keysymName != null) {
            var mods = Modifiers.NONE
            for (mc in c.child("modifiers").items()) {
                val name = mc.stringOrNull() ?: continue
                val bit = Modifiers.byName(name)
                if (bit == null) {
                    diag.warn(mc.path, "unknown modifier '$name'; ignored", mc.node?.line)
                    continue
                }
                if (name.trim().equals("Release", ignoreCase = true)) continue
                mods = mods or bit
            }
            val code = Keysym.resolve(keysymName)
            if (code == Keysym.VOID_SYMBOL && !Keysym.isKnownName(keysymName)) {
                // 不是致命錯誤：執行期仍會回落到 RimeGetKeycodeByName()。
                diag.warn(
                    "${c.path}.keysym",
                    "'$keysymName' is not in the static keysym table; will be resolved by librime at runtime",
                    c.node?.line
                )
            }
            return SendSpec.Keysym(keysymName, code, mods)
        }
        if (text != null) {
            if (text.isEmpty()) {
                diag.warn("${c.path}.text", "empty text; key becomes noop", c.node?.line)
                return null
            }
            return SendSpec.Text(text)
        }
        diag.warn(c.path, "'send' needs either 'keysym' or 'text'; key becomes noop", c.node?.line)
        return null
    }

    private fun parsePopup(c: Cursor, layerIds: Set<String>, ctx: ParseContext): Popup? {
        c.warnUnknownKeys(POPUP_KEYS)
        val keys = ArrayList<SubKey>()
        for (kn in c.child("keys").items()) keys.add(parseSubKey(kn.asMapping(), layerIds, ctx))
        if (keys.isEmpty()) return null
        return Popup(
            layout = c.child("layout").enumValue(PopupLayout.ROW),
            columns = c.child("columns").int(4, 1, 12),
            keys = keys
        )
    }

    private fun parseSubKey(c: Cursor, layerIds: Set<String>, ctx: ParseContext): SubKey {
        c.warnUnknownKeys(SUB_KEY_KEYS)
        return SubKey(
            label = c.child("label").string(""),
            hint = c.child("hint").string(""),
            style = c.child("style").stringOrNull(),
            send = parseSend(c.mapping("send"), ctx),
            tap = parseAction(c, "tap", layerIds, ctx)
        )
    }
}
