package org.luminakey.ime

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import org.luminakey.ime.core.RimeRuntime
import org.luminakey.ime.home.RimeAppScreen
import org.luminakey.ime.prefs.PrefsStore
import org.luminakey.ime.prefs.SettingsActivity
import org.luminakey.ime.ui.RimeTheme
import org.luminakey.ime.update.UpdateController

/**
 * App 的唯一入口。同時也是 `method.xml` 指定的 settingsActivity。
 *
 * 這個 Activity 只做三件事：把 runtime 叫起來、決定第一幀要畫引導還是設定、
 * 以及把「視窗焦點變了」這個事件轉給畫面。第三件事看起來最不起眼，
 * 卻是整個引導流程能不能用的關鍵，理由見下面 [onWindowFocusChanged]。
 */
class MainActivity : ComponentActivity() {

    /**
     * 每次視窗焦點變化就 +1。畫面拿它當作「立刻重查一次系統狀態」的訊號。
     *
     * ⚠ 為什麼不能只靠 `onResume()`：叫出系統輸入法選擇器的
     * `showInputMethodPicker()` 開的是一個**系統對話框**，不是 Activity ——
     * 我們全程維持 `RESUMED`，`onResume()` 一次都不會再被呼叫。實測（API 35）
     * 只有 `mCurrentFocus` 變了兩次。寫在 onResume 裡的重新檢查因此永遠不跑，
     * 使用者選好鍵盤回來，畫面還停在「還差一步」—— 這幾乎確定就是競品那個
     * 三步精靈卡住的真正成因。
     */
    private var focusEpoch by mutableIntStateOf(0)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        RimeRuntime.start(applicationContext)

        // ⚠ 這一個 key 必須**阻塞讀**。
        //
        // PrefsStore 背後是 DataStore，第一次讀是非同步的：若在 Compose 裡用
        // `collectAsState(initial = …)` 拿它，第一幀一定是預設值（false），
        // 於是**每一個既有使用者每次冷啟動都會看到引導頁閃一下**才切到設定頁。
        // 那種閃爍比慢半秒惹人厭得多。`current` 的首次存取會阻塞讀一次檔案
        // （幾百 bytes），成本可以忽略。
        //
        // 語義也要釘死：這個布林**只**決定開在哪一頁，它**不是**「輸入法可以用」
        // 的證據 —— 那件事一律去問系統，見 home/ImeSetupState.kt。
        val onboardingDone = PrefsStore.get(applicationContext).current.onboardingDone == true

        // 啟動時的靜默更新檢查。沒網路、被連網開關擋下、被節流、使用者關掉了
        // —— 每一種情形都只是安靜地什麼都不做，絕不打斷他。
        UpdateController.get(applicationContext)
            .autoCheckOnStart(PrefsStore.get(applicationContext).current.autoCheckUpdate)

        val openStore = intent?.getIntExtra(SettingsActivity.EXTRA_TAB, 0) == SettingsActivity.TAB_STORE

        setContent {
            RimeTheme {
                Surface(
                    modifier = Modifier
                        .fillMaxSize()
                        .background(MaterialTheme.colorScheme.background),
                    color = MaterialTheme.colorScheme.background,
                ) {
                    RimeAppScreen(
                        openStore = openStore,
                        focusEpoch = focusEpoch,
                        startInOnboarding = !onboardingDone,
                    )
                }
            }
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        focusEpoch++
    }

    /**
     * 使用者是離開 app 去系統設定開「安裝未知的應用程式」的，回來時 Compose
     * 不會自己知道那個開關變了。在這裡推一下，畫面才不會一直停在「尚未授權」。
     */
    override fun onResume() {
        super.onResume()
        UpdateController.get(applicationContext).refreshInstallPermission()
    }
}
