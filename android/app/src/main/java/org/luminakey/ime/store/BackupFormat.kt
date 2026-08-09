package org.luminakey.ime.store

/**
 * 備份容器的**格式**。規範在 `docs/backup-format.md`，本檔是 Android 端的實作。
 *
 * ── 為什麼要有這個檔案 ─────────────────────────────────────────────────
 * `AndroidManifest.xml` 把 `allowBackup` 關掉了（那一段註解說明了為什麼：
 * 系統備份會繞過 `net/NetworkGate`，使用者照我們教的方法查連網紀錄會查到
 * 「乾淨」，但詞庫其實已經上傳到某家公司的雲端）。代價是**換手機就什麼
 * 都沒了**，而在這個檔案出現之前，完全沒有替代路徑。
 *
 * ── 這一層刻意不碰 Android ─────────────────────────────────────────────
 * 整個檔案只用 `kotlin` 與 `java.*`。序列化的正確性（尤其是「讀到壞掉的
 * manifest 不可以崩潰」）因此在 JVM 單元測試裡測得起來，不必進模擬器。
 * 與 [MiniJson]、[LayoutRemapJson] 同一個原則。
 *
 * ── 相容性規則寫在哪裡 ─────────────────────────────────────────────────
 * [verdictFor] 是**唯一**的版本判定入口。四端都必須照同一張表辦，
 * 所以規則寫成一個純函式而不是散在讀檔流程裡：規則長成什麼樣子，
 * 看得到、也測得到。
 */
object BackupFormat {

    /**
     * 容器內的清單檔。**寫出端只寫這個名字。**
     *
     * 讀取端要連 [LEGACY_MANIFEST_NAME] 一起認 —— 見 [MANIFEST_NAMES]。
     */
    const val MANIFEST_NAME = "luminakey-backup.json"

    /** 產品改名前的清單檔名。這裡留著的是舊名，讀取端必須認得。 */
    const val LEGACY_MANIFEST_NAME = "rimequad-backup.json" // 舊名

    /**
     * 找 manifest 的順序:新名字在前，舊名在後。`docs/backup-format.md` §1
     * 的改名相容條款是**規範性**的,四端都照這一張表辦。
     *
     * ⚠ 少了第二項的下場:使用者升級之後,**他自己匯出的那份備份**會被判成
     * [BackupProblem.NOT_A_BACKUP]。那句話會叫他去找一個壞掉的檔案,而檔案
     * 是好的、壞掉的是我們。這一類失敗沒有任何錯誤紀錄,畫面上只是一片乾淨,
     * 使用者會以為備份是空的。
     *
     * 哪天要拿掉第二項,那必須是一個**明確的決定**(確定沒有人手上還有舊備份),
     * 不是因為沒有人記得它存在。
     */
    val MANIFEST_NAMES: List<String> = listOf(MANIFEST_NAME, LEGACY_MANIFEST_NAME)

    /**
     * 用來一眼認出「這個 zip 是我們的備份」而不是別的東西。
     *
     * 為什麼不靠副檔名：SAF 交回來的 Uri 沒有可信的檔名（見
     * [StoreController.displayNameOf] 的註解，DocumentsUI 給的是
     * `msf:1000000072` 這種不透明 id）。認容器內容比認檔名可靠。
     */
    const val KIND = "luminakey-backup"

    /** 產品改名前的 `kind`。這裡留著的是舊名,理由同 [MANIFEST_NAMES]。 */
    const val LEGACY_KIND = "rimequad-backup" // 舊名

    /** 讀取端接受的 `kind`。**寫出端一律只寫 [KIND]。** */
    val ACCEPTED_KINDS: Set<String> = setOf(KIND, LEGACY_KIND)

