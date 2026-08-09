package org.luminakey.ime.net

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
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.launch
import org.luminakey.ime.R
import org.luminakey.ime.prefs.PrefsStore

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
                        text = stringResource(R.string.network_switch_title),
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.SemiBold,
                    )
                    Text(
                        text = stringResource(
                            if (on) R.string.network_on_summary else R.string.network_off_summary
                        ),
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
                    text = stringResource(R.string.network_on_detail),
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
            // ⚠ 用 placeholder 而不是字串相接。中文是「需要連網才能下載鍵盤」，
            //   英文是 “To download keyboards, this needs to go online” ——
            //   那一段插在句子的**開頭**。相接會把中文的語序寫死在 Kotlin 裡。
            Text(
                stringResource(R.string.network_required_title, what),
                fontWeight = FontWeight.SemiBold,
            )
            Text(
                text = stringResource(R.string.network_required_body, what),
                fontSize = 13.sp,
                modifier = Modifier.padding(top = 6.dp),
            )
            // ⚠ 這一段是刻意放在「開啟連網」按鈕**上面**的，不是放在說明的最後。
            //   使用者按下按鈕之前就該知道代價，按完才告訴他等於沒說。
            Text(
                text = stringResource(R.string.network_required_cost),
                fontSize = 12.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 8.dp),
            )
            Spacer(Modifier.height(10.dp))
            Button(
                onClick = { scope.launch { NetworkAudit.setEnabled(context, true) } },
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(stringResource(R.string.network_required_action))
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
        title = { Text(stringResource(R.string.first_run_title)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text(stringResource(R.string.first_run_body_1), fontSize = 14.sp)
                Text(stringResource(R.string.first_run_body_2), fontSize = 14.sp)
                Text(
                    stringResource(R.string.first_run_body_3),
                    fontSize = 13.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        },
        confirmButton = {
            TextButton(onClick = { scope.launch { NetworkAudit.markNoticeSeen(context) } }) {
                Text(stringResource(R.string.first_run_confirm))
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
