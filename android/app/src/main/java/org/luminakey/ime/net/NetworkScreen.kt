package org.luminakey.ime.net

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
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.luminakey.ime.R
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
                    text = pluralStringResource(
                        R.plurals.network_log_heading, entries.size, entries.size
                    ),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.weight(1f),
                )
                if (entries.isNotEmpty()) {
                    OutlinedButton(onClick = { NetworkAudit.clear() }) {
                        Text(stringResource(R.string.network_log_clear))
                    }
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
            Text(stringResource(R.string.honesty_title), fontWeight = FontWeight.SemiBold)
            Text(
                text = stringResource(R.string.honesty_body),
                fontSize = 13.sp,
                modifier = Modifier.padding(top = 6.dp),
            )
            Spacer(Modifier.height(8.dp))
            Text(
                stringResource(R.string.honesty_checkable_title),
                fontWeight = FontWeight.SemiBold,
            )
            Text(
                text = stringResource(R.string.honesty_checkable_body),
                fontSize = 13.sp,
                modifier = Modifier.padding(top = 6.dp),
            )
            Text(
                text = stringResource(R.string.honesty_log_file, NetworkAudit.logFilePath),
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
            Text(
                stringResource(R.string.network_log_empty_title),
                fontWeight = FontWeight.SemiBold,
            )
            Text(
                text = stringResource(R.string.network_log_empty_body),
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
                    text = stringResource(e.outcome.labelRes),
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
                text = if (e.label.isEmpty()) stringResource(e.purpose.labelRes)
                else stringResource(
                    R.string.network_reason_with_label,
                    stringResource(e.purpose.labelRes),
                    e.label,
                ),
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
