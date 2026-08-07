package org.rimequad.ime.core

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import org.rimequad.ime.store.BuiltinMigration
import org.rimequad.ime.store.SchemaListPatch
import java.io.File
import java.util.concurrent.CopyOnWriteArrayList

/**
 * 資料目錄與全域初始化。
 *
 * 目錄配置（對應 `rs_setup`）：
 *   filesDir/rime/shared  ← shared_data_dir，由 assets/rime/shared 解出，唯讀語意
 *   filesDir/rime/user    ← user_data_dir，可寫；assets/rime/user 只在檔案
 *                            不存在時「種下」初始內容，絕不覆蓋使用者的修改
 *                            （新版新增的內建方案改由 BuiltinMigration
 *                             逐項補上，見 migrateBuiltinSchemas）
 *   filesDir/rime/log     ← log_dir
 *
 * iOS 上這幾個路徑必須落在 App Group 容器內，見 rime_shell.h 的註解；
 * Android 沒這個限制，但目錄命名刻意保持一致，方便四端對照。
 *
 * ── 為什麼要非同步 ──────────────────────────────────────────────────────
 * 隨附資料有 13MB / 54 個檔案，解壓在模擬器上要好幾秒；接著 librime 首次
 * 部署要編三本詞庫加 5.7MB 語言模型，實測要一到兩分鐘。這些如果壓在
 * InputMethodService.onCreate() 上，鍵盤根本來不及出現。
 *
 * 所以拆成兩段：
 *   1. 解壓 assets   → 背景執行緒（純檔案 IO，不碰 rime）
 *   2. rs_init       → 主執行緒（librime 的 start_maintenance 本身是非同步的，
 *                      rs_init 會立刻返回，部署結果之後才由回呼送達）
 *
 * 這樣所有 rime_shell 呼叫仍然全部落在同一條（主）執行緒上，
 * 完全符合 rime_shell.h 的執行緒約定。
 */
object RimeRuntime {

    private const val TAG = "RimeRuntime"
    private const val APP_NAME = "rime.android"

    /**
     * assets 內容有變時遞增，強制重新解壓 shared。
     * 1 → 只有佔位檔
     * 2 → 併入 core/data/shared（54 檔）與 core/data/user 初始內容
     * 3 → 加入自撰的 t9_pinyin（九宮格）方案檔
     *
     * ⚠ **加東西進 core/data/shared 就必須遞增這個數字。** 這不是慣例問題，
     * 是使用者拿不拿得到新方案的問題：舊裝置的 `.revision` 檔若還等於同一個
     * 數字，整個 shared 目錄就會被判定「已是最新」而完全不解壓，新方案檔
     * 永遠不會落地。
     *
     * 九宮格加進來時漏了這一步，實測後果是升級的使用者 shared 目錄裡沒有
     * `t9_pinyin.schema.yaml`，於是 [BuiltinMigration] 把它加進 schema_list
     * 之後 librime 部署直接失敗（回滾機制救了回來，但九宮格還是沒有）。
     */
    private const val ASSET_REVISION = 3

    private const val ASSET_SHARED = "rime/shared"
    private const val ASSET_USER = "rime/user"

    enum class Phase {
        /** 還沒開始。 */
        IDLE,

        /** 正在解壓 assets。 */
        EXTRACTING,

        /** rs_init 已成功，librime 正在背景部署（編譯 schema）。 */
        DEPLOYING,

        /** 可以建立 session、可以輸入。 */
        READY,

        /** 出事了，看 [initError]。 */
        FAILED,
    }

    @Volatile
    var phase: Phase = Phase.IDLE
        private set

    @Volatile
    var initError: String? = null
        private set

    /** 解壓耗時（毫秒），-1 代表這次啟動沒有解壓（已是最新版本）。 */
    @Volatile
    var extractMillis: Long = -1
        private set

    /** 從 rs_init 到收到第一個部署完成通知的耗時（毫秒），-1 代表尚未完成。 */
    @Volatile
    var deployMillis: Long = -1
        private set

    /**
     * 這次啟動的內建方案遷移結果，給診斷畫面看的一行字；null = 沒發生任何事。
     * 見 [BuiltinMigration] 的檔頭：升級的使用者拿不到新增內建方案的那個 bug。
     */
    @Volatile
    var migrationNote: String? = null
        private set

