package org.rimequad.ime.store

import android.content.Context
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.runBlocking
import org.rimequad.ime.BuildConfig
import org.rimequad.ime.R
import org.rimequad.ime.core.RimeCore
import org.rimequad.ime.core.RimeRuntime
import org.rimequad.ime.keyboard.LayoutRemapJson
import org.rimequad.ime.keyboard.UserLayoutStore
import org.rimequad.ime.prefs.PrefsStore
import org.rimequad.ime.prefs.UserPrefs
import java.io.File
import java.util.concurrent.Executors

/**
 * 匯出／匯入的流程控制。
 *
 * ── 為什麼是自己一個 controller，不併進 [StoreController] ────────────────
 * 兩者的失敗語義不一樣。市集的作業失敗了，使用者最多是少一個方案；
 * 匯入失敗**會動到他現有的詞庫**，所以每一步都要有回滾點，而且結果要停在
 * 畫面上讓他讀完。硬併進去只會讓那個已經六百行的檔案再長一倍，而且兩條
 * 流程的 job/result 狀態會互相蓋掉。
 *
 * ── 執行緒 ──────────────────────────────────────────────────────────────
 * 整條流程在單一背景執行緒（[worker]）上跑：SAF 讀寫、sha256、部署都慢。
 * 唯一的例外是 [UserDbSnapshot.flushEngine]，它自己會 post 回主執行緒 ——
 * 理由見那個檔案（librime 的 session 容器沒有鎖）。
 *
 * ── 字串 ────────────────────────────────────────────────────────────────
 * 背景執行緒上沒有 composition，所以走 `appContext.getString()`，
 * 與 [StoreController.str] 同一個作法：同一份資源、同一個語系，只是不需要
 * Compose 在場。**這一層一句寫死的中文都沒有。**
 */
class BackupController private constructor(context: Context) {

    private val appContext = context.applicationContext
    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor { r -> Thread(r, "rime-backup") }

    private fun str(id: Int, vararg args: Any): String =
        if (args.isEmpty()) appContext.getString(id) else appContext.getString(id, *args)

    /** 正在做的事；null = 閒置。 */
    var stage by mutableStateOf<String?>(null)
        private set

    /** 上一次的結果。要使用者自己關掉 —— 成功也是，因為裡面可能有他要讀的提醒。 */
    var result by mutableStateOf<Result?>(null)
        private set

    /** 等待確認的匯入。匯入會覆蓋現有詞庫，不能按一下就走。 */
    var pendingImport by mutableStateOf<Uri?>(null)
        private set

    val busy: Boolean get() = stage != null

    data class Result(val ok: Boolean, val message: String, val notes: List<String>)

    fun dismissResult() { result = null }

    fun askImport(uri: Uri) { if (!busy) pendingImport = uri }

    fun cancelImport() { pendingImport = null }

    /** 匯出時建議的檔名。SAF 只是「建議」，使用者可以改。 */
    fun suggestedFileName(): String {
        val day = java.text.SimpleDateFormat("yyyyMMdd", java.util.Locale.US)
            .format(java.util.Date())
        return str(R.string.backup_file_name, day)
    }

    /* ═══════════════════════════ 匯出 ═══════════════════════════ */

