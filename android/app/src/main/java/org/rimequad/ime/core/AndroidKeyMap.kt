package org.rimequad.ime.core

import android.view.KeyCharacterMap
import android.view.KeyEvent

/**
 * 實體鍵盤：Android `KeyEvent` → X11 keysym / modifier。
 *
 * rime_shell.h 檔頭說「行動端因為鍵盤是自繪的，可直接由 layout yaml 指定
 * keysym，是四端中唯一能繞開這個問題的形態」—— 那句話只對**軟**鍵盤成立。
 * 只要接上實體鍵盤（或 adb 注入按鍵事件），Android 一樣得做這張映射表。
 *
 * 這裡刻意**不寫死** a-z 的對應，而是走 [KeyEvent.getUnicodeChar]：
 * 它會套用使用者當前的實體鍵盤佈局（QWERTY / Dvorak / 各國佈局），
 * 正是標頭要求的「不可寫死」。只有沒有字元表示的功能鍵才查表。
 */
object AndroidKeyMap {

    /** 回傳 0 表示這個按鍵無法對應到 keysym，呼叫端應交還給宿主處理。 */
    fun keysymOf(event: KeyEvent): Int {
        specialKeysym(event.keyCode)?.let { return it }

        // Ctrl / Alt 會讓 getUnicodeChar 產生控制字元或 0，
        // 算字面時先把它們濾掉，之後再以 modifier 位元回報給 librime。
        val metaForChar = event.metaState and
            (KeyEvent.META_CTRL_MASK or KeyEvent.META_ALT_MASK).inv()

        val raw = event.getUnicodeChar(metaForChar)
        if (raw == 0) return 0

        // 死鍵（組合附加符號）目前不支援，交還宿主。
        if (raw and KeyCharacterMap.COMBINING_ACCENT != 0) return 0

        // X11 的規則：Latin-1 範圍內 keysym == 碼位。
        if (raw in 0x20..0xFF) return raw

        // 其餘 Unicode 走 X11 的 0x01000000 + codepoint 形式。
        return 0x01000000 or raw
    }

    fun modifiersOf(event: KeyEvent, release: Boolean = false): Int {
        var mods = 0
        if (event.isShiftPressed) mods = mods or RimeModifier.SHIFT
        if (event.isCtrlPressed) mods = mods or RimeModifier.CONTROL
        if (event.isAltPressed) mods = mods or RimeModifier.ALT
        if (event.isMetaPressed) mods = mods or RimeModifier.SUPER
        if (event.isCapsLockOn) mods = mods or RimeModifier.CAPS
        if (release) mods = mods or RimeModifier.RELEASE
        return mods
    }

    /** 沒有字元表示、必須查表的功能鍵。 */
    private fun specialKeysym(keyCode: Int): Int? = when (keyCode) {
        KeyEvent.KEYCODE_DEL -> RimeKeysym.BACKSPACE
        KeyEvent.KEYCODE_FORWARD_DEL -> RimeKeysym.DELETE
        KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> RimeKeysym.RETURN
        KeyEvent.KEYCODE_ESCAPE -> RimeKeysym.ESCAPE
        KeyEvent.KEYCODE_TAB -> RimeKeysym.TAB
        KeyEvent.KEYCODE_DPAD_LEFT -> RimeKeysym.LEFT
        KeyEvent.KEYCODE_DPAD_RIGHT -> RimeKeysym.RIGHT
        KeyEvent.KEYCODE_DPAD_UP -> RimeKeysym.UP
        KeyEvent.KEYCODE_DPAD_DOWN -> RimeKeysym.DOWN
        KeyEvent.KEYCODE_PAGE_UP -> RimeKeysym.PAGE_UP
        KeyEvent.KEYCODE_PAGE_DOWN -> RimeKeysym.PAGE_DOWN
        KeyEvent.KEYCODE_MOVE_HOME -> RimeKeysym.HOME
        KeyEvent.KEYCODE_MOVE_END -> RimeKeysym.END
        // 純修飾鍵本身不送給 librime，只透過 modifier 位元表達。
        KeyEvent.KEYCODE_SHIFT_LEFT, KeyEvent.KEYCODE_SHIFT_RIGHT,
        KeyEvent.KEYCODE_CTRL_LEFT, KeyEvent.KEYCODE_CTRL_RIGHT,
        KeyEvent.KEYCODE_ALT_LEFT, KeyEvent.KEYCODE_ALT_RIGHT,
        KeyEvent.KEYCODE_META_LEFT, KeyEvent.KEYCODE_META_RIGHT,
        KeyEvent.KEYCODE_CAPS_LOCK,
        -> 0

        else -> null
    }
}
