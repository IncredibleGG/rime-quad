package org.luminakey.ime.store

import android.util.Log
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
        data class RollingBack(val reason: String) : Progress()
    }

    sealed class Outcome {
        data class Ok(
            val message: String,
            val enabledSchemas: List<String> = emptyList(),
            /** 不擋啟用、但使用者該知道的事（例如反查詞典不在）。 */
            val details: List<String> = emptyList(),
        ) : Outcome()

        /**
         * [rolledBack] = 是否動過 schema_list 又還原了。
         * [recoverable] = false 代表回滾本身也失敗，使用者需要人工介入。
         */
        data class Failed(
            val message: String,
            val details: List<String> = emptyList(),
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
                    "下載「${pkg.name}」失敗：${dl.message}",
                    details = listOf("URL: $url") + doneNote(installed),
                )
            }
            val got = (dl as NetworkGate.Result.Ok).value

            // ── sha256：不符即整包丟棄，不可先解壓再說（規範 §1）──────────
            progress(Progress.Verifying(pkg.name))
            if (!got.sha256.equals(pkg.sha256, ignoreCase = true)) {
                tmp.delete()
                return Outcome.Failed(
                    "「${pkg.name}」的 sha256 不符，整包已丟棄（未解壓）",
                    details = listOf(
                        "索引宣告: ${pkg.sha256}",
                        "實際下載: ${got.sha256}",
                        "URL: $url",
                    ) + doneNote(installed),
                )
            }

            // ── zip 安全檢查 + 解壓（規範 §4）────────────────────────────
            progress(Progress.Extracting(pkg.name))
            when (val ex = ArchiveGuard.extract(tmp, userDataDir, workDir)) {
                is ExtractResult.Rejected -> {
                    tmp.delete()
                    return Outcome.Failed(
                        "「${pkg.name}」沒有通過壓縮檔安全檢查，已整包拒絕",
                        details = ex.report.rejections.map { it.humanMessage() } + doneNote(installed),
                    )
                }

                is ExtractResult.Failed -> {
                    tmp.delete()
                    return Outcome.Failed(
                        "「${pkg.name}」解壓失敗：${ex.message}",
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

        return Outcome.Ok("已安裝 ${installed.size} 個套件")
    }

    private fun doneNote(installed: List<String>): List<String> =
        if (installed.isEmpty()) emptyList()
        else listOf("已經成功安裝的套件保留在裝置上（未啟用）：${installed.joinToString("、")}")

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
        if (schemaIds.isEmpty()) return Outcome.Ok("沒有要變更的方案")

        val warnings = ArrayList<String>()

        if (enabled) {
            progress(Progress.Preflight)
            // 分兩堆。blocking 是「librime 部署一定會失敗」，warnings 是
            // 「少了某個附屬功能，但打得出字」—— 理由與分界線見 SchemaPreflight。
            val blocking = ArrayList<String>()
            for (id in schemaIds) {
                val f = searchDirs.map { File(it, "$id.schema.yaml") }.firstOrNull { it.isFile }
                if (f == null) {
                    blocking.add("找不到方案檔 $id.schema.yaml —— 這個方案並未安裝。")
                    continue
                }
                val report = SchemaPreflight.check(f, searchDirs)
                report.blocking.forEach { blocking.add(it.humanMessage()) }
                report.warnings.forEach { warnings.add(it.humanMessage()) }
            }
            if (blocking.isNotEmpty()) {
                return Outcome.Failed(
                    "缺少相依檔案，已停止（沒有動到 schema_list）",
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
            return Outcome.Ok(if (enabled) "這些方案已經是啟用狀態" else "這些方案本來就沒有啟用")
        }
        Log.i(TAG, "schema_list: $before → ${SchemaListPatch.read(userDataDir)}")

        val outcome = DeployGate.deployAndWait { progress(Progress.Deploying(it)) }
        if (outcome is DeployGate.Outcome.Success) {
            return Outcome.Ok(
                if (enabled) "已啟用並部署完成（${outcome.elapsedMs} ms）"
                else "已停用並部署完成（${outcome.elapsedMs} ms）",
                enabledSchemas = SchemaListPatch.read(userDataDir),
                details = warnings,
            )
        }

        // ── 回滾 ──────────────────────────────────────────────────────
        val why = when (outcome) {
            is DeployGate.Outcome.Failure -> "部署失敗：${outcome.lastError.ifEmpty { "librime 未提供原因" }}"
            is DeployGate.Outcome.Timeout -> "部署逾時（${outcome.elapsedMs} ms）"
            is DeployGate.Outcome.NotStarted -> outcome.reason
            else -> "未知原因"
        }
        Log.e(TAG, "$why —— 回滾 schema_list")
        progress(Progress.RollingBack(why))
        SchemaListPatch.restore(userDataDir, snapshot)

        val redeploy = DeployGate.deployAndWait { progress(Progress.Deploying(it)) }
        val restored = SchemaListPatch.read(userDataDir)
        val backOk = redeploy is DeployGate.Outcome.Success && restored == before

        return Outcome.Failed(
            message = why,
            details = buildList {
                add("schema_list 已還原為導入前的狀態：${before.joinToString("、")}")
                if (!backOk) {
                    add("⚠ 還原後重新部署沒有成功，輸入法可能無法使用。請重新啟動 app。")
                }
                if (enabled) {
                    add("套件檔案仍留在裝置上（已安裝但未啟用），修好相依之後可直接啟用，不必重新下載。")
                }
            },
            rolledBack = true,
            recoverable = backOk,
        )
    }

    /* ───────────────────────── 解除安裝 ───────────────────────── */

    /**
     * 真的刪檔案。呼叫前會：
     *   · 確認沒有其他**已安裝**的套件宣告 requires 它（否則那些會一起壞掉）
     *   · 先把它提供的方案從 schema_list 拿掉並部署
     */
    fun uninstall(packageId: String, progress: (Progress) -> Unit): Outcome {
        val pkg = registry.get(packageId)
            ?: return Outcome.Failed("「$packageId」不在已安裝清單裡")

        val dependents = registry.dependents(packageId)
        if (dependents.isNotEmpty()) {
            return Outcome.Failed(
                "「${pkg.name}」被其他已安裝的套件依賴，不能移除",
                details = dependents.map { "${it.name}（${it.id}）需要它" },
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
        return Outcome.Ok("已移除「${pkg.name}」（刪除 $deleted 個檔案）")
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
                        "「$displayName」沒有通過壓縮檔安全檢查，已整包拒絕（沒有寫出任何檔案）",
                        details = ex.report.rejections.map { it.humanMessage() },
                    )

                    is ExtractResult.Failed ->
                        return Outcome.Failed("「$displayName」解壓失敗：${ex.message}")

                    is ExtractResult.Ok -> ex.files
                }
            }

            lower.endsWith(".yaml") || lower.endsWith(".yml") -> {
                if (file.length() > MAX_SINGLE_YAML_BYTES) {
                    return Outcome.Failed(
                        "「$displayName」有 ${formatBytes(file.length())}，超過單檔 " +
                            "${formatBytes(MAX_SINGLE_YAML_BYTES)} 的上限"
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
                ArchiveGuard.pathProblemOf(safeName, ArchiveLimits())?.let {
                    return Outcome.Failed("「$displayName」的檔名不安全：$it")
                }
                ArchiveGuard.extensionProblemOf(safeName, ArchiveLimits())?.let {
                    return Outcome.Failed("「$displayName」不是可接受的檔案：$it")
                }
                val dst = File(userDataDir, safeName)
                if (!ArchiveGuard.isInside(dst.canonicalFile, userDataDir.canonicalFile)) {
                    return Outcome.Failed("「$displayName」正規化後跳出了使用者資料目錄，已拒絕")
                }
                file.copyTo(dst, overwrite = true)
                listOf(safeName)
            }

            else -> return Outcome.Failed("只接受 .zip 與 .yaml，收到「$displayName」")
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
                "已匯入 ${files.size} 個檔案，但裡面沒有 *.schema.yaml，" +
                    "所以沒有新方案可以啟用（可能是純詞典或配置套件）。"
            )
        }
        return Outcome.Ok("已匯入 ${files.size} 個檔案，可啟用的方案：${schemaIds.joinToString("、")}")
    }
}
