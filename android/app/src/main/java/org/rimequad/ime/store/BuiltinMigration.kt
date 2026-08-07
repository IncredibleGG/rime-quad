package org.rimequad.ime.store

import java.io.File

/**
 * 內建方案的「曾經引入過」帳本，以及升級時的遷移。
 *
 * ── 這個檔案解決的是什麼 bug ──────────────────────────────────────────
 * `RimeRuntime` 種下 user 資料時用的是 `overwrite = false`：使用者改過的
 * `default.custom.yaml` 絕不能被 app 升級洗掉。這條規則本身是對的，但它
 * 有一個副作用 —— **舊使用者永遠拿不到新增的內建方案**。
 *
 * 實際回報的情形：舊版隨附三個方案，新版加了 `t9_pinyin`（九宮格）。
 * APK 裡確實有 `t9_pinyin.schema.yaml`，隨附的 `default.custom.yaml` 也
 * 列了四個，但使用者裝置上那份三個方案的檔案「已存在」，於是被跳過，
 * 九宮格永遠不會出現在方案清單裡。
 *
 * ── 為什麼不是「把隨附清單無條件併回去」──────────────────────────────
 * 因為那會推翻使用者的決定。市集允許停用內建方案（停用是有意義的操作：
 * librime 每次部署會編譯 schema_list 上的**每一個**方案，少一個就少幾秒）。
 * 若每次啟動都把隨附清單併回去，使用者刻意停用的方案會在下次開機自己
 * 長回來，而且他找不到原因。
 *
 * ── 帳本 ────────────────────────────────────────────────────────────────
 * 所以要記的不是「現在啟用了什麼」（那是 schema_list 的事），而是
 * **「這個內建方案曾經被引入過沒有」**。只有從未引入過的才補進去，
 * 補完就記帳；使用者事後停用或移除，帳本仍記著「引入過」，不會被塞回來。
 *
 * 帳本與 [InstalledRegistry] 一樣放在 user_data_dir，理由相同：它描述的是
 * 那個目錄的狀態，使用者清掉 app 資料時兩者一起消失，不會出現「帳本說
 * 引入過但檔案沒了」的鬼狀態。
 *
 * 格式（`builtin_introduced.json`）：
 * ```json
 * {
 *   "format_version": 1,
 *   "introduced": ["luna_pinyin_tw", "bopomofo_tw", "luna_pinyin", "t9_pinyin"]
 * }
 * ```
 *
 * ── 一個誠實的限制 ─────────────────────────────────────────────────────
 * 帳本是這一版才引進的，所以**升級當下沒有歷史可查**。對那一次啟動而言，
 * 「使用者早就把 luna_pinyin 停用了」與「t9_pinyin 是全新的」在檔案上
 * 長得一模一樣：兩者都是「隨附清單裡有、schema_list 裡沒有」。因此
 * 第一次遷移會把兩者都補回去。這是引進帳本無法迴避的一次性成本；
 * 從第二次啟動開始，停用就會被完整尊重。
 */
object BuiltinMigration {

    const val FILE_NAME = "builtin_introduced.json"
    const val FORMAT_VERSION = 1

    /** APK 內隨附的那份 `default.custom.yaml`（判斷「本版內建哪些方案」的唯一真相）。 */
    const val SHIPPED_ASSET = "rime/user/default.custom.yaml"

    /** 該補、但檔案根本不在裝置上，於是這次跳過的方案。 */
    data class Skipped(val id: String, val reasons: List<String>)

    data class Result(
        /** 本版隨附的內建方案（依隨附檔的順序）。 */
        val shipped: List<String>,
        /** 這次真的補進 schema_list 的 id。空 = 沒動過檔案。 */
        val added: List<String>,
        /** 預檢沒過而跳過的。**不記帳**，下次啟動會再試一次。 */
        val skipped: List<Skipped>,
        /** 遷移前的帳本內容。 */
        val ledgerBefore: Set<String>,
        /** 遷移後的帳本內容。 */
        val ledgerAfter: Set<String>,
    ) {
        val changed: Boolean get() = added.isNotEmpty()
    }

    fun ledgerFile(userDataDir: File): File = File(userDataDir, FILE_NAME)

    /** 讀帳本。檔案不存在或壞掉一律當成「空的」—— 壞帳本不該讓 app 開不起來。 */
    fun readLedger(userDataDir: File): Set<String> {
        val f = ledgerFile(userDataDir)
        if (!f.isFile) return emptySet()
        val text = runCatching { f.readText(Charsets.UTF_8) }.getOrNull() ?: return emptySet()
        val root = MiniJson.parseOrNull(text) ?: return emptySet()
        return root.strings("introduced").filter { it.isNotBlank() }.toSet()
    }

    /** 寫帳本。先寫暫存再改名，寫到一半斷電不會留下半份壞檔。 */
    fun writeLedger(userDataDir: File, ids: Collection<String>) {
        val f = ledgerFile(userDataDir)
        val body = ids.filter { it.isNotBlank() }.distinct()
        val sb = StringBuilder()
        sb.append("{\n")
        sb.append("  \"format_version\": ").append(FORMAT_VERSION).append(",\n")
        sb.append("  \"introduced\": [")
        sb.append(body.joinToString(", ") { quote(it) })
        sb.append("]\n}\n")
        f.parentFile?.mkdirs()
        val tmp = File(f.parentFile, f.name + ".tmp")
        tmp.writeText(sb.toString(), Charsets.UTF_8)
        if (f.exists()) f.delete()
        if (!tmp.renameTo(f)) {
            tmp.copyTo(f, overwrite = true)
            tmp.delete()
        }
    }

