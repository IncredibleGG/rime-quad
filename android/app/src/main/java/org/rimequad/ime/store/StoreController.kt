package org.rimequad.ime.store

import android.content.Context
import android.content.SharedPreferences
import android.net.Uri
import android.provider.OpenableColumns
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.rimequad.ime.BuildConfig
import org.rimequad.ime.core.RimeCore
import org.rimequad.ime.core.RimeRuntime
import org.rimequad.ime.net.NetworkGate
import org.rimequad.ime.net.NetworkPurpose
import java.io.File
import java.util.concurrent.Executors

/** 索引來源設定。預設值來自 BuildConfig，可在設定畫面改成本機測試用的位址。 */
class StoreSettings(context: Context) {

    private val prefs: SharedPreferences =
        context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    var indexUrl: String
        get() = prefs.getString(KEY_INDEX_URL, null)?.takeIf { it.isNotBlank() }
            ?: BuildConfig.SCHEMA_INDEX_URL
        set(value) {
            prefs.edit().putString(KEY_INDEX_URL, value.trim()).apply()
        }

    fun resetIndexUrl() {
        prefs.edit().remove(KEY_INDEX_URL).apply()
    }

    val defaultIndexUrl: String get() = BuildConfig.SCHEMA_INDEX_URL

    /**
     * 「剛啟用了這個方案，鍵盤下次醒來請切過去」。
     *
     * 為什麼要繞一手：使用者是在**設定 Activity** 裡按下啟用的，而
     * `rs_select_schema()` 需要一個 session，session 必須由 IME service
     * 在它自己的執行緒上建（rime_shell.h 的執行緒約定）。兩邊在同一個
     * 行程但不同的生命週期，所以用一個持久化的交接點，讓 IME 在部署完成、
     * 重建 session 之後自己去取。
     *
     * 順帶解決一個實際問題：使用者剛裝好一個方案，預期就是「馬上能用它」，
     * 而不是自己去長按方案鍵翻選單。
     */
    var pendingSchema: String?
        get() = prefs.getString(KEY_PENDING_SCHEMA, null)
        set(value) {
            if (value == null) prefs.edit().remove(KEY_PENDING_SCHEMA).apply()
            else prefs.edit().putString(KEY_PENDING_SCHEMA, value).apply()
        }

    companion object {
        private const val PREFS = "rimequad-store"
        private const val KEY_INDEX_URL = "index_url"
        private const val KEY_PENDING_SCHEMA = "pending_schema"
    }
}

/**
 * 市集畫面的狀態持有者。
 *
 * ── 為什麼是 Thread + Handler 而不是 coroutine ─────────────────────────
 * 本模組 production 端零第三方依賴（見 gradle/libs.versions.toml）。
 * kotlinx-coroutines 目前只是 Compose 的傳遞相依，把它當成自己的 API 用
 * 會讓「Compose 換版本」變成「市集編不起來」。RimeRuntime 已經示範過
 * 同一個做法，這裡照抄它的形狀。
 *
 * ── 執行緒 ──────────────────────────────────────────────────────────────
 * 整條導入流程跑在單一背景執行緒上（[worker]），因為它會做網路 IO 與
 * 數秒到數十秒的部署。這是合法的：rime_shell.h 明訂 `rs_deploy()` 是
 * **唯一**允許跨執行緒呼叫的函式，而導入流程只呼叫它。
 * `rs_schema_list()` 之類的讀取一律在主執行緒（UI）做。
 */
class StoreController(context: Context) {

    private val appContext = context.applicationContext
    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor { r -> Thread(r, "rime-store") }

    val settings = StoreSettings(appContext)

    private val storeOrNull: SchemaStore?
        get() {
            val user = RimeRuntime.userDirOrNull ?: return null
            val shared = RimeRuntime.sharedDirOrNull ?: return null
            return cachedStore ?: SchemaStore(user, shared, File(appContext.cacheDir, "store"))
                .also { cachedStore = it }
        }

    private var cachedStore: SchemaStore? = null

    /* ───────────────────────── UI 狀態 ───────────────────────── */

    /** 列表項的三種狀態。UI 上是三種不同的操作，不能合成一個開關。 */
    enum class PackageState { NOT_INSTALLED, INSTALLED_DISABLED, ENABLED }

    data class ConfirmPlan(
        val rootPackage: StorePackage,
        val plan: DependencyResolver.Plan,
    )

