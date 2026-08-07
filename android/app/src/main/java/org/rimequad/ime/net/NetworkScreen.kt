package org.rimequad.ime.net

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 「連網」分頁：開關 + 誠實說明 + 連網紀錄。
 *
 * 這一頁是整個定位最有價值的東西 —— 它把「你要相信我們」換成「你自己看」。
 * 畫面本身沒有任何邏輯：開關讀寫 [NetworkAudit]，紀錄讀 [NetworkAudit.entries]。
 * 之後 UI 重新設計時整個搬走也不會帶走任何邏輯。
 */
@Composable
fun NetworkScreen(modifier: Modifier = Modifier) {
    val entries = NetworkAudit.entries

    LazyColumn(
        modifier = modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(10.dp),
        contentPadding = PaddingValues(bottom = 32.dp),
    ) {
        item { NetworkSwitchCard() }

        item { HonestyCard() }

        item {
            Row(
                modifier = Modifier.fillMaxWidth().padding(top = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = "連網紀錄（${entries.size}）",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.weight(1f),
                )
                if (entries.isNotEmpty()) {
                    OutlinedButton(onClick = { NetworkAudit.clear() }) { Text("清除") }
                }
            }
        }

        if (entries.isEmpty()) {
            item { EmptyLogCard() }
        } else {
            items(entries, key = { it.atMillis.toString() + it.host + it.outcome.name }) { e ->
                LogRow(e)
            }
        }
    }
}

/* ─────────────────────────── 說明 ─────────────────────────── */

@Composable
private fun HonestyCard() {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(14.dp)) {
            Text("為什麼權限清單上還是有「網路存取」", fontWeight = FontWeight.SemiBold)
            Text(
                text = "因為 Android 的網路權限是「安裝時」給的，執行期取消不了。" +
                    "只要這個 app 帶著方案市集與應用內更新，權限清單上就一定看得到它 —— " +
                    "開關關掉也一樣看得到。我們不會宣稱自己沒有網路權限，那句話一查就穿幫。",
                fontSize = 13.sp,
                modifier = Modifier.padding(top = 6.dp),
            )
            Spacer(Modifier.height(8.dp))
            Text("我們改成讓你查得到", fontWeight = FontWeight.SemiBold)
            Text(
                text = "· 整個 app 只有一個連網出口：原始碼的 " +
                    "android/app/src/main/java/org/rimequad/ime/net/NetworkGate.kt。\n" +
                    "· 開關關閉時，那個出口直接拒絕並拋出例外，一個連線都不會開。\n" +
                    "· 每一次真的建立的連線都記在下面，轉址的每一跳也各記一筆。\n" +
                    "· 紀錄裡沒有你輸入的任何內容，也沒有網址的路徑，只有主機名與原因。",
                fontSize = 13.sp,
                modifier = Modifier.padding(top = 6.dp),
            )
            Text(
                text = "紀錄檔：${NetworkAudit.logFilePath}",
                fontSize = 11.sp,
                fontFamily = FontFamily.Monospace,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 8.dp),
            )
        }
    }
}

@Composable
private fun EmptyLogCard() {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(14.dp)) {
            Text("目前沒有任何連網紀錄", fontWeight = FontWeight.SemiBold)
            Text(
                text = "開關從來沒開過的話，這裡本來就會是空的 —— 這正是你要看到的結果。",
                fontSize = 13.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 6.dp),
            )
        }
    }
}

/* ─────────────────────────── 一筆紀錄 ─────────────────────────── */

@Composable
private fun LogRow(e: NetworkLogEntry) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    text = e.host,
                    fontWeight = FontWeight.SemiBold,
                    fontFamily = FontFamily.Monospace,
                    fontSize = 13.sp,
                    modifier = Modifier.weight(1f),
                )
                Text(
                    text = e.outcome.zh,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Medium,
                    color = when (e.outcome) {
                        NetworkOutcome.OK -> MaterialTheme.colorScheme.primary
                        NetworkOutcome.FAILED -> MaterialTheme.colorScheme.error
                        NetworkOutcome.REDIRECTED -> MaterialTheme.colorScheme.onSurfaceVariant
                    },
                )
            }
            Text(
                text = e.reasonText,
                fontSize = 13.sp,
                modifier = Modifier.padding(top = 2.dp),
            )
            Text(
                text = buildString {
                    append(formatTime(e.atMillis))
                    if (e.outcome == NetworkOutcome.OK) append("　·　${formatBytes(e.bytes)}")
                    if (e.detail.isNotEmpty()) append("　·　${e.detail}")
                },
                fontSize = 11.sp,
                fontFamily = FontFamily.Monospace,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 2.dp),
            )
        }
    }
}

private fun formatTime(millis: Long): String =
    SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date(millis))

private fun formatBytes(n: Long): String = when {
    n >= 1024 * 1024 -> String.format(Locale.US, "%.1f MB", n / 1024.0 / 1024.0)
    n >= 1024 -> String.format(Locale.US, "%.0f KB", n / 1024.0)
    else -> "$n B"
}
