package org.rimequad.ime.store

/**
 * 索引檔的資料模型與解析（docs/schema-store.md §1）。
 *
 * 解析政策：**單一套件壞掉不能弄垮整份索引**。缺欄位、類別對不上、
 * requires 指向不存在的套件——這些都只讓那一個套件出局並留下一條警告，
 * 其餘照常顯示。理由是索引由伺服器側產生，行動端無法修，
 * 若整份拒收，使用者連原本好好的方案都裝不了。
 *
 * 唯一會整份拒收的是 `format_version` 不相容：那代表欄位語意變了，
 * 硬讀下去只會產生沉默的錯誤行為。
 */

data class StoreCategory(
    val id: String,
    val name: String,
    val order: Int,
    /** true = 不在市集列表顯示（例如 prelude / essay 這類只作為相依的元件）。 */
    val hidden: Boolean,
)

/**
 * 套件提供的一個可切換方案。
 *
 * [language] 是 BCP 47 標記（`zh-Hant-TW`、`yue-Hant-HK`、`ja`…），
 * 由打包端的 `scripts/schema_store/languages.py` 判定，鍵盤類型選單用它分組。
 * `null` 或 `"und"` = 索引沒說（舊索引，或判不出來）—— 這種時候 app 會回落到
 * 隨 APK 出貨的對照表，再回落到字面啟發式，**不會**因此把方案藏起來。
 *
 * 為什麼掛在「套件的方案」而不是全域的 id 表：**方案 id 不是全域唯一的**。
 * `double_pinyin` 同時存在於 `double-pinyin`（借朙月拼音的繁體詞庫）與
 * `ice`（簡體詞庫），字集相反 —— 只有知道是哪個套件裝的才分得出來。
 */
data class StoreSchemaRef(
    val id: String,
    val name: String,
    val language: String? = null,
)

/** `verified.probe`：打包端實際驗證過的輸入樣本。 */
data class StoreProbe(val schema: String, val keys: String, val expect: String)

data class StorePackage(
    val id: String,
    val name: String,
    val category: String,
    val description: String,
    val upstream: String?,
    val upstreamCommit: String?,
    val license: String?,
    /** 相對於 base_url 的檔名，也可以是完整 URL。 */
    val file: String,
    val size: Long,
    val sha256: String,
    val schemas: List<StoreSchemaRef>,
    val requires: List<String>,
    val verifiedDeployed: Boolean,
    val probe: StoreProbe?,
    /**
     * 這個方案配哪一份軟鍵盤佈局（`qwerty` / `bopomofo-dachen` …）。
     * **啟用方案時必須連帶切過去**，否則使用者啟用注音卻看到 QWERTY，
     * 鍵面上根本沒有ㄅㄆㄇ，打不出任何東西。
     */
    val recommendedLayout: String?,
    /** 鍵面不理想時的說明，直接顯示給使用者。 */
    val layoutNote: String?,
    /** 推薦款：排在同類別最前面並加標記。 */
    val recommended: Boolean,
) {
    /** 只作為相依而不出現在切換清單裡的套件（規範：此陣列為空）。 */
    val isComponentOnly: Boolean get() = schemas.isEmpty()

    val schemaIds: List<String> get() = schemas.map { it.id }
}

/** 索引頂層的語言分組表：BCP 47 標記 → 顯示名與排序。 */
data class StoreLanguage(val tag: String, val name: String, val order: Int)

