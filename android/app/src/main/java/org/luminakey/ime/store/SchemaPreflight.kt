package org.luminakey.ime.store

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

    data class Missing(val kind: Kind, val fileName: String, val referencedBy: String) {
        fun humanMessage(): String = when (kind) {
            Kind.DICTIONARY ->
                "方案「$referencedBy」需要詞典 $fileName，但 user 與 shared 目錄裡都沒有。" +
                    "請一併導入提供這本詞典的套件。"
            Kind.SCHEMA ->
                "方案「$referencedBy」宣告相依於方案檔 $fileName，目前找不到。"
            Kind.CONFIG ->
                "方案「$referencedBy」引用了配置檔 $fileName，目前找不到。"
            Kind.GRAMMAR ->
                "方案「$referencedBy」需要語言模型 $fileName，目前找不到。"
        }
    }

    data class Report(val schemaId: String, val missing: List<Missing>) {
        val ok: Boolean get() = missing.isEmpty()
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
        val wanted = LinkedHashMap<String, Kind>()
        collect(root, null, wanted)

        val declaredId = ((root as? YamlNode.Mapping)
            ?.entries?.get("schema") as? YamlNode.Mapping)
            ?.entries?.get("schema_id")
            ?.let { (it as? YamlNode.Scalar)?.value } ?: schemaId

        return wanted.entries
            .filterNot { (name, _) -> exists(name, searchDirs) }
            .map { (name, kind) -> Missing(kind, name, declaredId) }
    }

    private fun exists(name: String, dirs: List<File>): Boolean =
        dirs.any { File(it, name).isFile }

    /** 遞迴走整棵樹，收集「這份 yaml 需要哪些檔案」。 */
    private fun collect(node: YamlNode, key: String?, out: MutableMap<String, Kind>) {
        when (node) {
            is YamlNode.Scalar -> {
                val v = node.value ?: return
                if (v.isEmpty()) return
                when (key) {
                    // translator/dictionary、reverse_lookup/dictionary
                    "dictionary" -> out.putIfAbsent("$v.dict.yaml", Kind.DICTIONARY)
                    // punctuator/import_preset: default → default.yaml
                    "import_preset" -> out.putIfAbsent("$v.yaml", Kind.CONFIG)
                    // __include: pinyin:/xxx 或 __include: symbols.yaml:/xxx
                    "__include", "__patch" -> includeTarget(v)?.let {
                        out.putIfAbsent(it, Kind.CONFIG)
                    }
                    // grammar/language: zh-hans-t-essay-bgw → .gram
                    "language" -> if (v.contains('-')) out.putIfAbsent("$v.gram", Kind.GRAMMAR)
                    // schema/dependencies 的每一項
                    "dependencies" -> out.putIfAbsent("$v.schema.yaml", Kind.SCHEMA)
                }
            }

            is YamlNode.Sequence ->
                // 序列項目沿用父層的鍵，這樣 dependencies: [- stroke] 才認得出來
                node.items.forEach { collect(it, key, out) }

            is YamlNode.Mapping ->
                node.entries.forEach { (k, v) -> collect(v, k, out) }
        }
    }

    /**
     * `__include` 的目標形如 `pinyin:/xxx`、`symbols.yaml:/xxx`、`/local/path`。
     * 只有前兩種指向別的檔案；以 `/` 開頭的是同檔案內的節點，不需要檔案。
     */
    internal fun includeTarget(raw: String): String? {
        val v = raw.trim()
        if (v.startsWith("/")) return null
        val head = v.substringBefore(":/").substringBefore(':').trim()
        if (head.isEmpty()) return null
        return if (head.endsWith(".yaml")) head else "$head.yaml"
    }
}