    fun export(target: Uri) {
        if (busy) return
        val userDir = RimeRuntime.userDirOrNull
        if (userDir == null) {
            result = Result(false, str(R.string.backup_err_no_data_dir), emptyList())
            return
        }
        result = null
        setStage(R.string.backup_stage_flush)

        worker.execute {
            val staging = File(File(appContext.cacheDir, "backup"), "snap-${System.nanoTime()}")
            try {
                staging.mkdirs()

                // 1. 讓 librime 把掛著的使用者詞典交易寫下去。做不到不是致命的，
                //    但一定要記進 manifest（見 UserDbSnapshot 檔頭）。
                val flush = UserDbSnapshot.flushEngine()

                // 2. 取一致的詞庫副本。
                setStage(R.string.backup_stage_copy)
                val dbEntries = ArrayList<BackupPlan.Entry>()
                val userDbs = ArrayList<BackupUserDb>()
                for (dir in BackupPlan.userDbDirs(userDir)) {
                    when (val c = UserDbSnapshot.copyStable(dir, staging)) {
                        is UserDbSnapshot.Copy.Ok -> {
                            val name = BackupPlan.userDbName(dir)
                            dbEntries += BackupPlan.userDbEntries(name, c.dir)
                            userDbs += BackupUserDb(
                                name = name,
                                encoding = BackupFormat.ENCODING_LEVELDB_DIR,
                                root = "${BackupFormat.DIR_DICT}$name${BackupPlan.USERDB_SUFFIX}",
                                flushed = flush.ok,
                            )
                        }

                        is UserDbSnapshot.Copy.Unstable ->
                            return@execute finish(
                                false, str(R.string.backup_err_unstable, c.name), emptyList()
                            )

                        is UserDbSnapshot.Copy.Failed ->
                            return@execute finish(
                                false, str(R.string.backup_err_io, "${c.name}: ${c.message}"),
                                emptyList(),
                            )
                    }
                }

                // 3. 其餘內容。
                val registry = InstalledRegistry.load(userDir)
                val entries = ArrayList<BackupPlan.Entry>()
                entries += dbEntries
                entries += BackupPlan.customConfigEntries(userDir)
                entries += BackupPlan.installedSchemaEntries(userDir, registry)
                File(userDir, InstalledRegistry.FILE_NAME).takeIf { it.isFile }?.let {
                    entries += BackupPlan.Entry(BackupFormat.REGISTRY_ENTRY, it)
                }
                File(userDir, UserLayoutStore.FILE_NAME).takeIf { it.isFile }?.let {
                    entries += BackupPlan.Entry(BackupFormat.LAYOUT_ENTRY, it)
                }

                // 偏好一定寫得出來（就算全空），所以「有沒有東西可以匯出」
                // 必須在加進去**之前**判斷 —— 否則這個檢查永遠是 false，
                // 而使用者會拿到一個只有一份空偏好的「備份」。
                if (entries.isEmpty()) {
                    return@execute finish(false, str(R.string.backup_err_nothing), emptyList())
                }
                val prefsFile = File(staging, "prefs.json").apply {
                    writeText(
                        BackupPrefsJson.encode(PrefsStore.get(appContext).current.toMap()),
                        Charsets.UTF_8,
                    )
                }
                entries += BackupPlan.Entry(BackupFormat.PREFS_ENTRY, prefsFile)

                // 4. 寫出去。
                setStage(R.string.backup_stage_pack)
                val enabled = SchemaListPatch.read(userDir)
                val schemaRefs = schemaRefsFor(registry, enabled)
                val out = appContext.contentResolver.openOutputStream(target)
                    ?: return@execute finish(
                        false, str(R.string.backup_err_io, target.toString()), emptyList()
                    )
                val manifest = out.use { stream ->
                    BackupArchive.pack(entries, stream) { files ->
                        BackupManifest(
                            formatVersion = BackupFormat.FORMAT_VERSION,
                            createdAt = System.currentTimeMillis() / 1000,
                            producer = BackupProducer(
                                platform = "android",
                                appVersion = BuildConfig.VERSION_NAME,
                                appVersionCode = BuildConfig.VERSION_CODE.toLong(),
                                rimeShellAbi = RimeCore.EXPECTED_ABI_VERSION,
                            ),
                            userDbs = userDbs,
                            schemas = schemaRefs,
                            enabledSchemas = enabled,
                            files = files,
                            omitted = BackupPlan.DECLARED_OMISSIONS,
                        )
                    }
                }

                val notes = ArrayList<String>()
                if (manifest.hasUnflushedUserDb) {
                    // 誠實勝過好看：我們沒能證明它完整，就要說。
                    notes += str(R.string.backup_note_unflushed)
                    Log.w(TAG, "匯出時無法 flush 使用者詞典: $flush")
                }
                finish(
                    true,
                    str(
                        R.string.backup_export_done,
                        manifest.userDbs.size,
                        manifest.files.size,
                    ),
                    notes,
                )
            } catch (e: Exception) {
                Log.e(TAG, "匯出失敗", e)
                finish(false, str(R.string.backup_err_io, e.message.orEmpty()), emptyList())
            } finally {
                staging.deleteRecursively()
            }
        }
    }

