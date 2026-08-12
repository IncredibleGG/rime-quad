package org.luminakey.ime.prefs

import android.content.Context
import android.view.View
import org.luminakey.ime.core.KeyRole
import org.luminakey.ime.theme.Theme

/**
 * 「選了之後當場試一次」。
 *
 * ── 為什麼這一支非有不可 ────────────────────────────────────────────────
 * 手感是**四個看不見的設定**。使用者在設定頁把音色從「輕點」換成「水滴」,
 * 畫面上唯一的變化是一格底色 —— 他得離開設定頁、找個地方打字,才知道自己
 * 選了什麼。四個檔位試一輪就是八次來回。
 *
 * 所以每一次選擇都當場播一次、震一次。這不是體貼,是**這一頁的設定沒有
 * 別的方式看得見**。
 *
 * ── 為什麼是「同一條路」而不是另寫一支試聽 ──────────────────────────────
 * 這裡刻意組出一份真的 [KeyBehavior] 再呼叫 [KeyBehavior.onKeyPress] ——
 * 與鍵盤上按一顆鍵**走的是同一行程式**。另寫一支「試聽用」的播放器是這一類
 * 功能最典型的缺陷來源:設定頁聽到的與真的打字不一樣,而且沒有人會發現,
 * 因為兩邊不會同時出現在同一個畫面上。
 *
 * ── ⚠ 要餵「下一份」偏好,不是現在這一份 ────────────────────────────────
 * DataStore 的寫入是非同步的。選完之後立刻用畫面上的 `prefs` 試播,聽到的
 * 是**上一次**的設定 —— 每一次都慢一格,而且看起來像「音色沒生效」。
 * 呼叫端必須自己把 `block(prefs)` 算出來的那一份傳進來,見 `FeelPage` 與
 * `FeelStripContent`。
 */
object FeedbackPreview {

    fun play(
        context: Context,
        view: View?,
        theme: Theme?,
        prefs: UserPrefs,
        role: KeyRole = KeyRole.STANDARD,
    ) {
        if (view == null) return
        val feedback = if (theme == null) {
            KeyBehavior.DEFAULT.let {
                org.luminakey.ime.theme.Feedback(
                    haptic = it.haptic,
                    hapticStrength = it.hapticStrength,
                    sound = it.sound,
                    soundVolume = it.soundVolume,
                )
            }
        } else {
            applyUserOverrides(theme, prefs).feedback
        }
        KeyBehavior.of(feedback, prefs, KeyHaptics.hasAmplitudeControl(context))
            .onKeyPress(view, role)
    }
}
