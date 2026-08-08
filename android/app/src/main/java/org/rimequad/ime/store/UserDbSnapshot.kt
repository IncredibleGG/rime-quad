package org.rimequad.ime.store

import android.os.Handler
import android.os.Looper
import android.util.Log
import org.rimequad.ime.core.RimeCore
import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

/**
 * 取得**一致**的使用者詞典快照。這是整個備份功能裡唯一有陷阱的地方。
 *
 * ══════════════════════════════════════════════════════════════════════
 *  ⚠ 執行中直接複製 `*.userdb/` 會拿到不完整的資料，而且不會有任何錯誤
 * ══════════════════════════════════════════════════════════════════════
 *
 * librime 的使用者詞典是 LevelDB，但真正的問題不在 LevelDB 的檔案格式，
 * 在 librime 用它的方式 —— 讀 librime 原始碼可以逐行確認：
 *
 *   1. `Memory::OnCommit`（`src/rime/gear/memory.cc`）在使用者上屏之後
 *      先呼叫 `StartSession()` → `UserDictionary::NewTransaction()`
 *      **開一個交易**，接著才把剛學到的詞寫進去。
 *   2. `LevelDb::Update(key, value, in_transaction=true)` 把它塞進一個
 *      **記憶體裡的 `leveldb::WriteBatch`**，一個位元組都還沒碰到磁碟。
 *   3. 那個 batch 要等到下一次 `FinishSession()`（下一次 `Query`，或者
 *      `OnUnhandledKey` 收到一顆沒人處理的鍵）或者 `~UserDictionary`
 *      才會 `CommitTransaction()` 寫下去。
 *
 * 也就是說：**使用者「剛剛打的那些字」通常只存在於記憶體。** 這時候複製
 * 目錄，複製到的是一份「上一輪」的詞庫。它能開、能用、大小也差不多，
 * 只是少了最近的學習成果 —— 沒有例外、沒有警告、沒有 log。
 * 這正是題目說的「匯入之後詞庫少了一截」。
 *
 * ── 怎麼把它逼出來（只用現有的 ABI）─────────────────────────────────────
 *
 * `rime_shell.h` 目前**沒有**暴露 `RimeSyncUserData()`（librime 自己的正解，
 * 見下方「還缺什麼」）。但有一條路，只用 `rs_session_create()` 與
 * `rs_session_destroy()` 就走得通，關鍵是 librime 的兩個實作細節：
 *
 *   · `UserDictionaryComponent` 有一個 `hash_map<string, weak<Db>> db_pool_`
 *     （`src/rime/dict/user_dictionary.cc`）。**同一本詞典在整個行程裡
 *     只有一個 `Db` 物件**，那個未提交的 WriteBatch 就住在它裡面。
 *     所以任何人提交它，等於替所有人提交。
 *   · `~UserDictionary` 會呼叫 `CommitPendingTransaction()`。
 *
 * 於是：**建立一個 session、立刻銷毀它**，就會把 IME 那一側掛著的交易
 * 一起寫下去。而且這條路徑刻意不碰任何會留下痕跡的東西：
 *
 *   · `Session::Session()` → `ConcreteEngine::InitializeComponents()`，
 *     在 `schema_id == ".default"` 時走 `switcher_->CreateSchema()` 並
 *     **直接 `schema_.reset(...)`**，**不經過 `ApplySchema()`**。
 *     因此不會呼叫 `Switcher::SetActiveSchema()`，也就
 *     **不會寫 `user.yaml` 的 `var/schema_access_time`、
 *     不會改 `var/previously_selected_schema`**。
 *     使用者的鍵盤下次開起來還是同一個方案，匯出沒有副作用。
 *     （相對的：用 `rs_select_schema()` 逐一切換去 flush 每一本詞典
 *     **會**寫那兩個值 —— 那等於為了備份而在使用者的機器上多記一筆
 *     「你何時用過哪個方案」，與這個 App 的定位直接牴觸。所以不那樣做。）
 *   · 只有**當前方案**的那一本詞典可能掛著未提交的交易：IME 換方案時
 *     舊的 `Memory` 就被銷毀了，那時已經提交過。所以一個 session 夠了。
 *
 * ── 為什麼在主執行緒上做 ────────────────────────────────────────────────
 * `rime_shell.h` 的約定是「同一 session 的呼叫要序列化在同一條執行緒」，
 * 但 librime 的 `Service::sessions_` 是一個**沒有鎖**的 map
 * （`src/rime/service.cc`）。在背景執行緒建 session，會與 IME 主執行緒
 * 的 `GetSession()` 同時動同一個容器。行動端唯一安全的做法是：
 * **所有 session 的生死都在主執行緒**。所以這裡 post 過去再等。
 *
 * ── 還缺什麼（已寫進 docs/coordination.md §5）──────────────────────────
 * 正解是 librime 的 `RimeSyncUserData()`：它會呼叫
 * `UserDictManager::Backup()` → `LevelDb::Backup()` → 寫出
 * **`<name>.userdb.txt` 純文字快照**。那份文字檔才是四端該互通的東西
 * （`UserDictManager::Restore()` 會**合併**而不是覆蓋，跨版本、跨 db 實作
 * 都成立，而且人看得懂）。`rime_shell.h` 沒有這個進入點，而 `core/` 的
 * ABI 歸協調端，本支線不自己加。在那之前 Android 只能匯出
 * `leveldb-dir`，規範 §3.2 已經把兩種載體都定義好了。
 */