    private fun schemaRefsFor(
        registry: InstalledRegistry,
        enabled: List<String>,
    ): List<BackupSchemaRef> {
        val out = LinkedHashMap<String, BackupSchemaRef>()
        registry.all.forEach { pkg ->
            pkg.schemas.forEach { s ->
                out[s.id] = BackupSchemaRef(s.id, s.name, pkg.id, bundled = false)
            }
        }
        // schema_list 上但沒有任何套件宣告它 → 隨 App 內建的那幾個。
        enabled.forEach { id -> out.putIfAbsent(id, BackupSchemaRef(id, id, null, bundled = true)) }
        return out.values.toList()
    }

    /* ═══════════════════════════ 匯入 ═══════════════════════════ */

    fun confirmImport() {
        val uri = pendingImport ?: return
        pendingImport = null
        importNow(uri)
    }

    private fun importNow(source: Uri) {
        if (busy) return
        val userDir = RimeRuntime.userDirOrNull
        if (userDir == null) {
            result = Result(false, str(R.string.backup_err_no_data_dir), emptyList())
            return
        }
        result = null
        setStage(R.string.backup_stage_read)

        worker.execute {
            val work = File(appContext.cacheDir, "backup").apply { mkdirs() }
            val tmp = File(work, "in-${System.nanoTime()}.zip")
            val staging = File(work, "unpack-${System.nanoTime()}")
            try {
                // 1. 先落地。SAF 的 InputStream 不能重複讀，而我們要讀兩遍
                //    （manifest 一遍、內容一遍）。
                appContext.contentResolver.openInputStream(source).use { input ->
                    if (input == null) {
                        return@execute finish(
                            false, str(R.string.backup_err_io, source.toString()), emptyList()
                        )
                    }
                    tmp.outputStream().use { out ->
                        var total = 0L
                        val buf = ByteArray(64 * 1024)
                        while (true) {
                            val n = input.read(buf)
                            if (n < 0) break
                            total += n
                            if (total > BackupArchive.MAX_TOTAL_BYTES) {
                                return@execute finish(
                                    false, str(R.string.backup_err_too_big), emptyList()
                                )
                            }
                            out.write(buf, 0, n)
                        }
                    }
                }

                // 2. manifest（含版本判定）。
                val manifest = when (val r = BackupArchive.readManifest(tmp)) {
                    is BackupManifestJson.Result.Err -> return@execute finish(
                        false, describe(r.issue), emptyList()
                    )

                    is BackupManifestJson.Result.Ok -> r.value
                }

                // 3. 解壓並逐檔驗摘要。
                setStage(R.string.backup_stage_verify)
                val extracted = when (val e = BackupArchive.extract(tmp, manifest, staging)) {
                    is BackupArchive.Extract.Err -> return@execute finish(
                        false, describe(e.issue), emptyList()
                    )

                    is BackupArchive.Extract.Ok -> e
                }

                // 4. 套用。
                //
                // ⚠ 快照必須在**動手之前**取。取在 applyInto 之後的話，回滾拿到的
                //    是剛剛才寫進去的那一份 —— 等於沒有回滾，而且看起來完全正常。
                val configSnapshot = SchemaListPatch.snapshot(userDir)
                setStage(R.string.backup_stage_apply)
                val notes = applyInto(userDir, staging, manifest, extracted.files)

                // 5. 重新部署。方案清單變了，不部署等於沒還原。
                setStage(R.string.backup_stage_deploy)
                when (val d = DeployGate.deployAndWait()) {
                    is DeployGate.Outcome.Success -> finish(
                        true,
                        str(
                            R.string.backup_import_done,
                            manifest.userDbs.size,
                            manifest.files.size,
                        ),
                        notes,
                    )

                    is DeployGate.Outcome.Failure -> {
                        // 與方案市集完全相同的回滾策略：還原 schema_list，再部署一次。
                        SchemaListPatch.restore(userDir, configSnapshot)
                        RimeCore.deploy()
                        finish(
                            false,
                            str(
                                R.string.backup_err_deploy_failed,
                                d.lastError.ifEmpty { str(R.string.backup_err_deploy_no_reason) },
                            ),
                            notes + str(R.string.backup_note_rolled_back),
                        )
                    }

                    is DeployGate.Outcome.Timeout ->
                        finish(false, str(R.string.backup_err_deploy_timeout), notes)

                    is DeployGate.Outcome.NotStarted ->
                        finish(false, str(R.string.backup_err_deploy_not_started), notes)
                }
            } catch (e: Exception) {
                Log.e(TAG, "匯入失敗", e)
                finish(false, str(R.string.backup_err_io, e.message.orEmpty()), emptyList())
            } finally {
                tmp.delete()
                staging.deleteRecursively()
            }
        }
    }