    /**
     * 遷移動過 `default.custom.yaml` 時，這裡存著動之前的**整份內容**。
     *
     * 為什麼要存：遷移是在 `rs_init` 之前做的，所以 `rs_init` 內建的那次
     * `start_maintenance(True)` 就是「加完之後的那一次部署」。萬一新方案
     * 讓部署失敗，使用者的鍵盤會整個不能用 —— 這時要能還原回遷移前的狀態，
     * 走的是與方案市集完全相同的回滾策略（存整份位元組、失敗原樣寫回、
     * 再部署一次），見 SchemaStore.setEnabled 的註解。
     *
     * 部署成功即清空（等於 commit）。
     */
    @Volatile
    private var migrationSnapshot: SchemaListPatch.Snapshot? = null

    lateinit var sharedDataDir: File
        private set
    lateinit var userDataDir: File
        private set
    lateinit var logDir: File
        private set

    /**
     * 主題／佈局搜尋路徑（docs/theme-format.md §2.3）用得到的目錄，
     * 在 [start] 之前為 null。刻意不用 lateinit 的 getter，因為 UI 這一側
     * 有可能在初始化完成之前就要畫鍵盤。
     */
    val userDirOrNull: File? get() = if (::userDataDir.isInitialized) userDataDir else null
    val sharedDirOrNull: File? get() = if (::sharedDataDir.isInitialized) sharedDataDir else null

    /**
     * 不依賴 [start] 是否已跑過的路徑推導。
     *
     * 使用者自訂鍵位（[org.rimequad.ime.keyboard.UserLayoutStore]）與設定畫面
     * 都可能在解壓／`rs_init` 完成之前就要讀寫 user_data_dir，而 [userDataDir]
     * 是 `lateinit`。這個函式讓「路徑」與「初始化狀態」脫鉤 —— 路徑本來就只
     * 由 `filesDir` 決定，跟 librime 有沒有起來無關。
     */
    fun userDataDirOf(context: Context): File =
        File(File(context.applicationContext.filesDir, "rime"), "user")

    private val mainHandler = Handler(Looper.getMainLooper())
    private val listeners = CopyOnWriteArrayList<(Phase) -> Unit>()
    private var started = false
    private var deployStartedAt = 0L

    val isReady: Boolean get() = phase == Phase.READY

    fun addListener(listener: (Phase) -> Unit) {
        listeners.add(listener)
        listener(phase)
    }

    fun removeListener(listener: (Phase) -> Unit) {
        listeners.remove(listener)
    }

    private fun setPhase(next: Phase) {
        phase = next
        Log.i(TAG, "phase → $next" + (initError?.let { " ($it)" } ?: ""))
        mainHandler.post { listeners.forEach { runCatching { it(next) } } }
    }

    /**
     * 啟動初始化。可重複呼叫，只有第一次會真的動作。
     * 必須在主執行緒呼叫。
     */
    @Synchronized
    fun start(context: Context) {
        if (started) return
        started = true

        val appContext = context.applicationContext
        val root = File(appContext.filesDir, "rime")
        sharedDataDir = File(root, "shared")
        // 與 [userDataDirOf] 共用同一份定義，免得兩處各推導一次而漂移。
        userDataDir = userDataDirOf(appContext)
        logDir = File(root, "log")

        if (!RimeCore.libraryLoaded) {
            initError = "librime_jni.so 載入失敗: ${RimeCore.libraryLoadError}"
            setPhase(Phase.FAILED)
            return
        }
        if (!RimeCore.abiCompatible()) {
            initError = "ABI 版本不符：so 回報 ${RimeCore.abiVersion()}，" +
                "上層要求 ${RimeCore.EXPECTED_ABI_VERSION}"
            setPhase(Phase.FAILED)
            return
        }

        // 部署完成通知：librime 的維護執行緒打上來，切回主執行緒再處理。
        RimeCore.addDeployListener { status ->
            mainHandler.post { onDeployStatus(status) }
        }

        setPhase(Phase.EXTRACTING)
        Thread({
            val started = SystemClock.elapsedRealtime()
            val result = runCatching { prepareDataDirs(appContext) }
            val elapsed = SystemClock.elapsedRealtime() - started

            mainHandler.post {
                result.onFailure {
                    Log.e(TAG, "準備資料目錄失敗", it)
                    initError = "解壓隨附資料失敗: ${it.message}"
                    setPhase(Phase.FAILED)
                    return@post
                }
                extractMillis = if (result.getOrDefault(false)) elapsed else -1
                Log.i(TAG, "資料目錄就緒，耗時 ${elapsed}ms（有無實際解壓: ${result.getOrNull()}）")
                initializeRimeOnMainThread()
            }
        }, "rime-asset-extract").start()
    }

