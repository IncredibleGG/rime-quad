package org.luminakey.ime.update

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 「線上那一版還是不是同一個 app」，以及 `version.json` 對新欄位的容忍度。
 *
 * ── 這一支守的是什麼 ────────────────────────────────────────────────────
 * 2026-08-09 使用者按下安裝，拿到「APK 檔案無效或已損毀」。檔案沒有壞：
 * 產品改名換了 applicationId，而 Android 不允許覆蓋安裝。兩件事要被釘住：
 *
 *   1. 這件事在**下載之前**判定得出來 —— 所以判定本身是純函式，這裡直接打；
 *   2. **舊的 version.json 沒有新欄位，而那必須完全無害。** 加一個必填欄位
 *      的下場是：所有裝著舊版的人從此再也檢查不到更新，而且畫面上寫的是
 *      「版本資訊格式錯誤」。那比原本的缺陷更糟。
 */
class PackageIdentityTest {

    private val manifestUrl = "https://example.invalid/rime/version.json"
    private val sha = "7856b19a5f10a6acb8c7cb485f55b2a9343348e01f620c73dd52af826ff7412a"

    private fun json(extra: String = ""): String = """
        {
          "version_code": 26080912,
          "version_name": "0.1.0-dev+26080912.abc1234",
          "size": 27965997,
          "sha256": "$sha",
          "url": "https://example.invalid/rime/rime-android-26080912.apk",
        $extra  "commit": "abc1234"
        }
    """.trimIndent()

    private fun ok(text: String): VersionManifest {
        val r = VersionManifestParser.parse(text, manifestUrl)
        assertTrue("預期解析成功，實際：$r", r is ManifestParseResult.Ok)
        return (r as ManifestParseResult.Ok).manifest
    }

    /* ═════════════════ 1. 舊的 version.json 必須照常運作 ═════════════════ */

    @Test
    fun `沒有 package 欄位不算錯誤`() {
        val m = ok(json())
        assertNull("舊格式沒有這個欄位，解析不可以失敗，也不可以自己編一個", m.packageId)
        assertEquals(emptyList<String>(), m.replacesPackages)
        assertNull(m.pageUrl)
    }

    /**
     * 缺欄位 = 不知道，**不是**「一樣」。
     *
     * 這一條看起來像在測一個 enum，其實它釘的是一個決定：不知道的時候要
     * 由呼叫端決定怎麼辦（我們的答案是「照常下載，但安裝前直接讀 APK」），
     * 而不是在這裡假設「大概一樣吧」。
     */
    @Test
    fun `缺欄位是 UNKNOWN 而不是 SAME`() {
        assertEquals(
            PackageIdentity.Verdict.UNKNOWN,
            PackageIdentity.compare("org.rimequad.ime", ok(json())),
        )
    }

    /* ═════════════════ 2. 有欄位時真的判得出來 ═════════════════ */

    @Test
    fun `套件名不同就是 CHANGED`() {
        val m = ok(json("""  "package": "org.luminakey.ime",${'\n'}"""))
        assertEquals("org.luminakey.ime", m.packageId)
        assertEquals(
            PackageIdentity.Verdict.CHANGED,
            PackageIdentity.compare("org.rimequad.ime", m),
        )
    }

    @Test
    fun `套件名相同就是 SAME`() {
        val m = ok(json("""  "package": "org.luminakey.ime",${'\n'}"""))
        assertEquals(
            PackageIdentity.Verdict.SAME,
            PackageIdentity.compare("org.luminakey.ime", m),
        )
    }

