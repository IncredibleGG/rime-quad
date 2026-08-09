package org.luminakey.ime.update

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.luminakey.ime.BuildConfig
import org.luminakey.ime.R
import org.luminakey.ime.net.NetworkGate
import org.luminakey.ime.net.NetworkPurpose
import org.luminakey.ime.store.FileDigest
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
 * 與 [org.luminakey.ime.store.StoreController] 同一個形狀，理由也一樣：
 * production 端零第三方依賴，不把 kotlinx-coroutines 當成自己的 API 用。
 * 本類別不呼叫任何 `rs_*`，與 rime_shell.h 的執行緒約定無關。
 */
class UpdateController private constructor(context: Context) {

    private val app = context.applicationContext
    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor { r -> Thread(r, "rime-update") }
    private val prefs: SharedPreferences =
        app.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    /**
     * 背景執行緒上沒有 composition，所以走 `app.getString()`。
     * 與 `store.BackupController.str` 同一個作法：同一份資源、同一個語系，
     * 只是不需要 Compose 在場。**這一層一句寫死的字面都不該有** ——
     * 寫死的中文對一個英文使用者來說就是另一種「訊息指向錯的地方」。
     */
    private fun str(id: Int, vararg args: Any): String =
        if (args.isEmpty()) app.getString(id) else app.getString(id, *args)

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

    /**
     * 線上那一版換了 app 的身分 —— 系統不會讓它蓋在這一支上面。
     *
     * ══════════════════════════════════════════════════════════════════
     *  ⚠ 這一格不是「錯誤」，是一條**不同的路**
     * ══════════════════════════════════════════════════════════════════
     *
     * 2026-08-09 產品改名（`org.rimequad.ime` → `org.luminakey.ime`）之後，  舊名
     * 使用者按下安裝拿到的是「APK 檔案無效或已損毀」。檔案沒有壞，是
     * 升級器提供了一個**它自己裝不起來的更新**，然後把系統的原始訊息
     * 原樣轉述。兩個缺陷，都在這一格裡修掉：
     *
     *   1. 這件事在**下載之前**就判定得出來（[PackageIdentity]），所以
     *      這一格非 null 時我們不下載、也不給「安裝」按鈕。
     *   2. 使用者需要知道的是「為什麼」與「他要做什麼」，包括那句
     *      **詞典與設定不會自動轉移** —— 那是他會真的失去東西的地方，
     *      而系統的任何一則訊息都不會提到它。
     */
    var migration by mutableStateOf<Migration?>(null)
        private set

    /**
     * @param fromPackage 使用者手上這一支的身分
     * @param toPackage   線上那一版的身分
     * @param declared    線上那一份有沒有**明說**它取代的就是我們。false 時
     *                    語氣要保守：那可能不是我們自己改名，而是版本資訊
     *                    的網址指錯了地方。
     * @param downloadUrl APK 的直接網址（複製連結用）
     * @param pageUrl     給人看的下載頁；沒有就退回 [downloadUrl]
     */
    data class Migration(
        val fromPackage: String,
        val toPackage: String,
        val declared: Boolean,
        val versionName: String,
        val downloadUrl: String,
        val pageUrl: String?,
    ) {
        val openUrl: String get() = pageUrl ?: downloadUrl
    }

