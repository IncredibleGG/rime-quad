package org.luminakey.ime.devtools

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.util.Log
import org.luminakey.ime.core.RimeCore
import org.luminakey.ime.core.RimeRuntime
import org.luminakey.ime.store.BackupController
import java.io.File

/**
 * 匯出／匯入的**無頭**驅動器。只存在於 debug 建置（`src/debug/`）。
 *
 * ══════════════════════════════════════════════════════════════════════
 *  ⚠ 這是測試設備，不是功能。release 建置裡沒有這個類別
 * ══════════════════════════════════════════════════════════════════════
 *
 * ── 為什麼需要它 ────────────────────────────────────────────────────────
 * 「把全部存成一個文件 / 從文件恢復」那條路徑**從來沒有人真的跑過一次
 * 完整往返**。而它壞掉的長相是最糟的一種：匯入回報成功，使用者過幾天才
 * 發現最近學的詞不見了，而且沒有任何錯誤訊息。
 *
 * 要驗它，必須：
 *   1. 讓**真的** IME 學到幾個詞（走 rs_process_key，不是塞假資料）；
 *   2. 在那個 session **還活著**的時候匯出 —— 因為未落地的交易就住在
 *      它裡面（見 `store/UserDbSnapshot.kt` 檔頭）；
 *   3. 清掉 app 資料；
 *   4. 匯入；
 *   5. **重新打一次同樣的鍵，確認那幾個詞真的還在候選裡。**
 *
 * 第 2 步是關鍵，也是唯一沒辦法用 JVM 單元測試模擬的一步：那個交易活在
 * librime 的行程記憶體裡。
 *
 * ── 為什麼是 BroadcastReceiver 而不是儀器測試 ───────────────────────────
 * 本模組沒有 androidTest 的相依（連 androidx.test.runner 都沒有），加一套
 * 進來要動 build.gradle.kts 與離線的 maven 快取。一個 debug-only 的
 * receiver 由 `am broadcast` 驅動，做得到完全一樣的事，而且**跑在使用者那條
 * 路徑的同一個行程裡** —— IME 與設定畫面同進程，所以它看得到 IME 那個
 * session 掛著的交易，這正是要驗的東西。
 *
 * ⚠ **沒有被這支涵蓋的**：SAF 的檔案選擇器本身（`ACTION_CREATE_DOCUMENT` /
 *   `ACTION_OPEN_DOCUMENT` 的對話框）。這裡直接給一個 `file:` Uri 進
 *   `BackupController`，`ContentResolver` 對 file scheme 走的是同一組
 *   `openOutputStream` / `openInputStream`，但**選檔器的互動沒有驗到**。
 *
 * ⚠ **不是 Activity。** 試過，行不通：`am start` 帶著 FLAG_ACTIVITY_NEW_TASK，
 *   第二次之後會被送進既有那個實例的 onNewIntent，於是同一個指令跑第一次
 *   有效、之後全部靜默無效 —— 一個**看起來成功、實際什麼都沒做**的驗證器，
 *   正是這個專案最不需要的東西。receiver 沒有那個問題。
 *
 * 用法：
 *   am broadcast -n <pkg>/org.luminakey.ime.devtools.BackupHarnessReceiver \
 *                --es op export --es path /sdcard/…/rt.zip
 *   am broadcast … --es op import --es path …
 *   am broadcast … --es op probe  --es keys nihao --ei top 5
 *
 * 結果一律以 `BACKUPRT` 這個 tag 印進 logcat（一行一則，`key=value`）。
 */
class BackupHarnessReceiver : BroadcastReceiver() {

    private lateinit var app: Context

    override fun onReceive(context: Context, intent: Intent) {
        app = context.applicationContext
        RimeRuntime.start(app)

        val op = intent.getStringExtra("op").orEmpty()
        val path = intent.getStringExtra("path").orEmpty()
        val keys = intent.getStringExtra("keys").orEmpty()
        val top = intent.getIntExtra("top", 5)
        say("begin op=$op path=$path keys=$keys")

        // goAsync() 讓行程在 onReceive 回來之後還能活著等我們做完 ——
        // 匯出／匯入要幾秒到幾十秒，而 onReceive 本身有 10 秒上限。
        val pending = goAsync()
        Thread {
            runCatching {
                when (op) {
                    "export" -> doExport(path)
                    "import" -> doImport(path)
                    "probe" -> doProbe(intent.getStringExtra("schema"), keys, top)
                    "learn" -> doLearn(
                        intent.getStringExtra("schema"),
                        keys,
                        intent.getIntExtra("pick", 0),
                    )
                    "release" -> releaseHeldSession()
                    "state" -> sayState()
                    "schema" -> setPendingSchema(intent.getStringExtra("id").orEmpty())
                    else -> say("result=error reason=unknown-op")
                }
            }.onFailure { say("result=error reason=threw detail=$it") }
            say("done op=$op")
            pending.finish()
        }.start()
    }

    /* ─────────────────────────── 匯出 ─────────────────────────── */

