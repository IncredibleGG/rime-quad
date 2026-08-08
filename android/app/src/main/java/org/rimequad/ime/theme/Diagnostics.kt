package org.rimequad.ime.theme

/**
 * 診斷模型 —— docs/theme-format.md §6.5 / §6.5.1。
 *
 * ⚠ 這一版是 **code + args**，不是自由文字。理由（規範 §6.5）：
 *
 * §10 檢核第 9 條要求「同一份壞檔案，四端報一樣多則、內容一致」，而診斷一旦
 * 上使用者畫面就必須在地化 —— 中文的「不是合法的顏色」與英文的
 * `is not a valid color` 是同一則診斷，字串比對卻永遠不相等。兩件事在自由文字
 * 的模型下互相矛盾。所以診斷的身分是 `(severity, code, path)`，
 * [Diagnostic.developerMessage] 只是給開發者看的英文回退，
 * **不上使用者畫面、不參與四端比對**。
 *
 * ── 這個檔案最關鍵的一條約定 ──────────────────────────────────────────
 *
 * **`severity` 是 `code` 上的函式，不是 `diag.add(...)` 呼叫點的參數。**
 *
 * 這不是潔癖。只要有一端（或同一端的某一個呼叫點）把同一件事記成 INFO，
 * 「四端報一樣多則 WARNING」就失守了，而且失守得無聲無息 —— 畫面正常、
 * 測試全綠、只有真的去比對四端輸出的人才會發現。改成函式之後，
 * 「這件事算幾級」在**整個專案裡只有一個地方寫得下**（[DiagnosticCode.severity]）。
 *
 * 這個模型改完之前，Android 這邊已經有一個真的犯過：`input_mode:<未知>`
 * 被記成 `diag.error(... "F10" ...)`，也就是致命錯誤，而規範 §6.3 說
 * 「已知 verb、參數不合法」是可回復的 WARNING。它是靠這條規則被抓出來的。
 */
enum class Severity { INFO, WARNING, ERROR }

/**
 * 穩定的診斷代碼。**字面值（[id]）是規範的一部分**，不可為了好看而更名 ——
 * 在地化樣板是照 id 查的（§6.5「更名或移除 code 算破壞性變更」）。
 *
 * @param id      規範 §6.5.1 碼表裡的字面值
 * @param arity   位置參數的個數。多數 code 是固定值；`unknown_field` 規範上
 *                允許 `[field]` 與 `[field, suggestion]` 兩種，所以是範圍。
 * @param provisional `true` = **還沒進規範 §6.5.1 的暫定碼**。這些是 §6.3／§9.7
 *                規範性地要求「產生 WARNING」、但 §6.5.1 的碼表漏掉的情況
 *                （已回報 docs/coordination.md §5）。名稱與 args 是 Android 端
 *                對桌面端的提案，macOS 端採用之後這個旗標要拿掉 ——
 *                [DiagnosticCodeSpecTest] 會在那一刻變紅提醒。
 */