    /**
     * 純函式：算出要補進 schema_list 的 id。
     *
     * 條件是三個「而且」：本版隨附、帳本沒記過、目前也不在 schema_list 裡。
     * 第三個條件只是省一次沒必要的寫入（[SchemaListPatch.enable] 本來就去重），
     * 但它讓「added 非空」精確等於「檔案真的被改過」，回滾判斷才乾淨。
     */
    fun plan(shipped: List<String>, ledger: Set<String>, enabled: List<String>): List<String> =
        shipped.filter { it.isNotBlank() && it !in ledger && it !in enabled }

    /**
     * 從隨附的 `default.custom.yaml` 原文解出本版內建方案清單。
     * 讀不到（例如 assets 缺檔）回空清單 —— 空清單會讓遷移變成 no-op，
     * 這是正確的保守行為：寧可少補一個方案，也不要憑空猜測。
     */
    fun shippedFrom(shippedText: String?): List<String> =
        if (shippedText.isNullOrBlank()) emptyList() else SchemaListPatch.readFrom(shippedText)

    /**
     * 補進去之前先確認檔案真的在裝置上。回傳空清單 = 可以補。
     *
     * ── 這一步是實測撞出來的，不是理論上的謹慎 ──────────────────────────
     * 第一次在模擬器上跑升級情境時，遷移正確地把 `t9_pinyin` 加進了
     * schema_list，然後部署**失敗**、觸發回滾 —— 因為 `RimeRuntime` 的
     * `ASSET_REVISION` 沒跟著 t9_pinyin 一起遞增，shared 目錄沒有被重新
     * 解壓，`t9_pinyin.schema.yaml` 根本不在裝置上。
     *
     * revision 那個 bug 當然要修（已修），但這裡的教訓更一般：
     * **「隨附清單說有」不等於「裝置上真的有」**。少了預檢，任何一次
     * 打包疏漏都會直接變成使用者開機時的部署失敗。有了預檢就只是
     * 「這個方案這次沒補上，下次再說」，鍵盤照樣能用。
     *
     * 用的是市集導入同一支 [SchemaPreflight]，理由見它的檔頭：
     * 檢查在**修改 schema_list 之前**跑，缺東西時根本不會去動檔案。
     */
    internal fun unavailableReasons(schemaId: String, searchDirs: List<File>): List<String> {
        if (searchDirs.isEmpty()) return emptyList()   // 無從檢查就別亂擋
        val file = searchDirs.map { File(it, "$schemaId.schema.yaml") }.firstOrNull { it.isFile }
            ?: return listOf("裝置上找不到 $schemaId.schema.yaml")
        return SchemaPreflight.check(file, searchDirs).missing.map { it.humanMessage() }
    }

    /**
     * 執行遷移。**不部署**，只改檔案 —— 部署與回滾由呼叫端負責
     * （[org.rimequad.ime.core.RimeRuntime] 讓 `rs_init` 的首次部署順便帶過，
     * 失敗時用呼叫端存下的快照還原）。
     *
     * [shippedText] 是 APK 內 [SHIPPED_ASSET] 的原文；[searchDirs] 依 librime
     * 的搜尋順序給（user 在前、shared 在後），供預檢用。
     */
    fun migrate(userDataDir: File, shippedText: String?, searchDirs: List<File>): Result {
        val shipped = shippedFrom(shippedText)
        val ledgerBefore = readLedger(userDataDir)
        if (shipped.isEmpty()) {
            return Result(shipped, emptyList(), emptyList(), ledgerBefore, ledgerBefore)
        }

        val enabled = SchemaListPatch.read(userDataDir)
        val skipped = ArrayList<Skipped>()
        val ready = ArrayList<String>()
        for (id in plan(shipped, ledgerBefore, enabled)) {
            val reasons = unavailableReasons(id, searchDirs)
            if (reasons.isEmpty()) ready.add(id) else skipped.add(Skipped(id, reasons))
        }

        // enable() 會保留使用者原有的順序與行末註解（見 SchemaListPatch）。
        val added = if (ready.isEmpty()) emptyList() else SchemaListPatch.enable(userDataDir, ready)

        // 帳本記的是「本版隨附的都算引入過」，不只這次補的那幾個：
        // 本來就在 schema_list 裡的當然也引入過，沒記下來的話使用者一旦
        // 停用它，下次啟動又會被當成「從未引入」塞回去。
        //
        // 唯一的例外是被預檢擋下的 —— 那些**沒有**被引入過，記了就等於
        // 永久放棄，一次打包疏漏會讓使用者從此拿不到那個方案。
        val skippedIds = skipped.mapTo(HashSet()) { it.id }
        val ledgerAfter = ledgerBefore + shipped.filter { it.isNotBlank() && it !in skippedIds }
        if (ledgerAfter != ledgerBefore) writeLedger(userDataDir, ledgerAfter)
        return Result(shipped, added, skipped, ledgerBefore, ledgerAfter)
    }

    private fun quote(s: String): String {
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
