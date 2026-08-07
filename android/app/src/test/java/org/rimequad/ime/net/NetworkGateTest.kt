package org.rimequad.ime.net

import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.io.File

/**
 * 閘門的不變式。
 *
 * ── 這個檔案在守什麼 ────────────────────────────────────────────────────
 * 「開關關閉時真的不連網」是這個專案對使用者的承諾。承諾要有測試守著，
 * 否則下一次重構就會悄悄把它弄丟。
 *
 * ── 注意本檔自己也沒有任何連網程式碼 ────────────────────────────────────
 * 測「開啟之後真的會去連」的那一則，是呼叫 [NetworkGate] 去連
 * `127.0.0.1:1`（保證沒有人在聽，connect 立刻被拒）。測試自己不 import
 * `java.net`，也不開 socket —— 因為在這個專案裡，**唯一**能發出網路請求的
 * 地方就是 NetworkGate，測試也不例外。這一點同樣可以用檔頭那條 grep 驗證。
 */
class NetworkGateTest {

    private val recorded = ArrayList<NetworkLogEntry>()

    @Before
    fun setUp() {
        recorded.clear()
        // NetworkGate 是單例，測試之間會互相汙染。每一則都自己從
        // 「預設拒絕」開始，與 app 冷啟動時的狀態一致。
        NetworkGate.policy = NetworkGate.Policy { false }
        NetworkGate.recorder = NetworkGate.Recorder { recorded.add(it) }
    }

    @After
    fun tearDown() {
        NetworkGate.policy = NetworkGate.Policy { false }
        NetworkGate.recorder = null
    }

    /* ───────────────────────── 關閉時 ───────────────────────── */

    @Test(expected = NetworkDisabledException::class)
    fun `關閉時 requireEnabled 直接拋例外`() {
        NetworkGate.requireEnabled(NetworkPurpose.STORE_INDEX)
    }

    @Test
    fun `例外訊息說得出被拒絕的是哪一件事`() {
        val e = runCatching {
            NetworkGate.requireEnabled(NetworkPurpose.STORE_PACKAGE, "萬象")
        }.exceptionOrNull()
        assertTrue(e is NetworkDisabledException)
        val msg = e!!.message ?: ""
        assertTrue("訊息應說明是哪一種用途：$msg", msg.contains(NetworkPurpose.STORE_PACKAGE.zh))
        assertTrue("訊息應帶上標籤：$msg", msg.contains("萬象"))
    }

    @Test
    fun `關閉時 fetchText 回報 blocked 而不是網路錯誤`() {
        val r = NetworkGate.fetchText("http://127.0.0.1:1/index.json", NetworkPurpose.STORE_INDEX)
        assertTrue(r is NetworkGate.Result.Err)
        // blocked 與一般失敗必須分得出來：UI 要顯示的是一顆開啟開關的按鈕，
        // 不是「請檢查你的網路連線」。
        assertTrue((r as NetworkGate.Result.Err).blocked)
    }

    @Test
    fun `關閉時 download 不建立任何檔案`() {
        val dest = File.createTempFile("gate-test", ".bin").also { it.delete() }
        val r = NetworkGate.download(
            "http://127.0.0.1:1/pkg.zip", dest, 1024, NetworkPurpose.STORE_PACKAGE, "測試套件",
        )
        assertTrue(r is NetworkGate.Result.Err)
        assertTrue((r as NetworkGate.Result.Err).blocked)
        assertFalse("被拒絕的下載不該留下任何檔案", dest.exists())
    }

    @Test
    fun `關閉時不留下任何連網紀錄`() {
        NetworkGate.fetchText("http://127.0.0.1:1/index.json", NetworkPurpose.STORE_INDEX)
        NetworkGate.download(
            "http://127.0.0.1:1/a.zip",
            File.createTempFile("gate-test", ".bin").also { it.delete() },
            1024, NetworkPurpose.STORE_PACKAGE,
        )
        // 「開關從沒開過 → 連網紀錄是空的」是使用者驗證我們的方式。
        // 若把被擋下來的嘗試也記進去，那句話就不成立了。
        assertEquals(emptyList<NetworkLogEntry>(), recorded)
    }

    @Test
    fun `政策自己爆掉時一律視為關閉`() {
        // fail-closed：讀不到偏好、DataStore 壞掉、行程正在被回收 ——
        // 任何一種意外都不該變成「那就連網吧」。
        NetworkGate.policy = NetworkGate.Policy { throw IllegalStateException("偏好讀不到") }
        assertFalse(NetworkGate.isEnabled)
        val r = NetworkGate.fetchText("http://127.0.0.1:1/x", NetworkPurpose.UPDATE_MANIFEST)
        assertTrue((r as NetworkGate.Result.Err).blocked)
    }

    /* ───────────────────────── 開啟時 ───────────────────────── */

    @Test
    fun `開啟後真的會去連線，而且失敗也會留下紀錄`() {
        NetworkGate.policy = NetworkGate.Policy { true }
        // 127.0.0.1:1 保證沒有人在聽，connect 立刻被拒 —— 不需要起一台
        // 測試用的伺服器，也不會有任何封包離開這台機器。
        val r = NetworkGate.fetchText(
            "http://127.0.0.1:1/index.json", NetworkPurpose.STORE_INDEX, "本機",
        )
        assertTrue(r is NetworkGate.Result.Err)
        assertFalse("這是真的連不上，不是被開關擋下", (r as NetworkGate.Result.Err).blocked)

        assertEquals("一次嘗試應留下一筆紀錄", 1, recorded.size)
        val e = recorded[0]
        assertEquals("127.0.0.1", e.host)
        assertEquals(NetworkPurpose.STORE_INDEX, e.purpose)
        assertEquals(NetworkOutcome.FAILED, e.outcome)
        assertEquals("本機", e.label)
        assertTrue(e.atMillis > 0)
    }

    @Test
    fun `非 http 的網址在連線之前就被擋下，也不會留下紀錄`() {
        NetworkGate.policy = NetworkGate.Policy { true }
        val dest = File.createTempFile("gate-test", ".bin").also { it.delete() }
        val r = NetworkGate.download(
            "file:///etc/passwd", dest, 1024, NetworkPurpose.STORE_PACKAGE,
        )
        assertTrue(r is NetworkGate.Result.Err)
        assertFalse((r as NetworkGate.Result.Err).blocked)
        // 沒有連線發生 → 紀錄裡不該多一筆。紀錄的語義是「真的連了」。
        assertEquals(emptyList<NetworkLogEntry>(), recorded)
        assertFalse(dest.exists())
    }

    /* ───────────────────────── 小工具 ───────────────────────── */

    @Test
    fun `hostOf 只取主機名，不帶路徑`() {
        assertEquals("cdn.example", NetworkGate.hostOf("https://cdn.example/rime/index.json?a=b"))
        assertEquals("127.0.0.1", NetworkGate.hostOf("http://127.0.0.1:8099/index.json"))
        // 解不開時回原字串的前段，不回 null —— 紀錄裡寧可有一個看不懂的值，
        // 也不要有一個空白欄位讓人以為我們藏了什麼。
        assertEquals("不是網址", NetworkGate.hostOf("不是網址"))
    }
}