enum class DiagnosticCode(
    val id: String,
    val arity: IntRange,
    val provisional: Boolean = false,
) {

    // ── 致命（§6.2 的 F1–F10 與文件層級）──────────────────────────────
    FATAL_YAML_SYNTAX("fatal.yaml_syntax", 1..1),
    FATAL_ROOT_NOT_MAPPING("fatal.root_not_mapping", 0..0),
    FATAL_FORMAT_MISSING("fatal.format_missing", 1..1),
    FATAL_FORMAT_MALFORMED("fatal.format_malformed", 2..2),
    FATAL_FORMAT_KIND_MISMATCH("fatal.format_kind_mismatch", 3..3),
    FATAL_FORMAT_MAJOR_UNSUPPORTED("fatal.format_major_unsupported", 4..4),
    FATAL_ID_MISSING("fatal.id_missing", 1..1),
    FATAL_ID_INVALID("fatal.id_invalid", 1..1),
    FATAL_ID_MISMATCH("fatal.id_mismatch", 2..2),
    FATAL_DOCUMENT_NOT_FOUND("fatal.document_not_found", 1..1),
    FATAL_PARENT_NOT_FOUND("fatal.parent_not_found", 1..1),
    FATAL_INHERITS_CYCLE("fatal.inherits_cycle", 1..1),
    FATAL_INHERITS_TOO_DEEP("fatal.inherits_too_deep", 1..1),
    FATAL_MIN_CLIENT("fatal.min_client", 2..2),
    FATAL_LAYERS_MISSING("fatal.layers_missing", 0..0),
    FATAL_DEFAULT_LAYER_UNKNOWN("fatal.default_layer_unknown", 1..1),
    FATAL_ALPHA_LAYER_UNKNOWN("fatal.alpha_layer_unknown", 1..1),
    FATAL_LAYER_EMPTY("fatal.layer_empty", 1..1),

    // ── 可回復（§6.3）──────────────────────────────────────────────────
    UNKNOWN_FIELD("unknown_field", 1..2),
    DUPLICATE_KEY("duplicate_key", 1..1),
    TYPE_MISMATCH("type_mismatch", 2..2),
    BAD_BOOL("bad_bool", 1..1),
    BAD_NUMBER("bad_number", 1..1),
    OUT_OF_RANGE("out_of_range", 4..4),
    BAD_ENUM("bad_enum", 3..3),
    BAD_COLOR("bad_color", 1..1),
    PALETTE_NOT_SCALAR("palette_not_scalar", 1..1),
    PALETTE_BAD_COLOR("palette_bad_color", 2..2),
    PALETTE_UNRESOLVED_REF("palette_unresolved_ref", 2..2),
    PALETTE_SELF_REFERENCE("palette_self_reference", 1..1),
    PALETTE_CYCLE_OR_TOO_DEEP("palette_cycle_or_too_deep", 1..1),
    ENTRY_DROPPED("entry_dropped", 0..0),
    ASSET_INCOMPLETE("asset_incomplete", 0..0),
    ASSET_PATH_ESCAPE("asset_path_escape", 1..1),
    UNKNOWN_SCRIPT_TAG("unknown_script_tag", 1..1),
    UNKNOWN_ICON("unknown_icon", 1..1),
    UNKNOWN_ACTION("unknown_action", 1..1),
    BAD_ACTION_ARGUMENT("bad_action_argument", 1..1),
    TOOLBAR_ITEM_NO_TAP("toolbar_item_no_tap", 0..0),
    STATUS_ITEM_NO_SOURCE("status_item_no_source", 0..0),
    NESTED_PLATFORM_OVERRIDES("nested_platform_overrides", 0..0),

    // ── INFO（§6.4）────────────────────────────────────────────────────
    REQUIRED_ITEM_RESTORED("required_item_restored", 1..1),
    DEPRECATED_FIELD("deprecated_field", 1..1),
    FEATURE_UNSUPPORTED("feature_unsupported", 2..2),
    LEGACY_BLOCK_IGNORED("legacy_block_ignored", 1..1),

    // ── 暫定碼：規範正文要求發診斷，但 §6.5.1 的碼表沒有對應的一格 ────
    //
    // 每一條都對得上規範的一行正文，不是自己發明的情況。留在這裡而不是硬塞
    // 進最接近的既有 code，是因為塞錯的代價比較高：桌面端日後為同一件事取了
    // 另一個名字，四端的 (severity, code, path) 序列就永遠對不上，而那是
    // §10 第 9 條唯一的比對依據。
    /** §6.3「佈局：無法解析的 `keysym` 名」。args: `[name]` */
    UNKNOWN_KEYSYM("unknown_keysym", 1..1, provisional = true),
    /** §9.6 `send.modifiers` 裡的未知修飾鍵名。args: `[name]` */
    UNKNOWN_MODIFIER("unknown_modifier", 1..1, provisional = true),
    /** §9.6 `swipe` 底下的未知方向。args: `[direction]` */
    UNKNOWN_SWIPE_DIRECTION("unknown_swipe_direction", 1..1, provisional = true),
    /** §6.3 的三條互斥規則（send/tap、repeat/long_press、keysym/text）。args: `[ignored, winner]` */
    MUTUALLY_EXCLUSIVE("mutually_exclusive", 2..2, provisional = true),
    /** §9.6 `send` 既沒有 `keysym` 也沒有可用的 `text`。args: `[]` */
    SEND_INCOMPLETE("send_incomplete", 0..0, provisional = true),
    /** §6.3「某 row 的 `width` 總和 ≠ `units`，差距 > 0.01」。args: `[sum, units, layer-id]` */
    ROW_WIDTH_MISMATCH("row_width_mismatch", 3..3, provisional = true),
    /** §9.7「patch 的 id 在佈局中找不到 → 忽略 + WARNING」。args: `[key-id]` */
    KEY_PATCH_NO_TARGET("key_patch_no_target", 1..1, provisional = true),
    /** `layer:` / `layer_once:` / `layer_lock:` 指向不存在的層。args: `[raw, target]` */
    ACTION_TARGET_MISSING("action_target_missing", 2..2, provisional = true),
    /** §9.1.1：`auto_for_schema` 只比對具名方案，`"*"` 在那裡沒有意義。args: `[]` */
    AUTO_FOR_SCHEMA_WILDCARD("auto_for_schema_wildcard", 0..0, provisional = true),
    /**
     * Android 專屬：使用者自訂鍵位套不到目前這份佈局上（上游改版把鍵刪了）。
     * 不是規範的情況 —— 桌面端沒有自訂鍵位，所以它**不會**出現在四端比對裡。
     * args: `[detail]`；⚠ `detail` 目前仍是預先組好的字面字串，見
     * [org.rimequad.ime.keyboard.LayoutRemapValidator]（task #42 的範圍）。
     */
    USER_REMAP_UNAPPLICABLE("user_remap_unapplicable", 1..1, provisional = true),
    ;

    /**
     * 規範規定的嚴重度。**碼決定嚴重度**，呼叫端不能自己選。
     *
     * 這個 `when` 是全專案唯一一處決定「這件事算幾級」的地方。
     */
    val severity: Severity
        get() = when {
            id.startsWith("fatal.") -> Severity.ERROR
            this in INFO_CODES -> Severity.INFO
            else -> Severity.WARNING
        }

    /**
     * 在地化樣板的資源名（`res/values.../strings_diag.xml`）。
     *
     * 由 [id] **純函式地**推導，不是手寫的對照表 —— 手寫的那一刻就會有一個
     * 只在某個語系下才走得到的錯字。多元 arity 的 code（只有 `unknown_field`）
     * 每一種 arity 一份樣板：最小的那個用基本名，其餘加 `_<n>` 後綴。
     */
    fun resourceName(argCount: Int = arity.first): String {
        val base = "diag_" + id.replace('.', '_')
        return if (argCount <= arity.first) base else "${base}_$argCount"
    }

    /** 這個 code 會用到的所有樣板資源名（每一種合法 arity 一份）。 */
    val resourceNames: List<String> get() = arity.map { resourceName(it) }

    companion object {
        private val INFO_CODES = setOf(
            REQUIRED_ITEM_RESTORED, DEPRECATED_FIELD, FEATURE_UNSUPPORTED, LEGACY_BLOCK_IGNORED
        )

        private val BY_ID: Map<String, DiagnosticCode> = values().associateBy { it.id }

        /** 讀到不認得的 code 時回 null；§6.5 規定 UI **不得丟棄**該則診斷。 */
        fun byId(id: String): DiagnosticCode? = BY_ID[id]
    }
}

