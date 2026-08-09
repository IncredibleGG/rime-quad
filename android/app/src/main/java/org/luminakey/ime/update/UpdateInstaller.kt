package org.luminakey.ime.update

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.net.Uri
import android.os.Build
import android.provider.Settings
import android.util.Log
import org.luminakey.ime.R
import java.io.File

/**
 * 用 [PackageInstaller] 安裝下載回來的 APK。
 *
 * ── 為什麼不是 ACTION_VIEW + FileProvider ───────────────────────────────
 * 舊寫法（丟一個 `application/vnd.android.package-archive` 的 Intent 出去）
 * 沒有任何回報管道：安裝成功、被使用者取消、還是因為簽章不符被系統拒絕，
 * app 一律收不到，只能在畫面上留一句「請稍候」然後永遠不知道下文。
 * `PackageInstaller` 會把結果送回我們自己的 receiver，失敗原因也拿得到 ——
 * 而失敗原因正是這條線最需要說清楚的東西（分類在 [InstallFailure]，
 * 字面在 [UpdateController.renderInstallFailure]）。
 *
 * ── 權限 ────────────────────────────────────────────────────────────────
 * 需要 `REQUEST_INSTALL_PACKAGES`，而且**光有這個權限不夠**：使用者還得
 * 在系統設定裡替本 app 打開「安裝未知的應用程式」。API 26 起這是逐 app
 * 的授權，只能引導他過去（[openUnknownSourcesSettings]），不能用對話框要。
 */
object UpdateInstaller {

    private const val TAG = "RimeUpdate"

    const val ACTION_STATUS = "org.luminakey.ime.UPDATE_INSTALL_STATUS"

    /**
     * 安裝結果的回呼。
     *
     * 為什麼是 process 層級的單一欄位而不是塞進 Intent：`PackageInstaller` 的
     * 結果由系統廣播回來，中間隔了一次跨行程往返，任何非 Parcelable 的東西
     * 都過不去。同一時間只會有一個安裝在跑，一個欄位就夠。
     */
    @Volatile
    private var pending: ((InstallOutcome) -> Unit)? = null

    /**
     * 讀一個**還沒安裝**的 APK 檔自己宣告的套件名。
     *
     * ══════════════════════════════════════════════════════════════════
     *  ⚠ 這是「按下安裝之前就知道」的最後一道、也是最可靠的一道防線
     * ══════════════════════════════════════════════════════════════════
     *
     * version.json 的 `package` 欄位要靠發布端寫，而且**舊的 version.json
     * 沒有它**。這一支不靠任何人：`getPackageArchiveInfo()` 直接剖析那個
     * 檔案裡的 AndroidManifest，不連網、不需要任何權限、不會安裝任何東西。
     *
     * 所以就算版本資訊完全沒提，我們仍然在把檔案交給系統之前就知道
     * 「它裝不上去」，而不是等系統回一句「APK 檔案無效或已損毀」。
     *
     * 回傳 null = 讀不出來（檔案被刪了、或系統剖析不了）。**null 不可以
     * 當成「不一樣」**：那會讓一個讀檔失敗變成「不給使用者升級」。
     */
    fun packageNameOf(context: Context, apk: File): String? = runCatching {
        context.packageManager.getPackageArchiveInfo(apk.absolutePath, 0)?.packageName
    }.getOrNull()

    /** 系統是否允許本 app 安裝 APK（使用者有沒有開「安裝未知的應用程式」）。 */
    fun canInstallPackages(context: Context): Boolean =
        context.packageManager.canRequestPackageInstalls()

    /** 帶使用者去開那個開關。開完他得自己按返回，系統沒有回呼可接。 */
    fun openUnknownSourcesSettings(context: Context) {
        val intent = Intent(
            Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
            Uri.parse("package:${context.packageName}"),
        ).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        context.startActivity(intent)
    }