    /**
     * 目前寫出的版本。
     *
     * **只有破壞相容性的變更才遞增。** 新增選用欄位、新增檔案類別、
     * 新增 `omitted` 的理由碼一律**不**遞增 —— 舊版讀到不認得的欄位會忽略，
     * 那不是不相容。判定標準見 `docs/backup-format.md` §6。
     */
    const val FORMAT_VERSION = 1

    /** 本版讀得動的最低版本。低於它的備份要明確拒絕，不可以「盡力而為」。 */
    const val MIN_READABLE_VERSION = 1

    /* ── 容器內的目錄。讀取端只接受這幾個前綴，其餘一律不落地 ── */

    const val DIR_DICT = "dict/"
    const val DIR_SCHEMA = "schema/"
    const val DIR_CONFIG = "config/"
    const val DIR_SETTINGS = "settings/"
    const val DIR_LAYOUT = "layout/"

    val ALLOWED_PREFIXES: List<String> =
        listOf(DIR_DICT, DIR_SCHEMA, DIR_CONFIG, DIR_SETTINGS, DIR_LAYOUT)

    const val PREFS_ENTRY = "settings/prefs.json"
    const val LAYOUT_ENTRY = "layout/luminakey-layouts.json"
    const val REGISTRY_ENTRY = "schema/luminakey-store.json"

    /* ── 改名前的兩個容器內路徑。留著的是舊名,只給讀取端用 ── */

    const val LEGACY_LAYOUT_ENTRY = "layout/rimequad-layouts.json" // 舊名
    const val LEGACY_REGISTRY_ENTRY = "schema/rimequad-store.json" // 舊名

    /**
     * 找自訂鍵位／安裝帳本的順序:新名字在前。
     *
     * ⚠ 這兩項漏掉的失敗**比 manifest 那一項更安靜**:整份備份會匯入成功、
     * 詞庫也回來了,只有「裝過哪些方案」與「調過的鍵位」變成空的,而且沒有
     * 任何一句話提到它們。
     */
    val LAYOUT_ENTRIES: List<String> = listOf(LAYOUT_ENTRY, LEGACY_LAYOUT_ENTRY)
    val REGISTRY_ENTRIES: List<String> = listOf(REGISTRY_ENTRY, LEGACY_REGISTRY_ENTRY)

    /**
     * 容器內用舊名寫出去的檔案,落地時要換成現在的名字。
     *
     * 只換名字不換內容:`schema/` 底下的東西是照相對路徑鋪回 user_data_dir 的,
     * 若原封不動地鋪回去,磁碟上會多出一份叫舊名的帳本,而
     * [InstalledRegistry] 之後寫的是新名字 —— 兩份帳本各說各話,
     * 使用者裝過的方案會憑「這次讀到哪一份」而時有時無。
     *
     * 右邊的值必須等於 [InstalledRegistry.FILE_NAME] 與
     * `UserLayoutStore.FILE_NAME`;`BackupLegacyNameTest` 釘住這件事
     * (這一層刻意不 import `keyboard/`,所以用測試而不是編譯期相依來釘)。
     */
    val LEGACY_LANDING_RENAMES: Map<String, String> = mapOf(
        "rimequad-store.json" to "luminakey-store.json", // 舊名 → 現在的名字
        "rimequad-layouts.json" to "luminakey-layouts.json", // 舊名 → 現在的名字
    )

    /**
     * 使用者詞典的載體格式。
     *
     * · [ENCODING_LEVELDB_DIR] —— 直接搬 librime 的 `*.userdb/` 目錄
     *   （LevelDB）。Android 目前只能產生這一種，理由見 [UserDbSnapshot] 檔頭。
     * · [ENCODING_USERDB_TEXT] —— librime 自己的 `*.userdb.txt` 純文字快照
     *   （`UserDictManager::Backup`）。這是**跨端的正式格式**：它可以被
     *   `UserDictManager::Restore` 合併，能跨 db 實作、跨版本，而且看得懂。
     *
     * 讀取端**必須兩種都認得**（見規範 §6.3）。寫出端可以只產生其中一種。
     */
    const val ENCODING_LEVELDB_DIR = "leveldb-dir"
    const val ENCODING_USERDB_TEXT = "rime-userdb-text"

