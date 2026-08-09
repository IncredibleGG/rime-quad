package org.luminakey.ime.keyboard

import android.content.res.AssetManager
import android.util.Log
import org.luminakey.ime.store.Json
import org.luminakey.ime.store.MiniJson

/**
 * 方案 → BCP 47 語言標記的對照表。
 *
 * ── 為什麼需要它 ────────────────────────────────────────────────────────
 * 鍵盤類型選單要依語言分組，但資料裡本來沒有語言資訊：`rs_schema_list()`
 * 只給 `(id, name)`，librime 的 `schema.yaml` 沒有語言／地區欄位，方案市集
 * 索引的 `category` 是「拼音類／形碼類」而不是語言、而且不涵蓋內建方案。
 * 於是 [KeyboardTypes.groupTitleOf] 只能從 id 後綴與方案名的字面推
 * —— `_tw` 是慣例不是規範，方案名是自由字串，而「名字裡有漢字」會把日文的
 * 「河童五筆」也算成中文。
 *
 * 判定搬到了打包端（`scripts/schema_store/languages.py`），依據可追溯：
 * rppi 的分類路徑、方案名裡的地區字樣、詞庫的繁簡字集探針。
 * 判不出來的標 `und`，**不猜**。
 *
 * ── 三層來源，由新到舊 ──────────────────────────────────────────────────
 *   1. [overlay]  已安裝套件的帳本（InstalledRegistry）。最準：它知道這個
 *                 方案是**哪一個套件**裝的。方案 id 不是全域唯一的 ——
 *                 `double_pinyin` 同時存在於 `double-pinyin`（繁體詞庫）與
 *                 `ice`（簡體詞庫），只看 id 分不出來。
 *   2. [shipped]  隨 APK 出貨的 `rime/schema-languages.json`。涵蓋內建的四個
 *                 方案（它們不在索引的 packages 裡）以及打包當下的全部市集
 *                 方案。索引可能比 app 舊、也可能還沒下載過，所以這一份必須存在。
 *   3. 都沒有  →  回落到 [KeyboardTypes] 的字面啟發式。索引比 app 新、
 *                 使用者自己丟進 user_data_dir 的方案，都會落在這裡。
 *
 * 第 3 層**刻意保留**。這張表是資料，資料一定會有涵蓋不到的一天；
 * 那一天使用者要的是「分組沒那麼準」，不是「方案從選單裡消失」。
 */
data class LanguageTable(
    /** schema id → BCP 47 標記。 */
    val tags: Map<String, String>,
    /** 標記 → 顯示名（zh-Hant-TW 語系，與 [ConfigRepository.LOCALE] 一致）。 */
    val names: Map<String, String>,
    /** 標記 → 分組排序。沒列到的排在已列到的後面。 */
    val order: Map<String, Int>,
) {
    fun tagOf(schemaId: String): String? = tags[schemaId]?.takeIf { it.isNotEmpty() && it != UND }

    fun titleOf(tag: String): String = names[tag] ?: tag

    fun orderOf(tag: String): Int = order[tag] ?: UNRANKED

    /** 疊上一層更新的對照（帳本）。新的勝出，顯示名沿用本表。 */
    fun overlay(extra: Map<String, String>): LanguageTable =
        if (extra.isEmpty()) this else copy(tags = tags + extra)

    companion object {
        const val UND = "und"

        /** 沒有標記的分組排在所有有標記的後面，但在「其他」之前。 */
        const val UNRANKED = 90

        val EMPTY = LanguageTable(emptyMap(), emptyMap(), emptyMap())
    }
}

/**
 * 對照表的**行程層單例**。
 *
 * 為什麼是單例而不是參數：[KeyboardTypes.build] 目前在 IME 打開選單的當下
 * 呼叫，那條路徑上拿不到 Context，而把 Context 一路傳進純函式會把
 * [KeyboardTypes] 的可測試性毀掉（它現在能在 JVM 單元測試裡直接餵資料跑）。
 * 折衷是把「資料從哪來」與「怎麼分組」分開：載入在這裡（有 Context 的地方），
 * 分組仍是純函式，測試直接指定 [table] 即可。
 */
object SchemaLanguages {

    private const val TAG = "SchemaLanguages"
    const val ASSET_PATH = "rime/schema-languages.json"

    @Volatile
    var table: LanguageTable = LanguageTable.EMPTY
        private set

    /** 隨 APK 出貨的那一份，載入一次就不再變。 */
    @Volatile
    private var shipped: LanguageTable = LanguageTable.EMPTY

    /**
     * 讀 APK 內的對照表。
     *
     * 讀不到、或內容壞掉，都只留一條警告就算了：分組不準是小事，
     * 為了一份錦上添花的中繼資料讓鍵盤起不來才是大事。
     */
    fun loadShipped(assets: AssetManager) {
        val text = runCatching {
            assets.open(ASSET_PATH).use { it.readBytes().toString(Charsets.UTF_8) }
        }.getOrElse {
            Log.w(TAG, "讀不到 $ASSET_PATH，分組回落到字面啟發式", it)
            return
        }
        val parsed = parse(text)
        if (parsed == null) {
            Log.w(TAG, "$ASSET_PATH 解析失敗，分組回落到字面啟發式")
            return
        }
        shipped = parsed
        table = parsed
        Log.i(TAG, "語言對照表載入 ${parsed.tags.size} 筆、${parsed.names.size} 種語言")
    }

    /**
     * 疊上已安裝套件帳本裡的語言標記。
     *
     * 帳本是從索引抄下來的，比隨 APK 出貨的那一份新，而且**知道方案是哪個
     * 套件裝的** —— 撞號的 `double_pinyin` 只有在這裡才分得出繁簡。
     */
    fun applyInstalled(extra: Map<String, String>) {
        table = shipped.overlay(extra.filterValues { it.isNotEmpty() && it != LanguageTable.UND })
    }

    /** 測試用：直接指定整張表。 */
    fun setForTest(t: LanguageTable) {
        shipped = t
        table = t
    }

    internal fun parse(text: String): LanguageTable? {
        val root = MiniJson.parseOrNull(text) ?: return null
        if (root.long("format_version") != 1L) {
            Log.w(TAG, "語言對照表 format_version=${root.long("format_version")}，不相容，略過")
            return null
        }
        val names = LinkedHashMap<String, String>()
        val order = LinkedHashMap<String, Int>()
        root.arr("languages").forEach { n ->
            val tag = n.str("tag") ?: return@forEach
            names[tag] = n.str("name") ?: tag
            order[tag] = n.long("order")?.toInt() ?: LanguageTable.UNRANKED
        }
        val tags = LinkedHashMap<String, String>()
        (root["schemas"] as? Json.Obj)?.entries?.forEach { (k, v) ->
            val tag = v.string
            if (!tag.isNullOrEmpty()) tags[k] = tag
        }
        if (tags.isEmpty() && names.isEmpty()) return null
        return LanguageTable(tags, names, order)
    }
}
