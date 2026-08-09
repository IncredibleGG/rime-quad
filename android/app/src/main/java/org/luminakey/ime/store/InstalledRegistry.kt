package org.luminakey.ime.store

import java.io.File

/**
 * 「裝了什麼」的帳本。
 *
 * ── 已安裝 ≠ 已啟用 ─────────────────────────────────────────────────────
 * 這兩件事是**獨立的狀態**，不能合成一個開關：
 *
 *   已安裝 = 檔案在 user_data_dir 裡（本檔案記錄）
 *   已啟用 = 出現在 default.custom.yaml 的 schema_list（[SchemaListPatch] 記錄）
 *
 * 理由是部署成本：librime 每次部署會編譯 schema_list 上的**每一個**方案。
 * 實測三個方案 7.2 秒，而且是累加的。使用者裝十個方案卻只用兩個，
 * 若「安裝」等於「啟用」，他往後每一次部署都在替八個用不到的方案編詞庫。
 *
 * 因此：
 *   · 停用 → 只從 schema_list 移除並重新部署，**檔案留著**，
 *            之後要用直接加回去，不必重新下載。
 *   · 解除安裝 → 才真的刪檔案，且必須先確認沒有其他已安裝套件依賴它。
 *
 * ── 為什麼帳本放在 user_data_dir 而不是 SharedPreferences ──────────────
 * 它記的是「那個目錄裡有哪些檔案是誰放的」，跟目錄同生共死。使用者清掉
 * app 資料時兩者一起消失，不會出現「帳本說裝了但檔案沒了」的鬼狀態。
 */
data class InstalledPackage(
    val id: String,
    val name: String,
    val sha256: String,
    val installedAt: Long,
    val schemas: List<StoreSchemaRef>,
    /** 解壓出來的檔案（相對 user_data_dir），解除安裝時要刪的就是這些。 */
    val files: List<String>,
    val requires: List<String>,
    val recommendedLayout: String?,
    val layoutNote: String?,
    /** "store" = 市集下載；"local" = 使用者自帶。 */
    val source: String,
) {
    val schemaIds: List<String> get() = schemas.map { it.id }
}