    /* ── `omitted` 的理由碼。字面說明在各端的資源檔裡，這裡只有碼 ── */

    const val OMIT_NETWORK_LOG = "network-log"
    const val OMIT_INSTALLATION_ID = "installation-id"
    const val OMIT_SCHEMA_ACCESS_TIME = "schema-access-time"
    const val OMIT_DERIVED_BINARIES = "derived-binaries"
    const val OMIT_BUILTIN_LEDGER = "builtin-ledger"
    const val OMIT_SYNC_DIR = "sync-dir"
    const val OMIT_LOGS = "logs"

    /** 版本判定的三種結果。四端共用同一張表。 */
    enum class Verdict {
        /** 讀得動。 */
        OK,

        /** 備份比這支 App 新 —— 使用者要做的事是**升級 App**。 */
        TOO_NEW,

        /** 備份比這支 App 支援的下限舊 —— 沒有升級路徑，只能說清楚。 */
        TOO_OLD,
    }

    fun verdictFor(formatVersion: Int): Verdict = when {
        formatVersion > FORMAT_VERSION -> Verdict.TOO_NEW
        formatVersion < MIN_READABLE_VERSION -> Verdict.TOO_OLD
        else -> Verdict.OK
    }

    /**
     * **本端**讀得動的詞典載體。
     *
     * 規範 §3.1 說「讀取端必須兩種都認得」，而 Android 現在只做得到一種
     * （`rime-userdb-text` 要靠 librime 的 `UserDictManager::Restore` 合併，
     * 而 `rime_shell.h` 沒有那個進入點）。這個集合就是那份差距的**唯一**
     * 記載處：讀不動的會被指名報給使用者，不會安靜地少一本。
     * 等 `rs_sync_user_data()` 進了 ABI，這裡多加一個字串就好。
     */
    val READABLE_ENCODINGS: Set<String> = setOf(ENCODING_LEVELDB_DIR)

    /** 路徑是不是落在允許的前綴底下。**這是解壓前的第一道閘門。** */
    fun isAllowedEntry(path: String): Boolean =
        ALLOWED_PREFIXES.any { path.startsWith(it) && path.length > it.length }

    /**
     * 容器內的一個路徑,落地到 user_data_dir 之後的**相對路徑**。
     *
     * 做兩件事:去掉容器目錄前綴、把改名前的檔名換成現在的名字
     * ([LEGACY_LANDING_RENAMES])。目錄那一段原樣保留 ——
     * `schema/opencc/x.json` 要落在 `opencc/x.json`,不是 `x.json`。
     *
     * **純函式**,所以「舊名容器落在哪裡」測得起來,不必進模擬器。
     */
    fun landingPath(entryPath: String): String {
        val prefix = ALLOWED_PREFIXES.firstOrNull { entryPath.startsWith(it) }
            ?: return entryPath
        val rel = entryPath.removePrefix(prefix)
        val dir = rel.substringBeforeLast('/', "")
        val base = rel.substringAfterLast('/')
        val landed = LEGACY_LANDING_RENAMES[base] ?: base
        return if (dir.isEmpty()) landed else "$dir/$landed"
    }
}

/**
 * 匯入可能出的問題。
 *
 * ⚠ **刻意是碼 + 參數，不是一串文字。** 兩個理由：
 *   1. 使用者看得懂的訊息必須跟著系統語言走，而這一層是純邏輯層，
 *      沒有 `Context` 也不該有；字面由 `res/values*` 底下的 `strings_dict.xml`
 *      提供。（那個路徑刻意不寫成一個帶星號斜線的樣式 —— 那會把這段註解關掉。）
 *   2. 「匯入失敗」四個字對使用者毫無用處。每一種問題都要有自己的一句話，
 *      而且要說出**他下一步能做什麼**。用列舉才擋得住「隨手寫一個泛用訊息」。
 *
 * 這與 macOS 端在規範 §6.5 採用的診斷模型（code + args）是同一個作法。
 */
