package org.rimequad.ime.update

import android.content.Context
import android.content.SharedPreferences
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.os.Build
import android.os.Handler
import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.rimequad.ime.BuildConfig
import org.rimequad.ime.store.Downloader
import java.io.File
import java.util.concurrent.Executors

/**
 * 應用內更新的狀態持有者。
 *
 * ── 為什麼「不彈窗」是設計而不是偷懶 ────────────────────────────────────
 * 這是一個**輸入法**。使用者叫出鍵盤的時候正在寫東西給別人看，一個蓋在
 * 畫面上的「有新版本」對話框會直接打斷他打字，而且他多半按不到「稍後」
 * 就先失去了輸入焦點。所以：啟動時靜默檢查，結果只留在設定頁的一顆小紅點
 * 與一行字上，要不要更新完全由他決定何時去看。
 *
 * 檢查失敗（沒網路、DNS 掛了、R2 回 503）一律**安靜略過**，連 toast 都不發：
 * 使用者沒有要求檢查更新，不該為此收到錯誤訊息。只有他自己按「檢查更新」
 * 時，失敗才需要說出來。
 *
 * ── 執行緒 ──────────────────────────────────────────────────────────────
 * 網路與檔案 IO 全在單一背景執行緒（[worker]），狀態一律回主執行緒改。
 * 與 [org.rimequad.ime.store.StoreController] 同一個形狀，理由也一樣：
 * production 端零第三方依賴，不把 kotlinx-coroutines 當成自己的 API 用。
 * 本類別不呼叫任何 `rs_*`，與 rime_shell.h 的執行緒約定無關。
 */
class UpdateController private constructor(context: Context) {

    private val app = context.applicationContext
    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor { r -> Thread(r, "rime-update") }
    private val prefs: SharedPreferences =
        app.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    enum class Phase { IDLE, CHECKING, DOWNLOADING, VERIFYING, READY_TO_INSTALL, INSTALLING }

    /* ───────────────────────── 本機版本 ───────────────────────── */

