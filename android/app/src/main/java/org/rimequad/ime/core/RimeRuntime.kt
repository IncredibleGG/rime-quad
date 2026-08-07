package org.rimequad.ime.core

import android.content.Context
import android.util.Log
import java.io.File

/**
 * 資料目錄與全域初始化。
 *
 * 目錄配置（對應 `rs_setup`）：
 *   filesDir/rime/shared  ← shared_data_dir，從 assets/rime 解出來，唯讀語意
 *   filesDir/rime/user    ← user_data_dir，可寫，放使用者詞典與個人配置
 *   filesDir/rime/log     ← log_dir
 *
 * 目前 assets/rime 只有佔位檔。真正的隨附 schema 會由主題／佈局那條線
 * （core/themes、core/layouts）產出後再塞進 assets。
 *
 * iOS 上這幾個路徑必須落在 App Group 容器內，見 rime_shell.h 的註解；
 * Android 沒這個限制，但目錄命名刻意保持一致，方便四端對照。
 */
object RimeRuntime {

    private const val TAG = "RimeRuntime"
    private const val APP_NAME = "rime.android"

    /** assets 內容有變時遞增，強制重新解壓。 */
    private const val ASSET_REVISION = 1
    private const val ASSET_ROOT = "rime"

    @Volatile
    private var initialized = false

    lateinit var sharedDataDir: File
        private set
    lateinit var userDataDir: File
        private set
    lateinit var logDir: File
        private set

    @Volatile
    var initError: String? = null
        private set

    @Synchronized
    fun ensureInitialized(context: Context): Boolean {
        if (initialized) return true

        val root = File(context.filesDir, "rime")
        sharedDataDir = File(root, "shared")
        userDataDir = File(root, "user")
        logDir = File(root, "log")

        listOf(sharedDataDir, userDataDir, logDir, File(userDataDir, "build")).forEach {
            if (!it.exists() && !it.mkdirs()) {
                Log.w(TAG, "建立目錄失敗: $it")
            }
        }

        runCatching { extractAssets(context) }
            .onFailure { Log.e(TAG, "解壓 assets 失敗", it) }

        if (!RimeCore.libraryLoaded) {
            initError = "librime_jni.so 載入失敗: ${RimeCore.libraryLoadError}"
            Log.e(TAG, initError!!)
            return false
        }
        if (!RimeCore.abiCompatible()) {
            initError = "ABI 版本不符：so 回報 ${RimeCore.abiVersion()}，" +
                "上層要求 ${RimeCore.EXPECTED_ABI_VERSION}"
            Log.e(TAG, initError!!)
            return false
        }

        val ok = RimeCore.init(
            userDataDir = userDataDir.absolutePath,
            sharedDataDir = sharedDataDir.absolutePath,
            logDir = logDir.absolutePath,
            appName = APP_NAME,
        )
        if (!ok) {
            initError = "rs_init 失敗: ${RimeCore.lastError()}"
            Log.e(TAG, initError!!)
            return false
        }

        initError = null
        initialized = true
        return true
    }

    val isInitialized: Boolean get() = initialized

    /** 供設定畫面顯示。 */
    fun describeDataDirs(): String = buildString {
        appendLine("shared: ${if (::sharedDataDir.isInitialized) sharedDataDir.absolutePath else "-"}")
        appendLine("user:   ${if (::userDataDir.isInitialized) userDataDir.absolutePath else "-"}")
        append("log:    ${if (::logDir.isInitialized) logDir.absolutePath else "-"}")
    }

    /** 把 assets/rime 底下所有檔案遞迴解到 sharedDataDir，用一個 .revision 戳記避免每次都做。 */
    private fun extractAssets(context: Context) {
        val stamp = File(sharedDataDir, ".revision")
        if (stamp.exists() && stamp.readText().trim() == ASSET_REVISION.toString()) return

        copyAssetDir(context, ASSET_ROOT, sharedDataDir)
        stamp.writeText(ASSET_REVISION.toString())
        Log.i(TAG, "assets 已解到 ${sharedDataDir.absolutePath}")
    }

    private fun copyAssetDir(context: Context, assetPath: String, target: File) {
        val children = context.assets.list(assetPath).orEmpty()
        if (children.isEmpty()) {
            // 沒有子項目 → 視為檔案
            target.parentFile?.mkdirs()
            context.assets.open(assetPath).use { input ->
                target.outputStream().use { output -> input.copyTo(output) }
            }
            return
        }
        if (!target.exists()) target.mkdirs()
        for (child in children) {
            copyAssetDir(context, "$assetPath/$child", File(target, child))
        }
    }
}