enum class BackupProblem {
    /** 根本不是備份檔：不是 zip、或 zip 裡沒有 manifest。 */
    NOT_A_BACKUP,

    /** manifest 在，但讀不懂（JSON 壞掉、kind 不對、缺必要欄位）。 */
    MANIFEST_BROKEN,

    /** 備份比這支 App 新。 */
    TOO_NEW,

    /** 備份比這支 App 支援的下限舊。 */
    TOO_OLD,

    /** manifest 說有這個檔案，容器裡卻沒有。 */
    MISSING_ENTRY,

    /** 檔案內容與 manifest 記的 sha256 對不上：傳輸途中壞了，或被動過。 */
    CONTENT_MISMATCH,

    /** 容器裡有會寫到目標目錄之外的路徑，或有符號連結。整包拒絕。 */
    UNSAFE_PATH,

    /** 備份是合法的，但裡面沒有任何可以還原的東西。 */
    EMPTY,

    /** 讀寫失敗（空間不足、SAF 的 Uri 失效…）。 */
    IO,
}

/** 一項問題。[args] 依 [problem] 而定，順序即字串資源的 `%1$s`、`%2$s`。 */
data class BackupIssue(val problem: BackupProblem, val args: List<String> = emptyList()) {
    /** 開發者用的英文回退，**不上畫面**。與 diagnosticsText() 同一個理由。 */
    override fun toString(): String =
        if (args.isEmpty()) problem.name else "${problem.name}(${args.joinToString(", ")})"
}

/** 容器內的一個檔案。[sha256] 是**解壓後**的內容摘要。 */
data class BackupFile(val path: String, val size: Long, val sha256: String)

/**
 * 一本使用者詞典。
 *
 * [flushed] 記的是「匯出前有沒有成功讓 librime 把待寫入的交易寫下來」。
 * 這個欄位存在的唯一理由，是讓「詞庫少了一截」這件事**留下痕跡** ——
 * 見 [UserDbSnapshot] 檔頭。false 不代表資料一定不完整，只代表我們沒能證明它完整。
 */
data class BackupUserDb(
    val name: String,
    val encoding: String,
    val root: String,
    val flushed: Boolean,
)

/**
 * 備份裡提到的一個方案。
 *
 * [packageId] 是市集套件 id；null = 隨 App 內建。⚠ **方案 id 不是全域唯一的**
 * （`double_pinyin` 同時存在於兩個套件而且字集相反，見 coordination §5），
 * 所以還原時要一併看套件 id，不能只認方案 id。
 */
data class BackupSchemaRef(
    val id: String,
    val name: String,
    val packageId: String?,
    val bundled: Boolean,
)

data class BackupProducer(
    val platform: String,
    val appVersion: String,
    val appVersionCode: Long,
    val rimeShellAbi: Int,
)

data class BackupManifest(
    val formatVersion: Int,
    val createdAt: Long,
    val producer: BackupProducer,
    val userDbs: List<BackupUserDb>,
    val schemas: List<BackupSchemaRef>,
    val enabledSchemas: List<String>,
    val files: List<BackupFile>,
    val omitted: List<String>,
) {
    /** 有沒有任何值得還原的東西。空備份要當成錯誤而不是「成功還原了 0 項」。 */
    val isEmpty: Boolean get() = files.isEmpty()

    /** 至少有一本詞典沒能證明自己是完整的。UI 要據此加一行說明。 */
    val hasUnflushedUserDb: Boolean get() = userDbs.any { !it.flushed }

    /**
     * 本端讀不動的那幾本詞典。
     *
     * ⚠ **這個函式存在的理由是「不要安靜地少一本」。** 規範 §3.1 定義了兩種
     * 載體，而 Android 目前只讀得動 `leveldb-dir`。若匯入流程只是「把 `dict/`
     * 底下的目錄搬過去」，一份 `rime-userdb-text` 的備份會**完全正常地匯入成功**，
     * 只是使用者的詞庫一本都沒回來 —— 又是一個沒有錯誤訊息的資料遺失。
     *
     * 所以：讀不動就要說出是哪一本、為什麼。
     */
    fun unreadableUserDbs(supported: Set<String>): List<BackupUserDb> =
        userDbs.filterNot { it.encoding in supported }
}

