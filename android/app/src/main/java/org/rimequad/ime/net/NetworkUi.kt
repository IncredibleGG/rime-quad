package org.rimequad.ime.net

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.launch
import org.rimequad.ime.prefs.PrefsStore

/**
 * 連網開關與說明的畫面零件。
 *
 * ── 為什麼零件都在 `net` 底下而不是散在各畫面裡 ─────────────────────────
 * UI 的整體重新設計還在進行中，這些卡片之後很可能被搬到別的地方去。
 * 每一個零件都自己讀偏好、自己寫偏好，呼叫端只要放一行就好 ——
 * 搬家時搬的是那一行，不是一段邏輯。
 */

/**
 * 開關現在是不是開的（**會隨偏好變更自動重組**）。
 *
 * 不要在 Compose 裡直接讀 [NetworkGate.isEnabled]：那是一個普通的
 * getter，值變了 Compose 不會知道，畫面會停在舊狀態。
 */
@Composable
fun rememberNetworkEnabled(): Boolean {
    val context = LocalContext.current
    val store = remember { PrefsStore.get(context) }
    val prefs by store.flow.collectAsState(initial = store.current)
    return prefs.networkEnabled == true
}

/**
 * 主開關。放在設定頁最上方與「連網」分頁裡各一份（同一個零件，不是兩份程式碼）。
 */
@Composable
fun NetworkSwitchCard(modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val scope = rememberCoroutineScopeCompat()
    val on = rememberNetworkEnabled()

    Card(
        modifier = modifier.fillMaxWidth(),
        colors = if (on) {
            CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.secondaryContainer)
        } else {
            CardDefaults.cardColors()
        },
    ) {
        Column(Modifier.padding(14.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(
                        text = "連網",
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.SemiBold,
                    )
                    Text(
                        text = if (on) "已開啟：現在會連網。只有你按下「安裝方案」或「檢查更新」時才會，"
                            + "打字內容永遠不會離開這台裝置。"
                        else "已關閉（預設）：完全離線。市集與更新檢查都不會執行。",
                        fontSize = 13.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(top = 4.dp),
                    )
                }
                Switch(
                    checked = on,
                    onCheckedChange = { v -> scope.launch { NetworkAudit.setEnabled(context, v) } },
                )
            }
            if (on) {
                Spacer(Modifier.height(6.dp))
                // 這顆開關在設定頁也按得到，使用者不一定看過 NetworkRequiredCard，
                // 所以「開著等於向你的網路告知」這句話在這裡也要講一次。
                Text(
                    text = "每一次實際發生的連網都會記在「連網」分頁的紀錄裡，你可以自己核對。" +
                        "另外要知道：開著的時候一旦真的連線，你的網路就會看得到這台裝置" +
                        "連了哪個網域（DNS 與 TLS 的連線目標是明文，改不掉）。" +
                        "我們的請求本身不含這個 app 的名字，但「有沒有連」瞞不了。",
                    fontSize = 12.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

/**
 * 「這件事需要連網」的說明卡，帶一顆開啟按鈕。
 *
 * 刻意**不是**把按鈕灰掉：灰掉的按鈕只告訴使用者「不能按」，不告訴他
 * 為什麼、也不告訴他怎麼辦。
 */
@Composable
fun NetworkRequiredCard(what: String, modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val scope = rememberCoroutineScopeCompat()
    Card(
        modifier = modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceVariant
        ),
    ) {
        Column(Modifier.padding(14.dp)) {
            Text("需要連網才能$what", fontWeight = FontWeight.SemiBold)
            Text(
                text = "這個輸入法預設完全離線，所以現在還沒有向任何伺服器要過東西。" +
                    "要$what，得從網路上取檔案；打開下面的開關就會開始，" +
                    "用完隨時關掉，關掉就回到完全離線。",
                fontSize = 13.sp,
                modifier = Modifier.padding(top = 6.dp),
            )
            // ⚠ 這一段是刻意放在「開啟連網」按鈕**上面**的，不是放在說明的最後。
            //   使用者按下按鈕之前就該知道代價，按完才告訴他等於沒說。
            Text(
                text = "要老實說的代價：連線一旦發生，你的網路（電信商、Wi-Fi 的主人、" +
                    "中間的任何一方）就會知道這台裝置連了 github.com 之類的網域。" +
                    "我們送出的請求裡不含這個 app 的名字，但 DNS 查詢與 TLS 的" +
                    "連線目標本來就是明文，這一點我們瞞不了，也不打算假裝瞞得了。" +
                    "如果你所在的環境連「裝了哪個輸入法」都不方便被知道，" +
                    "那就別開這個開關 —— 離線的功能已經是完整的。",
                fontSize = 12.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 8.dp),
            )
            Spacer(Modifier.height(10.dp))
            Button(
                onClick = { scope.launch { NetworkAudit.setEnabled(context, true) } },
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text("開啟連網")
            }
        }
    }
}

/**
 * 首次啟動的說明。**只講一次**，看過就不再出現。
 *
 * ⚠ 這裡刻意**不**宣稱「本 app 沒有網路權限」—— 那是假的，`INTERNET` 是
 * 安裝時權限，執行期取消不了，任何人查一下權限清單就會抓到。寧可講得
 * 複雜一點，也不要講一句會被抓包的話。所以誠實的版本是：
 * 「權限取消不了，所以我們用開關和連網紀錄讓你能自己查」。
 */
@Composable
fun FirstRunNoticeHost() {
    val context = LocalContext.current
    val scope = rememberCoroutineScopeCompat()
    val store = remember { PrefsStore.get(context) }
    val prefs by store.flow.collectAsState(initial = store.current)
    if (prefs.offlineNoticeSeen == true) return

    AlertDialog(
        onDismissRequest = { /* 刻意不可點外面關掉：這一段只講一次，要確定他看過。 */ },
        title = { Text("這個輸入法預設不連網") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text(
                    "我們不會自己連網。打字、選字、換方案、換佈局，全部在你的裝置上完成。",
                    fontSize = 14.sp,
                )
                Text(
                    "要裝更多方案或收到更新，可以隨時打開開關；關掉就回到完全離線。" +
                        "開關預設是關的。",
                    fontSize = 14.sp,
                )
                Text(
                    "有一件事要老實說：Android 的網路權限是安裝時給的，執行期取消不了，" +
                        "所以你在系統的權限清單上一定看得到它，就算開關關著也一樣。" +
                        "既然做不到「沒有權限」，我們改成讓你查得到 —— " +
                        "整個 app 只有一個連網出口，開關關閉時它直接拒絕；" +
                        "每一次真的發生的連網都記在「連網」分頁裡，時間、對象、原因、結果都在。",
                    fontSize = 13.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        },
        confirmButton = {
            TextButton(onClick = { scope.launch { NetworkAudit.markNoticeSeen(context) } }) {
                Text("知道了，維持離線")
            }
        },
    )
}

/**
 * `rememberCoroutineScope()` 的轉包。
 *
 * 為什麼包一層：本模組 production 端刻意零第三方依賴，`kotlinx.coroutines`
 * 是 DataStore 帶進來的傳遞相依（見 gradle/libs.versions.toml 對
 * datastore 的註解）。偏好寫入是 suspend 的，逃不掉，但把它收斂在這一個
 * 函式裡，日後換掉儲存層時要改的地方只有一處。
 */
@Composable
private fun rememberCoroutineScopeCompat() = androidx.compose.runtime.rememberCoroutineScope()
