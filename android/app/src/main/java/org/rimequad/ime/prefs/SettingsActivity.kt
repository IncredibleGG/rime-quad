package org.rimequad.ime.prefs

import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import org.rimequad.ime.MainActivity
import org.rimequad.ime.core.RimeRuntime
import org.rimequad.ime.ui.RimeTheme

/**
 * 設定畫面的獨立入口。
 *
 * 為什麼不只當成 `MainActivity` 的一個分頁：軟鍵盤工具列上的 `settings`
 * action（規範 §8.6.6.1 把它列為**必備項**）按下去應該直接落在設定，
 * 而不是先落在診斷分頁再要使用者自己找。`MainActivity` 那邊仍然有一個
 * 「設定」分頁，兩個入口指向同一個 [SettingsScreen] composable。
 */
class SettingsActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // 設定畫面要顯示診斷資訊與可用主題，這兩者都依賴資料目錄已備妥。
        RimeRuntime.start(applicationContext)
        setContent {
            RimeTheme {
                Scaffold { inset ->
                    Column(
                        modifier = Modifier
                            .fillMaxSize()
                            .padding(inset)
                            .padding(horizontal = 16.dp),
                    ) {
                        Text(
                            text = "設定",
                            style = MaterialTheme.typography.headlineSmall,
                            modifier = Modifier.padding(vertical = 10.dp),
                        )
                        SettingsScreen(
                            modifier = Modifier.fillMaxSize(),
                            // 方案管理畫面是另一位負責的 StoreScreen，掛在
                            // MainActivity 的分頁上。這裡只導過去，不自己再做一份。
                            onOpenSchemaStore = {
                                startActivity(
                                    Intent(this@SettingsActivity, MainActivity::class.java)
                                        .putExtra(EXTRA_TAB, TAB_STORE)
                                )
                            },
                        )
                    }
                }
            }
        }
    }

    companion object {
        /**
         * `MainActivity` 若讀得懂這個 extra，就會直接開在方案市集分頁。
         * 讀不懂也無妨（那個檔案由另一位負責，本輪不改它的分頁邏輯），
         * 使用者只是會落在預設分頁。
         */
        const val EXTRA_TAB = "org.rimequad.ime.EXTRA_TAB"
        const val TAB_STORE = 1
    }
}