/**
 * `type_mismatch` 兩個參數用的**穩定代號**。
 *
 * ── 為什麼 args 不能直接寫成給人看的字 ───────────────────────────────
 * `type_mismatch` 的規範參數是 `[expected, found]`，而這兩個位置放的是
 * 「映射／序列／純量」這種術語 —— 它們是**要翻譯的**。如果產生端直接塞
 * `"a mapping"` 進 args，中文使用者就會看到「這裡應該是 a mapping」：
 * 訊息本身翻好了，洞開在參數上，而且外面看起來一切正常。
 *
 * 所以產生端只放代號，字面由各端自己的資源提供
 * （Android 是 `diag_kind_*`，見 [org.rimequad.ime.DiagnosticStrings]）。
 * 這些代號同時也是四端 args 的共同語彙。
 *
 * 順帶一提：對使用者不要說「純量」。這個 app 的使用者是全世界的麻瓜，
 * 英文那份寫的是 “a single value”，中文寫的是「單一的值」。
 */
enum class DiagnosticTerm(val id: String) {
    MAPPING("mapping"),
    SEQUENCE("sequence"),
    SCALAR("scalar"),
    NOTHING("null"),
    STRING_LIST("string-list"),
    LOCALIZED_STRING("localized-string");

    /** 字面值的資源名。與 [DiagnosticCode.resourceName] 同樣是純函式推導。 */
    val resourceName: String get() = "diag_kind_" + id.replace('-', '_')

    companion object {
        val ALL_RESOURCE_NAMES: List<String> get() = values().map { it.resourceName }
    }
}

