package org.rimequad.ime.update

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.net.Uri
import android.os.Build
import android.provider.Settings
import android.util.Log
import java.io.File

/**
 * 用 [PackageInstaller] 安裝下載回來的 APK。
 *
 * ── 為什麼不是 ACTION_VIEW + FileProvider ───────────────────────────────
 * 舊寫法（丟一個 `application/vnd.android.package-archive` 的 Intent 出去）
 * 沒有任何回報管道：安裝成功、被使用者取消、還是因為簽章不符被系統拒絕，
 * app 一律收不到，只能在畫面上留一句「請稍候」然後永遠不知道下文。
 * `PackageInstaller` 會把結果送回我們自己的 receiver，失敗原因也拿得到 ——
 * 而失敗原因正是這條線最需要說清楚的東西（見 [describeStatus]）。
 *
 * ── 權限 ────────────────────────────────────────────────────────────────
 * 需要 `REQUEST_INSTALL_PACKAGES`，而且**光有這個權限不夠**：使用者還得
 * 在系統設定裡替本 app 打開「安裝未知的應用程式」。API 26 起這是逐 app
 * 的授權，只能引導他過去（[openUnknownSourcesSettings]），不能用對話框要。
 */
object UpdateInstaller {

    private const val TAG = "RimeUpdate"

    const val ACTION_STATUS = "org.rimequad.ime.UPDATE_INSTALL_STATUS"

    /**
     * 安裝結果的回呼。
     *
     * 為什麼是 process 層級的單一欄位而不是塞進 Intent：`PackageInstaller` 的
     * 結果由系統廣播回來，中間隔了一次跨行程往返，任何非 Parcelable 的東西
     * 都過不去。同一時間只會有一個安裝在跑，一個欄位就夠。
     */
    @Volatile
    private var pending: ((ok: Boolean, message: String) -> Unit)? = null

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
    fun install(context: Context, apk: File, onResult: (ok: Boolean, message: String) -> Unit) {
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
            dispatch(false, "無法啟動安裝：${e.message ?: e}")
        }
    }

    internal fun dispatch(ok: Boolean, message: String) {
        val cb = pending
        pending = null
        cb?.invoke(ok, message)
    }

    /**
     * 把 `PackageInstaller` 的 status 翻成使用者看得懂、而且**知道下一步做什麼**的話。
     *
     * `STATUS_FAILURE_CONFLICT` 特別重要：那是簽章不符時最常見的回報。
     * 使用者看到「衝突」四個字毫無頭緒，所以直說 —— 那代表這份 APK 不是
     * 同一把金鑰簽的，除了解除安裝重裝沒有別的路，而重裝會清掉他的詞典。
     */
    internal fun describeStatus(status: Int, raw: String?): String {
        val detail = raw?.takeIf { it.isNotBlank() }?.let { "\n（系統訊息：$it）" }.orEmpty()
        return when (status) {
            PackageInstaller.STATUS_FAILURE_ABORTED ->
                "安裝已取消。$detail"

            PackageInstaller.STATUS_FAILURE_BLOCKED ->
                "安裝被系統或裝置管理原則擋下。$detail"

            PackageInstaller.STATUS_FAILURE_CONFLICT ->
                "系統拒絕安裝：這份 APK 與已安裝的版本衝突。\n" +
                    "最常見的原因是**簽章不符** —— 下載到的檔案不是本專案的金鑰簽的。" +
                    "請不要用解除安裝的方式硬裝，那會清掉你的個人詞典與所有設定。" +
                    "先回報這個訊息。$detail"

            PackageInstaller.STATUS_FAILURE_INCOMPATIBLE ->
                "這份 APK 與本裝置不相容（ABI 或系統版本）。$detail"

            PackageInstaller.STATUS_FAILURE_INVALID ->
                "APK 檔案無效或已損毀。$detail"

            PackageInstaller.STATUS_FAILURE_STORAGE ->
                "儲存空間不足，無法安裝。$detail"

            else -> "安裝失敗（status=$status）。$detail"
        }
    }
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
                    UpdateInstaller.dispatch(false, "系統要求確認，但沒有給確認畫面。")
                    return
                }
                confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                runCatching { context.startActivity(confirm) }.onFailure {
                    UpdateInstaller.dispatch(false, "打不開系統的安裝確認畫面：${it.message}")
                }
            }

            PackageInstaller.STATUS_SUCCESS ->
                UpdateInstaller.dispatch(true, "安裝完成。新版本已生效。")

            else ->
                UpdateInstaller.dispatch(false, UpdateInstaller.describeStatus(status, raw))
        }
    }
}
