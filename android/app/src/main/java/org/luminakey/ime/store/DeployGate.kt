package org.luminakey.ime.store

import android.os.SystemClock
import android.util.Log
import org.luminakey.ime.core.RimeCore
import org.luminakey.ime.core.RimeDeployStatus
import java.util.concurrent.atomic.AtomicBoolean

/**
 * 把非同步的 `rs_deploy()` 包成「呼叫端等它結束」的形態。
 *
 * rime_shell.h 的兩條約定決定了這個檔案的長相：
 *
 *   1. `rs_deploy()` 是**唯一**允許跨執行緒呼叫的函式。所以整個導入流程
 *      可以在背景執行緒上跑，不必跟 IME 主執行緒搶。
 *   2. `rs_deploy_callback` **不在呼叫端的執行緒上**觸發，而且可能在
 *      `rs_deploy()` 早就返回之後才來。所以不能用「呼叫完就當作做完了」，
 *      必須真的等回呼。
 *
 * ⚠ [RimeCore.addDeployListener] 在註冊當下會**立刻重播**最近一次的狀態。
 * 若不擋掉那一次重播，上一輪部署留下的 SUCCESS 會被誤認成這一輪的結果，
 * 於是「部署失敗」變成「部署成功」，回滾永遠不會被觸發。這裡用 [armed]
 * 在真正呼叫 `rs_deploy()` 之後才開始採信回呼。
 */
object DeployGate {

    private const val TAG = "DeployGate"

    /**
     * 上限刻意放到遠大於 [org.luminakey.ime.core.DeployEstimate] 的量級：
     * 那個估計值講的是隨附的三本詞庫，而使用者裝了大方案之後可以久得多。
     * 這個常數要擋的是「回呼永遠不來」，不是「跑得比預期久」。
     */
    const val DEFAULT_TIMEOUT_MS = 10 * 60 * 1000L

    /**
     * 為什麼「沒開始」要用代號而不是一句話。
     *
     * 這裡原本是 `NotStarted(reason = "librime 尚未初始化")` —— 一句寫死的
     * 繁體中文，而預設語系是英文，它會原樣上畫面（`StoreController.redeploy()`
     * 把 `reason` 直接當成對話框的內容）。同一個 commit 才剛把鍵盤上那幾句
     * 寫死的中文改成資源，卻在自己新開的這條路上又犯了一次。
     *
     * 代號留在引擎層、字留在 `res/values…/strings_fail.xml`，
     * 「有人又寫死一句中文」就變成守門看得見的事（`ImeNoticeStringsTest`）。
     */
    enum class NotStartedReason {
        /**
         * `RimeCore.isInitialized == false` —— `nativeInit()` 沒成功過。
         *
         * ⚠ 這是**畫面上那顆「重新整理字詞」的死路**：五條 `RimeRuntime` 走到
         * FAILED 的路裡有四條停在這裡，按幾次都一樣。所以那四條路現在根本
         * 不畫那顆按鈕，見 [org.luminakey.ime.home.actionOf]。
         * 這條代號仍然存在，因為診斷頁的「重新部署」按得到。
         */
        NOT_INITIALIZED,

        /** `rs_deploy()` 拒絕啟動 —— 多半是已經有一個部署在跑。 */
        REFUSED,
    }

    sealed class Outcome {
        data class Success(val elapsedMs: Long) : Outcome()
        data class Failure(val elapsedMs: Long, val lastError: String) : Outcome()
        data class Timeout(val elapsedMs: Long) : Outcome()

        /**
         * [detail] 是引擎的原始訊息（`rs_last_error()`），可能是空字串。
         * 它是**故障載荷**，不是介面文字 —— 讀者是收到問題回報的我們。
         */
        data class NotStarted(
            val reason: NotStartedReason,
            val detail: String = "",
        ) : Outcome()
    }

    /**
     * 觸發部署並等到結果。**不可在主執行緒呼叫**。
     *
     * [onTick] 每 200ms 回報一次已耗時，供進度 UI 顯示 —— librime 不提供
     * 百分比，唯一誠實的進度就是「已經跑了幾秒」。
     */
    fun deployAndWait(
        timeoutMs: Long = DEFAULT_TIMEOUT_MS,
        onTick: ((elapsedMs: Long) -> Unit)? = null,
    ): Outcome {
        if (!RimeCore.isInitialized) {
            return Outcome.NotStarted(NotStartedReason.NOT_INITIALIZED)
        }

        val lock = Object()
        val armed = AtomicBoolean(false)
        var result: RimeDeployStatus? = null

        val listener: (RimeDeployStatus) -> Unit = { status ->
            if (armed.get()) {
                when (status) {
                    RimeDeployStatus.SUCCESS, RimeDeployStatus.FAILURE ->
                        synchronized(lock) {
                            if (result == null) {
                                result = status
                                lock.notifyAll()
                            }
                        }

                    else -> Unit
                }
            }
        }

        RimeCore.addDeployListener(listener)   // 這一行會重播舊狀態，故意還沒 arm
        val started = SystemClock.elapsedRealtime()
        try {
            armed.set(true)
            if (!RimeCore.deploy()) {
                return Outcome.NotStarted(NotStartedReason.REFUSED, RimeCore.lastError())
            }
            synchronized(lock) {
                while (result == null) {
                    val elapsed = SystemClock.elapsedRealtime() - started
                    if (elapsed >= timeoutMs) return Outcome.Timeout(elapsed)
                    lock.wait(200)
                    onTick?.invoke(SystemClock.elapsedRealtime() - started)
                }
            }
            val elapsed = SystemClock.elapsedRealtime() - started
            Log.i(TAG, "部署結束: $result，耗時 ${elapsed}ms")
            return if (result == RimeDeployStatus.SUCCESS) {
                Outcome.Success(elapsed)
            } else {
                Outcome.Failure(elapsed, RimeCore.lastError())
            }
        } catch (e: InterruptedException) {
            Thread.currentThread().interrupt()
            return Outcome.Timeout(SystemClock.elapsedRealtime() - started)
        } finally {
            RimeCore.removeDeployListener(listener)
        }
    }
}
