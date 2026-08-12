package org.luminakey.ime.keyboard

import android.content.Context
import android.graphics.Color
import android.inputmethodservice.ExtractEditText
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import org.luminakey.ime.theme.Theme

/**
 * 橫屏全螢幕 extract 模式那一條輸入條 —— **由我們自己畫，吃 core/themes**。
 *
 * ── 這個檔案存在的理由 ──────────────────────────────────────────────────
 * 橫屏時 `InputMethodService` 會進 extract 模式：宿主的編輯框被鍵盤蓋住了，
 * 所以系統在鍵盤上方另外畫一個輸入框，把宿主的文字整段拉出來。那是**必要的
 * 補償** —— 實測 emulator-5558 轉橫屏，鍵盤佔掉 851/1080 px，宿主縮排之後
 * 連一個輸入框都排不下。
 *
 * 但系統畫的那一條長得不像這個 app：`ExtractEditText` + 一顆原生 `Done` 鈕，
 * 底是 `?android:attr/inputMethodFullscreenBackground` 的系統漸層，
 * 而 `core/themes` 的任何一個欄位它都不看（G46 抱怨的就是這一件）。
 *
 * `onCreateExtractTextView()` 是官方的接縫：回傳的 View 會被
 * `InputMethodService.setExtractView()` 塞進 extract 區，框架接著用
 * **id 找人**：`android.R.id.inputExtractEditText` 必須存在、而且必須是一個
 * `android.inputmethodservice.ExtractEditText`（框架會對它呼叫 `setIME()`）。
 *
 * ⚠ 那個 id 不能改名、不能包在別的 class 裡。找不到的話框架會
 *   `mExtractEditText.setIME(this)` 直接 NPE，輸入法當場死在橫屏。
 *   `ExtractViewTest` 守的就是這兩件事。
 *
 * ⚠ **不提供 `inputExtractAction` / `inputExtractAccessories`。** 那兩個 id 是
 *   `com.android.internal`，app 引用不到，所以那顆原生 `Done` 鈕不會出現 ——
 *   這正是我們要的（鍵盤自己那顆 `⏎` 走 `sendDefaultEditorAction`，功能沒有少）。
 *   框架對這兩個是 null-safe 的：`setExtractView()` 先問 `mExtractAction != null`
 *   才去找 accessories，`onUpdateExtractingViews()` 開頭也擋掉 null。
 */
class ThemedExtractView(context: Context) : LinearLayout(context) {

    /**
     * 框架用 [android.R.id.inputExtractEditText] 找的就是這一個。
     * 型別必須是 [ExtractEditText]：框架會 cast 過去呼叫 `setIME()`。
     */
    val editText: ExtractEditText = ExtractEditText(context).apply {
        id = android.R.id.inputExtractEditText
        // 背景交給外層那一塊畫，這裡透明 —— 否則系統的 editTextStyle 會在
        // 我們的底色上再蓋一層它自己的底線／圓角。
        setBackgroundColor(Color.TRANSPARENT)
        gravity = Gravity.TOP or Gravity.START
        setHorizontallyScrolling(false)
    }

    /** 與鍵盤上緣的分界線，對應主題的 `candidates.bar.border_top_*`。 */
    private val divider = View(context)

    /** 已經套上去的那一份。Theme 是不可變物件，所以比對識別就夠。 */
    private var applied: Theme? = null

    init {
        orientation = VERTICAL
        addView(
            editText,
            LayoutParams(LayoutParams.MATCH_PARENT, 0, 1f),
        )
        addView(divider, LayoutParams(LayoutParams.MATCH_PARENT, 0))
        applyTheme(null)
    }

    /**
     * 套主題。沒有主題（還沒載完）時退到一組看得見的預設值 —— 這一條
     * 一定要撐得住：橫屏 + 主題還沒好的那一瞬間如果畫成黑字黑底，
     * 使用者看到的就是「打了字但畫面上什麼都沒有」，而那正是要修的缺陷。
     */
    fun applyTheme(theme: Theme?) {
        if (theme != null && theme === applied) return
        applied = theme

        val bar = theme?.candidates?.bar
        val bg = bar?.background ?: FALLBACK_BG
        // 底色**必須不透明**：底下就是系統那塊漸層，半透明等於把它放回來。
        setBackgroundColor(opaque(bg, FALLBACK_BG))

        val text = bar?.style?.text
        editText.setTextColor(text?.color ?: FALLBACK_FG)
        editText.setTextSize(
            TypedValue.COMPLEX_UNIT_SP,
            (text?.size ?: FALLBACK_SP).coerceAtLeast(MIN_SP),
        )
        theme?.preedit?.selection?.let { editText.highlightColor = it.color }

        val padPx = dp(theme?.metrics?.padding ?: FALLBACK_PAD_DP)
        editText.setPadding(padPx, padPx, padPx, padPx)

        divider.setBackgroundColor(bar?.borderTopColor ?: FALLBACK_FG)
        divider.layoutParams = (divider.layoutParams as LayoutParams).apply {
            height = if (bar == null) 0 else dp(bar.borderTopWidth)
        }
        requestLayout()
    }

    private fun dp(v: Float): Int =
        (v * resources.displayMetrics.density + 0.5f).toInt().coerceAtLeast(0)

    private companion object {
        /** 主題還沒載進來時用的底：亮色系的候選列色，看得見字就好。 */
        const val FALLBACK_BG = 0xFFF2F2F7.toInt()
        const val FALLBACK_FG = 0xFF1C1C1E.toInt()
        const val FALLBACK_SP = 20f
        const val FALLBACK_PAD_DP = 8f

        /** 字級再小也要讀得出來；主題把候選字設成 0 不該讓這一條變成看不見。 */
        const val MIN_SP = 12f

        /** 透明或半透明一律換成不透明版本（見 [applyTheme] 的理由）。 */
        fun opaque(color: Int, fallback: Int): Int {
            val a = (color ushr 24) and 0xFF
            if (a == 0) return fallback
            return color or (0xFF shl 24)
        }
    }
}