data class SchemaIndex(
    val formatVersion: Int,
    val generatedAt: String?,
    val baseUrl: String?,
    val categories: List<StoreCategory>,
    val packages: List<StorePackage>,
    /** 語言分組表。舊索引沒有這個欄位 → 空清單，分組回落到 app 內的對照表。 */
    val languages: List<StoreLanguage> = emptyList(),
    /**
     * 隨 APK 出貨的內建方案的語言標記。
     *
     * 它們不是從市集裝的，所以不在 [packages] 裡，但選單一樣要分組。
     * 放在索引裡是為了讓「新增一個內建方案」不必等 app 改版；
     * app 內另有一份同源的對照表（`core/schema-languages.json`）作為離線回落。
     */
    val builtinSchemas: List<StoreSchemaRef> = emptyList(),
) {
    private val byId = packages.associateBy { it.id }

    /** 方案 id → BCP 47 標記。內建方案也算在內。`und` 視同沒有。 */
    fun languageTags(): Map<String, String> {
        val out = LinkedHashMap<String, String>()
        packages.forEach { p ->
            p.schemas.forEach { s ->
                val t = s.language
                if (!t.isNullOrEmpty() && t != "und") out[s.id] = t
            }
        }
        builtinSchemas.forEach { s ->
            val t = s.language
            if (!t.isNullOrEmpty() && t != "und") out[s.id] = t
        }
        return out
    }

    fun packageById(id: String): StorePackage? = byId[id]

    /** 依 order 排好、且未隱藏的類別。 */
    fun visibleCategories(): List<StoreCategory> =
        categories.filter { !it.hidden }.sortedBy { it.order }

    /**
     * 某類別下要顯示的套件。推薦款排前面（見 `recommended`），
     * 其餘依名稱排序 —— 上游光拼音類就十幾個變體，沒有排序使用者選不下去。
     */
    fun packagesIn(categoryId: String): List<StorePackage> =
        packages.filter { it.category == categoryId }
            .sortedWith(compareByDescending<StorePackage> { it.recommended }.thenBy { it.name })

    /** 找出提供某個方案 id 的套件。 */
    fun packageProviding(schemaId: String): StorePackage? =
        packages.firstOrNull { p -> p.schemas.any { it.id == schemaId } }

    companion object {
        const val SUPPORTED_FORMAT_VERSION = 1
    }
}

sealed class IndexParseResult {
    data class Ok(val index: SchemaIndex, val warnings: List<String>) : IndexParseResult()
    data class Err(val message: String) : IndexParseResult()
}

object IndexParser {

