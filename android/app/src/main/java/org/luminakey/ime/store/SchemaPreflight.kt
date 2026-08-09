package org.luminakey.ime.store

import org.luminakey.ime.R
import org.luminakey.ime.theme.MiniYaml
import org.luminakey.ime.theme.YamlNode
import java.io.File

/**
 * 部署前的相依檢查。
 *
 * 規範 §4 要求：使用者自帶檔案部署失敗時，「錯誤訊息必須明確告訴使用者
 * **缺少哪一個詞典**，而不是只說部署失敗」。
 *
 * 而 `rs_last_error()` 給不了這個資訊 —— librime 的部署失敗只會回一個
 * 布林，細節散在 glog 的輸出裡，行動端拿不到。所以與其事後解讀，
 * 不如**事前自己算**：把 schema yaml 讀一遍，把它宣告要用的檔案列出來，
 * 逐一確認在 user / shared 目錄裡找得到。
 *
 * 這樣做還有一個附帶好處：檢查在**修改 schema_list 之前**跑，
 * 缺東西時根本不會去動 default.custom.yaml，也就不需要回滾。
 * 回滾仍然必須存在（見 [SchemaStore]），但它只需要處理
 * 「檔案都在、librime 仍然編不起來」這種預測不到的情形。
 *
 * ⚠ 這**不是**一個完整的 librime 配置解析器。它刻意只認幾個高頻欄位，
 * 而且只在「找不到」時說話 —— 誤報會擋住合法套件，比漏報更糟。
 *
 * ## 為什麼要分 [Severity]
 *
 * 第一版把每一個找不到的檔案都當成「不給啟用」，結果**市集 98 個方案裡
 * 有 20 個被自己的預檢擋死**（倉頡系列的 `luna_quanpin`、粵拼與魔然的
 * `cangjie5`、五筆與鄭碼的 `pinyin_simp`……）—— 而 librime 根本不在乎
 * 這些檔案在不在。查 librime 原始碼確認過分界線：
 *
 * | 宣告 | librime 的行為 | 我們 |
 * |---|---|---|
 * | `translator/dictionary` 缺 | `SchemaUpdate::Run` 回 false，**部署真的失敗** | 擋 |
 * | `__include` / `import_preset` 的目標缺 | 配置編譯器 `resource not found` → 回 false | 擋 |
 * | `schema/dependencies` 缺 | `WorkspaceUpdate`：`skipped unsatisfied dependency`，只是 warning | 放行 |
 * | `reverse_lookup/dictionary` 等次要詞典缺 | `SchemaUpdate` 只看 `translator/dictionary`，碰都沒碰 | 放行 |
 * | `grammar/language` 的 `.gram` 缺 | 語言模型建不起來，翻譯器照常運作 | 放行 |
 *
 * 放行的那些仍然照樣回報，只是標成 [Severity.WARNING]：使用者該知道
 * 「這個方案的筆畫反查用不了」，但那不是不讓他打字的理由。
 */
object SchemaPreflight {

    enum class Kind {
        /** translator / reverse_lookup 的 dictionary。這是最常缺的東西。 */
        DICTIONARY,

        /** schema/dependencies 指名的另一個方案。 */
        SCHEMA,

        /** __include / import_preset 指向的配置檔。 */
        CONFIG,

        /** grammar/language 指向的語言模型。 */
        GRAMMAR,
    }

    /** 缺這個檔案會不會讓 librime 的部署整個失敗。 */
    enum class Severity {
        /** 部署一定失敗 —— 不要動 schema_list。 */
        BLOCKING,

        /** 部署會成功，但某個功能不會有作用。告知，不阻擋。 */
        WARNING,
    }

