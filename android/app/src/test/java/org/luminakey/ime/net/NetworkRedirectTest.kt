package org.luminakey.ime.net

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 轉址邊界的不變式。
 *
 * ── 這個檔案在守什麼 ────────────────────────────────────────────────────
 * 「使用者給的網址是哪一台，就只會連到哪一台。」
 *
 * 沒有這條規則的話，連網紀錄就只是「事後看得到」而不是「擋得住」：一份被改過
 * 的索引、或一台被接管的伺服器，可以用一個 302 把每一次下載都導去第三方，
 * 而使用者要等到翻紀錄才會發現。
 *
 * ── 為什麼不架一台會回 302 的測試伺服器 ─────────────────────────────────
 * 因為這個專案的規矩是**連測試都不准出現第二個連網出口**（見
 * scripts/audit_offline.sh 第 1 項，測試程式碼一起檢查）。所以判斷邏輯抽成
 * [NetworkGate.redirectAllowed] 這個純字串函式，這裡直接對它下斷言，
 * 一個 socket 都不用開。
 */
class NetworkRedirectTest {

    private fun allow(from: String, to: String) =
        assertTrue("應該允許 $from -> $to", NetworkGate.redirectAllowed(from, to))

    private fun deny(from: String, to: String) =
        assertFalse("應該擋下 $from -> $to", NetworkGate.redirectAllowed(from, to))

    /* ───────────────────── 同一台主機：跟 ───────────────────── */

    @Test
    fun `同主機同協定的轉址可以跟`() {
        allow("https://cdn.example/rime/index.json", "https://cdn.example/rime/v2/index.json")
        // R2 這類物件儲存常見的做法：換路徑不換主機。
        allow(
            "https://pub-abc.r2.dev/rime/schemas/ice.zip",
            "https://pub-abc.r2.dev/rime/schemas/2026/ice.zip",
        )
    }

    @Test
    fun `主機名大小寫不同仍算同一台`() {
        allow("https://CDN.Example/a", "https://cdn.example/b")
    }

    @Test
    fun `同主機換連接埠可以跟`() {
        // 同一台主機的另一個埠仍然是同一個經營者。刻意不比連接埠，
        // 理由寫在 NetworkGate.redirectAllowed 的註解裡。
        allow("http://127.0.0.1:8099/index.json", "http://127.0.0.1:9000/index.json")
    }

    @Test
    fun `http 升級成 https 可以跟`() {
        allow("http://cdn.example/a", "https://cdn.example/a")
    }

    /* ───────────────────── 換了主機：不跟 ───────────────────── */

    @Test
    fun `換主機一律擋下`() {
        deny("https://cdn.example/pkg.zip", "https://evil.example/pkg.zip")
        // 子網域也算換主機 —— 「同一個母網域」不是我們給過的保證。
        deny("https://cdn.example/pkg.zip", "https://tracker.cdn.example/pkg.zip")
        deny("https://cdn.example/pkg.zip", "https://cdn.example.evil/pkg.zip")
    }

    @Test
    fun `換成 IP 位址也算換主機`() {
        deny("https://cdn.example/a", "https://203.0.113.7/a")
    }

    /* ───────────────────── 協定降級：不跟 ───────────────────── */

    @Test
    fun `https 降級成 http 擋下`() {
        // 同一台主機也不行。降級之後內容是明文，中間人可以直接改包。
        deny("https://cdn.example/pkg.zip", "http://cdn.example/pkg.zip")
    }

    @Test
    fun `轉去非 http 協定一律擋下`() {
        deny("https://cdn.example/a", "file:///etc/passwd")
        deny("https://cdn.example/a", "ftp://cdn.example/a")
        deny("https://cdn.example/a", "content://com.other.app/x")
        deny("https://cdn.example/a", "intent://cdn.example/#Intent;end")
        deny("https://cdn.example/a", "javascript:alert(1)")
    }

    /* ───────────────────── 壞輸入：不跟（fail-closed）───────────────── */

    @Test
    fun `解不開的網址一律當成不可以跟`() {
        // 這裡的預設值必須是「擋下」。解析失敗時放行是 fail-open，
        // 而 fail-open 是這個專案最不能接受的失敗方式。
        deny("https://cdn.example/a", "這不是網址")
        deny("這不是網址", "https://cdn.example/a")
        deny("https://cdn.example/a", "")
        deny("", "")
        deny("https://cdn.example/a", "///a")
        deny("https:///a", "https:///a")
    }

    /* ─────────────── 這條規則**沒有**擋住什麼（寫成測試免得被誤讀）────── */

    @Test
    fun `最初就指向第三方的網址不在這條規則的守備範圍`() {
        // 索引的 base_url 與套件的 file 都是遠端給的。若一開始那個網址就是
        // 第三方，這裡看到的「原主機」已經是第三方了，於是它「同主機」。
        // 這不是漏洞，是分工：那條路要靠索引簽章擋，見
        // docs/offline-threat-model.md §5。寫成測試是為了讓下一個讀的人
        // 不會以為這條規則保證了它沒保證的事。
        allow("https://evil.example/pkg.zip", "https://evil.example/other.zip")
    }
}