    fun parse(text: String): IndexParseResult {
        val root = try {
            MiniJson.parse(text)
        } catch (e: JsonSyntaxException) {
            return IndexParseResult.Err("索引不是合法的 JSON：${e.message}")
        }
        if (root !is Json.Obj) return IndexParseResult.Err("索引的頂層必須是物件")

        val version = root.long("format_version")?.toInt()
            ?: return IndexParseResult.Err("索引缺少 format_version")
        if (version != SchemaIndex.SUPPORTED_FORMAT_VERSION) {
            return IndexParseResult.Err(
                "索引 format_version=$version，本版只支援 " +
                    "${SchemaIndex.SUPPORTED_FORMAT_VERSION}。請更新 app。"
            )
        }

        val warnings = ArrayList<String>()

        val categories = root.arr("categories").mapIndexedNotNull { i, node ->
            val id = node.str("id")
            if (id.isNullOrEmpty()) {
                warnings.add("categories[$i] 缺少 id，已略過")
                null
            } else {
                StoreCategory(
                    id = id,
                    name = node.str("name") ?: id,
                    order = node.long("order")?.toInt() ?: (100 + i),
                    hidden = node.bool("hidden") ?: false,
                )
            }
        }
        val categoryIds = categories.map { it.id }.toSet()

        // 語言分組表與內建方案的語言標記。兩者都是**選用**欄位：
        // 舊索引沒有它們時只是分組退回 app 內的對照表，不該讓整份索引出局。
        val languages = root.arr("languages").mapNotNull { node ->
            val tag = node.str("tag") ?: return@mapNotNull null
            StoreLanguage(tag, node.str("name") ?: tag, node.long("order")?.toInt() ?: 100)
        }
        val builtinSchemas = root.arr("builtin_schemas").mapNotNull { node ->
            val sid = node.str("id") ?: return@mapNotNull null
            StoreSchemaRef(sid, node.str("name") ?: sid, node.str("language"))
        }

        val seen = HashSet<String>()
        val packages = ArrayList<StorePackage>()
        root.arr("packages").forEachIndexed { i, node ->
            val where = "packages[$i]"
            val id = node.str("id")
            if (id.isNullOrEmpty()) { warnings.add("$where 缺少 id，已略過"); return@forEachIndexed }
            if (!seen.add(id)) { warnings.add("$where 的 id「$id」重複，已略過後者"); return@forEachIndexed }

            val file = node.str("file")
            if (file.isNullOrEmpty()) { warnings.add("套件 $id 缺少 file，已略過"); return@forEachIndexed }

            val sha = node.str("sha256")?.lowercase()
            if (sha.isNullOrEmpty()) {
                // 規範：行動端**必須**驗證 sha256。沒有 sha256 就沒有辦法驗，
                // 直接讓這個套件出局，而不是「先裝了再說」。
                warnings.add("套件 $id 缺少 sha256，無法驗證完整性，已略過")
                return@forEachIndexed
            }
            if (!sha.matches(SHA256_RE)) {
                warnings.add("套件 $id 的 sha256 不是 64 位十六進位字串，已略過")
                return@forEachIndexed
            }

            var category = node.str("category") ?: "other"
            if (categoryIds.isNotEmpty() && category !in categoryIds) {
                warnings.add("套件 $id 的 category「$category」不在 categories 中，歸入 other")
                category = "other"
            }

            val schemas = node.arr("schemas").mapNotNull { s ->
                val sid = s.str("id") ?: return@mapNotNull null
                StoreSchemaRef(sid, s.str("name") ?: sid, s.str("language"))
            }

            val verified = node["verified"]
            val probeNode = verified?.get("probe")
            val probe = probeNode?.let {
                val schema = it.str("schema") ?: return@let null
                StoreProbe(schema, it.str("keys") ?: "", it.str("expect") ?: "")
            }

            packages.add(
                StorePackage(
                    id = id,
                    name = node.str("name") ?: id,
                    category = category,
                    description = node.str("description") ?: "",
                    upstream = node.str("upstream"),
                    upstreamCommit = node.str("upstream_commit"),
                    license = node.str("license"),
                    file = file,
                    size = node.long("size") ?: 0L,
                    sha256 = sha,
                    schemas = schemas,
                    requires = node.strings("requires"),
                    verifiedDeployed = verified?.bool("deployed") ?: false,
                    probe = probe,
                    recommendedLayout = node.str("recommended_layout"),
                    layoutNote = node.str("layout_note"),
                    recommended = node.bool("recommended") ?: false,
                )
            )
        }

        // requires 指向不存在的套件：不在這裡淘汰套件（相依可能是刻意留給
        // 之後的索引版本），但要在解析階段就留下警告，否則使用者按下「安裝」
        // 才會第一次看到問題。
        val ids = packages.map { it.id }.toSet()
        packages.forEach { p ->
            p.requires.filterNot { it in ids }.forEach {
                warnings.add("套件 ${p.id} 的 requires 指向不存在的套件「$it」")
            }
        }

        if (packages.isEmpty()) return IndexParseResult.Err("索引裡沒有任何可用的套件")

        return IndexParseResult.Ok(
            SchemaIndex(
                formatVersion = version,
                generatedAt = root.str("generated_at"),
                baseUrl = root.str("base_url"),
                categories = categories,
                packages = packages,
                languages = languages,
                builtinSchemas = builtinSchemas,
            ),
            warnings,
        )
    }

    private val SHA256_RE = Regex("^[0-9a-f]{64}$")
}

/**
 * 相依解析（規範 §3：「遞迴展開 requires，扣掉已安裝的」）。
 *
 * ── 循環相依是**合法**的，不是錯誤 ─────────────────────────────────────
 * 真索引裡 `luna-pinyin` 與 `stroke` 互相 `requires`：彼此互為反查詞庫，
 * 上游本來就設計成要一起裝。規範 §3 只說「遞迴展開」，沒說循環怎麼辦；
 * 照字面實作會無限遞迴，而把循環當錯誤擋掉則會讓這兩個最常用的套件
 * **永遠裝不了**。
 *
 * 所以政策是：用 visited set 終止遞迴，循環中的每個套件各收一次，
 * 全部一起下載。這也是為什麼下面的順序只是「盡量讓相依在前」而不是
 * 嚴格的拓撲序 —— 有環的圖根本沒有拓撲序。
 *
 * 順序之所以不影響正確性，是因為 [SchemaStore.install] 會把**所有**套件
 * 都解壓完才呼叫一次 rs_deploy()，中間不會有「詞典還沒到就先部署」的空窗。
 */