    /**
     * 把驗過的內容搬進 user_data_dir，回傳要給使用者看的提醒。
     *
     * ⚠ **詞庫是整本換掉，不是合併。** 兩個 LevelDB 沒有辦法在不經過 librime
     * 的情況下合併（那需要 `UserDictManager::Restore` 的 merge 邏輯，而
     * `rime_shell.h` 沒有那個進入點）。「合併」若做成「把檔案疊上去」，
     * 結果是一個 MANIFEST 與 .ldb 對不起來的目錄，連打開都打不開。
     * 所以這裡誠實地覆蓋，而使用者按下匯入之前已經看過那句確認。
     */
    private fun applyInto(
        userDir: File,
        staging: File,
        manifest: BackupManifest,
        files: List<String>,
    ): List<String> {
        val notes = ArrayList<String>()

        // 4a. 詞庫：整個目錄換掉。
        val dictRoot = File(staging, BackupFormat.DIR_DICT.trimEnd('/'))
        dictRoot.listFiles()?.filter { it.isDirectory }?.forEach { src ->
            val target = File(userDir, src.name)
            target.deleteRecursively()
            if (!src.renameTo(target)) {
                target.mkdirs()
                src.listFiles()?.forEach { f -> f.copyTo(File(target, f.name), overwrite = true) }
            }
        }

        // 4b. 方案檔與帳本、4c. *.custom.yaml：直接落在 user_data_dir。
        for (prefix in listOf(BackupFormat.DIR_SCHEMA, BackupFormat.DIR_CONFIG)) {
            files.filter { it.startsWith(prefix) }.forEach { path ->
                val rel = path.removePrefix(prefix)
                val src = File(staging, path)
                val dst = File(userDir, rel)
                dst.parentFile?.mkdirs()
                if (dst.exists()) dst.delete()
                if (!src.renameTo(dst)) src.copyTo(dst, overwrite = true)
            }
        }

        // 4d. 偏好。**只換備份帶來的那些 key**，本機的連網開關等一律留著。
        File(staging, BackupFormat.PREFS_ENTRY).takeIf { it.isFile }?.let { f ->
            val imported = BackupPrefsJson.decode(f.readText(Charsets.UTF_8))
            runCatching {
                runBlocking {
                    PrefsStore.get(appContext).update { cur ->
                        val keep = cur.toMap().filterKeys { it in BackupPrefsJson.NOT_BACKED_UP }
                        UserPrefs.fromMap(imported + keep)
                    }
                }
            }.onFailure { Log.w(TAG, "套用偏好失敗，其餘照常", it) }
        }

        // 4e. 自訂鍵位。**走 UserLayoutStore 的公開 API 而不是直接寫檔** ——
        //     它是單例而且在記憶體裡有一份快取，直接寫檔會讓畫面與 IME
        //     繼續用舊的那一份，而且 version 不變，IME 永遠不會重載。
        File(staging, BackupFormat.LAYOUT_ENTRY).takeIf { it.isFile }?.let { f ->
            runCatching {
                val store = UserLayoutStore.get(userDir)
                store.resetAll()
                LayoutRemapJson.decode(f.readText(Charsets.UTF_8)).forEach { store.put(it) }
            }.onFailure { Log.w(TAG, "套用自訂鍵位失敗，其餘照常", it) }
        }

        // 4f. 已啟用的方案清單。**這台機器上找不到的方案要被剔除**，
        //     否則 librime 部署時會直接失敗，使用者連鍵盤都打不開。
        if (manifest.enabledSchemas.isNotEmpty()) {
            val searchDirs = listOfNotNull(userDir, RimeRuntime.sharedDirOrNull)
            val present = manifest.enabledSchemas.filter { id -> schemaExists(id, searchDirs) }
            val missing = manifest.enabledSchemas - present.toSet()
            if (present.isNotEmpty()) {
                SchemaListPatch.write(userDir, present)
            }
            if (missing.isNotEmpty()) {
                val names = missing.joinToString(str(R.string.backup_list_separator)) { id ->
                    manifest.schemas.firstOrNull { it.id == id }?.name ?: id
                }
                notes += str(R.string.backup_note_missing_schema, names)
            }
        }

        // 4g. 這個版本讀不動的載體（規範 §3.1 的 rime-userdb-text）。
        //     不指名報出來的話，使用者會拿到一個「匯入成功」但一本詞庫都
        //     沒回來的結果 —— 又是一次沒有錯誤訊息的資料遺失。
        val unreadable = manifest.unreadableUserDbs(BackupFormat.READABLE_ENCODINGS)
        if (unreadable.isNotEmpty()) {
            notes += str(
                R.string.backup_note_unreadable_dict,
                unreadable.joinToString(str(R.string.backup_list_separator)) { it.name },
            )
        }

        if (manifest.hasUnflushedUserDb) notes += str(R.string.backup_note_imported_unflushed)
        return notes
    }