class InstalledRegistry private constructor(
    private val file: File,
    private val entries: LinkedHashMap<String, InstalledPackage>,
) {

    val all: List<InstalledPackage> get() = entries.values.toList()

    val ids: Set<String> get() = entries.keys.toSet()

    fun get(id: String): InstalledPackage? = entries[id]

    fun isInstalled(id: String): Boolean = entries.containsKey(id)

    /** 某個方案 id 建議搭配的佈局。IME 切換方案時查這裡。 */
    fun layoutForSchema(schemaId: String): String? =
        entries.values.firstOrNull { p -> p.schemas.any { it.id == schemaId } }?.recommendedLayout

    fun noteForSchema(schemaId: String): String? =
        entries.values.firstOrNull { p -> p.schemas.any { it.id == schemaId } }?.layoutNote

    /**
     * 已安裝方案的 BCP 47 語言標記（見 [StoreSchemaRef.language]）。
     *
     * 這是三層來源裡最準的一層：帳本知道每個方案是**哪一個套件**裝的，
     * 而方案 id 不是全域唯一的 —— `double_pinyin` 在 `double-pinyin` 是繁體、
     * 在 `ice` 是簡體，只看 id 分不出來。`und` 與空字串視同沒有標記。
     */
    fun languageTags(): Map<String, String> {
        val out = LinkedHashMap<String, String>()
        entries.values.forEach { p ->
            p.schemas.forEach { s ->
                val t = s.language
                if (!t.isNullOrEmpty() && t != "und") out[s.id] = t
            }
        }
        return out
    }

    /** 本地已安裝套件之中，有誰宣告 requires [id]。 */
    fun dependents(id: String): List<InstalledPackage> =
        entries.values.filter { it.id != id && it.requires.contains(id) }

    fun put(pkg: InstalledPackage) {
        entries[pkg.id] = pkg
        save()
    }

    fun remove(id: String): InstalledPackage? {
        val gone = entries.remove(id)
        if (gone != null) save()
        return gone
    }

    fun save() {
        val sb = StringBuilder()
        sb.append("{\n")
        sb.append("  \"format_version\": ").append(FORMAT_VERSION).append(",\n")
        sb.append("  \"packages\": [\n")
        entries.values.forEachIndexed { i, p ->
            sb.append("    {\n")
            sb.append("      \"id\": ").append(q(p.id)).append(",\n")
            sb.append("      \"name\": ").append(q(p.name)).append(",\n")
            sb.append("      \"sha256\": ").append(q(p.sha256)).append(",\n")
            sb.append("      \"installed_at\": ").append(p.installedAt).append(",\n")
            sb.append("      \"source\": ").append(q(p.source)).append(",\n")
            sb.append("      \"recommended_layout\": ").append(qn(p.recommendedLayout)).append(",\n")
            sb.append("      \"layout_note\": ").append(qn(p.layoutNote)).append(",\n")
            sb.append("      \"requires\": ")
                .append(p.requires.joinToString(", ", "[", "]") { q(it) }).append(",\n")
            sb.append("      \"files\": ")
                .append(p.files.joinToString(", ", "[", "]") { q(it) }).append(",\n")
            sb.append("      \"schemas\": [")
            sb.append(p.schemas.joinToString(", ") {
                "{\"id\": ${q(it.id)}, \"name\": ${q(it.name)}, " +
                    "\"language\": ${qn(it.language)}}"
            })
            sb.append("]\n")
            sb.append("    }").append(if (i == entries.size - 1) "\n" else ",\n")
        }
        sb.append("  ]\n}\n")
        file.parentFile?.mkdirs()
        // 先寫暫存再改名：帳本寫到一半斷電會讓使用者永久卡在壞掉的狀態。
        val tmp = File(file.parentFile, file.name + ".tmp")
        tmp.writeText(sb.toString(), Charsets.UTF_8)
        if (file.exists()) file.delete()
        if (!tmp.renameTo(file)) {
            tmp.copyTo(file, overwrite = true)
            tmp.delete()
        }
    }

    companion object {
        const val FILE_NAME = "rimequad-store.json"
        const val FORMAT_VERSION = 1

        fun load(userDataDir: File): InstalledRegistry {
            val f = File(userDataDir, FILE_NAME)
            val map = LinkedHashMap<String, InstalledPackage>()
            if (f.isFile) {
                val root = runCatching { f.readText(Charsets.UTF_8) }.getOrNull()
                    ?.let { MiniJson.parseOrNull(it) }
                root?.arr("packages")?.forEach { n ->
                    val id = n.str("id") ?: return@forEach
                    map[id] = InstalledPackage(
                        id = id,
                        name = n.str("name") ?: id,
                        sha256 = n.str("sha256") ?: "",
                        installedAt = n.long("installed_at") ?: 0L,
                        schemas = n.arr("schemas").mapNotNull { s ->
                            val sid = s.str("id") ?: return@mapNotNull null
                            StoreSchemaRef(sid, s.str("name") ?: sid, s.str("language"))
                        },
                        files = n.strings("files"),
                        requires = n.strings("requires"),
                        recommendedLayout = n.str("recommended_layout"),
                        layoutNote = n.str("layout_note"),
                        source = n.str("source") ?: "store",
                    )
                }
            }
            return InstalledRegistry(f, map)
        }

        private fun q(s: String): String {
            val sb = StringBuilder("\"")
            for (c in s) {
                when {
                    c == '"' -> sb.append("\\\"")
                    c == '\\' -> sb.append("\\\\")
                    c == '\n' -> sb.append("\\n")
                    c == '\r' -> sb.append("\\r")
                    c == '\t' -> sb.append("\\t")
                    c.code < 0x20 -> sb.append(String.format("\\u%04x", c.code))
                    else -> sb.append(c)
                }
            }
            return sb.append('"').toString()
        }

        private fun qn(s: String?): String = if (s == null) "null" else q(s)
    }
}