    data class Missing(
        val kind: Kind,
        val fileName: String,
        val referencedBy: String,
        val severity: Severity = Severity.BLOCKING,
    ) {
        /**
         * 給使用者看的一行說明。
         *
         * ⚠ 回傳 [UiMessage]（資源 id + 參數）而不是字串 —— 這些話會直接畫在
         * 「啟用方案」失敗的對話框上，而預設語系是英文。理由見 [UiMessage] 檔頭。
         *
         * 資源名的前綴就是嚴重度（`preflight_block_` / `preflight_warn_`），
         * 而 `SchemaPreflightTest` 用反射比對 `R.string` 的欄位名 —— 所以
         * 「擋的那一條指到放行的樣板」這種手滑會被抓到，不必靠讀措辭。
         */
        fun uiMessage(): UiMessage = UiMessage.of(resId(), referencedBy, fileName)

        private fun resId(): Int = when (severity) {
            Severity.BLOCKING -> when (kind) {
                Kind.DICTIONARY -> R.string.preflight_block_dictionary
                Kind.SCHEMA -> R.string.preflight_block_schema
                Kind.CONFIG -> R.string.preflight_block_config
                Kind.GRAMMAR -> R.string.preflight_block_grammar
            }
            // 措辭刻意跟上面不同：使用者要能一眼看出「這條不擋我」。
            Severity.WARNING -> when (kind) {
                Kind.DICTIONARY -> R.string.preflight_warn_dictionary
                Kind.SCHEMA -> R.string.preflight_warn_schema
                Kind.CONFIG -> R.string.preflight_warn_config
                Kind.GRAMMAR -> R.string.preflight_warn_grammar
            }
        }
    }

    data class Report(val schemaId: String, val missing: List<Missing>) {
        val blocking: List<Missing> get() = missing.filter { it.severity == Severity.BLOCKING }
        val warnings: List<Missing> get() = missing.filter { it.severity == Severity.WARNING }

        /** 「可以啟用嗎」。注意這**不是**「沒有任何東西缺」。 */
        val ok: Boolean get() = blocking.isEmpty()
    }

    /**
     * 檢查 [schemaFile]（一份 `*.schema.yaml`）。
     * [searchDirs] 依 librime 的搜尋順序給：user 在前、shared 在後。
     */
    fun check(schemaFile: File, searchDirs: List<File>): Report {
        val text = runCatching { schemaFile.readText(Charsets.UTF_8) }.getOrNull()
            ?: return Report(
                schemaFile.name,
                listOf(Missing(Kind.SCHEMA, schemaFile.name, schemaFile.name)),
            )
        val id = schemaFile.name.removeSuffix(".schema.yaml")
        return Report(id, checkText(id, text, searchDirs))
    }

    internal fun checkText(schemaId: String, text: String, searchDirs: List<File>): List<Missing> {
        val root = runCatching { MiniYaml.parse(schemaId, text).root }.getOrNull()
            ?: return emptyList() // 解析不了就別亂猜；讓 librime 自己去報錯
        val wanted = LinkedHashMap<String, Pair<Kind, Severity>>()
        collect(root, key = null, topKey = null, out = wanted)

        val declaredId = ((root as? YamlNode.Mapping)
            ?.entries?.get("schema") as? YamlNode.Mapping)
            ?.entries?.get("schema_id")
            ?.let { (it as? YamlNode.Scalar)?.value } ?: schemaId

        return wanted.entries
            .filterNot { (name, _) -> exists(name, searchDirs) }
            .map { (name, kv) -> Missing(kv.first, name, declaredId, kv.second) }
    }

    private fun exists(name: String, dirs: List<File>): Boolean =
        dirs.any { File(it, name).isFile }

