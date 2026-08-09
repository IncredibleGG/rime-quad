package org.luminakey.ime.keyboard

import org.luminakey.ime.store.Json
import org.luminakey.ime.store.MiniJson
import java.io.File

/**
 * 使用者自訂鍵位的儲存層。
 *
 * ── 存在哪裡、為什麼 ────────────────────────────────────────────────────
 * `<user_data_dir>/luminakey-layouts.json`（改名前的舊名 `rimequad-layouts.json`
 * 讀得到就會接手，見 [UserLayoutStore.LEGACY_FILE_NAME]），與
 * [org.luminakey.ime.store.InstalledRegistry] 的 `luminakey-store.json`、
 * [org.luminakey.ime.store.BuiltinMigration] 的 `builtin_introduced.json` 同區。
 *
 * 理由與那兩份帳本相同：它記的是「這台裝置上的使用者資料」，跟 user_data_dir
 * 同生共死。使用者清掉 app 資料時三者一起消失，不會出現「設定說換過但佈局
 * 是原樣」的鬼狀態。放 DataStore 反而會分家 —— DataStore 在 app 私有目錄的
 * 另一處，清 rime 資料不會清它。
 *
 * ⚠ **不寫回 `core/layouts/`。** 那些 yaml 隨 APK 散佈，`syncRimeData` 會在每次
 * 建置時從 repo 覆蓋一次，升級也會覆蓋。寫回去等於使用者的自訂在下一次更新
 * 時無聲消失。
 *
 * ── 存操作、不存快照 ────────────────────────────────────────────────────
 * 見 [LayoutRemap] 的檔頭。這也直接決定了「重置」的語義：重置 = 刪掉這份
 * 佈局的操作清單，於是使用者拿到的是**當前**基礎佈局的原樣，而不是他當初
 * 開始改之前那一版的快照。一個改不回去、或只能改回舊版的重置本身就是陷阱。
 *
 * ── 執行緒 ──────────────────────────────────────────────────────────────
 * IME 服務與設定畫面在同一個行程（見 [org.luminakey.ime.prefs.PrefsStore] 的
 * 檔頭），所以這裡是**單例 + synchronized**。[version] 是給 IME 用的變更旗標：
 * 設定畫面寫入時遞增，IME 在 `onStartInputView` 比對一次，不同就丟掉佈局快取。
 * 不用 listener 是刻意的 —— IME 可能在設定畫面寫入時根本沒有活著的 view，
 * 一個「下次要用的時候再問」的旗標比一條要小心生命週期的回呼簡單得多。
 */
class UserLayoutStore private constructor(
    private val file: File,
    /** 改名前的那一份。只讀不寫,見 [reload] 與 [save]。 */
    private val legacyFile: File,
) {

    private val entries = LinkedHashMap<String, LayoutRemap>()

    /** 每次寫入遞增。消費端比對這個數字決定要不要重載佈局。 */
    @Volatile
    var version: Int = 0
        private set

    @Synchronized
    private fun reload() {
        entries.clear()
        // 讀:先新名字,沒有才退回改名前的那一份。
        val src = if (file.isFile) file else legacyFile
        if (!src.isFile) return
        val text = runCatching { src.readText(Charsets.UTF_8) }.getOrNull() ?: return
        for (r in LayoutRemapJson.decode(text)) entries[r.layoutId] = r
    }

    /** 這份佈局有沒有自訂。沒有回 null（而不是空的 [LayoutRemap]）。 */
    @Synchronized
    fun remapFor(layoutId: String): LayoutRemap? = entries[layoutId]?.takeIf { !it.isEmpty }

    @Synchronized
    fun all(): List<LayoutRemap> = entries.values.filter { !it.isEmpty }

    @Synchronized
    fun hasAny(): Boolean = entries.values.any { !it.isEmpty }

    /** 寫入一份佈局的自訂。空的 [LayoutRemap] 等同 [reset]。 */
    @Synchronized
    fun put(remap: LayoutRemap) {
        if (remap.isEmpty) entries.remove(remap.layoutId) else entries[remap.layoutId] = remap
        save()
    }

    /** 單獨重置一份佈局 → 回到該佈局**當前**的原樣。 */
    @Synchronized
    fun reset(layoutId: String) {
        if (entries.remove(layoutId) == null) return
        save()
    }

    /** 全部重置。 */
    @Synchronized
    fun resetAll() {
        if (entries.isEmpty()) return
        entries.clear()
        save()
    }

    private fun save() {
        version++
        val text = LayoutRemapJson.encode(entries.values)
        file.parentFile?.mkdirs()
        // 先寫暫存再改名：寫到一半斷電會讓使用者的鍵盤永久停在半份自訂上。
        val tmp = File(file.parentFile, file.name + ".tmp")
        runCatching {
            tmp.writeText(text, Charsets.UTF_8)
            if (file.exists()) file.delete()
            if (!tmp.renameTo(file)) {
                tmp.copyTo(file, overwrite = true)
                tmp.delete()
            }
        }.onSuccess {
            // 寫:一律寫新名字,寫成功之後才收掉改名前的那一份。
            // 兩份同時在的話,之後每次讀都得決定信哪一份。
            if (legacyFile.isFile) legacyFile.delete()
        }
    }

    companion object {
        const val FILE_NAME = "luminakey-layouts.json"

        /**
         * 產品改名前的檔名。這裡留著的是舊名。
         *
         * ⚠ **讀取端必須認得**(`docs/coordination.md` §5 的相容條款)。
         * 漏掉的下場是升級之後使用者調過的鍵位全部回到原樣 ——
         * 檔案還在磁碟上,只是沒有人再去讀它,而且沒有任何錯誤訊息。
         */
        const val LEGACY_FILE_NAME = "rimequad-layouts.json" // 舊名

        private val instances = HashMap<String, UserLayoutStore>()

        @Synchronized
        fun get(userDataDir: File): UserLayoutStore {
            val f = File(userDataDir, FILE_NAME)
            return instances.getOrPut(f.absolutePath) {
                UserLayoutStore(f, File(userDataDir, LEGACY_FILE_NAME)).also { it.reload() }
            }
        }
    }
}