    /**
     * 把 [apk] 寫進一個安裝 session 並提交。
     *
     * 呼叫端**必須**已經驗過 sha256。這裡不再驗一次，因為到這一步檔案已經
     * 落在私有 cacheDir 裡，重驗只是重讀 28MB。
     */
    fun install(context: Context, apk: File, onResult: (InstallOutcome) -> Unit) {
        pending = onResult
        val app = context.applicationContext
        try {
            val installer = app.packageManager.packageInstaller
            val params = PackageInstaller.SessionParams(
                PackageInstaller.SessionParams.MODE_FULL_INSTALL
            ).apply {
                // 指定套件名，讓系統在確認畫面上顯示成「更新」而不是「安裝新 app」。
                setAppPackageName(app.packageName)
            }
            val sessionId = installer.createSession(params)
            installer.openSession(sessionId).use { session ->
                session.openWrite("rime-update.apk", 0, apk.length()).use { out ->
                    apk.inputStream().use { it.copyTo(out, DEFAULT_BUFFER_SIZE) }
                    session.fsync(out)
                }
                val statusIntent = Intent(app, UpdateInstallReceiver::class.java)
                    .setAction(ACTION_STATUS)
                    .setPackage(app.packageName)
                // FLAG_MUTABLE 是必要的：系統要往這個 PendingIntent 塞
                // EXTRA_STATUS／EXTRA_INTENT。不可變的話什麼都收不到。
                var flags = PendingIntent.FLAG_UPDATE_CURRENT
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    flags = flags or PendingIntent.FLAG_MUTABLE
                }
                val sender = PendingIntent.getBroadcast(app, sessionId, statusIntent, flags)
                session.commit(sender.intentSender)
            }
        } catch (e: Exception) {
            Log.w(TAG, "建立安裝 session 失敗", e)
            dispatch(
                InstallOutcome.DidNotStart(
                    app.getString(R.string.upgrade_err_session, e.message ?: e.toString())
                )
            )
        }
    }

    internal fun dispatch(outcome: InstallOutcome) {
        val cb = pending
        pending = null
        cb?.invoke(outcome)
    }
}

/**
 * 一次安裝嘗試的結果。
 *
 * ⚠ **刻意把「系統拒絕」與「還沒走到系統那一關」分開。** 兩者要說的話不同：
 * 前者的原因由 [InstallFailure] 分類、由使用者的下一步決定字面；後者是我們
 * 自己的問題（建不出 session、開不了確認畫面），使用者只能回報。
 * 併成一個「安裝失敗」字串正是這一輪在修的那種形狀。
 */
sealed class InstallOutcome {

    object Success : InstallOutcome()

    /**
     * 系統看過檔案之後拒絕了。
     *
     * [raw] 是系統原文，**要原樣留著**（附在訊息末尾）—— 它是回報時唯一
     * 有價值的東西。但它不可以是**主要**的那一句話：使用者看不懂
     * `INSTALL_FAILED_INVALID_APK`，而照字面理解會把他帶去錯的地方。
     */
    data class Rejected(val kind: InstallFailureKind, val raw: String?) : InstallOutcome()

    /** 還沒交給系統就失敗了。[message] 已經是在地化過的完整句子。 */
    data class DidNotStart(val message: String) : InstallOutcome()
}

/**
 * 接 `PackageInstaller` 的狀態廣播。
 *
 * 必須是 manifest 宣告的靜態 receiver：安裝過程中系統可能把我們的
 * Activity 收掉（安裝自己這個 app 尤其如此），動態註冊的 receiver 會跟著
 * 消失，結果就是永遠收不到成功通知。
 */
class UpdateInstallReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, Int.MIN_VALUE)
        val raw = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE)
        when (status) {
            PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                // 系統要使用者親自按下「安裝」。這是**必經**的一步，不是可以
                // 繞過的障礙：側載安裝的確認畫面只有系統畫得出來。
                @Suppress("DEPRECATION")
                val confirm = intent.getParcelableExtra<Intent>(Intent.EXTRA_INTENT)
                if (confirm == null) {
                    UpdateInstaller.dispatch(
                        InstallOutcome.DidNotStart(
                            context.getString(R.string.upgrade_status_no_confirm_screen)
                        )
                    )
                    return
                }
                confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                runCatching { context.startActivity(confirm) }.onFailure {
                    UpdateInstaller.dispatch(
                        InstallOutcome.DidNotStart(
                            context.getString(
                                R.string.upgrade_status_no_confirm_open,
                                it.message ?: it.toString(),
                            )
                        )
                    )
                }
            }

            PackageInstaller.STATUS_SUCCESS ->
                UpdateInstaller.dispatch(InstallOutcome.Success)

            else ->
                // ⚠ 分類在這裡做，字面在 UpdateController 做。
                //    receiver 是跨行程回來的，拿得到 Context 但拿不到那一版
                //    manifest（例如檔案多大），而「空間不足」那一句需要它。
                UpdateInstaller.dispatch(
                    InstallOutcome.Rejected(InstallFailure.classify(status, raw), raw)
                )
        }
    }
}
