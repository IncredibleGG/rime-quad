package org.luminakey.ime.store

import java.io.File

/**
 * 「什麼進備份、什麼**刻意**不進」。
 *
 * ── 為什麼排除清單要是一個純函式 ───────────────────────────────────────
 * 排除規則是這個功能裡**最容易出錯而且錯了沒人看得出來**的部分：
 * 多帶一個檔案，使用者的隱私就跟著跑到另一台機器上；少帶一個檔案，
 * 他換手機之後少了一截詞庫而且沒有任何錯誤訊息。兩種錯都是靜默的。
 *
 * 所以規則集中在 [omissionReason] 與 [userDbFileIncluded] 兩個純函式裡，
 * 由單元測試逐條釘住，而不是散在「走訪目錄」的迴圈中間。
 *
 * ── ⚠ `.userdb/` 裡面有 `.log`，那不是日誌 ─────────────────────────────
 * LevelDB 的**寫入前紀錄（WAL）叫 `000003.log`**。使用者最近打的字就住在
 * 那裡面 —— 已經寫進 memtable、還沒被壓實成 `.ldb` 的資料，全部只存在
 * 那個檔案裡。一條看起來很合理的「排除所有 *.log」會讓備份看起來
 * 一切正常、大小也差不多，而使用者還原之後發現最近學的詞全沒了。
 *
 * 本檔因此把兩件事**分開處理**：`.userdb/` 目錄內部走
 * [userDbFileIncluded]（白名單心態：只排除三個確定無用的），
 * 目錄外的檔案才走 [omissionReason]。兩者不共用任何規則。
 */
object BackupPlan {

    /** librime 使用者詞典目錄的字尾。 */
    const val USERDB_SUFFIX = ".userdb"

    /**
     * `.userdb/` 內部**不**收的檔案。
     *
     * · `LOCK`  —— LevelDB 的行程鎖，開檔時自己會建，帶著走沒有意義。
     * · `LOG` / `LOG.old` —— LevelDB 自己的除錯輸出（壓實紀錄），不含詞條。
     *
     * 其餘一律收，包含 `CURRENT`、`MANIFEST-*`、`*.log`（WAL）、`*.ldb` / `*.sst`。
     * **少收任何一個，LevelDB 就開不起來或少一截資料。**
     */
    private val USERDB_SKIP = setOf("LOCK", "LOG", "LOG.old")

    fun userDbFileIncluded(relName: String): Boolean {
        val base = relName.substringAfterLast('/')
        if (base.isEmpty()) return false
        return base !in USERDB_SKIP
    }

    /**
     * user_data_dir 底下（`*.userdb/` 之外）的某個相對路徑要不要排除。
     * 回傳 `BackupFormat.OMIT_*` 的理由碼，null = 收。
     *
     * 每一條的理由都寫在 `docs/backup-format.md` §4，改這裡就要改那裡。
     */
    fun omissionReason(rel: String): String? {
        val path = rel.replace('\\', '/')
        val base = path.substringAfterLast('/')

        // 連網紀錄。它其實不住在 user_data_dir（在 files/net/），走不到這裡；
        // 這一條是**防守用的**：哪天有人把它搬進來，備份不會默默把它帶走。
        //
        // 為什麼刻意不匯出：那份紀錄的用途是讓使用者**稽核我們**——
        // 「這台裝置什麼時候連過網、為了什麼」。跟著備份跑到另一台機器上，
        // 它就不再是那台機器的證據，卻長得一模一樣；使用者會拿一份與現況
        // 無關的紀錄來判斷現在的行為。而且它本身就是一條上網時間軸。
        if (path == "net" || path.startsWith("net/")) return BackupFormat.OMIT_NETWORK_LOG

        // installation_id：一組跨重裝穩定的 UUID。帶到新機器上等於把兩台
        // 裝置釘成同一個身分 —— 那正是我們拒絕 Google 自動備份的理由之一。
        // librime 在新機器上會自己生一個。
        if (path == "installation.yaml") return BackupFormat.OMIT_INSTALLATION_ID

        // user.yaml 記的是 var/schema_access_time（何時用過哪個方案）與
        // var/previously_selected_schema。前者是使用行為的時間軸，不是設定；
        // 後者一行資訊不值得為了它把前者一起搬走。使用者真正在意的「用哪個
        // 方案」由 default.custom.yaml 的 schema_list 帶著走。
        if (path == "user.yaml") return BackupFormat.OMIT_SCHEMA_ACCESS_TIME

        // 內建方案的引入帳本。它記的是「**這個安裝**曾經引入過哪些內建方案」，
        // 屬於安裝史不是使用者資料。搬過去反而會讓新機器上的 BuiltinMigration
        // 以為某個方案已經引入過而跳過它（見 BuiltinMigration 檔頭那個 bug）。
        if (base == BuiltinMigration.FILE_NAME) return BackupFormat.OMIT_BUILTIN_LEDGER

        // librime 自己的同步目錄，裡面按 installation_id 分子目錄，
        // 可能含別台裝置的 id。
        if (path == "sync" || path.startsWith("sync/")) return BackupFormat.OMIT_SYNC_DIR

        // 部署產物：table.bin / prism.bin / reverse.bin 與 build/。
        // 匯入之後一定要重新部署（方案清單變了），這些會被重新產生出來，
        // 帶著走只是讓備份大上十倍。
        if (path == "build" || path.startsWith("build/")) return BackupFormat.OMIT_DERIVED_BINARIES
        if (base.endsWith(".bin")) return BackupFormat.OMIT_DERIVED_BINARIES

        // 半成品：帳本寫到一半留下的 .tmp（見 InstalledRegistry.save）。
        if (base.endsWith(".tmp")) return BackupFormat.OMIT_DERIVED_BINARIES

        return null
    }