/**
 * [LayoutRemap] 的 JSON 編解碼。**純函式**，不碰檔案 —— 序列化的正確性
 * （尤其是「解不開的條目要丟掉而不是崩潰」）因此測得起來。
 *
 * 格式：
 * ```json
 * {
 *   "format_version": 1,
 *   "layouts": [
 *     { "id": "qwerty",
 *       "ops": [ { "op": "swap", "layer": "lower", "a": "a", "b": "s" } ] }
 *   ]
 * }
 * ```
 *
 * 讀取用 [MiniJson]（專案的零依賴讀取器），寫入手工組字串 —— 與
 * [org.luminakey.ime.store.InstalledRegistry] 同一個作法，理由見那裡。
 */
object LayoutRemapJson {

    const val FORMAT_VERSION = 1

    fun encode(all: Collection<LayoutRemap>): String {
        val kept = all.filter { !it.isEmpty }
        val sb = StringBuilder()
        sb.append("{\n")
        sb.append("  \"format_version\": ").append(FORMAT_VERSION).append(",\n")
        sb.append("  \"layouts\": [\n")
        kept.forEachIndexed { i, remap ->
            sb.append("    {\n")
            sb.append("      \"id\": ").append(q(remap.layoutId)).append(",\n")
            sb.append("      \"ops\": [\n")
            remap.ops.forEachIndexed { j, op ->
                sb.append("        ").append(encodeOp(op))
                sb.append(if (j == remap.ops.size - 1) "\n" else ",\n")
            }
            sb.append("      ]\n")
            sb.append("    }").append(if (i == kept.size - 1) "\n" else ",\n")
        }
        sb.append("  ]\n}\n")
        return sb.toString()
    }

    private fun encodeOp(op: RemapOp): String = when (op) {
        is RemapOp.Swap ->
            "{\"op\": \"swap\", \"layer\": ${q(op.layerId)}, " +
                "\"a\": ${q(op.first)}, \"b\": ${q(op.second)}}"

        is RemapOp.Move ->
            "{\"op\": \"move\", \"layer\": ${q(op.layerId)}, " +
                "\"key\": ${q(op.keyId)}, \"before\": ${q(op.beforeKeyId)}}"
    }

    /**
     * 解不開的條目一律**略過**，不丟例外。
     *
     * 一份壞掉的自訂檔不該讓輸入法打不開字：使用者頂多是少了幾個自訂，
     * 這比「鍵盤整個不出來」好得多。未知的 `op` 值同理 —— 那是未來版本寫的
     * 操作，舊版讀到時安靜跳過就好（前向相容）。
     */
    fun decode(text: String): List<LayoutRemap> {
        val root = MiniJson.parseOrNull(text) ?: return emptyList()
        val out = ArrayList<LayoutRemap>()
        for (node in root.arr("layouts")) {
            val id = node.str("id")?.takeIf { it.isNotEmpty() } ?: continue
            val ops = node.arr("ops").mapNotNull(::decodeOp)
            if (ops.isNotEmpty()) out += LayoutRemap(id, ops)
        }
        return out
    }

    private fun decodeOp(n: Json): RemapOp? {
        val layer = n.str("layer")?.takeIf { it.isNotEmpty() } ?: return null
        return when (n.str("op")) {
            "swap" -> {
                val a = n.str("a")?.takeIf { it.isNotEmpty() } ?: return null
                val b = n.str("b")?.takeIf { it.isNotEmpty() } ?: return null
                RemapOp.Swap(layer, a, b)
            }

            "move" -> {
                val key = n.str("key")?.takeIf { it.isNotEmpty() } ?: return null
                val before = n.str("before")?.takeIf { it.isNotEmpty() } ?: return null
                RemapOp.Move(layer, key, before)
            }

            else -> null
        }
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
}