    /** 目前安裝的 versionCode。取自 PackageInfo 而不是 BuildConfig —— 兩者
     *  正常時相同，但取 PackageInfo 才是「系統認定的本 app 版本」。 */
    val installedVersionCode: Long = runCatching {
        val info = app.packageManager.getPackageInfo(app.packageName, 0)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) info.longVersionCode
        else @Suppress("DEPRECATION") info.versionCode.toLong()
    }.getOrDefault(0L)

    val installedVersionName: String = runCatching {
        app.packageManager.getPackageInfo(app.packageName, 0).versionName
    }.getOrNull() ?: "?"

    val manifestUrl: String = BuildConfig.VERSION_MANIFEST_URL

    /* ───────────────────────── UI 狀態 ───────────────────────── */

    var phase by mutableStateOf(Phase.IDLE)
        private set

    /** 最近一次成功取得的遠端版本資訊（含快取回填）。 */
    var latest by mutableStateOf<VersionManifest?>(null)
        private set

    var verdict by mutableStateOf<UpdateVerdict?>(null)
        private set

    /** 0..1；負數代表「進行中但算不出百分比」。 */
    var progress by mutableStateOf(-1f)
        private set

    /** 給使用者看的最近一則狀態（成功訊息、進度說明）。 */
    var status by mutableStateOf<String?>(null)
        private set

    /**
     * 需要使用者注意的失敗。與 [status] 分開：進度訊息會被下一則蓋掉，
     * 失敗訊息不該被蓋掉。
     */
    var error by mutableStateOf<String?>(null)
        private set

    /** 已下載且**已驗過 sha256** 的 APK。null = 還沒有可安裝的檔案。 */
    var verifiedApk by mutableStateOf<File?>(null)
        private set

    var lastCheckAt by mutableStateOf(prefs.getLong(KEY_LAST_CHECK, 0L))
        private set

    /** 設定頁那顆小紅點就看這個。 */
    val hasUpdate: Boolean get() = verdict == UpdateVerdict.UPDATE_AVAILABLE

    val busy: Boolean get() = phase != Phase.IDLE && phase != Phase.READY_TO_INSTALL

    init {
        // 冷啟動時先把上次檢查的結果吃回來，小紅點才不必等到網路回來才出現。
        prefs.getString(KEY_CACHED_MANIFEST, null)?.let { cached ->
            when (val r = VersionManifestParser.parse(cached, manifestUrl)) {
                is ManifestParseResult.Ok -> applyManifest(r.manifest, announce = false)
                is ManifestParseResult.Err -> prefs.edit().remove(KEY_CACHED_MANIFEST).apply()
            }
        }
    }

    /* ───────────────────────── 檢查 ───────────────────────── */

    /**
     * 啟動時的靜默檢查。
     *
     * @param autoEnabled 使用者偏好「啟動時自動檢查更新」。null（未設定）視為開。
     */
    fun autoCheckOnStart(autoEnabled: Boolean?) {
        if (autoEnabled == false) return
        if (busy) return
        if (!hasNetwork()) return
        val since = System.currentTimeMillis() - lastCheckAt
        if (since in 0 until MIN_AUTO_INTERVAL_MS) return
        check(silent = true)
    }

    /** 使用者手動按「檢查更新」。失敗會說出來。 */
    fun checkNow() = check(silent = false)

    private fun check(silent: Boolean) {
        if (busy) return
        phase = Phase.CHECKING
        error = null
        if (!silent) status = "檢查中…"
        worker.execute {
            val result = Downloader.fetchText(manifestUrl, MAX_MANIFEST_BYTES)
            main.post {
                phase = Phase.IDLE
                lastCheckAt = System.currentTimeMillis()
                prefs.edit().putLong(KEY_LAST_CHECK, lastCheckAt).apply()
                when (result) {
                    is Downloader.Result.Err -> {
                        // 靜默模式什麼都不說 —— 使用者沒問，就不要拿網路錯誤煩他。
                        if (!silent) {
                            status = null
                            error = "取不到版本資訊：${result.message}"
                        }
                    }

                    is Downloader.Result.Ok -> {
                        when (val parsed = VersionManifestParser.parse(result.value, manifestUrl)) {
                            is ManifestParseResult.Err -> {
                                if (!silent) {
                                    status = null
                                    error = parsed.message
                                }
                            }

                            is ManifestParseResult.Ok -> {
                                prefs.edit()
                                    .putString(KEY_CACHED_MANIFEST, result.value)
                                    .apply()
                                applyManifest(parsed.manifest, announce = !silent)
                            }
                        }
                    }
                }
            }
        }
    }

    private fun applyManifest(m: VersionManifest, announce: Boolean) {
        latest = m
        verdict = UpdateCheck.verdict(installedVersionCode, m)
        // 換了一個版本，先前下載好的那份就不算數了。
        if (verifiedApk?.name?.contains(m.versionCode.toString()) == false) {
            verifiedApk = null
            if (phase == Phase.READY_TO_INSTALL) phase = Phase.IDLE
        }
        // 已經下載好、檔案還在、摘要也對得上 → 直接進入可安裝狀態，
        // 不要讓使用者為了同一份檔案下載第二次。
        cachedApkFor(m)?.let {
            verifiedApk = it
            phase = Phase.READY_TO_INSTALL
        }
        if (announce) {
            status = when (verdict) {
                UpdateVerdict.UPDATE_AVAILABLE -> "有新版本：${m.versionName}"
                UpdateVerdict.UP_TO_DATE -> "已是最新版本。"
                UpdateVerdict.DOWNGRADE ->
                    "伺服器上的版本（${m.versionName}）比本機舊，不提供更新。"
                null -> null
            }
        }
    }

    /* ───────────────────────── 下載 ───────────────────────── */

    fun downloadAndVerify() {
        val m = latest ?: return
        if (busy) return
        if (verdict != UpdateVerdict.UPDATE_AVAILABLE) return
        error = null
        phase = Phase.DOWNLOADING
        progress = 0f
        status = "下載中…"
        val dest = apkFileFor(m)
        worker.execute {
            dest.parentFile?.mkdirs()
            // 上限就是宣告的 size：多一個位元組都代表這不是我們要的那個檔案，
            // 沒有理由把它讀完再說。
            val r = Downloader.download(m.url, dest, m.size) { read, _ ->
                val f = if (m.size > 0) read.toFloat() / m.size else -1f
                main.post {
                    progress = f
                    status = "下載中… ${fmtBytes(read)} / ${fmtBytes(m.size)}"
                }
            }
            main.post {
                when (r) {
                    is Downloader.Result.Err -> {
                        dest.delete()
                        phase = Phase.IDLE
                        progress = -1f
                        status = null
                        error = "下載失敗：${r.message}"
                    }

                    is Downloader.Result.Ok -> {
                        phase = Phase.VERIFYING
                        status = "驗證檔案完整性…"
                        if (UpdateCheck.sha256Matches(m.sha256, r.value.sha256)) {
                            verifiedApk = dest
                            phase = Phase.READY_TO_INSTALL
                            progress = 1f
                            status = "已下載並通過 sha256 驗證，可以安裝。"
                        } else {
                            // 規範與市集那條線一致：不符即整包丟棄，不留著、
                            // 不「先裝再說」。
                            dest.delete()
                            verifiedApk = null
                            phase = Phase.IDLE
                            progress = -1f
                            status = null
                            error = buildString {
                                appendLine("下載的檔案未通過 sha256 驗證，已丟棄，不會安裝。")
                                appendLine("預期：${m.sha256}")
                                appendLine("實際：${r.value.sha256}")
                                append(
                                    "通常是傳輸過程壞掉（或中途被快取／代理動過）。" +
                                        "稍後再試一次；一直失敗請回報。"
                                )
                            }
                        }
                    }
                }
            }
        }
    }

    /* ───────────────────────── 安裝 ───────────────────────── */

    /**
     * 系統是否已允許本 app 安裝 APK。
     *
     * 為什麼要存成 state 而不是每次組合時現查：使用者是**離開 app 去系統設定**
     * 打開那個開關的，回來時 Compose 不會因此重組，畫面會一直停在「尚未授權」。
     * 由 Activity 在 `onResume` 呼叫 [refreshInstallPermission] 把它推一下。
     */
    var installPermitted by mutableStateOf(false)
        private set

    fun refreshInstallPermission() {
        installPermitted = canInstallPackages()
    }

    fun canInstallPackages(): Boolean = UpdateInstaller.canInstallPackages(app)

    fun openUnknownSourcesSettings() = UpdateInstaller.openUnknownSourcesSettings(app)

    fun install() {
        val apk = verifiedApk ?: return
        if (phase == Phase.INSTALLING) return
        if (!canInstallPackages()) {
            error = "系統尚未允許本 app 安裝應用程式。請先開啟「安裝未知的應用程式」。"
            return
        }
        error = null
        phase = Phase.INSTALLING
        status = "等待系統的安裝確認…"
        UpdateInstaller.install(app, apk) { ok, message ->
            main.post {
                phase = if (ok) Phase.IDLE else Phase.READY_TO_INSTALL
                if (ok) {
                    status = message
                    // 裝完了，快取檔沒有留著的理由（28MB）。
                    apk.delete()
                    verifiedApk = null
                } else {
                    status = null
                    error = message
                }
            }
        }
    }

    fun dismissError() {
        error = null
    }

    /* ───────────────────────── 雜項 ───────────────────────── */

    private fun updateDir(): File = File(app.cacheDir, "update")

    private fun apkFileFor(m: VersionManifest): File =
        File(updateDir(), "rime-${m.versionCode}.apk")

    /** 已下載且摘要正確的那一份；沒有就 null。 */
    private fun cachedApkFor(m: VersionManifest): File? {
        val f = apkFileFor(m)
        if (!f.isFile || f.length() != m.size) return null
        return runCatching {
            if (UpdateCheck.sha256Matches(m.sha256, Downloader.sha256Of(f))) f else null
        }.getOrNull()
    }

    private fun hasNetwork(): Boolean = runCatching {
        val cm = app.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val net = cm.activeNetwork ?: return false
        val caps = cm.getNetworkCapabilities(net) ?: return false
        caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
    }.getOrDefault(false)

    companion object {
        private const val PREFS = "rimequad-update"
        private const val KEY_CACHED_MANIFEST = "cached_manifest"
        private const val KEY_LAST_CHECK = "last_check_at"

        /** version.json 是一份幾百 bytes 的小檔，8KB 綽綽有餘。 */
        const val MAX_MANIFEST_BYTES = 64L * 1024

        /**
         * 兩次自動檢查之間的最短間隔。
         *
         * 輸入法的 Activity 可能一天被開好幾次（每次調設定都算）。沒有節流的話
         * 等於每次開設定都打一次伺服器，對使用者的行動數據與 R2 的請求數都不禮貌。
         * 手動按「檢查更新」不受這個限制。
         */
        const val MIN_AUTO_INTERVAL_MS = 6L * 60 * 60 * 1000

        @Volatile
        private var instance: UpdateController? = null

        /**
         * 單例。IME 服務與各個 Activity 在同一個行程，共用同一份狀態才不會
         * 出現「設定頁說有更新、診斷頁說沒有」這種事，下載到一半換頁也不會重來。
         */
        fun get(context: Context): UpdateController = instance ?: synchronized(this) {
            instance ?: UpdateController(context).also { instance = it }
        }
    }
}

internal fun fmtBytes(n: Long): String = when {
    n >= 1024 * 1024 -> "%.1f MB".format(n / 1024.0 / 1024.0)
    n >= 1024 -> "%.0f KB".format(n / 1024.0)
    else -> "$n B"
}