/**
 * 一則診斷。
 *
 * @param path YAML 路徑（如 `keyboard.key_styles.default.background`）；根層級為空字串
 * @param line 來源行號；讀取層無法提供時為 null
 */
data class Diagnostic(
    val code: DiagnosticCode,
    val args: List<String>,
    val path: String,
    val line: Int? = null,
) {
    val severity: Severity get() = code.severity

    /**
     * §10 第 9 條的比對依據。**不含 args、不含行號、不含訊息**：
     * args 裡有檔案內容（會隨測試資料改動），行號隨排版變。
     */
    val identity: String
        get() = "$severity|${code.id}|${if (path.isEmpty()) "<document>" else path}"

    /**
     * 給開發者看的英文回退。**不上使用者畫面** —— UI 端請用
     * [org.rimequad.ime.theme.DiagnosticText] 查在地化樣板。
     */
    val developerMessage: String
        get() {
            val at = if (path.isEmpty()) "<document>" else path
            val where = if (line != null) ":$line" else ""
            return "[$severity] $at$where ${code.id}(${args.joinToString(", ")})"
        }

    override fun toString(): String = developerMessage
}

/**
 * 診斷累積器。
 *
 * ── 為什麼要去重 ──────────────────────────────────────────────────────
 * 同一個 `(severity, code, path)` 報兩次，對使用者是雜訊，對 §10 第 9 條是
 * **靜默失守**：某一端的取值器多被呼叫了一次，那一份壞檔案的診斷數就跟別端
 * 對不上，而畫面上什麼都看不出來。這在 Android 端已經真的發生過一次 ——
 * `LayoutParser` 為了先算 `auto_for_schema` 再檢查它含不含 `"*"`，
 * 對同一個節點呼叫了兩次 `stringList()`，型別錯的時候就是兩則一模一樣的
 * WARNING。
 *
 * 去重之後，「同一個路徑上真的有兩個不同的問題」就必須用**不同的 path**
 * 表達（例如每一列各自帶 `rows[i]`），這也是規範想要的：path 是身分的一部分。
 */
class Diagnostics {
    private val list = ArrayList<Diagnostic>()
    private val seen = HashSet<String>()

    val items: List<Diagnostic> get() = list

    val hasErrors: Boolean get() = list.any { it.severity == Severity.ERROR }

    val warningCount: Int get() = list.count { it.severity == Severity.WARNING }

    val infoCount: Int get() = list.count { it.severity == Severity.INFO }

    val errorCount: Int get() = list.count { it.severity == Severity.ERROR }

    /** §10 第 9 條要比對的序列。 */
    val identities: List<String> get() = list.map { it.identity }

    /**
     * 記一則診斷。嚴重度由 [code] 決定，這裡**沒有**可以指定它的參數 ——
     * 那正是重點。
     */
    fun add(
        code: DiagnosticCode,
        path: String,
        line: Int? = null,
        args: List<String> = emptyList(),
    ) {
        require(args.size in code.arity) {
            // 開發期的自我檢查：樣板的參數個數是照 code 固定的，數量不對會在
            // 使用者的畫面上變成 MissingFormatArgumentException 或一段空白。
            "${code.id} 需要 ${code.arity} 個參數，收到 ${args.size} 個"
        }
        val d = Diagnostic(code, args, path, line)
        if (seen.add(d.identity)) list.add(d)
    }

    fun addAll(other: Diagnostics) {
        for (d in other.list) if (seen.add(d.identity)) list.add(d)
    }

    fun snapshot(): List<Diagnostic> = ArrayList(list)
}

/** 載入結果。[value] 為 null 表示致命錯誤，呼叫端必須退回上一個成功的主題。 */
class LoadResult<T>(val value: T?, val diagnostics: List<Diagnostic>) {
    val isSuccess: Boolean get() = value != null
    val errors: List<Diagnostic> get() = diagnostics.filter { it.severity == Severity.ERROR }
    val warnings: List<Diagnostic> get() = diagnostics.filter { it.severity == Severity.WARNING }
    val infos: List<Diagnostic> get() = diagnostics.filter { it.severity == Severity.INFO }
}

/** 目標平台。決定 `platform_overrides` 取哪一個分支。 */
enum class Platform(val key: String) {
    ANDROID("android"),
    IOS("ios"),
    MACOS("macos"),
    WINDOWS("windows");

    companion object {
        val ALL_KEYS: Set<String> = values().map { it.key }.toSet()
    }
}
