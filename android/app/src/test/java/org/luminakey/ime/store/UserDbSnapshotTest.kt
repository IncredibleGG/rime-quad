package org.luminakey.ime.store

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/**
 * 「複製途中目錄變動了」這條防線。
 *
 * ── 為什麼特別要有這一組 ────────────────────────────────────────────────
 * 這是整個匯出流程裡**失效之後最看不出來**的一段。LevelDB 隨時可能在背景
 * 壓實：產生新的 `.ldb`、刪掉舊的、換一份 `MANIFEST`、改寫 `CURRENT`。
 * 複製到一半撞上這件事，拿到的是一組互相不對應的檔案 —— 檔案數量正常、
 * 大小正常、zip 也打得開，只有真的拿去還原時 LevelDB 才會開不起來，
 * 而那通常是使用者換手機的那一天。
 *
 * 這一組是在做完變異測試（刻意把 `if (before == after)` 拿掉）之後補的：
 * 當時**43 條測試全綠**，等於這條防線完全沒有被守。
 *
 * ── 兩件事要分開驗 ──────────────────────────────────────────────────────
 *   1. 指紋本身抓不抓得到變動（[fingerprint] 是不是真的會不一樣）
 *   2. 抓到之後的處置對不對（重試、用完仍不穩就明白地失敗）
 * 合在一起驗的話，指紋壞掉與處置壞掉會互相掩護。
 */
class UserDbSnapshotTest {

    @get:Rule
    val tmp = TemporaryFolder()

    private fun db(parent: File, name: String = "luna_pinyin.userdb"): File {
        val dir = File(parent, name).apply { mkdirs() }
        File(dir, "CURRENT").writeText("MANIFEST-000002\n")
        File(dir, "000003.log").writeText("你好 c=1")
        File(dir, "LOCK").writeBytes(ByteArray(0))
        return dir
    }

    /* ── 1. 指紋抓得到變動嗎 ── */

    @Test
    fun `檔案長大了，指紋就不一樣`() {
        val src = db(tmp.newFolder("src"))
        val before = UserDbSnapshot.fingerprint(src)
        File(src, "000003.log").appendText("再見 c=1")
        assertNotEquals("內容變多了，指紋必須改變", before, UserDbSnapshot.fingerprint(src))
    }

    @Test
    fun `多一個檔案或少一個檔案，指紋都不一樣`() {
        val src = db(tmp.newFolder("src"))
        val before = UserDbSnapshot.fingerprint(src)

        File(src, "000007.ldb").writeText("new sst")
        val added = UserDbSnapshot.fingerprint(src)
        assertNotEquals("多一個 .ldb（壓實的產物）必須被看見", before, added)

        // 壓實同時會刪掉舊的。
        File(src, "000003.log").delete()
        assertNotEquals(added, UserDbSnapshot.fingerprint(src))
    }

    @Test
    fun `什麼都沒動的話指紋一樣`() {
        val src = db(tmp.newFolder("src"))
        assertEquals(UserDbSnapshot.fingerprint(src), UserDbSnapshot.fingerprint(src))
    }

    /* ── 2. 抓到之後的處置 ── */

    @Test
    fun `目錄穩定時複製成功且內容一致`() {
        val src = db(tmp.newFolder("src"))
        val staging = tmp.newFolder("staging")

        val r = UserDbSnapshot.copyStable(src, staging)
        assertTrue("穩定的目錄應該複製成功，實際是 $r", r is UserDbSnapshot.Copy.Ok)
        val ok = r as UserDbSnapshot.Copy.Ok
        assertEquals("LOCK 不該被複製", 2, ok.files)
        assertEquals("你好 c=1", File(ok.dir, "000003.log").readText())
        assertFalse(File(ok.dir, "LOCK").exists())
    }

    /**
     * 目錄一直在變 → 重試用完之後**明白地失敗**，而不是交出一份撕裂的副本。
     *
     * 這裡用 [UserDbSnapshot.copyStable] 的指紋接縫模擬「每次看都不一樣」，
     * 因為真實的壓實時序在 JVM 測試裡重現不出來，而**重現不出來不等於不會發生**
     * （這個專案在按鍵那條線上已經被同一句話咬過一次）。
     */
    @Test
    fun `目錄一直在變的話會明白地失敗，不會交出撕裂的副本`() {
        val src = db(tmp.newFolder("src"))
        val staging = tmp.newFolder("staging")
        var tick = 0

        val r = UserDbSnapshot.copyStable(src, staging, attempts = 3) {
            listOf("changing-${tick++}")   // 每一次都不一樣
        }
        assertTrue("必須是 Unstable，實際是 $r", r is UserDbSnapshot.Copy.Unstable)
        assertEquals(3, (r as UserDbSnapshot.Copy.Unstable).attempts)
        assertFalse(
            "失敗之後不可以留下半份副本 —— 它看起來會跟成功的一模一樣",
            File(staging, src.name).exists(),
        )
    }

    @Test
    fun `第一次不穩、第二次穩定時會重試成功`() {
        val src = db(tmp.newFolder("src"))
        val staging = tmp.newFolder("staging")
        var call = 0

        // 呼叫順序是 before, after, before, after…
        // 前兩次（第一輪）不同 → 判定不穩定；之後固定 → 第二輪成功。
        val r = UserDbSnapshot.copyStable(src, staging, attempts = 3) {
            call++
            if (call <= 2) listOf("round1-$call") else listOf("stable")
        }
        assertTrue("第二輪應該成功，實際是 $r", r is UserDbSnapshot.Copy.Ok)
        assertTrue(File(staging, src.name).isDirectory)
    }

    @Test
    fun `來源不是目錄時不會崩潰`() {
        val f = File(tmp.newFolder("x"), "not-a-dir.userdb").apply { writeText("x") }
        val r = UserDbSnapshot.copyStable(f, tmp.newFolder("staging"))
        assertTrue(r is UserDbSnapshot.Copy.Failed)
    }
}