    /**
     * `replaces_package` 分得出「我們自己改名」與「這份版本資訊根本不是給
     * 這個 app 的」。兩者要對使用者說的話完全不同，所以不可以混。
     */
    @Test
    fun `replaces_package 認得字串也認得陣列`() {
        val one = ok(
            json(
                """  "package": "org.luminakey.ime",""" + "\n" +
                    """  "replaces_package": "org.rimequad.ime",""" + "\n"
            )
        )
        assertTrue(PackageIdentity.declaresReplacing("org.rimequad.ime", one))

        val many = ok(
            json(
                """  "package": "org.luminakey.ime",""" + "\n" +
                    """  "replaces_package": ["org.old.one", "org.rimequad.ime"],""" + "\n"
            )
        )
        assertEquals(listOf("org.old.one", "org.rimequad.ime"), many.replacesPackages)
        assertTrue(PackageIdentity.declaresReplacing("org.rimequad.ime", many))
        assertFalse(PackageIdentity.declaresReplacing("org.someone.else", many))
    }

    /**
     * 垃圾值當成沒有，**不是**照收。
     *
     * 一個 `"package": "not a package name"` 進到比對裡，會讓「一不一樣」
     * 得出一個看起來確定、實際上沒有根據的答案 —— 而那個答案的後果是
     * 「不給使用者升級」或「叫他解除安裝」。缺席至少會退回讀 APK 那道防線。
     */
    @Test
    fun `不像套件名的值當成沒有`() {
        for (bad in listOf("\"not a package\"", "\"nodots\"", "\"\"", "42", "true")) {
            val m = ok(json("""  "package": $bad,""" + "\n"))
            assertNull("「$bad」不該被當成套件名", m.packageId)
        }
    }

    @Test
    fun `page_url 相對路徑以 version-json 自己的位置為基底`() {
        val m = ok(json("""  "page_url": "downloads/",""" + "\n"))
        assertEquals("https://example.invalid/rime/downloads/", m.pageUrl)
    }

    @Test
    fun `page_url 只收 http 與 https`() {
        val m = ok(json("""  "page_url": "javascript:alert(1)",""" + "\n"))
        assertNull("非 http(s) 的下載頁不可以進到一顆按鈕後面", m.pageUrl)
    }

    /* ═════════════════ 3. 從系統訊息裡撈套件名 ═════════════════ */

    /** 使用者 2026-08-09 回報的那一則，一字不改。 */
    @Test
    fun `撈得出使用者實際收到的那一則`() {
        val raw = "INSTALL_FAILED_INVALID_APK: android.content.pm.parsing.ApkLite@fff160d " +
            "specified package org.rimequad.ime inconsistent with org.luminakey.ime"
        assertEquals(
            "org.rimequad.ime" to "org.luminakey.ime",
            PackageIdentity.inconsistentPackages(raw),
        )
    }

    /**
     * 反向：認不出來時回 null，**不可以**丟例外、也不可以回一個湊出來的答案。
     * 這是在剖析一個沒有人承諾過的字串，AOSP 換一個字我們就撈不到。
     */
    @Test
    fun `認不出來時回 null`() {
        assertNull(PackageIdentity.inconsistentPackages(null))
        assertNull(PackageIdentity.inconsistentPackages(""))
        assertNull(PackageIdentity.inconsistentPackages("INSTALL_FAILED_INVALID_APK"))
        assertNull(PackageIdentity.inconsistentPackages("something else entirely"))
    }

    @Test
    fun `looksLikePackageName 擋掉空白與沒有點的值`() {
        assertTrue(PackageIdentity.looksLikePackageName("org.luminakey.ime"))
        assertTrue(PackageIdentity.looksLikePackageName("a.b"))
        assertFalse(PackageIdentity.looksLikePackageName(""))
        assertFalse(PackageIdentity.looksLikePackageName("   "))
        assertFalse(PackageIdentity.looksLikePackageName("nodots"))
        assertFalse(PackageIdentity.looksLikePackageName("org.lumina key.ime"))
        assertFalse(PackageIdentity.looksLikePackageName("org.luminakey.ime\n"))
        assertFalse(PackageIdentity.looksLikePackageName("1org.luminakey.ime"))
    }
}