/**
 * manifest 的編解碼。**純函式。**
 *
 * 讀用 [MiniJson]（專案的零依賴讀取器），寫手工組字串 —— 與
 * [InstalledRegistry] 與 [LayoutRemapJson] 同一個作法。
 *
 * ⚠ 那兩個檔案各自有一份 private 的 `q()`。這裡不去改它們（跨支線就是衝突），
 * 而是自己帶一份；三份加起來三十行，比一次跨界改動便宜。
 */
object BackupManifestJson {

    fun encode(m: BackupManifest): String {
        val sb = StringBuilder()
        sb.append("{\n")
        sb.append("  \"kind\": ").append(q(BackupFormat.KIND)).append(",\n")
        sb.append("  \"format_version\": ").append(m.formatVersion).append(",\n")
        sb.append("  \"created_at\": ").append(m.createdAt).append(",\n")
        sb.append("  \"producer\": {")
        sb.append("\"platform\": ").append(q(m.producer.platform)).append(", ")
        sb.append("\"app_version\": ").append(q(m.producer.appVersion)).append(", ")
        sb.append("\"app_version_code\": ").append(m.producer.appVersionCode).append(", ")
        sb.append("\"rime_shell_abi\": ").append(m.producer.rimeShellAbi)
        sb.append("},\n")

        sb.append("  \"user_db\": [\n")
        m.userDbs.forEachIndexed { i, d ->
            sb.append("    {\"name\": ").append(q(d.name))
                .append(", \"encoding\": ").append(q(d.encoding))
                .append(", \"root\": ").append(q(d.root))
                .append(", \"flushed\": ").append(d.flushed).append("}")
            sb.append(if (i == m.userDbs.lastIndex) "\n" else ",\n")
        }
        sb.append("  ],\n")

        sb.append("  \"schemas\": [\n")
        m.schemas.forEachIndexed { i, s ->
            sb.append("    {\"id\": ").append(q(s.id))
                .append(", \"name\": ").append(q(s.name))
                .append(", \"package\": ").append(qn(s.packageId))
                .append(", \"bundled\": ").append(s.bundled).append("}")
            sb.append(if (i == m.schemas.lastIndex) "\n" else ",\n")
        }
        sb.append("  ],\n")

        sb.append("  \"enabled_schemas\": ")
            .append(m.enabledSchemas.joinToString(", ", "[", "]") { q(it) }).append(",\n")
        sb.append("  \"omitted\": ")
            .append(m.omitted.joinToString(", ", "[", "]") { q(it) }).append(",\n")

        sb.append("  \"files\": [\n")
        m.files.forEachIndexed { i, f ->
            sb.append("    {\"path\": ").append(q(f.path))
                .append(", \"size\": ").append(f.size)
                .append(", \"sha256\": ").append(q(f.sha256)).append("}")
            sb.append(if (i == m.files.lastIndex) "\n" else ",\n")
        }
        sb.append("  ]\n")
        sb.append("}\n")
        return sb.toString()
    }

