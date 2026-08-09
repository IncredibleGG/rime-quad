package org.luminakey.ime.store

import android.util.Log
import org.luminakey.ime.R
import org.luminakey.ime.net.NetworkGate
import org.luminakey.ime.net.NetworkPurpose
import java.io.File

/**
 * 方案市集的執行引擎。**沒有任何 Android UI 相依**，所有方法都是同步的，
 * 由呼叫端放到背景執行緒上跑（見 [StoreController]）。
 *
 * 規範 §3 的流程被拆成兩個獨立動作，因為「已安裝」和「已啟用」是兩件事
 * （見 [InstalledRegistry] 的說明）：
 *
 *   install(...)   下載 → 驗 sha256 → 驗 zip 安全 → 解壓 → 記帳
 *   setEnabled(..) 改 schema_list → 部署 → 失敗回滾
 *
 * 只有第二步會碰 librime，也只有第二步需要回滾。
 */
class SchemaStore(
    val userDataDir: File,
    val sharedDataDir: File,
    private val workDir: File,
) {

    companion object {
        private const val TAG = "SchemaStore"

        /** 單一套件下載上限。索引宣告的 size 只是參考，這個是硬牆。 */
        const val MAX_PACKAGE_BYTES = 128L * 1024 * 1024

        /** SAF 匯入的單一 yaml 上限。 */
        const val MAX_SINGLE_YAML_BYTES = 8L * 1024 * 1024
    }

    val registry: InstalledRegistry = InstalledRegistry.load(userDataDir)

    /** librime 的檔案搜尋順序：user 在前，shared 在後。 */
    private val searchDirs: List<File> get() = listOf(userDataDir, sharedDataDir)

    /* ───────────────────────── 進度回報 ───────────────────────── */

    sealed class Progress {
        data class Downloading(
            val packageName: String,
            val ordinal: Int,
            val total: Int,
            val read: Long,
            val bytes: Long,
        ) : Progress()

        data class Verifying(val packageName: String) : Progress()
        data class Extracting(val packageName: String) : Progress()
        data object Preflight : Progress()
        data class Deploying(val elapsedMs: Long) : Progress()

        /** [reason] 會直接畫在進度覆蓋層上，所以它是 [UiMessage] 不是字串。 */
        data class RollingBack(val reason: UiMessage) : Progress()
    }

    /**
     * ⚠ [message] / [details] 是 [UiMessage]（資源 id + 參數），**不是字串**。
     *
     * 這一層沒有 `Context`，所以「順手寫一句中文」是阻力最小的路，而這個 app
     * 的預設語系是英文 —— 這些訊息會**原樣上畫面**（`StoreController.finish()`
     * → `ResultDialog`）。理由與規矩見 [UiMessage] 的檔頭。
     */
    sealed class Outcome {
        data class Ok(
            val message: UiMessage,
            val enabledSchemas: List<String> = emptyList(),
            /** 不擋啟用、但使用者該知道的事（例如反查詞典不在）。 */
            val details: List<UiMessage> = emptyList(),
        ) : Outcome()

        /**
         * [rolledBack] = 是否動過方案清單又還原了。
         * [recoverable] = false 代表回滾本身也失敗，使用者需要人工介入。
         */
        data class Failed(
            val message: UiMessage,
            val details: List<UiMessage> = emptyList(),
            val rolledBack: Boolean = false,
            val recoverable: Boolean = true,
        ) : Outcome()
    }

    /* ───────────────────────── 安裝 ───────────────────────── */

    /**
     * 下載並解壓 [plan] 裡的每一個套件。**不碰 schema_list、不部署。**
     *
     * 中途失敗時，已經成功落地的套件維持「已安裝但未啟用」，不做清除 ——
     * 使用者重試時就不必重新下載，而未啟用的檔案對 librime 完全無影響。
     */
    fun install(
        indexUrl: String,
        index: SchemaIndex,
        plan: DependencyResolver.Plan,
        progress: (Progress) -> Unit,
    ): Outcome {
        val installed = ArrayList<String>()
        workDir.mkdirs()

        plan.toDownload.forEachIndexed { i, pkg ->
            progress(Progress.Downloading(pkg.name, i + 1, plan.count, 0, pkg.size))
            val url = NetworkGate.resolveUrl(indexUrl, index.baseUrl, pkg.file)
            val tmp = File(workDir, "${pkg.id}.zip.part")

            // 走 NetworkGate 而不是自己開連線：全 app 只有那一個出口，
            // 開關關閉時它會直接拒絕（dl 會是 blocked = true 的 Err）。
            val dl = NetworkGate.download(
                url, tmp, MAX_PACKAGE_BYTES, NetworkPurpose.STORE_PACKAGE, pkg.name,
            ) { read, _ ->
                progress(Progress.Downloading(pkg.name, i + 1, plan.count, read, pkg.size))
            }
            if (dl is NetworkGate.Result.Err) {
                tmp.delete()
                return Outcome.Failed(
                    UiMessage.of(R.string.store_err_download, pkg.name, dl.message),
                    details = listOf(UiMessage.of(R.string.store_detail_url, url)) +
                        doneNote(installed),
                )
            }
            val got = (dl as NetworkGate.Result.Ok).value

            // ── sha256：不符即整包丟棄，不可先解壓再說（規範 §1）──────────
            progress(Progress.Verifying(pkg.name))
            if (!got.sha256.equals(pkg.sha256, ignoreCase = true)) {
                tmp.delete()
                return Outcome.Failed(
                    UiMessage.of(R.string.store_err_sha_mismatch, pkg.name),
                    details = listOf(
                        UiMessage.of(R.string.store_detail_sha_declared, pkg.sha256),
                        UiMessage.of(R.string.store_detail_sha_actual, got.sha256),
                        UiMessage.of(R.string.store_detail_url, url),
                    ) + doneNote(installed),
                )
            }

            // ── zip 安全檢查 + 解壓（規範 §4）────────────────────────────
            progress(Progress.Extracting(pkg.name))
            when (val ex = ArchiveGuard.extract(tmp, userDataDir, workDir)) {
                is ExtractResult.Rejected -> {
                    tmp.delete()
                    return Outcome.Failed(
                        UiMessage.of(R.string.store_err_archive_rejected, pkg.name),
                        details = ex.report.rejections.map { it.uiMessage() } + doneNote(installed),
                    )
                }

                is ExtractResult.Failed -> {
                    tmp.delete()
                    return Outcome.Failed(
                        UiMessage.of(R.string.store_err_extract_failed, pkg.name, ex.message),
                        details = doneNote(installed),
                    )
                }

                is ExtractResult.Ok -> {
                    tmp.delete()
                    registry.put(
                        InstalledPackage(
                            id = pkg.id,
                            name = pkg.name,
                            sha256 = pkg.sha256,
                            installedAt = System.currentTimeMillis(),
                            schemas = pkg.schemas,
                            files = ex.files,
                            requires = pkg.requires,
                            recommendedLayout = pkg.recommendedLayout,
                            layoutNote = pkg.layoutNote,
                            source = "store",
                        )
                    )
                    installed.add(pkg.id)
                    Log.i(TAG, "已安裝 ${pkg.id}：${ex.files.size} 個檔案 / ${ex.bytes} bytes")
                }
            }
        }

        return Outcome.Ok(UiMessage.of(R.string.store_ok_installed, installed.size))
    }

    /**
     * 清單一律用 `", "` 接，不用「、」：預設語系是英文，而
     * 「a、b」對英文讀者是一個看不懂的符號。中文讀者看到逗號沒有損失。
     */
    private fun joined(items: List<String>): String = items.joinToString(", ")

    private fun doneNote(installed: List<String>): List<UiMessage> =
        if (installed.isEmpty()) emptyList()
        else listOf(UiMessage.of(R.string.store_detail_kept, joined(installed)))

    /* ───────────────────────── 啟用 / 停用 ───────────────────────── */

    val enabledSchemas: List<String> get() = SchemaListPatch.read(userDataDir)

    fun isEnabled(schemaId: String): Boolean = schemaId in enabledSchemas

    /**
     * 改 schema_list 並重新部署。**這是唯一需要回滾的地方。**
     *
     * 順序刻意如下：
     *   1. 先做相依預檢（[SchemaPreflight]）—— 缺詞典時根本不動 schema_list，
     *      使用者拿到的是「缺少 xxx.dict.yaml」而不是「部署失敗」。
     *   2. 存下 default.custom.yaml 的**完整內容**當快照。
     *   3. 改 schema_list → 部署。
     *   4. 部署失敗 → 把快照原樣寫回 → 再部署一次把狀態拉回來。
     *
     * 第 4 步是規範 §3 的硬性要求：少了它，使用者會卡在「每次啟動都部署
     * 失敗」而且沒有自救途徑。
     */
    fun setEnabled(
        schemaIds: List<String>,
        enabled: Boolean,
        progress: (Progress) -> Unit,
    ): Outcome {
        if (schemaIds.isEmpty()) return Outcome.Ok(UiMessage(R.string.store_ok_nothing_to_change))

        val warnings = ArrayList<UiMessage>()

        if (enabled) {
            progress(Progress.Preflight)
            // 分兩堆。blocking 是「部署一定會失敗」，warnings 是
            // 「少了某個附屬功能，但打得出字」—— 理由與分界線見 SchemaPreflight。
            val blocking = ArrayList<UiMessage>()
            for (id in schemaIds) {
                val f = searchDirs.map { File(it, "$id.schema.yaml") }.firstOrNull { it.isFile }
                if (f == null) {
                    blocking.add(
                        UiMessage.of(R.string.store_err_schema_file_missing, "$id.schema.yaml")
                    )
                    continue
                }
                val report = SchemaPreflight.check(f, searchDirs)
                report.blocking.forEach { blocking.add(it.uiMessage()) }
                report.warnings.forEach { warnings.add(it.uiMessage()) }
            }
            if (blocking.isNotEmpty()) {
                return Outcome.Failed(
                    UiMessage(R.string.store_err_missing_deps),
                    details = blocking,
                    rolledBack = false,
                )
            }
        }

        val snapshot = SchemaListPatch.snapshot(userDataDir)
        val before = SchemaListPatch.read(userDataDir)
        val changed = if (enabled) {
            SchemaListPatch.enable(userDataDir, schemaIds)
        } else {
            SchemaListPatch.disable(userDataDir, schemaIds)
        }
        if (changed.isEmpty()) {
            return Outcome.Ok(
                UiMessage(
                    if (enabled) R.string.store_ok_already_enabled
                    else R.string.store_ok_already_disabled
                )
            )
        }
        Log.i(TAG, "schema_list: $before → ${SchemaListPatch.read(userDataDir)}")

        val outcome = DeployGate.deployAndWait { progress(Progress.Deploying(it)) }
        if (outcome is DeployGate.Outcome.Success) {
            return Outcome.Ok(
                UiMessage.of(
                    if (enabled) R.string.store_ok_enabled else R.string.store_ok_disabled,
                    outcome.elapsedMs,
                ),
                enabledSchemas = SchemaListPatch.read(userDataDir),
                details = warnings,
            )
        }

        // ── 回滾 ──────────────────────────────────────────────────────
        val why = describe(outcome)
        Log.e(TAG, "$outcome —— 回滾 schema_list")
        // 進度覆蓋層上的那一行也是使用者看得到的字，同樣走資源。
        progress(Progress.RollingBack(why))
        SchemaListPatch.restore(userDataDir, snapshot)

        val redeploy = DeployGate.deployAndWait { progress(Progress.Deploying(it)) }
        val restored = SchemaListPatch.read(userDataDir)
        val backOk = redeploy is DeployGate.Outcome.Success && restored == before

        return Outcome.Failed(
            message = why,
            details = buildList {
                add(UiMessage.of(R.string.store_detail_list_restored, joined(before)))
                if (!backOk) {
                    add(UiMessage(R.string.store_detail_rollback_failed))
                }
                if (enabled) {
                    add(UiMessage(R.string.store_detail_files_kept))
                }
            },
            rolledBack = true,
            recoverable = backOk,
        )
    }

    /**
     * 部署沒成功時，要對使用者說的那一句。
     *
     * [DeployGate.Outcome.NotStarted] 的兩種代號各自有自己的句子 —— 上一版
     * 這裡是 `outcome.reason`，也就是把引擎層寫死的那句中文原樣轉出去。
     */
    private fun describe(outcome: DeployGate.Outcome): UiMessage = when (outcome) {
        // 引擎沒給原因時走另一句，而不是把「沒有原因」這四個字當成參數塞進去
        // —— 那個「沒有原因」本身也得是資源，繞一圈只是把問題往裡藏一層。
        is DeployGate.Outcome.Failure ->
            if (outcome.lastError.isEmpty()) UiMessage(R.string.store_err_deploy_failed)
            else UiMessage.of(R.string.store_err_deploy_failed_reason, outcome.lastError)

        is DeployGate.Outcome.Timeout ->
            UiMessage.of(R.string.store_err_deploy_timeout_ms, outcome.elapsedMs)

        is DeployGate.Outcome.NotStarted -> UiMessage(
            when (outcome.reason) {
                DeployGate.NotStartedReason.NOT_INITIALIZED ->
                    R.string.deploy_not_started_engine

                DeployGate.NotStartedReason.REFUSED -> R.string.deploy_not_started_busy
            }
        )

        is DeployGate.Outcome.Success -> UiMessage(R.string.store_err_unknown)
    }

    /* ───────────────────────── 解除安裝 ───────────────────────── */

    /**
     * 真的刪檔案。呼叫前會：
     *   · 確認沒有其他**已安裝**的套件宣告 requires 它（否則那些會一起壞掉）
     *   · 先把它提供的方案從 schema_list 拿掉並部署
     */
    fun uninstall(packageId: String, progress: (Progress) -> Unit): Outcome {
        val pkg = registry.get(packageId)
            ?: return Outcome.Failed(UiMessage.of(R.string.store_err_not_installed, packageId))

        val dependents = registry.dependents(packageId)
        if (dependents.isNotEmpty()) {
            return Outcome.Failed(
                UiMessage.of(R.string.store_err_has_dependents, pkg.name),
                details = dependents.map {
                    UiMessage.of(R.string.store_detail_dependent, it.name, it.id)
                },
            )
        }

        val toDisable = pkg.schemaIds.filter { it in enabledSchemas }
        if (toDisable.isNotEmpty()) {
            when (val r = setEnabled(toDisable, enabled = false, progress = progress)) {
                is Outcome.Failed -> return r
                is Outcome.Ok -> Unit
            }
        }

        var deleted = 0
        val root = userDataDir.canonicalFile
        for (name in pkg.files) {
            val f = File(userDataDir, name).canonicalFile
            // 帳本也可能被竄改；刪檔前再確認一次目標在 user_data_dir 內。
            if (!ArchiveGuard.isInside(f, root)) {
                Log.w(TAG, "帳本中的路徑 $name 指向 $f，不在 user 目錄內，拒絕刪除")
                continue
            }
            if (f.isFile && f.delete()) deleted++
        }
        registry.remove(packageId)
        return Outcome.Ok(UiMessage.of(R.string.store_ok_uninstalled, pkg.name, deleted))
    }

    /* ───────────────────────── 使用者自帶檔案 ───────────────────────── */

    /**
     * 匯入使用者從 SAF 選來的檔案（已先落到 [file]）。
     * [displayName] 只影響顯示與副檔名判斷。
     *
     * 與市集導入的差別只有兩點（規範 §4）：沒有 sha256 可驗、沒有相依資訊。
     * 安全檢查完全相同 —— 事實上這條路徑才是 zip slip 真正的入口。
     */
    fun importLocal(file: File, displayName: String, progress: (Progress) -> Unit): Outcome {
        val lower = displayName.lowercase()
        val id = "local:" + displayName.substringBeforeLast('.').ifEmpty { "import" }

        val files: List<String> = when {
            lower.endsWith(".zip") -> {
                progress(Progress.Extracting(displayName))
                when (val ex = ArchiveGuard.extract(file, userDataDir, workDir)) {
                    is ExtractResult.Rejected -> return Outcome.Failed(
                        UiMessage.of(R.string.store_err_import_rejected, displayName),
                        details = ex.report.rejections.map { it.uiMessage() },
                    )

                    is ExtractResult.Failed -> return Outcome.Failed(
                        UiMessage.of(R.string.store_err_extract_failed, displayName, ex.message)
                    )

                    is ExtractResult.Ok -> ex.files
                }
            }

            lower.endsWith(".yaml") || lower.endsWith(".yml") -> {
                if (file.length() > MAX_SINGLE_YAML_BYTES) {
                    return Outcome.Failed(
                        UiMessage.of(
                            R.string.store_err_too_big,
                            displayName,
                            formatBytes(file.length()),
                            formatBytes(MAX_SINGLE_YAML_BYTES),
                        )
                    )
                }
                // 單檔一樣要過路徑檢查。
                //
                // 這裡刻意**拒絕**而不是「把目錄部分剝掉再用」：SAF 的 display
                // name 是外部給的字串，一個正常的方案檔不會叫做 `../x.yaml`。
                // 靜靜地把它改成 `x.yaml` 會讓使用者以為匯入了他挑的那個東西，
                // 而我們其實換了一個檔名 —— 這種「善意的修正」正是繞過檢查的
                // 常見縫隙。整包拒絕，並把原因說清楚。
                val safeName = displayName
                // `it` 是英文的故障載荷（見 ArchiveGuard.pathProblemOf），當參數帶出去。
                ArchiveGuard.pathProblemOf(safeName, ArchiveLimits())?.let {
                    return Outcome.Failed(
                        UiMessage.of(R.string.store_err_unsafe_name, displayName, it)
                    )
                }
                ArchiveGuard.extensionProblemOf(safeName, ArchiveLimits())?.let {
                    return Outcome.Failed(
                        UiMessage.of(R.string.store_err_bad_file, displayName, it)
                    )
                }
                val dst = File(userDataDir, safeName)
                if (!ArchiveGuard.isInside(dst.canonicalFile, userDataDir.canonicalFile)) {
                    return Outcome.Failed(
                        UiMessage.of(R.string.store_err_escapes_dir, displayName)
                    )
                }
                file.copyTo(dst, overwrite = true)
                listOf(safeName)
            }

            else -> return Outcome.Failed(
                UiMessage.of(R.string.store_err_bad_type, displayName)
            )
        }

        val schemaIds = files
            .filter { it.endsWith(".schema.yaml") }
            .map { it.substringAfterLast('/').removeSuffix(".schema.yaml") }

        registry.put(
            InstalledPackage(
                id = id,
                name = displayName,
                sha256 = runCatching { FileDigest.sha256(file) }.getOrDefault(""),
                installedAt = System.currentTimeMillis(),
                schemas = schemaIds.map { StoreSchemaRef(it, it) },
                files = files,
                requires = emptyList(),
                recommendedLayout = null,
                layoutNote = null,
                source = "local",
            )
        )

        if (schemaIds.isEmpty()) {
            return Outcome.Ok(
                UiMessage.of(R.string.store_ok_imported_no_schema, files.size)
            )
        }
        return Outcome.Ok(
            UiMessage.of(R.string.store_ok_imported, files.size, joined(schemaIds))
        )
    }
}
