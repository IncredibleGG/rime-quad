package org.rimequad.ime.update

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.rimequad.ime.net.rememberNetworkEnabled

/**
 * 設定頁的「更新」區塊。
 *
 * 這是整條更新線**唯一**的出口：不彈窗、不發通知、不在鍵盤上放紅點。
 * 使用者要看才看得到（見 [UpdateController] 檔頭的理由）。
 */
@Composable
fun UpdateSection(
    controller: UpdateController,
    autoCheck: Boolean?,
    onAutoCheckChange: (Boolean?) -> Unit,
    modifier: Modifier = Modifier,
) {
    val hasUpdate = controller.hasUpdate
    val networkOn = rememberNetworkEnabled()
    Card(
        modifier = modifier.fillMaxWidth(),
        colors = if (hasUpdate) {
            CardDefaults.cardColors(
                containerColor = MaterialTheme.colorScheme.secondaryContainer
            )
        } else {
            CardDefaults.cardColors()
        },
    ) {
        Column(modifier = Modifier.padding(14.dp)) {

            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    text = "更新",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                )
                if (hasUpdate) {
                    Spacer(Modifier.size(6.dp))
                    RedDot()
                }
            }

            Text(
                text = "目前版本：${controller.installedVersionName}" +
                    "（versionCode ${controller.installedVersionCode}）",
                fontSize = 12.sp,
                fontFamily = FontFamily.Monospace,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 4.dp),
            )

            /* ── 連網開關關閉時，這一整區都不會動作 ── */
            if (!networkOn) {
                Spacer(Modifier.height(8.dp))
                Text(
                    text = "連網開關目前是關閉的，所以不會檢查更新，也不會下載任何東西。" +
                        "要收到新版本，先到「連網」分頁把開關打開。",
                    fontSize = 13.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }

            /* ── 有新版本 ── */
            val latest = controller.latest
            if (hasUpdate && latest != null) {
                Spacer(Modifier.height(8.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    RedDot()
                    Spacer(Modifier.size(6.dp))
                    Text(
                        text = "有新版本可用：${latest.versionName}",
                        fontWeight = FontWeight.SemiBold,
                    )
                }
                Text(
                    text = "大小 ${fmtBytes(latest.size)}" +
                        (latest.commit?.let { "・commit $it" } ?: ""),
                    fontSize = 12.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                if (latest.notes.isNotBlank()) {
                    Text(
                        text = latest.notes,
                        fontSize = 13.sp,
                        modifier = Modifier.padding(top = 6.dp),
                    )
                }
            }

            /* ── 進度 ── */
            if (controller.phase == UpdateController.Phase.DOWNLOADING) {
                Spacer(Modifier.height(8.dp))
                val p = controller.progress
                if (p >= 0f) {
                    LinearProgressIndicator(
                        progress = { p.coerceIn(0f, 1f) },
                        modifier = Modifier.fillMaxWidth(),
                    )
                } else {
                    LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                }
            }

            controller.status?.let {
                Text(
                    text = it,
                    fontSize = 12.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(top = 6.dp),
                )
            }

            /* ── 失敗 ── */
            controller.error?.let { err ->
                Spacer(Modifier.height(8.dp))
                Card(
                    colors = CardDefaults.cardColors(
                        containerColor = MaterialTheme.colorScheme.errorContainer
                    ),
                ) {
                    Column(Modifier.padding(10.dp)) {
                        Text(
                            text = err,
                            fontSize = 13.sp,
                            color = MaterialTheme.colorScheme.onErrorContainer,
                        )
                        TextButton(onClick = { controller.dismissError() }) { Text("知道了") }
                    }
                }
            }

            /* ── 未知來源授權引導 ── */
            if (controller.verifiedApk != null && !controller.installPermitted) {
                Spacer(Modifier.height(8.dp))
                Text(
                    text = "系統還沒允許本 app 安裝應用程式。側載安裝一定要先開這個開關，" +
                        "開完按返回鍵回來就能繼續。",
                    fontSize = 13.sp,
                )
                OutlinedButton(
                    onClick = { controller.openUnknownSourcesSettings() },
                    modifier = Modifier.fillMaxWidth().padding(top = 6.dp),
                ) {
                    Text("去開啟「安裝未知的應用程式」")
                }
            }

            /* ── 操作 ── */
            Spacer(Modifier.height(10.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(
                    onClick = { controller.checkNow() },
                    enabled = !controller.busy,
                    modifier = Modifier.weight(1f),
                ) {
                    Text(
                        if (controller.phase == UpdateController.Phase.CHECKING) "檢查中…"
                        else "檢查更新"
                    )
                }
                when {
                    controller.verifiedApk != null -> Button(
                        onClick = { controller.install() },
                        enabled = controller.phase != UpdateController.Phase.INSTALLING &&
                            controller.installPermitted,
                        modifier = Modifier.weight(1f),
                    ) {
                        Text(
                            if (controller.phase == UpdateController.Phase.INSTALLING) "安裝中…"
                            else "安裝"
                        )
                    }

                    hasUpdate -> Button(
                        onClick = { controller.downloadAndVerify() },
                        enabled = !controller.busy,
                        modifier = Modifier.weight(1f),
                    ) {
                        Text(
                            when (controller.phase) {
                                UpdateController.Phase.DOWNLOADING -> "下載中…"
                                UpdateController.Phase.VERIFYING -> "驗證中…"
                                else -> "下載並安裝"
                            }
                        )
                    }
                }
            }

            /* ── 自動檢查開關 ── */
            Spacer(Modifier.height(6.dp))
            Row(
                modifier = Modifier.fillMaxWidth().padding(top = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(Modifier.weight(1f)) {
                    Text(text = "啟動時自動檢查更新", fontWeight = FontWeight.Medium)
                    Text(
                        // 「未設定」與「開」在行為上相同，但顯示要分得出來 ——
                        // 使用者才知道自己有沒有動過這一項。
                        // 這一項是**連網開關底下的**子設定：開關關著時它一律不生效，
                        // 說明必須寫清楚，否則使用者會以為自己被偷偷檢查了。
                        text = (if (autoCheck == null) "預設：開。只在背景抓一份幾百 bytes 的" +
                            "版本資訊，不會自動下載 APK，也不會彈窗。"
                        else "已設定：${if (autoCheck) "開" else "關"}") +
                            if (networkOn) "" else "（連網開關關閉中，這一項目前不生效）",
                        fontSize = 12.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                if (autoCheck != null) {
                    TextButton(onClick = { onAutoCheckChange(null) }) {
                        Text("回復預設", fontSize = 12.sp)
                    }
                }
                Switch(
                    checked = autoCheck ?: true,
                    onCheckedChange = { onAutoCheckChange(it) },
                )
            }

            /* ── 安全性說明 ── */
            Text(
                text = "下載完成後會核對 sha256。要提醒的是：摘要和 APK 來自同一台伺服器，" +
                    "所以它防的是傳輸過程壞掉，不是伺服器被入侵。真正擋住惡意替換的是 " +
                    "Android 強制「升級必須是同一把金鑰簽的」——別人簽的 APK 系統會直接拒絕。",
                fontSize = 11.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 10.dp),
            )
        }
    }
}

/** 小紅點。刻意小、刻意只出現在設定頁。 */
@Composable
private fun RedDot() {
    Spacer(
        Modifier
            .size(9.dp)
            .clip(CircleShape)
            .background(MaterialTheme.colorScheme.error)
    )
}