    /**
     * 解析。**任何說不通的地方都回 [BackupProblem.MANIFEST_BROKEN]，不丟例外。**
     *
     * ⚠ 版本判定刻意**先於**其他欄位的檢查：一份 v2 的 manifest 對 v1 的解析器
     * 來說「缺欄位」是必然的，若先報「格式壞掉」，使用者會去找一個不存在的
     * 壞檔案，而他真正該做的事是升級 App。
     */
    fun decode(text: String): Result<BackupManifest> {
        val root = MiniJson.parseOrNull(text)
            ?: return Result.failure(BackupIssue(BackupProblem.MANIFEST_BROKEN, listOf("json")))

        // ⚠ 新舊兩個 kind 都認(見 BackupFormat.ACCEPTED_KINDS)。只認新的那一個
        //    等於把使用者改名前匯出的備份判成「別人的檔案」。
        val kind = root.str("kind")
        if (kind == null || kind !in BackupFormat.ACCEPTED_KINDS) {
            return Result.failure(
                BackupIssue(BackupProblem.MANIFEST_BROKEN, listOf(kind ?: "-"))
            )
        }
        val version = root.long("format_version")?.toInt()
            ?: return Result.failure(
                BackupIssue(BackupProblem.MANIFEST_BROKEN, listOf("format_version"))
            )
        when (BackupFormat.verdictFor(version)) {
            BackupFormat.Verdict.TOO_NEW -> return Result.failure(
                BackupIssue(
                    BackupProblem.TOO_NEW,
                    listOf(version.toString(), BackupFormat.FORMAT_VERSION.toString()),
                )
            )

            BackupFormat.Verdict.TOO_OLD -> return Result.failure(
                BackupIssue(
                    BackupProblem.TOO_OLD,
                    listOf(version.toString(), BackupFormat.MIN_READABLE_VERSION.toString()),
                )
            )

            BackupFormat.Verdict.OK -> Unit
        }

        val producer = root["producer"]
        val files = root.arr("files").mapNotNull { n ->
            val path = n.str("path") ?: return@mapNotNull null
            BackupFile(
                path = path,
                size = n.long("size") ?: -1L,
                sha256 = n.str("sha256")?.lowercase() ?: return@mapNotNull null,
            )
        }
        return Result.success(
            BackupManifest(
                formatVersion = version,
                createdAt = root.long("created_at") ?: 0L,
                producer = BackupProducer(
                    platform = producer?.str("platform") ?: "unknown",
                    appVersion = producer?.str("app_version") ?: "",
                    appVersionCode = producer?.long("app_version_code") ?: 0L,
                    rimeShellAbi = producer?.long("rime_shell_abi")?.toInt() ?: -1,
                ),
                userDbs = root.arr("user_db").mapNotNull { n ->
                    val name = n.str("name") ?: return@mapNotNull null
                    val rootPath = n.str("root") ?: return@mapNotNull null
                    BackupUserDb(
                        name = name,
                        encoding = n.str("encoding") ?: BackupFormat.ENCODING_LEVELDB_DIR,
                        root = rootPath,
                        // 缺席時當成 false：沒有證據就不是「有」。
                        flushed = n.bool("flushed") ?: false,
                    )
                },
                schemas = root.arr("schemas").mapNotNull { n ->
                    val id = n.str("id") ?: return@mapNotNull null
                    BackupSchemaRef(
                        id = id,
                        name = n.str("name") ?: id,
                        packageId = n.str("package"),
                        bundled = n.bool("bundled") ?: false,
                    )
                },
                enabledSchemas = root.strings("enabled_schemas"),
                files = files,
                omitted = root.strings("omitted"),
            )
        )
    }

    /** 成功／失敗兩態，失敗一定帶一個 [BackupIssue]。刻意不用 kotlin.Result。 */
    sealed class Result<out T> {
        data class Ok<T>(val value: T) : Result<T>()
        data class Err(val issue: BackupIssue) : Result<Nothing>()

        companion object {
            fun <T> success(v: T): Result<T> = Ok(v)
            fun failure(i: BackupIssue): Result<Nothing> = Err(i)
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

    private fun qn(s: String?): String = if (s == null) "null" else q(s)
}