    data class JobUi(
        val title: String,
        val detail: String,
        /** 0..1；負數代表不確定進度（部署沒有百分比可言）。 */
        val fraction: Float,
    )

    /**
     * **失敗**的結果。會停在一個要手動關掉的對話框上。
     *
     * ── 為什麼成功不走這裡 ─────────────────────────────────────────────
     * 使用者按下「重新部署」的目的是**完成部署**，不是讀一份報告。成功時
     * 再彈一個要他按「知道了」的對話框，等於在他已經達成目的之後多收一次
     * 過路費 —— 真機回報的原話是「部署完就要退出界面對不」。
     *
     * 失敗則相反：訊息裡有 `rs_last_error()`、有回滾結果、有「請重新啟動
     * app」這種需要他採取行動的指示。那種東西不能三秒後自己消失。
     *
     * 所以：成功 → [toast]（短暫、不擋路）；失敗 → 本對話框（不自動關）。
     */
    data class ResultUi(
        val ok: Boolean,
        val message: String,
        val details: List<String>,
    )

    var indexUrl by mutableStateOf(settings.indexUrl)
        private set

    var loading by mutableStateOf(false)
        private set

    var index by mutableStateOf<SchemaIndex?>(null)
        private set

    var indexError by mutableStateOf<String?>(null)
        private set

    var indexWarnings by mutableStateOf<List<String>>(emptyList())
        private set

    var confirm by mutableStateOf<ConfirmPlan?>(null)
        private set

    var job by mutableStateOf<JobUi?>(null)
        private set

    var result by mutableStateOf<ResultUi?>(null)
        private set

    /** 成功時的短暫提示（snackbar）。[TOAST_MS] 之後自己消失。 */
    var toast by mutableStateOf<String?>(null)
        private set

    /** 每次 showToast +1；延遲清除時比對，避免前一則的計時器把後一則關掉。 */
    private var toastToken = 0

    var installedIds by mutableStateOf<Set<String>>(emptySet())
        private set

    var installedPackages by mutableStateOf<List<InstalledPackage>>(emptyList())
        private set

    var enabledSchemas by mutableStateOf<List<String>>(emptyList())
        private set

    /** 每完成一次作業 +1。畫面用它當 `remember` 的鍵，強制重讀 schema 清單。 */
    var refreshTick by mutableStateOf(0)
        private set

    val busy: Boolean get() = job != null

    /* ───────────────────────── 動作 ───────────────────────── */

    fun refreshLocalState() {
        val s = storeOrNull ?: return
        installedIds = s.registry.ids
        installedPackages = s.registry.all
        enabledSchemas = s.enabledSchemas
    }

    fun applyIndexUrl(url: String) {
        settings.indexUrl = url
        indexUrl = settings.indexUrl
    }

    fun resetIndexUrl() {
        settings.resetIndexUrl()
        indexUrl = settings.indexUrl
    }

    fun dismissResult() { result = null }

    fun dismissConfirm() { confirm = null }

    fun dismissToast() { toast = null }

    /** 一律在主執行緒呼叫（[finish] 已經 post 回主執行緒了）。 */
    private fun showToast(text: String) {
        toast = text
        val token = ++toastToken
        main.postDelayed({ if (toastToken == token) toast = null }, TOAST_MS)
    }

    /**
     * 取索引。**開關關閉時直接什麼都不做。**
     *
     * [NetworkGate] 本來就會拒絕，這一層是為了「離線時連一個 loading 轉圈
     * 與一則錯誤訊息都不該出現」—— 使用者沒有做錯任何事，他只是選擇了離線，
     * 畫面上該看到的是說明卡而不是紅字。
     */
    fun loadIndex() {
        if (loading) return
        if (!NetworkGate.isEnabled) {
            index = null
            indexError = null
            return
        }
        loading = true
        indexError = null
        val url = indexUrl
        worker.execute {
            val fetched = NetworkGate.fetchText(url, NetworkPurpose.STORE_INDEX)
            main.post {
                loading = false
                when (fetched) {
                    is NetworkGate.Result.Err -> {
                        index = null
                        // 被開關擋下來的不是錯誤，畫面上有專門的說明卡。
                        indexError = if (fetched.blocked) null
                        else "取得索引失敗：${fetched.message}\n來源：$url"
                    }

                    is NetworkGate.Result.Ok -> when (val parsed = IndexParser.parse(fetched.value)) {
                        is IndexParseResult.Err -> {
                            index = null
                            indexError = parsed.message
                        }

                        is IndexParseResult.Ok -> {
                            index = parsed.index
                            indexWarnings = parsed.warnings
                            indexError = null
                        }
                    }
                }
                refreshLocalState()
            }
        }
    }

