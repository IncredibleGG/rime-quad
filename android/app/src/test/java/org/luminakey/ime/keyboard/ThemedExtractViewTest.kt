package org.luminakey.ime.keyboard

import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * 橫屏那一條 extract 輸入條，守的是**框架用 id 找人**這件事。
 *
 * 為什麼要一支測試守一個 id：`InputMethodService.setExtractView()` 拿到我們回傳
 * 的 View 之後，會用 `com.android.internal.R.id.inputExtractEditText`（= 公開的
 * `android.R.id.inputExtractEditText`）去 `findViewById`，接著**無條件**呼叫
 * `mExtractEditText.setIME(this)`。找不到就是 NPE —— 而那個 NPE 只有在
 * **橫屏、而且宿主沒有帶 IME_FLAG_NO_FULLSCREEN** 的時候才會發生。
 * 換句話說：改壞了它，直屏的每一條測試、每一次手動操作都還是綠的。
 *
 * 這裡沒有辦法真的建出那個 View（本模組沒有 Robolectric，
 * `unitTests.isReturnDefaultValues` 讓 android.jar 的方法回預設值），
 * 所以掃的是原始碼。真正跑起來的證據在 emulator-5558 的橫屏截圖，
 * 見這一輪的 commit 訊息。
 */
class ThemedExtractViewTest {

    @Test
    fun `框架找得到的那個 id 與型別都在`() {
        val problems = problems(source())
        assertTrue(
            "extract 那一條接不上框架：\n  " + problems.joinToString("\n  "),
            problems.isEmpty(),
        )
    }

    /** 反向：把它改壞的每一種寫法都必須被抓到。 */
    @Test
    fun `改壞了會被抓到`() {
        val src = source()
        val mutations = listOf(
            "id 改成自己的" to
                src.replace("id = android.R.id.inputExtractEditText", "id = 12345"),
            "型別換成一般的 EditText（框架會在 setIME 那一行 ClassCastException）" to
                src.replace("ExtractEditText(context)", "android.widget.EditText(context)"),
            "建出來但沒有加進 view 樹（findViewById 找不到）" to
                src.replace("            editText,\n", "            View(context),\n"),
        )
        for ((name, mutated) in mutations) {
            assertTrue("植入失敗（錨點對不上）：$name", mutated != src)
            assertTrue("拆法「$name」沒有被抓到", problems(mutated).isNotEmpty())
        }
    }

    private fun source(): String {
        val f = File("src/main/java/org/luminakey/ime/keyboard/ThemedExtractView.kt")
        return f.takeIf { it.isFile }?.readText(Charsets.UTF_8)
            ?: error("找不到 ${f.path} —— 單元測試的工作目錄應該是 android/app")
    }

    private fun problems(src: String): List<String> {
        val out = mutableListOf<String>()
        if (!src.contains("id = android.R.id.inputExtractEditText")) {
            out += "沒有把 android.R.id.inputExtractEditText 設上去 —— " +
                "框架的 findViewById 會回 null，setIME() 當場 NPE"
        }
        if (!src.contains("ExtractEditText(context)")) {
            out += "那個 View 不是 android.inputmethodservice.ExtractEditText —— " +
                "框架會 cast 它去呼叫 setIME()"
        }
        if (!Regex("""addView\(\s*\n?\s*editText,""").containsMatchIn(src)) {
            out += "editText 沒有被加進 view 樹，findViewById 找不到它"
        }
        return out
    }
}