    /**
     * 遞迴走整棵樹，收集「這份 yaml 需要哪些檔案」。
     *
     * [topKey] 是「從根往下走時經過的第一層鍵」。分界線就靠它：librime 的
     * `SchemaUpdate::Run` 只編 `translator/dictionary` 這一本，別處的
     * `dictionary:`（`reverse_lookup`、`custom_phrase`、`translator@xxx`……）
     * 它連讀都沒讀。
     */
    private fun collect(
        node: YamlNode,
        key: String?,
        topKey: String?,
        out: MutableMap<String, Pair<Kind, Severity>>,
    ) {
        fun put(name: String, kind: Kind, severity: Severity) {
            val old = out[name]
            // 同一個檔案被兩處引用時，取比較嚴的那一個。
            if (old == null || (old.second == Severity.WARNING && severity == Severity.BLOCKING)) {
                out[name] = kind to severity
            }
        }

        when (node) {
            is YamlNode.Scalar -> {
                val v = node.value ?: return
                if (v.isEmpty()) return
                when (key) {
                    // translator/dictionary 是主詞典，缺了部署一定失敗。
                    // 其餘位置的 dictionary 是次要詞典，librime 不在部署期碰它。
                    "dictionary" -> put(
                        "$v.dict.yaml",
                        Kind.DICTIONARY,
                        if (topKey == "translator") Severity.BLOCKING else Severity.WARNING,
                    )
                    // punctuator/import_preset: default → default.yaml
                    "import_preset" -> put("$v.yaml", Kind.CONFIG, Severity.BLOCKING)
                    // __include: pinyin:/xxx 或 __include: symbols.yaml:/xxx
                    "__include", "__patch" -> includeTarget(v)?.let {
                        put(
                            it.fileName,
                            Kind.CONFIG,
                            // 結尾的 `?` 是 librime 的「可有可無」記號：
                            // ResolveReference 找不到時只印 INFO，Resolve 回 true。
                            if (it.optional) Severity.WARNING else Severity.BLOCKING,
                        )
                    }
                    // grammar/language: zh-hans-t-essay-bgw → .gram
                    "language" -> if (v.contains('-')) {
                        put("$v.gram", Kind.GRAMMAR, Severity.WARNING)
                    }
                    // schema/dependencies 的每一項。librime 找不到時只印 warning
                    // 「skipped unsatisfied dependency」，主方案照樣部署成功。
                    "dependencies" -> put("$v.schema.yaml", Kind.SCHEMA, Severity.WARNING)
                }
            }

            is YamlNode.Sequence ->
                // 序列項目沿用父層的鍵，這樣 dependencies: [- stroke] 才認得出來
                node.items.forEach { collect(it, key, topKey, out) }

            is YamlNode.Mapping ->
                node.entries.forEach { (k, v) -> collect(v, k, topKey ?: k, out) }
        }
    }

    /** [includeTarget] 的結果。 */
    internal data class IncludeRef(val fileName: String, val optional: Boolean)

    /**
     * `__include` 指到哪個檔案，或者根本不指到檔案。
     *
     * ⚠ 這裡曾經整段寫錯，代價是市集裡十個方案按下「啟用」被擋死。
     * librime 自己的註解（`config_compiler.cc`，`ParseInclude` 上方）就是答案：
     *
     * ```
     * // __include: path/to/local/node
     * // __include: filename[.yaml]:/path/to/external/node
     * ```
     *
     * **有沒有冒號才是分界線，不是開頭的斜線。** `CreateReference()` 找不到 `:`
     * 時，resource_id 直接取 `current_resource_id()` —— 也就是同一個檔案。所以
     * 倉頡的 `__include: array30_format`、魔然的 `__include: reverse_format`
     * 都是同檔案裡的節點（那兩份 yaml 各自都有這個頂層鍵），一個檔案都不缺。
     *
     * 結尾的 `?` 代表可有可無（`Reference::optional`），缺了不算失敗。
     */
    internal fun includeTarget(raw: String): IncludeRef? {
        var v = raw.trim()
        val optional = v.endsWith("?")
        if (optional) v = v.dropLast(1).trim()
        val sep = v.indexOf(':')
        if (sep <= 0) return null           // 沒有冒號 → 同檔案內的節點；冒號在開頭 → 同上
        val head = v.substring(0, sep).trim()
        if (head.isEmpty()) return null
        return IncludeRef(if (head.endsWith(".yaml")) head else "$head.yaml", optional)
    }
}