    /** 規範 §3：先展開相依、扣掉已安裝的，再讓使用者確認要下載多少。 */
    fun prepareInstall(pkg: StorePackage) {
        val idx = index ?: return
        val s = storeOrNull ?: run {
            result = ResultUi(false, "輸入法資料目錄尚未就緒，請稍候再試", emptyList())
            return
        }
        when (val r = DependencyResolver.resolve(idx, listOf(pkg.id), s.registry.ids)) {
            is DependencyResolver.Result.Ok -> {
                if (r.plan.count == 0) {
                    showToast("「${pkg.name}」與它的相依都已安裝，直接啟用即可")
                } else {
                    confirm = ConfirmPlan(pkg, r.plan)
                }
            }

            is DependencyResolver.Result.MissingDependency -> result = ResultUi(
                false, "「${pkg.name}」需要的相依套件不在索引裡",
                listOf("${r.requiredBy} 需要「${r.missing}」，但索引裡沒有這個套件"),
            )

            is DependencyResolver.Result.UnknownPackage -> result = ResultUi(
                false, "索引裡沒有套件「${r.id}」", emptyList(),
            )
        }
    }

    /** 確認之後真正開工：安裝，成功則接著啟用它提供的方案。 */
    fun confirmInstall(alsoEnable: Boolean) {
        val plan = confirm ?: return
        val idx = index ?: return
        val s = storeOrNull ?: return
        confirm = null
        result = null
        job = JobUi("準備中", "正在展開相依…", -1f)

        val url = indexUrl
        worker.execute {
            val installOutcome = s.install(url, idx, plan.plan) { p -> postProgress(p) }
            if (installOutcome is SchemaStore.Outcome.Failed) {
                finish(false, installOutcome.message, installOutcome.details)
                return@execute
            }

            if (!alsoEnable) {
                finish(true, "已安裝（尚未啟用）—— 到「已安裝」區塊按「啟用」才會部署", emptyList())
                return@execute
            }

            val schemaIds = plan.plan.toDownload.flatMap { it.schemaIds }
            if (schemaIds.isEmpty()) {
                finish(true, "已安裝（這些是基礎元件，沒有可切換的方案）", emptyList())
                return@execute
            }
            when (val en = s.setEnabled(schemaIds, enabled = true) { p -> postProgress(p) }) {
                // 成功只留一行 —— 這行會變成 snackbar，使用者不必按確認。
                // 佈局說明照樣塞得進去，因為它是他下一步真的會用到的資訊。
                is SchemaStore.Outcome.Ok -> finish(
                    true,
                    "「${plan.rootPackage.name}」已安裝並啟用，鍵盤會切到「${schemaIds.first()}」" +
                        (plan.rootPackage.layoutNote?.let { "（$it）" } ?: ""),
                    pendingSchema = schemaIds.first(),
                    details = emptyList(),
                )

                is SchemaStore.Outcome.Failed ->
                    finish(false, en.message, en.details)
            }
        }
    }

    fun setEnabled(schemaIds: List<String>, enabled: Boolean) {
        val s = storeOrNull ?: return
        if (busy) return
        result = null
        job = JobUi(if (enabled) "啟用方案" else "停用方案", "正在檢查相依…", -1f)
        worker.execute {
            when (val r = s.setEnabled(schemaIds, enabled) { p -> postProgress(p) }) {
                is SchemaStore.Outcome.Ok -> finish(
                    true, r.message, emptyList(),
                    pendingSchema = if (enabled) schemaIds.firstOrNull() else null,
                )

                is SchemaStore.Outcome.Failed -> finish(false, r.message, r.details)
            }
        }
    }