    /** 一律在主執行緒執行，維持 rime_shell 的執行緒約定。 */
    private fun initializeRimeOnMainThread() {
        deployStartedAt = SystemClock.elapsedRealtime()
        val ok = RimeCore.init(
            userDataDir = userDataDir.absolutePath,
            sharedDataDir = sharedDataDir.absolutePath,
            logDir = logDir.absolutePath,
            appName = APP_NAME,
        )
        if (!ok) {
            initError = "rs_init 失敗: ${RimeCore.lastError()}"
            setPhase(Phase.FAILED)
            return
        }
        initError = null
        // rs_init 內部已經呼叫 start_maintenance(True)，首次啟動一定會進部署。
        // 真正可用要等 RS_DEPLOY_SUCCESS。
        setPhase(if (RimeCore.lastDeployStatus == RimeDeployStatus.SUCCESS) Phase.READY else Phase.DEPLOYING)
    }

    private fun onDeployStatus(status: RimeDeployStatus) {
        when (status) {
            RimeDeployStatus.RUNNING -> {
                if (phase == Phase.READY || phase == Phase.DEPLOYING) setPhase(Phase.DEPLOYING)
            }

            RimeDeployStatus.SUCCESS -> {
                if (deployMillis < 0 && deployStartedAt > 0L) {
                    deployMillis = SystemClock.elapsedRealtime() - deployStartedAt
                }
                // ⚠ 這裡刻意**不**限定「只有 DEPLOYING / READY 才能轉 READY」。
                //
                // 方案市集的回滾流程是：部署失敗 → phase 變 FAILED → 還原
                // schema_list → 再部署一次。若這裡守著「只有 DEPLOYING 才轉」，
                // 那第二次的成功就吞不掉 FAILED，使用者的鍵盤會永遠停在
                // 「初始化失敗」的紅字上 —— 回滾等於白做。
                //
                // 一次成功的部署就是「現在可以用了」的權威證據，不管前一刻
                // 是什麼狀態。唯一的例外是還在解壓 assets（那時 librime 的
                // 資料目錄還沒齊）。
                if (phase != Phase.EXTRACTING) {
                    // 遷移加進去的方案編得起來 → 這次遷移正式成立，快照可以丟了。
                    migrationSnapshot = null
                    initError = null
                    setPhase(Phase.READY)
                }
            }

            RimeDeployStatus.FAILURE -> {
                initError = "部署失敗: ${RimeCore.lastError()}"
                setPhase(Phase.FAILED)
                rollbackMigrationIfNeeded()
            }

            RimeDeployStatus.IDLE -> Unit
        }
    }

    /**
     * 遷移之後那次部署失敗了 → 把 `default.custom.yaml` 還原成遷移前的樣子，
     * 再部署一次把使用者拉回可用狀態。
     *
     * 執行緒：檔案 IO 與 `rs_deploy()` 都放到背景執行緒。這是合法的 ——
     * rime_shell.h 明訂 `rs_deploy()` 是**唯一**允許跨執行緒呼叫的函式。
     * 還原後那次部署成功時，上面 SUCCESS 分支會把 phase 從 FAILED 拉回
     * READY（那段註解已經說明為什麼不能守住「只有 DEPLOYING 才轉」）。
     *
     * ⚠ 帳本**不**跟著還原。帳本記的是「曾經引入過」，而這次確實引入過了，
     * 只是編不起來。若連帳本一起還原，下次啟動會再引入一次、再失敗一次，
     * 使用者會陷入每次開機都壞掉的迴圈。試一次、記下來、不再自動重試，
     * 之後要用就到市集手動啟用（那條路徑有完整的預檢與錯誤訊息）。
     */
    private fun rollbackMigrationIfNeeded() {
        val snapshot = migrationSnapshot ?: return
        migrationSnapshot = null
        val note = migrationNote
        Log.e(TAG, "遷移後的首次部署失敗，還原 default.custom.yaml（$note）")
        Thread({
            runCatching { SchemaListPatch.restore(userDataDir, snapshot) }
                .onFailure { Log.e(TAG, "還原 default.custom.yaml 失敗", it) }
            migrationNote = "新增的內建方案讓部署失敗，已還原設定（$note）"
            RimeCore.deploy()
        }, "rime-migration-rollback").start()
    }

    /** 供設定畫面顯示。 */
    fun describeDataDirs(): String = buildString {
        appendLine("shared: ${if (::sharedDataDir.isInitialized) sharedDataDir.absolutePath else "-"}")
        appendLine("user:   ${if (::userDataDir.isInitialized) userDataDir.absolutePath else "-"}")
        append("log:    ${if (::logDir.isInitialized) logDir.absolutePath else "-"}")
    }