object UserDbSnapshot {

    private const val TAG = "UserDbSnapshot"

    /** 主執行緒被佔住時的等待上限。等不到就記 `flushed=false`，不阻塞匯出。 */
    private const val FLUSH_TIMEOUT_MS = 5_000L

    /** 目錄「看起來不再變動」要重試幾次。 */
    private const val COPY_ATTEMPTS = 3

    sealed class Flush {
        /** 已經走完 建立 → 銷毀，未提交的交易確定寫下去了。 */
        object Committed : Flush()

        /** 沒能做到。[reason] 是開發者用的英文回退，不上畫面。 */
        data class Skipped(val reason: String) : Flush()

        val ok: Boolean get() = this is Committed
    }

    /**
     * 讓 librime 把掛著的使用者詞典交易寫到磁碟。可從任意執行緒呼叫。
     *
     * 回傳 [Flush.Skipped] **不代表資料一定不完整**，只代表我們沒能證明它完整；
     * 這個事實會被記進 manifest 的 `flushed` 欄位，而不是被吞掉。
     */
    fun flushEngine(): Flush {
        if (!RimeCore.isInitialized) return Flush.Skipped("librime not initialized")

        val main = Handler(Looper.getMainLooper())
        val latch = CountDownLatch(1)
        // 跨執行緒交棒，所以不是普通的區域變數（Kotlin 的區域變數不能標
        // @Volatile，而 AtomicReference 本身就帶著需要的記憶體可見性）。
        val outcome = AtomicReference<Flush>(
            Flush.Skipped("main thread did not run the flush")
        )

        val work = Runnable {
            outcome.set(
                try {
                    val s = RimeCore.sessionCreate()
                    if (s == RimeCore.INVALID_SESSION) {
                        // 部署中 librime 會拒發 session（Service::disabled()）。
                        Flush.Skipped("rs_session_create returned 0: ${RimeCore.lastError()}")
                    } else {
                        RimeCore.sessionDestroy(s)
                        Flush.Committed
                    }
                } catch (t: Throwable) {
                    Flush.Skipped("flush threw: $t")
                }
            )
            latch.countDown()
        }

        if (Looper.myLooper() == Looper.getMainLooper()) {
            work.run()
        } else {
            main.post(work)
            if (!latch.await(FLUSH_TIMEOUT_MS, TimeUnit.MILLISECONDS)) {
                Log.w(TAG, "等不到主執行緒完成 flush（${FLUSH_TIMEOUT_MS}ms）")
                return Flush.Skipped("timed out waiting for the main thread")
            }
        }
        Log.i(TAG, "userdb flush: ${outcome.get()}")
        return outcome.get()
    }

    /* ─────────────────────────── 穩定副本 ─────────────────────────── */

    sealed class Copy {
        data class Ok(val dir: File, val files: Int, val bytes: Long) : Copy()

