package org.rimequad.ime.net

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.nio.file.Files

/**
 * 連網紀錄的儲存與編解碼。
 *
 * 這一份紀錄是「你自己看」的兌現物，所以它壞掉的方式必須被測到：
 * 一行壞資料不能把整份紀錄弄不見，遠端可控的字串不能把一行拆成兩行，
 * 清除必須真的清掉。
 */
class NetworkLogTest {

    private fun tmpDir(): File = Files.createTempDirectory("netlog").toFile()

    private fun entry(
        at: Long = 1_700_000_000_000L,
        host: String = "cdn.example",
        purpose: NetworkPurpose = NetworkPurpose.STORE_INDEX,
        label: String = "",
        outcome: NetworkOutcome = NetworkOutcome.OK,
        bytes: Long = 1234,
        detail: String = "",
    ) = NetworkLogEntry(at, host, purpose, label, outcome, bytes, detail)

    /* ───────────────────────── 編解碼 ───────────────────────── */

    @Test
    fun `一筆紀錄可以原樣繞一圈回來`() {
        val e = entry(label = "萬象", outcome = NetworkOutcome.FAILED, detail = "HTTP 404")
        assertEquals(e, NetworkLogCodec.decode(NetworkLogCodec.encode(e)))
    }

    @Test
    fun `遠端可控的字串不能把一行拆成兩行`() {
        // host 來自轉址目的地，也就是**遠端說了算**。若它能塞進換行，
        // 就能在紀錄裡偽造出一筆「看起來很無害」的假紀錄。
        val e = entry(host = "evil\n1970-01-01\tgood.example", detail = "a\tb\nc")
        val line = NetworkLogCodec.encode(e)
        assertEquals("編碼結果必須是單獨一行", 1, line.lines().size)
        val back = NetworkLogCodec.decode(line)!!
        assertFalse(back.host.contains("\n"))
        assertFalse(back.host.contains("\t"))
        assertFalse(back.detail.contains("\n"))
    }

    @Test
    fun `過長的欄位會被截斷`() {
        val e = entry(host = "h".repeat(500), detail = "d".repeat(500), label = "l".repeat(500))
        val back = NetworkLogCodec.decode(NetworkLogCodec.encode(e))!!
        assertEquals(NetworkLogCodec.MAX_HOST, back.host.length)
        assertEquals(NetworkLogCodec.MAX_DETAIL, back.detail.length)
        assertEquals(NetworkLogCodec.MAX_LABEL, back.label.length)
    }

    @Test
    fun `解不開的行回 null 而不是丟例外`() {
        assertNull(NetworkLogCodec.decode(""))
        assertNull(NetworkLogCodec.decode("欄位不夠"))
        assertNull(NetworkLogCodec.decode("x\tcdn\tSTORE_INDEX\t\tOK\t1\t")) // 時間不是數字
        assertNull(NetworkLogCodec.decode("1\tcdn\tNOT_A_PURPOSE\t\tOK\t1\t"))
        assertNull(NetworkLogCodec.decode("1\tcdn\tSTORE_INDEX\t\tNOPE\t1\t"))
    }

    /* ───────────────────────── 儲存 ───────────────────────── */

    @Test
    fun `沒有連過網就是空清單，而且不會建立檔案`() {
        val dir = tmpDir()
        val f = File(dir, "connections.tsv")
        val store = NetworkLogStore(f)
        assertEquals(emptyList<NetworkLogEntry>(), store.read())
        assertFalse("只是讀不該把檔案生出來", f.exists())
    }

    @Test
    fun `寫進去讀得回來，順序由舊到新`() {
        val store = NetworkLogStore(File(tmpDir(), "connections.tsv"))
        store.append(entry(at = 100, host = "a.example"))
        store.append(entry(at = 200, host = "b.example"))
        val all = store.read()
        assertEquals(listOf("a.example", "b.example"), all.map { it.host })
    }

    @Test
    fun `超過上限時丟掉最舊的`() {
        val store = NetworkLogStore(File(tmpDir(), "connections.tsv"), maxEntries = 3)
        (1..5).forEach { store.append(entry(at = it.toLong(), host = "h$it")) }
        assertEquals(listOf("h3", "h4", "h5"), store.read().map { it.host })
    }

    @Test
    fun `清除是真的把檔案刪掉`() {
        val dir = tmpDir()
        val f = File(dir, "connections.tsv")
        val store = NetworkLogStore(f)
        store.append(entry())
        assertTrue(f.isFile)
        store.clear()
        assertFalse(f.exists())
        assertEquals(emptyList<NetworkLogEntry>(), store.read())
    }

    @Test
    fun `一行壞資料不會把整份紀錄拖垮`() {
        val f = File(tmpDir(), "connections.tsv")
        f.parentFile?.mkdirs()
        f.writeText(
            NetworkLogCodec.encode(entry(at = 1, host = "good1.example")) + "\n" +
                "這一行是垃圾\n" +
                NetworkLogCodec.encode(entry(at = 2, host = "good2.example")) + "\n"
        )
        val store = NetworkLogStore(f)
        assertEquals(listOf("good1.example", "good2.example"), store.read().map { it.host })
    }

    /* ───────────────────────── 顯示 ───────────────────────── */

    @Test
    fun `原因是給人看的，不是列舉名`() {
        assertEquals("下載方案套件：萬象", entry(purpose = NetworkPurpose.STORE_PACKAGE, label = "萬象").reasonText)
        assertEquals("瀏覽方案市集（取索引）", entry().reasonText)
    }
}