    private fun schemaExists(id: String, dirs: List<File>): Boolean =
        dirs.any { File(it, "$id.schema.yaml").isFile }

    /* ═══════════════════════════ 共用 ═══════════════════════════ */

    /**
     * 把一個 [BackupIssue] 翻成使用者看得懂的一句話。
     *
     * ⚠ **每一種問題都要有自己的說法。** 「匯入失敗」對使用者毫無用處 ——
     * 他不知道該重試、該換檔案、還是該升級 App。`when` 是窮舉的，新增一種
     * 問題而忘了寫訊息會編不過去，這是刻意的。
     */
    private fun describe(issue: BackupIssue): String {
        val a0 = issue.args.getOrElse(0) { "" }
        return when (issue.problem) {
            BackupProblem.NOT_A_BACKUP -> str(R.string.backup_err_not_a_backup)
            BackupProblem.MANIFEST_BROKEN -> str(R.string.backup_err_manifest_broken)
            BackupProblem.TOO_NEW -> str(R.string.backup_err_too_new)
            BackupProblem.TOO_OLD -> str(R.string.backup_err_too_old)
            BackupProblem.MISSING_ENTRY -> str(R.string.backup_err_missing_entry, a0)
            BackupProblem.CONTENT_MISMATCH -> str(R.string.backup_err_content_mismatch)
            BackupProblem.UNSAFE_PATH -> str(R.string.backup_err_unsafe_path)
            BackupProblem.EMPTY -> str(R.string.backup_err_empty)
            BackupProblem.IO -> str(R.string.backup_err_io, a0)
        }
    }

    private fun setStage(id: Int) {
        val text = str(id)
        main.post { stage = text }
    }

    private fun finish(ok: Boolean, message: String, notes: List<String>) {
        main.post {
            stage = null
            result = Result(ok, message, notes)
            Log.i(TAG, "備份作業結束 ok=$ok：$message")
        }
    }

    companion object {
        private const val TAG = "BackupController"

        @Volatile
        private var instance: BackupController? = null

        /**
         * 行程單例。與 [org.rimequad.ime.update.UpdateController] 同一個理由：
         * 作業要跨得過畫面重組與返回鍵，狀態不能跟著 composition 一起消失。
         */
        fun get(context: Context): BackupController = instance ?: synchronized(this) {
            instance ?: BackupController(context).also { instance = it }
        }
    }
}