    /** 最近一次安裝失敗的分類。UI 靠它決定要不要順便把搬家步驟畫出來。 */
    var lastInstallFailure by mutableStateOf<InstallFailureKind?>(null)
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
        // 連網開關關閉 → **完全不執行**。
        //
        // 這一行是這個專案定位的一部分，不是最佳化。啟動時的靜默檢查是一次
        // 使用者沒有同意過的連網；在一個主打「離線為預設、連網由使用者掌控」
        // 的輸入法裡，那件事只有在他自己打開開關之後才說得通。
        if (!NetworkGate.isEnabled) return
        if (autoEnabled == false) return
        if (busy) return
        if (!hasNetwork()) return
        val since = System.currentTimeMillis() - lastCheckAt
        if (since in 0 until MIN_AUTO_INTERVAL_MS) return
        check(silent = true)
    }

    /**
     * 使用者手動按「檢查更新」。失敗會說出來。
     *
     * 開關關閉時不去連（連了也會被 [NetworkGate] 拒絕），但**要說**：
     * 他剛剛主動按了一顆按鈕，什麼都不發生會被當成壞掉。
     */
    fun checkNow() {
        if (!NetworkGate.isEnabled) {
            status = null
            error = str(R.string.upgrade_err_network_off_check)
            return
        }
        check(silent = false)
    }

    private fun check(silent: Boolean) {
        if (busy) return
        phase = Phase.CHECKING
        error = null
        if (!silent) status = str(R.string.upgrade_status_checking)
        worker.execute {
            val result = NetworkGate.fetchText(
                manifestUrl, NetworkPurpose.UPDATE_MANIFEST, maxBytes = MAX_MANIFEST_BYTES,
            )
            main.post {
                phase = Phase.IDLE
                lastCheckAt = System.currentTimeMillis()
                prefs.edit().putLong(KEY_LAST_CHECK, lastCheckAt).apply()
                when (result) {
                    is NetworkGate.Result.Err -> {
                        // 靜默模式什麼都不說 —— 使用者沒問，就不要拿網路錯誤煩他。
                        if (!silent) {
                            status = null
                            error = str(R.string.upgrade_err_no_manifest, result.message)
                        }
                    }

                    is NetworkGate.Result.Ok -> {
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
        if (announce) {
            status = when (verdict) {
                // 不重複「有新版本：<版號>」—— 卡片標題已經用紅點與粗體說了一次，
                // 在它正下方再說一遍只是噪音。這裡只確認「你按的那一下有作用」。
                UpdateVerdict.UPDATE_AVAILABLE -> str(R.string.upgrade_status_check_done)
                UpdateVerdict.UP_TO_DATE -> str(R.string.upgrade_status_up_to_date)
                UpdateVerdict.DOWNGRADE ->
                    str(R.string.upgrade_status_downgrade, m.versionName)
                null -> null
            }
        }

        if (verdict != UpdateVerdict.UPDATE_AVAILABLE) {
            // 沒有東西要裝了。快取裡那 26MB 必須清掉,而且「安裝」按鈕必須消失 ——
            // 實測踩到過:更新裝完之後系統會把行程收掉,install() 裡那個刪檔的
            // 回呼根本沒機會跑,下次開起來就看到一顆「安裝」按鈕要你再裝一次
            // 已經裝好的版本。清理因此不能只掛在成功回呼上。
            verifiedApk = null
            migration = null
            if (phase == Phase.READY_TO_INSTALL) phase = Phase.IDLE
            worker.execute { updateDir().listFiles()?.forEach { it.delete() } }
            return
        }

        // ── ⚠ 換 app 身分的判定，**排在下載之前** ────────────────────────
        //
        // 順序就是這一輪要修的第一個缺陷：這一段只要排到下載之後，就變成
        // 「下載 28MB，再讓系統拒絕，再把系統的話原樣轉述給使用者」。
        // 套件名一不一樣是**事先判定得出來的事實**，不是要等系統開口的事。
        //
        // ⚠ 只在 UPDATE_AVAILABLE 這一格判。降級的情況下叫使用者「解除安裝
        //    再裝一個更舊的」是把他推下懸崖；那種組合代表發布端出了事，
        //    上面 DOWNGRADE 那一句已經說了。
        //
        // ⚠ 缺 `package` 欄位（[PackageIdentity.Verdict.UNKNOWN]）時**行為與
        //    從前完全相同**：照常下載。使用者手上的舊版會讀到新的 version.json，
        //    新版也可能讀到還沒更新的舊 version.json，兩個方向都得活下去。
        //    真正兜底的是安裝前那道「直接讀 APK 檔自己的套件名」（見 [install]），
        //    它不需要發布端配合，也就不會因為誰忘了改而失效。
        migration = when (PackageIdentity.compare(app.packageName, m)) {
            PackageIdentity.Verdict.CHANGED -> Migration(
                fromPackage = app.packageName,
                toPackage = m.packageId.orEmpty(),
                declared = PackageIdentity.declaresReplacing(app.packageName, m),
                versionName = m.versionName,
                downloadUrl = m.url,
                pageUrl = m.pageUrl,
            )

            PackageIdentity.Verdict.SAME, PackageIdentity.Verdict.UNKNOWN -> null
        }
        if (migration != null) {
            // 不下載、不留半份檔案、也不給「安裝」按鈕。使用者要做的事
            // 全部在 UpdateSection 的搬家卡片上。
            verifiedApk = null
            if (phase == Phase.READY_TO_INSTALL) phase = Phase.IDLE
            worker.execute { updateDir().listFiles()?.forEach { it.delete() } }
            if (announce) status = str(R.string.upgrade_not_downloaded)
            return
        }

        // 已經下載好、檔案還在、摘要也對得上 → 直接進入可安裝狀態，
        // 不要讓使用者為了同一份檔案下載第二次。
        //
        // 這一段丟到背景做：驗一次摘要要讀 26MB，在主執行緒上做會讓設定頁
        // 卡住半秒以上。
        worker.execute {
            purgeStaleApks(m)
            val cached = cachedApkFor(m)
            main.post {
                // 期間可能又檢查到別的版本、或使用者已經自己按了下載。
                if (cached != null && latest?.versionCode == m.versionCode && !busy &&
                    verifiedApk == null
                ) {
                    verifiedApk = cached
                    phase = Phase.READY_TO_INSTALL
                    status = str(R.string.upgrade_status_ready_cached)
                }
            }
        }
    }

    /** 刪掉快取裡不是目標版本的 APK。留著只是佔空間。 */
    private fun purgeStaleApks(m: VersionManifest) {
        val keep = apkFileFor(m).name
        updateDir().listFiles()?.forEach { if (it.name != keep) it.delete() }
    }

    /* ───────────────────────── 下載 ───────────────────────── */

    fun downloadAndVerify() {
        val m = latest ?: return
        if (busy) return
        if (verdict != UpdateVerdict.UPDATE_AVAILABLE) return
        // ⚠ 已知裝不上去就不要下載。這一行就是「不要下載完再讓系統拒絕」。
        if (migration != null) return
        if (!NetworkGate.isEnabled) {
            error = str(R.string.upgrade_err_network_off_download)
            return
        }
        error = null
        phase = Phase.DOWNLOADING
        progress = 0f
        status = str(R.string.upgrade_status_downloading, fmtBytes(0), fmtBytes(m.size))
        val dest = apkFileFor(m)
        worker.execute {
            dest.parentFile?.mkdirs()
            // 上限就是宣告的 size：多一個位元組都代表這不是我們要的那個檔案，
            // 沒有理由把它讀完再說。
            val r = NetworkGate.download(
                m.url, dest, m.size, NetworkPurpose.UPDATE_APK, m.versionName,
            ) { read, _ ->
                val f = if (m.size > 0) read.toFloat() / m.size else -1f
                main.post {
                    progress = f
                    status = str(
                        R.string.upgrade_status_downloading,
                        fmtBytes(read),
                        fmtBytes(m.size),
                    )
                }
            }
            main.post {
                when (r) {
                    is NetworkGate.Result.Err -> {
                        dest.delete()
                        phase = Phase.IDLE
                        progress = -1f
                        status = null
                        error = str(R.string.upgrade_err_download_failed, r.message)
                    }

                    is NetworkGate.Result.Ok -> {
                        phase = Phase.VERIFYING
                        status = str(R.string.upgrade_status_verifying)
                        if (UpdateCheck.sha256Matches(m.sha256, r.value.sha256)) {
                            verifiedApk = dest
                            phase = Phase.READY_TO_INSTALL
                            progress = 1f
                            status = str(R.string.upgrade_status_ready)
                        } else {
                            // 規範與市集那條線一致：不符即整包丟棄，不留著、
                            // 不「先裝再說」。
                            dest.delete()
                            verifiedApk = null
                            phase = Phase.IDLE
                            progress = -1f
                            status = null
                            // ⚠ **整條線上唯一一句可以說「檔案壞了」的話** ——
                            //    因為只有這裡真的比對過雜湊而且不符。
                            //    安裝失敗那一側不准說（見 InstallFailure 檔頭）。
                            error = str(
                                R.string.upgrade_err_sha_mismatch,
                                m.sha256,
                                r.value.sha256,
                            )
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
            error = str(R.string.upgrade_err_install_not_permitted)
            return
        }

        // ── ⚠ 最後一道：直接問這個檔案「你是誰」 ────────────────────────
        //
        // version.json 的 `package` 欄位靠發布端寫，而**舊的 version.json
        // 沒有它** —— 也就是說，光靠上面那道 preflight，下一次換套件名時
        // 仍然有一批人會走到系統那句「APK 檔案無效或已損毀」。
        //
        // 這一行不靠任何人：它剖析的是我們手上這個檔案自己宣告的套件名。
        // 系統會拒絕的事情，我們在把檔案交給它之前就先知道，並且說得比它清楚。
        //
        // 讀不出來（null）時**照常安裝**：一個讀檔失敗不該變成「不給你升級」。
        val apkPackage = UpdateInstaller.packageNameOf(app, apk)
        if (apkPackage != null && apkPackage != app.packageName) {
            val m = latest
            migration = Migration(
                fromPackage = app.packageName,
                toPackage = apkPackage,
                declared = m != null && PackageIdentity.declaresReplacing(app.packageName, m),
                versionName = m?.versionName ?: installedVersionName,
                downloadUrl = m?.url ?: manifestUrl,
                pageUrl = m?.pageUrl,
            )
            // 檔案留著沒有意義：它裝不上去，而且是 28MB。
            apk.delete()
            verifiedApk = null
            phase = Phase.IDLE
            status = null
            error = null
            return
        }

        error = null
        lastInstallFailure = null
        phase = Phase.INSTALLING
        status = str(R.string.upgrade_status_waiting_confirm)
        UpdateInstaller.install(app, apk) { outcome ->
            main.post { onInstallOutcome(apk, outcome) }
        }
    }

    private fun onInstallOutcome(apk: File, outcome: InstallOutcome) {
        when (outcome) {
            is InstallOutcome.Success -> {
                phase = Phase.IDLE
                status = str(R.string.upgrade_status_installed)
                lastInstallFailure = null
                // 裝完了，快取檔沒有留著的理由（28MB）。
                apk.delete()
                verifiedApk = null
            }

            is InstallOutcome.DidNotStart -> {
                phase = Phase.READY_TO_INSTALL
                status = null
                error = outcome.message
            }

            is InstallOutcome.Rejected -> {
                phase = Phase.READY_TO_INSTALL
                status = null
                lastInstallFailure = outcome.kind
                error = renderInstallFailure(outcome)

                // 系統說的是「套件名不同」→ 這不是重試得好的事，是要搬家。
                // 把搬家卡片叫出來（連 version.json 都沒說的情況下，系統的
                // 那一則訊息就是我們唯一的來源）。
                if (outcome.kind == InstallFailureKind.PACKAGE_ID_CHANGED) {
                    val pair = PackageIdentity.inconsistentPackages(outcome.raw)
                    val m = latest
                    migration = Migration(
                        fromPackage = pair?.first ?: app.packageName,
                        toPackage = pair?.second ?: m?.packageId.orEmpty(),
                        declared = m != null &&
                            PackageIdentity.declaresReplacing(app.packageName, m),
                        versionName = m?.versionName ?: installedVersionName,
                        downloadUrl = m?.url ?: manifestUrl,
                        pageUrl = m?.pageUrl,
                    )
                    apk.delete()
                    verifiedApk = null
                    phase = Phase.IDLE
                }
            }
        }
    }

    /**
     * 把一則安裝失敗寫成使用者讀得懂、而且**知道下一步做什麼**的話。
     *
     * 三段，順序固定：
     *   1. 這是什麼（[InstallFailureKind] 各有各的一句，含下一步）；
     *   2. 「檔案沒有壞」—— 因為它剛剛才通過 sha256，而使用者的第一個念頭
     *      一定是「是不是下載壞了」。先替他把那條錯路關掉；
     *   3. 系統原文，原樣保留，給回報用。
     *
     * ⚠ 第 2 段刻意不出現在「使用者自己取消」那一則：他沒有懷疑檔案，
     *    多說一句只是噪音。
     */
    private fun renderInstallFailure(outcome: InstallOutcome.Rejected): String {
        val size = latest?.size
        val head = when (outcome.kind) {
            InstallFailureKind.CANCELLED -> str(R.string.upgrade_fail_cancelled)
            InstallFailureKind.PACKAGE_ID_CHANGED -> str(R.string.upgrade_fail_package_changed)
            InstallFailureKind.SIGNATURE_MISMATCH -> str(R.string.upgrade_fail_signature)
            InstallFailureKind.DOWNGRADE -> str(R.string.upgrade_fail_downgrade)
            InstallFailureKind.NOT_ENOUGH_SPACE ->
                str(R.string.upgrade_fail_storage, fmtBytes(size ?: 0L))
            InstallFailureKind.DEVICE_INCOMPATIBLE -> str(R.string.upgrade_fail_incompatible)
            InstallFailureKind.BLOCKED -> str(R.string.upgrade_fail_blocked)
            InstallFailureKind.REJECTED_UNEXPLAINED -> str(R.string.upgrade_fail_unexplained)
        }
        return buildString {
            append(head)
            if (outcome.kind != InstallFailureKind.CANCELLED) {
                append("\n\n")
                append(str(R.string.upgrade_fail_verified))
            }
            outcome.raw?.takeIf { it.isNotBlank() }?.let {
                append("\n\n")
                append(str(R.string.upgrade_fail_system_detail, it))
            }
        }
    }

    /* ───────────────────── 搬家時使用者做得到的事 ───────────────────── */

    /**
     * 把下載網址放進剪貼簿。
     *
     * 為什麼這顆按鈕存在：搬家卡片上的每一句話都在告訴使用者「你要自己去
     * 下載」，而一個 60 個字元的網址是他**抄不下來**的。沒有這一顆，
     * 那段說明等於沒有下一步。
     */
    fun copyDownloadLink(): Boolean {
        val url = migration?.downloadUrl ?: latest?.url ?: return false
        return runCatching {
            val cm = app.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
            cm.setPrimaryClip(ClipData.newPlainText(app.packageName, url))
            status = str(R.string.upgrade_link_copied)
            true
        }.getOrDefault(false)
    }

    /**
     * 用外部瀏覽器打開下載頁。
     *
     * ⚠ 打不開時**要說**，而且要說得出替代方案 —— 一顆按下去什麼都不發生的
     * 按鈕，在這個畫面上會被理解成「連這個也壞了」。
     */
    fun openDownloadPage(): Boolean {
        val url = migration?.openUrl ?: latest?.url ?: return false
        val intent = Intent(Intent.ACTION_VIEW, Uri.parse(url))
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        return runCatching { app.startActivity(intent); true }.getOrElse {
            error = str(R.string.upgrade_no_browser, str(R.string.upgrade_copy_link))
            false
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
            if (UpdateCheck.sha256Matches(m.sha256, FileDigest.sha256(f))) f else null
        }.getOrNull()
    }

    /**
     * 系統目前有沒有網路。只讀系統狀態、不送封包，實作與註解都在
     * [NetworkGate.systemHasNetwork] —— 跟網路沾邊的東西全部集中在那一個檔案，
     * 審計時才不會有「還有一個地方也在碰網路」的意外。
     */
    private fun hasNetwork(): Boolean = NetworkGate.systemHasNetwork(app)

    companion object {
        private const val PREFS = "luminakey-update"
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
