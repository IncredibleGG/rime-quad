package org.rimequad.ime.prefs

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import org.rimequad.ime.core.RimeRuntime
import org.rimequad.ime.home.RimeAppScreen
import org.rimequad.ime.ui.RimeTheme
import org.rimequad.ime.update.UpdateController

/**
 * 設定畫面的獨立入口。
 *
 * 為什麼不只是 `MainActivity` 的一條路由：鍵盤面板右下角那一行「全部設定 ›」
 * 按下去應該直接落在設定，而不是先落在別的地方再要使用者自己找。兩個 Activity
 * 畫的是**同一個** [RimeAppScreen]，只差在這一個永遠不進引導 ——
 * 能從鍵盤上按到這裡的人，顯然已經走完引導了。
 */
class SettingsActivity : ComponentActivity() {

    /** 見 `MainActivity.onWindowFocusChanged` 的說明。 */
    private var focusEpoch by mutableIntStateOf(0)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        RimeRuntime.start(applicationContext)
        UpdateController.get(applicationContext)
            .autoCheckOnStart(PrefsStore.get(applicationContext).current.autoCheckUpdate)
        val openStore = intent?.getIntExtra(EXTRA_TAB, 0) == TAB_STORE
        setContent {
            RimeTheme {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background,
                ) {
                    RimeAppScreen(
                        openStore = openStore,
                        focusEpoch = focusEpoch,
                        startInOnboarding = false,
                    )
                }
            }
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        focusEpoch++
    }

    override fun onResume() {
        super.onResume()
        UpdateController.get(applicationContext).refreshInstallPermission()
    }

    companion object {
        /** 帶著它進 `MainActivity` 或本 Activity，就直接落在市集那一頁。 */
        const val EXTRA_TAB = "org.rimequad.ime.EXTRA_TAB"
        const val TAB_STORE = 1
    }
}