    private fun doExport(path: String) {
        if (!waitReady()) return
        val f = resolve(path)
        say("file=${f.absolutePath}")
        f.parentFile?.mkdirs()
        f.delete()
        val c = BackupController.get(app)
        // ⚠ 上一次的結果會擋住下一次(controller 是行程單例),先清掉。
        onMain { c.dismissResult() }
        Thread.sleep(200)
        onMain { c.export(Uri.fromFile(f)) }
        val r = awaitResult(c) ?: return
        say("result=${if (r.ok) "ok" else "fail"} bytes=${f.length()} message=${oneLine(r.message)}")
        r.notes.forEach { say("note=${oneLine(it)}") }
        // manifest 的 flushed 欄位是「這份詞庫證明得了自己是完整的嗎」。
        // 把它印出來 —— 匯出成功但 flushed=false 是一個必須被看見的差別。
        say("manifest=${oneLine(readManifest(f))}")
    }

    /** 直接把容器裡的 manifest 撈出來看，不經過任何我們自己的解析。 */
    private fun readManifest(zip: File): String = runCatching {
        java.util.zip.ZipFile(zip).use { z ->
            val e = z.entries().toList().firstOrNull { it.name.endsWith("-backup.json") }
                ?: return "no-manifest"
            z.getInputStream(e).bufferedReader().readText()
        }
    }.getOrElse { "unreadable: $it" }

    /* ─────────────────────────── 匯入 ─────────────────────────── */

    private fun doImport(path: String) {
        if (!waitReady()) return
        val f = resolve(path)
        say("file=${f.absolutePath}")
        if (!f.isFile) {
            say("result=error reason=no-such-file")
            return
        }
        val c = BackupController.get(app)
        onMain { c.dismissResult() }
        Thread.sleep(200)
        onMain {
            c.askImport(Uri.fromFile(f))
            c.confirmImport()
        }
        val r = awaitResult(c) ?: return
        say("result=${if (r.ok) "ok" else "fail"} message=${oneLine(r.message)}")
        r.notes.forEach { say("note=${oneLine(it)}") }
    }

    private fun awaitResult(c: BackupController): BackupController.Result? {
        val deadline = System.currentTimeMillis() + 180_000
        while (System.currentTimeMillis() < deadline) {
            c.result?.let { return it }
            Thread.sleep(250)
        }
        say("result=error reason=timeout stage=${c.stage}")
        return null
    }

    /* ─────────────────────────── 教它一個詞 ─────────────────────────── */

    /**
     * 打一串鍵、挑第 [pick] 個候選上屏 —— 也就是**讓 librime 學到一個詞**。
     *
     * ══════════════════════════════════════════════════════════════════
     *  ⚠ session 刻意**不銷毀**。整個驗證的意義就在這一行
     * ══════════════════════════════════════════════════════════════════
     *
     * `Memory::OnCommit` 在上屏之後開一個交易才把學到的詞寫進去，而那個交易
     * 住在記憶體裡的 `leveldb::WriteBatch`，要等 `FinishSession()` 或
     * `~UserDictionary` 才落地。session 一銷毀就落地了 —— 那時候再匯出，
     * 就算匯出端完全沒有 flush 也會通過，**測試會綠，而缺陷還在**。
     *
     * 所以這裡把 session 存進 [held]，跨 broadcast 活著，重現真實情況：
     * 使用者剛在別的 app 打完字（IME 的 session 還活著），才切過來按匯出。
     */
    private fun doLearn(schema: String?, keys: String, pick: Int) {
        if (!waitReady()) return
        val s = heldSession(schema) ?: return
        val committed = onMain {
            RimeCore.clearComposition(s)
            keys.forEach { ch -> RimeCore.processKey(s, ch.code) }
            val before = RimeCore.snapshot(s)?.menu?.candidates.orEmpty()
            val want = before.getOrNull(pick)?.text
            RimeCore.selectCandidate(s, pick)
            val after = RimeCore.snapshot(s)
            (after?.commitText ?: want).orEmpty()
        }
        say("result=ok committed=$committed keys=$keys pick=$pick")
    }

    /** 跨 broadcast 活著的那個 session。見 [doLearn]。 */
    private fun heldSession(schema: String?): Long? {
        synchronized(LOCK) {
            if (held == RimeCore.INVALID_SESSION || !onMain { RimeCore.sessionAlive(held) }) {
                held = onMain { RimeCore.sessionCreate() }
                if (held == RimeCore.INVALID_SESSION) {
                    say("result=error reason=no-session detail=${RimeCore.lastError()}")
                    return null
                }
                if (!schema.isNullOrBlank()) {
                    val ok = onMain { RimeCore.selectSchema(held, schema) }
                    say("schema=$schema selected=$ok")
                }
            }
            return held
        }
    }

    private fun releaseHeldSession() {
        synchronized(LOCK) {
            if (held != RimeCore.INVALID_SESSION) {
                onMain { RimeCore.sessionDestroy(held) }
                held = RimeCore.INVALID_SESSION
            }
        }
        say("result=ok released=true")
    }

    /* ─────────────────────────── 候選探針 ─────────────────────────── */