    /**
     * 純粹重新部署，不改任何設定。診斷畫面的「重新部署」按鈕用它。
     *
     * 為什麼不讓那顆按鈕直接呼叫 `RimeCore.deploy()`：`rs_deploy()` 是
     * **非同步**的，直接呼叫會立刻返回，畫面上什麼都不會發生，而背景
     * 其實跑了十幾秒（實測：模擬器 7.2 秒、三星 S24U 首次 12.5 秒）。
     * 使用者只能猜它成功了沒 —— 這正是真機回報的那個 bug。
     *
     * 走這條路徑就自動得到：進行中狀態、耗時顯示、成功／失敗的明確結果
     * （失敗時附 `rs_last_error()`），以及部署完成後由 RimeRuntime 的
     * phase 回呼觸發的 session 重建。
     */
    fun redeploy() {
        if (busy) return
        result = null
        job = JobUi("部署中（librime 正在編譯詞庫）", "已耗時 0 秒", -1f)
        worker.execute {
            when (val r = DeployGate.deployAndWait { ms ->
                postProgress(SchemaStore.Progress.Deploying(ms))
            }) {
                // 成功 → snackbar 一行，畫面自己退掉（見 ResultUi 的註解）。
                // 「舊 session 已失效，鍵盤會自動重建」是實作細節，使用者
                // 不需要知道，也不需要為了它按一次「知道了」。
                is DeployGate.Outcome.Success ->
                    finish(true, "部署完成（耗時 ${formatSeconds(r.elapsedMs)} 秒）", emptyList())

                is DeployGate.Outcome.Failure -> finish(
                    false, "部署失敗（${r.elapsedMs} ms）",
                    listOf("rs_last_error(): ${r.lastError.ifEmpty { "librime 未提供原因" }}"),
                )

                is DeployGate.Outcome.Timeout ->
                    finish(false, "部署逾時（${r.elapsedMs} ms）", emptyList())

                is DeployGate.Outcome.NotStarted ->
                    finish(false, "部署沒有啟動", listOf(r.reason))
            }
        }
    }

    fun uninstall(packageId: String) {
        val s = storeOrNull ?: return
        if (busy) return
        result = null
        job = JobUi("解除安裝", "正在移除檔案…", -1f)
        worker.execute {
            when (val r = s.uninstall(packageId) { p -> postProgress(p) }) {
                is SchemaStore.Outcome.Ok -> finish(true, r.message, emptyList())
                is SchemaStore.Outcome.Failed -> finish(false, r.message, r.details)
            }
        }
    }

    /**
     * 從 SAF 的 Uri 問出真正的檔名。
     *
     * ⚠ **不能用 `uri.lastPathSegment`。** DocumentsUI 交回來的是不透明的
     * document id，實測長這樣：`msf:1000000072`。拿它當檔名的後果是
     * 每一個檔案都因為「副檔名不是 .zip / .yaml」被拒絕 —— 使用者永遠
     * 匯入不了東西，而錯誤訊息還會顯示一串他看不懂的 id。
     * （這條是在模擬器上實測撞出來的，單元測試看不到，因為單元測試給的是
     * 真檔名。）
     */
    private fun displayNameOf(uri: Uri): String {
        runCatching {
            appContext.contentResolver.query(
                uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null
            )?.use { c ->
                val idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (idx >= 0 && c.moveToFirst()) {
                    c.getString(idx)?.takeIf { it.isNotBlank() }?.let { return it }
                }
            }
        }
        // 回落：有些 provider 給的是真的路徑。仍然只取最後一段當**候選**，
        // 安全檢查在 SchemaStore.importLocal 裡照跑。
        return uri.lastPathSegment?.substringAfterLast('/') ?: "import"
    }

    /** SAF 匯入。內容先複製到快取，再走與市集完全相同的安全檢查。 */
    fun importLocal(uri: Uri, nameHint: String? = null) {
        val s = storeOrNull ?: return
        if (busy) return
        val displayName = nameHint?.takeIf { it.isNotBlank() } ?: displayNameOf(uri)
        result = null
        job = JobUi("匯入檔案", displayName, -1f)
        worker.execute {
            val work = File(appContext.cacheDir, "store").apply { mkdirs() }
            val tmp = File(work, "import-${System.nanoTime()}")
            try {
                appContext.contentResolver.openInputStream(uri).use { input ->
                    if (input == null) {
                        finish(false, "無法讀取所選檔案", emptyList())
                        return@execute
                    }
                    tmp.outputStream().use { out ->
                        var total = 0L
                        val buf = ByteArray(64 * 1024)
                        while (true) {
                            val n = input.read(buf)
                            if (n < 0) break
                            total += n
                            if (total > SchemaStore.MAX_PACKAGE_BYTES) {
                                finish(false, "檔案超過 ${formatBytes(SchemaStore.MAX_PACKAGE_BYTES)}", emptyList())
                                return@execute
                            }
                            out.write(buf, 0, n)
                        }
                    }
                }

                val imported = s.importLocal(tmp, displayName) { p -> postProgress(p) }
                if (imported is SchemaStore.Outcome.Failed) {
                    finish(false, imported.message, imported.details)
                    return@execute
                }
                val schemaIds = s.registry.get(
                    "local:" + displayName.substringBeforeLast('.').ifEmpty { "import" }
                )?.schemaIds.orEmpty()
                if (schemaIds.isEmpty()) {
                    finish(true, (imported as SchemaStore.Outcome.Ok).message, emptyList())
                    return@execute
                }
                when (val en = s.setEnabled(schemaIds, enabled = true) { p -> postProgress(p) }) {
                    is SchemaStore.Outcome.Ok ->
                        finish(true, "已匯入並啟用：${schemaIds.joinToString("、")}", listOf(en.message))

                    is SchemaStore.Outcome.Failed -> finish(false, en.message, en.details)
                }
            } catch (e: Exception) {
                Log.e("StoreController", "匯入失敗", e)
                finish(false, "匯入失敗：${e.message}", emptyList())
            } finally {
                tmp.delete()
            }
        }
    }

