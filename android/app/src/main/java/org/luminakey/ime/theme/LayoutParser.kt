package org.luminakey.ime.theme

import org.luminakey.ime.theme.DiagnosticCode.ACTION_TARGET_MISSING
import org.luminakey.ime.theme.DiagnosticCode.AUTO_FOR_SCHEMA_WILDCARD
import org.luminakey.ime.theme.DiagnosticCode.BAD_ACTION_ARGUMENT
import org.luminakey.ime.theme.DiagnosticCode.ENTRY_DROPPED
import org.luminakey.ime.theme.DiagnosticCode.FATAL_ALPHA_LAYER_UNKNOWN
import org.luminakey.ime.theme.DiagnosticCode.FATAL_DEFAULT_LAYER_UNKNOWN
import org.luminakey.ime.theme.DiagnosticCode.FATAL_LAYERS_MISSING
import org.luminakey.ime.theme.DiagnosticCode.FATAL_LAYER_EMPTY
import org.luminakey.ime.theme.DiagnosticCode.MUTUALLY_EXCLUSIVE
import org.luminakey.ime.theme.DiagnosticCode.ROW_WIDTH_MISMATCH
import org.luminakey.ime.theme.DiagnosticCode.SEND_INCOMPLETE
import org.luminakey.ime.theme.DiagnosticCode.UNKNOWN_ACTION
import org.luminakey.ime.theme.DiagnosticCode.UNKNOWN_ICON
import org.luminakey.ime.theme.DiagnosticCode.UNKNOWN_KEYSYM
import org.luminakey.ime.theme.DiagnosticCode.UNKNOWN_MODIFIER
import org.luminakey.ime.theme.DiagnosticCode.UNKNOWN_SWIPE_DIRECTION

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
                diag.add(BAD_ACTION_ARGUMENT, path, line, listOf(t))
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

            // `input_mode:toggle` —— 見 ActionVerb.INPUT_MODE_TOGGLE 的說明。
            // 刻意不寫成 `toggle:ascii_mode` 的別名：兩者語義不同，
            // 舊動詞仍然合法（只切模式），佈局作者要哪一種是他的決定。
            "input_mode" -> if (need(1)) {
                when (rest[0]) {
                    "toggle" -> KeyAction(ActionVerb.INPUT_MODE_TOGGLE, emptyList(), t)
                    else -> {
                        // ⚠ 這裡原本是 diag.error(... "F10" ...)，也就是**致命錯誤**。
                        // §6.2 的致命清單裡沒有這一條，而 §6.3 明寫「已知 verb、
                        // 參數不合法 → 該鍵變 noop + WARNING」。錯誤的那一版會讓
                        // 一顆鍵上的錯字整份佈局載不起來 —— 使用者看到的是鍵盤
                        // 退回上一份，而不是那一顆鍵沒反應。
                        // 嚴重度改由 code 決定之後，這種「產生點自己選一級」的
                        // 分歧就沒有地方可以寫了。
                        diag.add(BAD_ACTION_ARGUMENT, path, line, listOf(t))
                        null
                    }
                }
            } else null
            "set" -> {
                if (!need(2)) return null
                val v = rest[1].lowercase()
                if (v != "on" && v != "off") {
                    diag.add(BAD_ACTION_ARGUMENT, path, line, listOf(t))
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
                            diag.add(BAD_ACTION_ARGUMENT, path, line, listOf(t))
                            return null
                        }
                        val v = if (rest[0] == "select") ActionVerb.CANDIDATE_SELECT else ActionVerb.CANDIDATE_DELETE
                        KeyAction(v, listOf(n.toString()), t)
                    }
                    else -> {
                        // verb 認得、參數不認得 → §6.5.1 是 bad_action_argument，
                        // 不是 unknown_action（那一格留給整個 verb 都不認得）。
                        diag.add(BAD_ACTION_ARGUMENT, path, line, listOf(t))
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
                        diag.add(BAD_ACTION_ARGUMENT, path, line, listOf(t))
                        null
                    }
                }
            }
            "clear" -> KeyAction(ActionVerb.CLEAR, emptyList(), t)
            "hide_keyboard" -> KeyAction(ActionVerb.HIDE_KEYBOARD, emptyList(), t)
            "settings" -> KeyAction(ActionVerb.SETTINGS, emptyList(), t)
            "emoji" -> KeyAction(ActionVerb.EMOJI, emptyList(), t)
            else -> {
                diag.add(UNKNOWN_ACTION, path, line, listOf(t))
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
        "inherits", "min_client", "kind", "targets", "for_schema", "auto_for_schema",
        "direction", "default_layer", "alpha_layer", "primary", "metrics", "layers",
        "key_patches"
    )
    private val METRICS_KEYS = setOf("row_spacing", "key_spacing", "height_scale")
    private val LAYER_KEYS = setOf("id", "label", "units", "rows", "syllable_slots")
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
            diag.add(FATAL_LAYERS_MISSING, "layers", root.line)
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
            // parseLayer 回 null 有兩種：致命（F10，診斷已寫入）與「這一層缺 id
            // 所以被丟掉」。前者要立刻放棄整份文件，後者繼續 —— 見 parseLayer。
            // ⚠ 致命的判斷要在 null 檢查**之前**：兩種都回 null，只看 null
            // 分不出來，先 `?: continue` 就會把 F10 吞掉。
            val layer = parseLayer(ln.asMapping(), layerIds, ctx)
            if (diag.hasErrors) return null
            if (layer == null) continue
            layers.add(layer)
        }
        // 每一層都被丟掉，等同 §6.2 F8 的「layers 為空」。
        if (layers.isEmpty()) {
            diag.add(FATAL_LAYERS_MISSING, "layers", root.line)
            return null
        }

        val defaultLayer = c.child("default_layer").string(layers[0].id)
        if (!layerIds.contains(defaultLayer)) {
            diag.add(FATAL_DEFAULT_LAYER_UNKNOWN, "default_layer", root.line, listOf(defaultLayer))
            return null
        }

        // `alpha_layer` 與 `default_layer` 同級：指到不存在的層是致命的，
        // 因為那顆「中／En」鍵按下去會把使用者送進一個不存在的狀態。
        val alphaLayer = c.child("alpha_layer").stringOrNull()
        if (alphaLayer != null && !layerIds.contains(alphaLayer)) {
            diag.add(FATAL_ALPHA_LAYER_UNKNOWN, "alpha_layer", root.line, listOf(alphaLayer))
            return null
        }

        val m = c.mapping("metrics")
        m.warnUnknownKeys(METRICS_KEYS)
        val metrics = LayoutMetrics(
            rowSpacing = if (m.child("row_spacing").isNull) null else m.child("row_spacing").length(6f, 0f, 32f),
            keySpacing = if (m.child("key_spacing").isNull) null else m.child("key_spacing").length(6f, 0f, 32f),
            heightScale = m.child("height_scale").number(1.0f, 0.5f, 2.0f)
        )

        val forSchema = c.child("for_schema").stringList(listOf("*"))
        // §9.1.1：`auto_for_schema` 沒寫時 = `for_schema` 去掉 `"*"`。
        // 這個預設讓「只給某方案用、也是它的預設佈局」這個最常見的情形一行都不用寫，
        // 同時保證 `"*"` 的泛用佈局**永遠不會**搶自動命中（`"*"` 濾掉之後是空的）。
        val autoNode = c.child("auto_for_schema")
        // ⚠ 只呼叫一次 stringList()。原本這裡呼叫了兩次（一次取值、一次檢查含不含
        // "*"），節點型別錯的時候就是**兩則一模一樣的 WARNING** —— 而 §10 第 9 條
        // 比對的正是診斷序列，多出來的那一則會讓四端無聲地對不上。
        val autoDeclared = if (autoNode.exists) autoNode.stringList(emptyList()) else null
        val autoForSchema =
            autoDeclared?.filter { it != "*" } ?: forSchema.filter { it != "*" }
        if (autoDeclared != null && autoDeclared.contains("*")) {
            diag.add(AUTO_FOR_SCHEMA_WILDCARD, "auto_for_schema", root.line)
        }

        return KeyboardLayout(
            id = id,
            revision = c.child("revision").int(1, 1, Int.MAX_VALUE),
            name = c.child("name").localized(),
            description = c.child("description").localized(),
            author = c.child("author").string(""),
            license = c.child("license").string(""),
            kind = c.child("kind").enumValue(LayoutKind.OTHER),
            targets = c.child("targets").stringList(listOf("android", "ios")),
            forSchema = forSchema,
            autoForSchema = autoForSchema,
            direction = c.child("direction").enumValue(Direction.LTR),
            defaultLayer = defaultLayer,
            alphaLayer = alphaLayer,
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
            // §6.2 的致命清單（F1–F10）沒有「layer 缺 id」這一條，而 §6.2 明寫
            // 「超出此清單者一律為可回復錯誤」。所以丟掉這一層、繼續解析；
            // 全部丟光時 bind() 會補上 F8。原本這裡是致命的，等於自己往致命清單
            // 加了一條，四端會因此拒絕不同的檔案。
            diag.add(ENTRY_DROPPED, c.path, c.node?.line)
            return null
        }
        val rowNodes = c.child("rows").items()
        if (rowNodes.isEmpty()) {
            diag.add(FATAL_LAYER_EMPTY, "${c.path}.rows", c.node?.line, listOf(id))
            return null
        }
        val rows = ArrayList<LayoutRow>()
        val rowPaths = ArrayList<String>()
        for (rn in rowNodes) {
            val r = rn.asMapping()
            r.warnUnknownKeys(ROW_KEYS)
            val keyNodes = r.child("keys").items()
            if (keyNodes.isEmpty()) {
                diag.add(FATAL_LAYER_EMPTY, "${r.path}.keys", r.node?.line, listOf(id))
                return null
            }
            val keys = ArrayList<LayoutKey>(keyNodes.size)
            for (kn in keyNodes) keys.add(parseKey(kn.asMapping(), layerIds, ctx))
            rows.add(LayoutRow(weight = r.child("weight").number(1.0f, 0.1f, 4.0f), keys = keys))
            rowPaths.add(r.path)
        }

        val declaredUnits = c.child("units")
        val maxSum = rows.fold(0f) { acc, r -> if (r.widthSum > acc) r.widthSum else acc }
        val units = if (declaredUnits.exists) declaredUnits.number(maxSum, 0.1f, 64f) else maxSum
        for ((i, r) in rows.withIndex()) {
            if (kotlin.math.abs(r.widthSum - units) > 0.01f) {
                // ⚠ 路徑要帶到「哪一列」。原本三列都掛在 `<layer>.rows` 上，
                // 於是三則診斷的 (severity, code, path) 一模一樣 —— 去重之後
                // 會只剩一則，而使用者其實有三列要修。
                diag.add(
                    ROW_WIDTH_MISMATCH, rowPaths[i], c.node?.line,
                    listOf(r.widthSum.toString(), units.toString(), id)
                )
            }
        }

        return LayoutLayer(
            id = id,
            label = c.child("label").localized(),
            units = units,
            rows = rows,
            syllableSlots = c.child("syllable_slots").stringList(emptyList())
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
            diag.add(UNKNOWN_ICON, "${c.path}.icon", c.node?.line, listOf(icon))
        }

        var send = parseSend(c.mapping("send"), ctx)
        val tap = parseAction(c, "tap", layerIds, ctx)
        if (tap != null && send != null) {
            diag.add(MUTUALLY_EXCLUSIVE, c.path, c.node?.line, listOf("send", "tap"))
            send = null
        }

        val repeat = c.child("repeat").bool(false)
        var longPress = parseAction(c, "long_press", layerIds, ctx)
        if (repeat && longPress != null) {
            diag.add(MUTUALLY_EXCLUSIVE, c.path, c.node?.line, listOf("long_press", "repeat"))
            longPress = null
        }

        val popupCursor = c.mapping("popup")
        val popup = if (popupCursor.exists) parsePopup(popupCursor, layerIds, ctx) else null

        val swipeCursor = c.mapping("swipe")
        val swipe = LinkedHashMap<SwipeDirection, SubKey>()
        for (dirName in swipeCursor.keys()) {
            val dir = SwipeDirection.values().firstOrNull { it.name.lowercase() == dirName.lowercase() }
            if (dir == null) {
                diag.add(UNKNOWN_SWIPE_DIRECTION, "${swipeCursor.path}.$dirName", args = listOf(dirName))
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
                ctx.diagnostics.add(
                    ACTION_TARGET_MISSING, node.path, node.node?.line,
                    listOf(raw, target ?: "")
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
            diag.add(MUTUALLY_EXCLUSIVE, c.path, c.node?.line, listOf("text", "keysym"))
        }
        if (keysymName != null) {
            var mods = Modifiers.NONE
            for (mc in c.child("modifiers").items()) {
                val name = mc.stringOrNull() ?: continue
                val bit = Modifiers.byName(name)
                if (bit == null) {
                    diag.add(UNKNOWN_MODIFIER, mc.path, mc.node?.line, listOf(name))
                    continue
                }
                if (name.trim().equals("Release", ignoreCase = true)) continue
                mods = mods or bit
            }
            val code = Keysym.resolve(keysymName)
            if (code == Keysym.VOID_SYMBOL && !Keysym.isKnownName(keysymName)) {
                // 不是致命錯誤：執行期仍會回落到 RimeGetKeycodeByName()。
                diag.add(UNKNOWN_KEYSYM, "${c.path}.keysym", c.node?.line, listOf(keysymName))
            }
            return SendSpec.Keysym(keysymName, code, mods)
        }
        if (text != null) {
            if (text.isEmpty()) {
                diag.add(SEND_INCOMPLETE, "${c.path}.text", c.node?.line)
                return null
            }
            return SendSpec.Text(text)
        }
        diag.add(SEND_INCOMPLETE, c.path, c.node?.line)
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