    /**
     * 建目錄、解壓 shared、種下 user 初始檔。
     * 回傳 true 代表這次真的做了解壓。在背景執行緒上跑。
     */
    private fun prepareDataDirs(context: Context): Boolean {
        listOf(sharedDataDir, userDataDir, logDir).forEach {
            if (!it.exists() && !it.mkdirs()) {
                throw IllegalStateException("建立目錄失敗: $it")
            }
        }

        val stamp = File(sharedDataDir, ".revision")
        val upToDate = stamp.exists() && stamp.readText().trim() == ASSET_REVISION.toString()

        if (!upToDate) {
            copyAssetTree(context, ASSET_SHARED, sharedDataDir, overwrite = true)
            stamp.writeText(ASSET_REVISION.toString())
            Log.i(TAG, "shared 已解到 ${sharedDataDir.absolutePath}")
        }

        // user 目錄：只補不覆蓋。使用者改過 default.custom.yaml 之後，
        // 升級 app 不可以把他的設定洗掉。
        copyAssetTree(context, ASSET_USER, userDataDir, overwrite = false)

        // ⚠ 上面那行有一個必須補救的副作用：舊使用者的 default.custom.yaml
        // 「已存在」，於是新版新增的內建方案（例如九宮格 t9_pinyin）永遠不會
        // 出現在他的 schema_list 裡 —— 真機回報的就是這個。
        //
        // 補救的方式不是改成覆蓋（那會洗掉使用者的設定），而是**遷移**：
        // 只把「從未引入過」的內建方案補進去，並記帳。使用者刻意停用過的
        // 不會被塞回來。詳見 BuiltinMigration 的檔頭。
        migrateBuiltinSchemas(context)

        return !upToDate
    }

    /** 在背景執行緒上跑（[prepareDataDirs] 的一部分），一定早於 `rs_init`。 */
    private fun migrateBuiltinSchemas(context: Context) {
        val shippedText = runCatching {
            context.assets.open(BuiltinMigration.SHIPPED_ASSET).use {
                it.readBytes().toString(Charsets.UTF_8)
            }
        }.getOrElse {
            Log.w(TAG, "讀不到隨附的 ${BuiltinMigration.SHIPPED_ASSET}，略過內建方案遷移", it)
            return
        }

        // 先存快照再動手：動了才有東西可以還原。
        val snapshot = SchemaListPatch.snapshot(userDataDir)
        // 搜尋順序與 librime 一致：user 在前、shared 在後。預檢靠它判斷
        // 「隨附清單說有的方案，裝置上是不是真的有」。
        val searchDirs = listOf(userDataDir, sharedDataDir)
        val result = runCatching { BuiltinMigration.migrate(userDataDir, shippedText, searchDirs) }
            .getOrElse {
                // 遷移失敗不該讓輸入法開不起來 —— 使用者頂多是少一個方案。
                Log.e(TAG, "內建方案遷移失敗，維持原設定", it)
                return
            }

        result.skipped.forEach {
            // 打包端的疏漏。使用者這次拿不到這個方案，但鍵盤照樣能用，
            // 而且帳本沒記，修好之後下一次啟動就會補上。
            Log.w(TAG, "內建方案 ${it.id} 預檢沒過，這次跳過：${it.reasons.joinToString("；")}")
        }
        if (result.changed) {
            migrationSnapshot = snapshot
            migrationNote = "已補上本版新增的內建方案：${result.added.joinToString("、")}"
            Log.i(
                TAG,
                "內建方案遷移：隨附 ${result.shipped}，帳本 ${result.ledgerBefore} → " +
                    "${result.ledgerAfter}，補上 ${result.added}",
            )
        } else {
            Log.i(TAG, "內建方案遷移：無事可做（帳本 ${result.ledgerAfter}）")
        }
    }

    private fun copyAssetTree(
        context: Context,
        assetPath: String,
        target: File,
        overwrite: Boolean,
    ) {
        val children = context.assets.list(assetPath).orEmpty()
        if (children.isEmpty()) {
            // 沒有子項目 → 視為檔案
            if (!overwrite && target.exists()) return
            target.parentFile?.mkdirs()
            context.assets.open(assetPath).use { input ->
                target.outputStream().use { output -> input.copyTo(output) }
            }
            return
        }
        if (!target.exists() && !target.mkdirs()) {
            throw IllegalStateException("建立目錄失敗: $target")
        }
        for (child in children) {
            copyAssetTree(context, "$assetPath/$child", File(target, child), overwrite)
        }
    }
}