    /**
     * 打一串 ASCII 鍵，印出候選清單。
     *
     * ⚠ **session 的生死一律在主執行緒**（librime 的 `Service::sessions_`
     * 沒有鎖，見 `store/UserDbSnapshot.kt`）。這裡照做。
     */
    private fun doProbe(schema: String?, keys: String, top: Int) {
        if (!waitReady()) return
        val session = onMain { RimeCore.sessionCreate() }
        if (session == RimeCore.INVALID_SESSION) {
            say("result=error reason=no-session detail=${RimeCore.lastError()}")
            return
        }
        try {
            onMain {
                if (!schema.isNullOrBlank()) RimeCore.selectSchema(session, schema)
                RimeCore.clearComposition(session)
                // X11 的 ASCII keysym 就等於 ASCII 碼。
                keys.forEach { ch -> RimeCore.processKey(session, ch.code) }
            }
            val snap = onMain { RimeCore.snapshot(session) }
            val cands = snap?.menu?.candidates.orEmpty().take(top)
            say("result=ok count=${cands.size}")
            cands.forEachIndexed { i, cand -> say("cand[$i]=${cand.text}") }
        } finally {
            onMain { RimeCore.clearComposition(session); RimeCore.sessionDestroy(session) }
        }
    }

    /**
     * 請 IME 下次醒來切到某個方案。
     *
     * 走的是市集與 IME 之間**既有的**交接點（`StoreSettings.pendingSchema`），
     * 不另闢一條測試專用的路 —— 測試用的路徑會與正式路徑分岔，而分岔的
     * 那一天測試仍然是綠的。
     *
     * 為什麼驗證需要它：全新安裝之後 IME 落在 `t9_pinyin`，而 T9 在實體
     * 鍵盤上打 `nihao` 不會組字（它要的是數字鍵）。那不是這條線的缺陷，
     * 但會讓「教它幾個詞」這一步靜靜地什麼都沒教到。
     */
    private fun setPendingSchema(id: String) {
        if (id.isBlank()) {
            say("result=error reason=no-id")
            return
        }
        org.luminakey.ime.store.StoreSettings(app).pendingSchema = id
        say("result=ok pending=$id")
    }

    private fun sayState() {
        say(
            "ready=${RimeRuntime.isReady} phase=${RimeRuntime.phase} " +
                "userdir=${RimeRuntime.userDirOrNull}"
        )
        RimeRuntime.userDirOrNull?.listFiles()
            ?.filter { it.isDirectory && it.name.endsWith(".userdb") }
            ?.forEach { d ->
                val files = d.listFiles().orEmpty()
                say("userdb=${d.name} files=${files.size} bytes=${files.sumOf { it.length() }}")
            }
    }

    /* ─────────────────────────── 雜項 ─────────────────────────── */

    private fun waitReady(): Boolean {
        val deadline = System.currentTimeMillis() + 180_000
        while (System.currentTimeMillis() < deadline) {
            if (RimeRuntime.isReady) return true
            if (RimeRuntime.phase == RimeRuntime.Phase.FAILED) {
                say("result=error reason=rime-failed detail=${RimeRuntime.initError}")
                return false
            }
            Thread.sleep(250)
        }
        say("result=error reason=rime-not-ready phase=${RimeRuntime.phase}")
        return false
    }

    /** 在主執行緒上跑一段並等它跑完。 */
    private fun <T> onMain(block: () -> T): T {
        if (Looper.myLooper() == Looper.getMainLooper()) return block()
        val latch = java.util.concurrent.CountDownLatch(1)
        val out = java.util.concurrent.atomic.AtomicReference<T>()
        val err = java.util.concurrent.atomic.AtomicReference<Throwable>()
        Handler(Looper.getMainLooper()).post {
            try {
                out.set(block())
            } catch (t: Throwable) {
                err.set(t)
            } finally {
                latch.countDown()
            }
        }
        latch.await()
        err.get()?.let { throw it }
        @Suppress("UNCHECKED_CAST")
        return out.get() as T
    }

    /**
     * 相對路徑一律落在 `getExternalFilesDir()` 底下。
     *
     * ⚠ 不要直接寫 `/sdcard/Android/data/<pkg>/files/…`:那個目錄要等
     * `getExternalFilesDir()` 被呼叫過才存在,app 自己 `mkdirs()` 建不出來
     * (ENOENT)。失敗的長相是「匯出失敗:開不了檔」,而檔名看起來完全正常。
     */
    private fun resolve(path: String): File =
        if (path.startsWith("/")) File(path)
        else File(app.getExternalFilesDir(null) ?: app.cacheDir, path)

    private fun oneLine(s: String): String = s.replace("\n", " ⏎ ")

    private fun say(line: String) = Log.i(TAG, line)

    companion object {
        const val TAG = "BACKUPRT"

        private val LOCK = Any()

        /**
         * 見 [doLearn]：跨 broadcast 活著的 session，未落地的使用者詞典交易
         * 就掛在它身上。**不要**在 op 結束時順手把它收掉。
         */
        @Volatile
        private var held: Long = RimeCore.INVALID_SESSION
    }
}
