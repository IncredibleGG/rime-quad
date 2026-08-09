package org.luminakey.ime.store

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.luminakey.ime.R
import org.luminakey.ime.home.NavRow
import org.luminakey.ime.home.PlainCard
import org.luminakey.ime.home.RowDivider

/**
 * 「進階」頁裡的備份區塊。
 *
 * ── 為什麼整塊住在 store/ 而不是 home/SettingsPages.kt ──────────────────
 * 那個檔案同時被好幾條支線改，是必然的衝突點。把 UI 收在自己的檔案裡，
 * 設定頁那邊只剩三行（一個小標題加一次呼叫）。
 *
 * ── SAF，不要儲存權限 ──────────────────────────────────────────────────
 * 匯出走 `ACTION_CREATE_DOCUMENT`、匯入走 `ACTION_OPEN_DOCUMENT`，
 * 兩者都由系統的檔案選擇器代勞，App 拿到的是一個一次性的 Uri。
 * 因此 `AndroidManifest.xml` 不需要（也不會出現）任何儲存權限 ——
 * 對一個以「權限越少越好」為賣點的輸入法，這一點跟功能本身一樣重要。
 * 使用者要存到哪裡（本機、SD 卡、還是他自己的雲端）由他決定，我們不知道。
 */
@Composable
fun BackupSection() {
    val context = LocalContext.current
    val controller = remember { BackupController.get(context) }

    val exporter = rememberLauncherForActivityResult(
        // 用 zip 這個 MIME：多數檔案管理器認得，使用者要用電腦打開來看
        // 裡面有什麼也打得開。備份不該是一個只有我們讀得懂的黑盒子。
        ActivityResultContracts.CreateDocument("application/zip")
    ) { uri -> if (uri != null) controller.export(uri) }

    val importer = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri -> if (uri != null) controller.askImport(uri) }

    PlainCard {
        NavRow(
            title = stringResource(R.string.backup_export),
            subtitle = stringResource(R.string.backup_export_sub),
            onClick = {
                if (!controller.busy) exporter.launch(controller.suggestedFileName())
            },
        )
        RowDivider()
        NavRow(
            title = stringResource(R.string.backup_import),
            subtitle = stringResource(R.string.backup_import_sub),
            onClick = {
                // 有些檔案管理器不會把 .zip 標成 application/zip（實測 Files by
                // Google 給的是 application/octet-stream），只收 zip 會讓使用者
                // 看到一整片灰掉的檔案而不知道為什麼。內容檢查在後面照跑。
                if (!controller.busy) importer.launch(arrayOf("application/zip", "*/*"))
            },
        )
    }

    controller.stage?.let { stage ->
        Spacer(Modifier.height(10.dp))
        Column(Modifier.fillMaxWidth()) {
            Text(text = stage, fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.height(6.dp))
            // 不確定進度：跟部署一樣，我們沒有百分比可以誠實地報。
            LinearProgressIndicator(Modifier.fillMaxWidth())
        }
    }

    controller.pendingImport?.let {
        AlertDialog(
            onDismissRequest = { controller.cancelImport() },
            title = { Text(stringResource(R.string.backup_confirm_title)) },
            text = { Text(stringResource(R.string.backup_confirm_body)) },
            confirmButton = {
                TextButton(onClick = { controller.confirmImport() }) {
                    Text(stringResource(R.string.backup_confirm_ok))
                }
            },
            dismissButton = {
                TextButton(onClick = { controller.cancelImport() }) {
                    Text(stringResource(R.string.backup_confirm_cancel))
                }
            },
        )
    }

    controller.result?.let { r ->
        AlertDialog(
            onDismissRequest = { controller.dismissResult() },
            title = {
                Text(
                    stringResource(
                        if (r.ok) R.string.backup_result_ok_title
                        else R.string.backup_result_failed_title
                    )
                )
            },
            text = {
                Column {
                    Text(r.message)
                    // 提醒（缺方案、詞庫可能不完整）不是次要資訊，所以不縮小、
                    // 不變灰 —— 它們正是使用者需要採取行動的部分。
                    r.notes.forEach {
                        Spacer(Modifier.height(8.dp))
                        Text(it, modifier = Modifier.padding(top = 2.dp))
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { controller.dismissResult() }) {
                    Text(stringResource(R.string.backup_result_dismiss))
                }
            },
        )
    }
}