object DependencyResolver {

    data class Plan(
        /** 需要下載的套件，盡量讓相依排在前面。 */
        val toDownload: List<StorePackage>,
        /** 已安裝而被扣掉的相依，UI 可以說明「另有 N 個已安裝」。 */
        val alreadyInstalled: List<String>,
        /** 偵測到的循環相依，僅供顯示／記錄，不是錯誤。 */
        val cycles: List<List<String>> = emptyList(),
    ) {
        /** 要下載的位元組數（zip）。 */
        val totalBytes: Long get() = toDownload.sumOf { it.size }

        /**
         * 安裝後實際佔用的**概估**。
         *
         * 索引的 `size` 是 zip 大小，嚴重低估實際佔用。實測 luna-pinyin：
         *   zip 0.4 MB → 解壓後 0.9 MB → librime 部署產生的 build 產物 13 MB
         * 也就是說「將下載 0.4 MB」會讓使用者以為只花幾百 KB，實際上多出十幾 MB。
         *
         * 索引目前沒有欄位可以表達這件事（建議補 `unpacked_size` 與
         * `deployed_size`，見交付報告）。在那之前只能用倍率估，
         * 而**估算值必須在 UI 上明講是概估**，不能假裝精確。
         */
        val estimatedInstalledBytes: Long get() = totalBytes * INSTALLED_SIZE_MULTIPLIER

        val count: Int get() = toDownload.size
    }

    /** 唯一的實測資料點（luna-pinyin：0.4MB zip → 13MB 部署後）約 32 倍，取 30。 */
    const val INSTALLED_SIZE_MULTIPLIER = 30L

    sealed class Result {
        data class Ok(val plan: Plan) : Result()
        data class MissingDependency(val missing: String, val requiredBy: String) : Result()
        data class UnknownPackage(val id: String) : Result()
    }

    fun resolve(
        index: SchemaIndex,
        selected: List<String>,
        installed: Set<String>,
    ): Result {
        val order = ArrayList<StorePackage>()
        val done = HashSet<String>()
        val onStack = LinkedHashSet<String>()
        val skipped = LinkedHashSet<String>()
        val cycles = ArrayList<List<String>>()

        fun visit(id: String, requiredBy: String?): Result? {
            if (id in installed) { skipped.add(id); return null }
            if (id in done) return null
            if (id in onStack) {
                // 循環：記下來給 UI 顯示，然後就地收手。這個套件在遞迴回捲時
                // 一定會被加進 order，不會漏掉。
                cycles.add(onStack.dropWhile { it != id } + id)
                return null
            }
            val pkg = index.packageById(id)
                ?: return if (requiredBy == null) Result.UnknownPackage(id)
                else Result.MissingDependency(id, requiredBy)

            onStack.add(id)
            for (dep in pkg.requires) {
                visit(dep, id)?.let { return it }
            }
            onStack.remove(id)
            done.add(id)
            order.add(pkg)
            return null
        }

        for (id in selected) {
            visit(id, null)?.let { return it }
        }
        return Result.Ok(Plan(order, skipped.toList(), cycles))
    }

    /**
     * 反向查詢：若移除 [candidate]，哪些**仍在使用中**的套件會失去相依。
     * 解除安裝前必須問這個問題，否則使用者刪掉 essay 之後所有方案一起壞掉。
     */
    fun dependentsOf(
        index: SchemaIndex,
        candidate: String,
        installed: Set<String>,
    ): List<String> = installed
        .filter { it != candidate }
        .filter { index.packageById(it)?.requires?.contains(candidate) == true }
}

/** 人看的大小字串。索引的 size 以位元組計。 */
fun formatBytes(bytes: Long): String = when {
    bytes >= 1024L * 1024 * 1024 -> String.format("%.2f GB", bytes / 1024.0 / 1024 / 1024)
    bytes >= 1024L * 1024 -> String.format("%.1f MB", bytes / 1024.0 / 1024)
    bytes >= 1024L -> String.format("%.0f KB", bytes / 1024.0)
    else -> "$bytes B"
}