        /** 複製期間目錄一直在變（多半是 LevelDB 正在壓實，或使用者還在打字）。 */
        data class Unstable(val name: String, val attempts: Int) : Copy()

        data class Failed(val name: String, val message: String) : Copy()
    }

    /**
     * 把一個 `*.userdb/` 目錄複製到 [stagingParent] 底下，並確認複製期間
     * 目錄沒有變動。
     *
     * ── 為什麼要比對前後 ────────────────────────────────────────────────
     * 就算交易已經提交，LevelDB 隨時可能在背景做壓實（compaction）：
     * 產生新的 `.ldb`、刪掉舊的、換一份 `MANIFEST`、改寫 `CURRENT`。
     * 複製到一半發生這件事，拿到的會是一組**互相不對應**的檔案 ——
     * `CURRENT` 指向一份已經被刪掉的 MANIFEST，或 MANIFEST 列的 `.ldb`
     * 少了一個。那份備份平常看不出異狀，還原之後 LevelDB 直接開不起來。
     *
     * 所以複製前後各取一次指紋（檔名 + 大小 + 修改時間），不一致就整份丟掉
     * 重來；重試用完仍然不穩定就**明白地失敗**。
     * 這一條就是題目說的「不會有錯誤訊息」的解藥：與其產生一份壞掉的備份
     * 讓使用者三個月後才發現，不如當場說「現在複製不了，請稍後再試」。
     *
     * @param fingerprintOf 測試用的接縫。正式路徑一律用預設值 [fingerprint]；
     *   單元測試靠它模擬「目錄在複製途中被 LevelDB 動過」—— 那種時序在 JVM 測試裡
     *   重現不出來，而**這條防線正是整個匯出功能裡最不能默默失效的一段**
     *   （失效的長相是一份看起來正常、實際上開不起來的備份）。
     */
    fun copyStable(
        src: File,
        stagingParent: File,
        attempts: Int = COPY_ATTEMPTS,
        fingerprintOf: (File) -> List<String> = ::fingerprint,
    ): Copy {
        if (!src.isDirectory) return Copy.Failed(src.name, "not a directory")
        val dst = File(stagingParent, src.name)

        repeat(attempts) { attempt ->
            dst.deleteRecursively()
            if (!dst.mkdirs()) return Copy.Failed(src.name, "cannot create $dst")

            val before = fingerprintOf(src)
            var files = 0
            var bytes = 0L
            val failure = runCatching {
                src.listFiles()?.forEach { f ->
                    if (!f.isFile) return@forEach
                    if (!BackupPlan.userDbFileIncluded(f.name)) return@forEach
                    val target = File(dst, f.name)
                    f.inputStream().use { input ->
                        target.outputStream().use { out -> bytes += input.copyTo(out) }
                    }
                    files++
                }
            }.exceptionOrNull()

            if (failure != null) {
                dst.deleteRecursively()
                // 壓實剛好刪掉我們正要讀的檔案，長相就是 FileNotFoundException。
                // 那本身就是「目錄在動」的證據，所以當成不穩定重試，不是硬失敗。
                if (attempt == attempts - 1) {
                    return Copy.Failed(src.name, failure.message ?: failure.toString())
                }
                return@repeat
            }

            val after = fingerprintOf(src)
            if (before == after) return Copy.Ok(dst, files, bytes)
            Log.w(TAG, "${src.name} 在複製期間變動了，重試（第 ${attempt + 1} 次）")
        }
        dst.deleteRecursively()
        return Copy.Unstable(src.name, attempts)
    }

    /**
     * 檔名 + 大小 + 修改時間。刻意不算內容摘要 —— 那要多讀一遍整個目錄。
     *
     * `internal` 是為了讓單元測試能直接驗「檔案長大了、指紋就要不一樣」；
     * 那一條與上面的重試邏輯是兩件事，要分開驗。
     */
    internal fun fingerprint(dir: File): List<String> =
        dir.listFiles()
            ?.filter { it.isFile }
            ?.map { "${it.name} ${it.length()} ${it.lastModified()}" }
            ?.sorted()
            ?: emptyList()
}
