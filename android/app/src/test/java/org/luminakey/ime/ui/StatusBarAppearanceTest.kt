package org.luminakey.ime.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import javax.xml.parsers.DocumentBuilderFactory
import kotlin.math.pow

/**
 * 系統狀態列的圖示,在**兩種模式**下都要看得見。
 *
 * ── 這條測試記著什麼(走查 A6 的實測)──────────────────────────────────
 * 淺色模式下,app 的每一頁上方都是:底 `#F4F6F5`、時間／Wi-Fi／電量
 * `#FFFFFF`,對比 **1.09:1**(在 `a2_twostep.png` 的狀態列區域取樣算出來的)。
 * 使用者在這個 app 裡看不到自己手機的時間和電量。
 *
 * 成因是「沒有人宣告」:targetSdk 35 強制 edge-to-edge,狀態列疊在內容上,
 * 圖示深淺完全交給 app 決定;而 `android:windowLightStatusBar` 的預設值是
 * false(淺色圖示)。**「漏了宣告」與「宣告成 false」在畫面上一模一樣**,
 * 所以它不會有任何錯誤訊息。
 *
 * ── 為什麼要斷言「兩份相反」而不是各自的值 ──────────────────────────────
 * 因為真正的錯誤形態是**兩份寫成一樣**:有人只改了淺色那一份,深色就變成
 * 深底配深圖示 —— 修好一種模式、弄壞另一種。所以這裡對兩份各自斷言,
 * 並額外斷言它們相反。
 *
 * 順帶把 `ui/Theme.kt` 的那兩個底色也算一次:資源裡的那個布林講的是
 * 「底是淺的」,而底色是 Compose 那一側決定的 —— 兩邊分岔的話這條測試會紅。
 */
class StatusBarAppearanceTest {

    @Test
    fun `淺色主題宣告淺底狀態列`() {
        assertEquals(
            "values/themes.xml 沒有把狀態列圖示轉成深色。" +
                "底是 #F4F6F5,配上系統預設的白色圖示 = 1.09:1。",
            "true",
            attr("values", "android:windowLightStatusBar"),
        )
        assertEquals(
            "手勢列那一條同一個問題。",
            "true",
            attr("values", "android:windowLightNavigationBar"),
        )
    }

    @Test
    fun `深色主題宣告深底狀態列`() {
        assertEquals("false", attr("values-night", "android:windowLightStatusBar"))
        assertEquals("false", attr("values-night", "android:windowLightNavigationBar"))
    }

    @Test
    fun `兩份的值必須相反`() {
        for (name in listOf("android:windowLightStatusBar", "android:windowLightNavigationBar")) {
            val l = attr("values", name)
            val d = attr("values-night", name)
            assertTrue(
                "$name 在深淺兩份裡都是 $l —— 其中一種模式必定看不見",
                l != d,
            )
        }
    }

    /**
     * 資源裡那個布林說「底是淺的」,而底色由 `ui/Theme.kt` 決定。
     * 兩邊必須說同一件事,否則就是「宣告了淺底,畫出來是深底」。
     */
    @Test
    fun `宣告的深淺與 Theme_kt 的底色一致`() {
        val src = File("src/main/java/org/luminakey/ime/ui/Theme.kt").readText()
        val light = colorOf(src, "LightColors", "background")
        val dark = colorOf(src, "DarkColors", "background")
        assertTrue("淺色的 background ($light) 其實不淺", luminance(light) > 0.5)
        assertTrue("深色的 background ($dark) 其實不深", luminance(dark) < 0.2)
    }

    private companion object {

        fun attr(dir: String, name: String): String {
            val f = File("src/main/res/$dir/themes.xml")
            assertTrue("找不到 ${f.path}", f.isFile)
            val doc = DocumentBuilderFactory.newInstance().newDocumentBuilder().parse(f)
            val styles = doc.getElementsByTagName("style")
            for (i in 0 until styles.length) {
                val style = styles.item(i)
                if (style.attributes?.getNamedItem("name")?.nodeValue != "Theme.LuminaKey") continue
                val kids = style.childNodes
                for (j in 0 until kids.length) {
                    val item = kids.item(j)
                    if (item.nodeName != "item") continue
                    if (item.attributes?.getNamedItem("name")?.nodeValue == name) {
                        return item.textContent.orEmpty().trim()
                    }
                }
            }
            return "<missing>"
        }

        /** `background = Color(0xFFF4F6F5),` → 0xFFF4F6F5。 */
        fun colorOf(src: String, table: String, role: String): Int {
            val at = src.indexOf("private val $table")
            assertTrue("Theme.kt 裡找不到 $table —— 這條測試已經對不上實作", at >= 0)
            val end = src.indexOf(")\n", src.indexOf("(", at))
            val block = src.substring(at, if (end > at) src.indexOf("\n)", at) else src.length)
            val m = Regex("""$role\s*=\s*Color\(0x([0-9A-Fa-f]{8})\)""").find(block)
            assertTrue("$table 裡找不到 $role", m != null)
            return m!!.groupValues[1].toLong(16).toInt()
        }

        fun channel(v: Int): Double {
            val c = v / 255.0
            return if (c <= 0.03928) c / 12.92 else ((c + 0.055) / 1.055).pow(2.4)
        }

        fun luminance(argb: Int): Double =
            0.2126 * channel((argb shr 16) and 0xFF) +
                0.7152 * channel((argb shr 8) and 0xFF) +
                0.0722 * channel(argb and 0xFF)
    }
}