    /** 匯出時要在 manifest 的 `omitted` 裡宣告的理由碼。順序固定，方便比對。 */
    val DECLARED_OMISSIONS: List<String> = listOf(
        BackupFormat.OMIT_NETWORK_LOG,
        BackupFormat.OMIT_INSTALLATION_ID,
        BackupFormat.OMIT_SCHEMA_ACCESS_TIME,
        BackupFormat.OMIT_BUILTIN_LEDGER,
        BackupFormat.OMIT_SYNC_DIR,
        BackupFormat.OMIT_DERIVED_BINARIES,
        BackupFormat.OMIT_LOGS,
    )

    /** 容器裡的一個檔案與它的來源。 */
    data class Entry(val path: String, val source: File)

    /**
     * 把一個 `*.userdb/` 目錄（或它的快照）攤成容器內的項目。
     *
     * [dir] 可以是 user_data_dir 底下的原始目錄，也可以是
     * [UserDbSnapshot] 產生的穩定副本 —— 這個函式不在乎，
     * 所以「怎麼取得一致的快照」與「怎麼打包」互不干擾。
     */
    fun userDbEntries(dbName: String, dir: File): List<Entry> {
        if (!dir.isDirectory) return emptyList()
        val out = ArrayList<Entry>()
        dir.listFiles()?.sortedBy { it.name }?.forEach { f ->
            if (!f.isFile) return@forEach          // LevelDB 目錄是扁的，沒有子目錄
            if (!userDbFileIncluded(f.name)) return@forEach
            out += Entry("${BackupFormat.DIR_DICT}$dbName$USERDB_SUFFIX/${f.name}", f)
        }
        return out
    }

    /** user_data_dir 裡的 `*.userdb/` 目錄，依名稱排序（讓備份是可重現的）。 */
    fun userDbDirs(userDataDir: File): List<File> =
        userDataDir.listFiles()
            ?.filter { it.isDirectory && it.name.endsWith(USERDB_SUFFIX) }
            ?.sortedBy { it.name }
            ?: emptyList()

    fun userDbName(dir: File): String = dir.name.removeSuffix(USERDB_SUFFIX)

    /**
     * 使用者改過的設定檔：`*.custom.yaml`。
     *
     * 為什麼不是整個 user_data_dir 的 yaml：非 `.custom.` 的那些是 librime
     * 部署時從 shared 抄過去的產物（`customization:` 標記），新機器上會自己
     * 重新產生。搬過去只會與新版的隨附檔打架。
     */
    fun customConfigEntries(userDataDir: File): List<Entry> =
        userDataDir.listFiles()
            ?.filter { it.isFile && it.name.endsWith(".custom.yaml") }
            ?.sortedBy { it.name }
            ?.map { Entry("${BackupFormat.DIR_CONFIG}${it.name}", it) }
            ?: emptyList()

    /**
     * 市集／自帶套件安裝在 user_data_dir 裡的檔案。
     *
     * ⚠ **為什麼連檔案一起帶走，而不是只記一份清單。** 這個 App 的定位是
     * 「離線為預設」：使用者換手機的當下很可能沒有開連網開關，甚至根本
     * 連不到我們的索引。只記清單的備份在那個時刻等於一張無法兌現的收據，
     * 而他的詞庫沒有對應的方案就是一堆打不開的資料。
     *
     * 排除規則照樣套用（[omissionReason]）—— 帳本裡列的檔案不會有 .bin，
     * 但這裡不假設帳本一定乾淨。
     */
    fun installedSchemaEntries(userDataDir: File, registry: InstalledRegistry): List<Entry> {
        val out = LinkedHashMap<String, Entry>()
        registry.all.forEach { pkg ->
            pkg.files.sorted().forEach { rel ->
                if (omissionReason(rel) != null) return@forEach
                val f = File(userDataDir, rel)
                if (!f.isFile) return@forEach       // 被刪掉了就當作沒有，不要讓匯出失敗
                out.putIfAbsent(rel, Entry("${BackupFormat.DIR_SCHEMA}$rel", f))
            }
        }
        return out.values.toList()
    }
}