    /* ───────────────────────── 狀態查詢 ───────────────────────── */

    fun stateOf(pkg: StorePackage): PackageState = when {
        pkg.id !in installedIds -> PackageState.NOT_INSTALLED
        pkg.schemaIds.isEmpty() -> PackageState.INSTALLED_DISABLED // 基礎元件沒有可啟用的方案
        pkg.schemaIds.any { it in enabledSchemas } -> PackageState.ENABLED
        else -> PackageState.INSTALLED_DISABLED
    }

    /* ───────────────────────── 內部 ───────────────────────── */

    private fun postProgress(p: SchemaStore.Progress) {
        val ui = when (p) {
            is SchemaStore.Progress.Downloading -> JobUi(
                "下載中（${p.ordinal}/${p.total}）",
                "${p.packageName}　${formatBytes(p.read)}" +
                    if (p.bytes > 0) " / ${formatBytes(p.bytes)}" else "",
                if (p.bytes > 0) (p.read.toFloat() / p.bytes).coerceIn(0f, 1f) else -1f,
            )

            is SchemaStore.Progress.Verifying -> JobUi("驗證 sha256", p.packageName, -1f)
            is SchemaStore.Progress.Extracting -> JobUi("安全檢查與解壓", p.packageName, -1f)
            SchemaStore.Progress.Preflight -> JobUi("檢查相依", "確認詞典與配置檔都在", -1f)
            is SchemaStore.Progress.Deploying -> JobUi(
                "部署中（librime 正在編譯詞庫）",
                "已耗時 ${p.elapsedMs / 1000} 秒。實測 3 個方案約 7 秒，大方案更久。",
                -1f,
            )

            is SchemaStore.Progress.RollingBack -> JobUi(
                "回滾中", "部署失敗，正在還原 schema_list：${p.reason}", -1f,
            )
        }
        main.post { job = ui }
    }

    private fun finish(
        ok: Boolean,
        message: String,
        details: List<String>,
        pendingSchema: String? = null,
    ) {
        main.post {
            if (ok && pendingSchema != null) settings.pendingSchema = pendingSchema
            job = null
            if (ok) {
                // 成功 → 覆蓋層與對話框一起退掉，只留 snackbar。
                result = null
                showToast(message)
            } else {
                // 失敗 → 停在對話框上，使用者要讀錯誤訊息並決定怎麼辦。
                toast = null
                result = ResultUi(false, message, details)
            }
            refreshLocalState()
            refreshTick++
            // 部署會讓舊 session 失效；schema 清單也變了。這裡只讀，不建 session
            // （建 session 是 IME service 的事，它會經 RimeRuntime 的 phase 回呼重建）。
            Log.i("StoreController", "完成：$message；目前 schema=${RimeCore.schemaList().size}")
        }
    }

    companion object {
        /**
         * snackbar 停留時間。取 Material 的 `SnackbarDuration.Long`（10 秒）與
         * `Short`（4 秒）之間：部署訊息帶著耗時數字，值得看清楚，但不該擋路。
         */
        const val TOAST_MS = 5_000L
    }
}

/** 毫秒 → 「7.2」。部署耗時用秒講，使用者的直覺單位是秒不是毫秒。 */
internal fun formatSeconds(ms: Long): String {
    val tenths = (ms + 50) / 100
    return "${tenths / 10}.${tenths % 10}"
}
