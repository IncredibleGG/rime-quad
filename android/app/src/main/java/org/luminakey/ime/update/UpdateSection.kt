package org.luminakey.ime.update

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
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.luminakey.ime.R
import org.luminakey.ime.net.rememberNetworkEnabled

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
                    text = stringResource(R.string.update_title),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                )
                if (hasUpdate) {
                    Spacer(Modifier.size(6.dp))
                    RedDot()
                }
            }

            Text(
                text = stringResource(
                    R.string.update_installed_version,
                    controller.installedVersionName,
                    controller.installedVersionCode,
                ),
                fontSize = 12.sp,
                fontFamily = FontFamily.Monospace,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 4.dp),
            )

            /* ── 連網開關關閉時，這一整區都不會動作 ── */
            if (!networkOn) {
                Spacer(Modifier.height(8.dp))
                Text(
                    text = stringResource(R.string.update_network_off),
                    fontSize = 13.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }

            /* ── 換了 app 身分：這一版裝不上去，要手動搬家 ── */
            controller.migration?.let { mig ->
                Spacer(Modifier.height(10.dp))
                MigrationCard(controller, mig)
            }

            /* ── 有新版本 ── */
            val latest = controller.latest
            if (hasUpdate && latest != null && controller.migration == null) {
                Spacer(Modifier.height(8.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    RedDot()
                    Spacer(Modifier.size(6.dp))
                    Text(
                        text = stringResource(R.string.update_available, latest.versionName),
                        fontWeight = FontWeight.SemiBold,
                    )
                }
                Text(
                    text = latest.commit?.let {
                        stringResource(R.string.update_size_commit, fmtBytes(latest.size), it)
                    } ?: stringResource(R.string.update_size, fmtBytes(latest.size)),
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
                        TextButton(onClick = { controller.dismissError() }) {
                            Text(stringResource(R.string.got_it))
                        }
                    }
                }
            }

            /* ── 未知來源授權引導 ── */
            if (controller.verifiedApk != null && !controller.installPermitted) {
                Spacer(Modifier.height(8.dp))
                Text(
                    text = stringResource(R.string.update_unknown_sources_body),
                    fontSize = 13.sp,
                )
                OutlinedButton(
                    onClick = { controller.openUnknownSourcesSettings() },
                    modifier = Modifier.fillMaxWidth().padding(top = 6.dp),
                ) {
                    Text(stringResource(R.string.update_unknown_sources_action))
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
                        stringResource(
                            if (controller.phase == UpdateController.Phase.CHECKING)
                                R.string.update_checking
                            else R.string.update_check
                        )
                    )
                }
                when {
                    // 裝不上去的那一版沒有「下載」也沒有「安裝」——
                    // 一顆按下去必定失敗的按鈕比沒有按鈕更糟。
                    controller.migration != null -> Unit

                    controller.verifiedApk != null -> Button(
                        onClick = { controller.install() },
                        enabled = controller.phase != UpdateController.Phase.INSTALLING &&
                            controller.installPermitted,
                        modifier = Modifier.weight(1f),
                    ) {
                        Text(
                            stringResource(
                                if (controller.phase == UpdateController.Phase.INSTALLING)
                                    R.string.update_installing
                                else R.string.update_install
                            )
                        )
                    }

                    hasUpdate -> Button(
                        onClick = { controller.downloadAndVerify() },
                        enabled = !controller.busy,
                        modifier = Modifier.weight(1f),
                    ) {
                        Text(
                            stringResource(
                                when (controller.phase) {
                                    UpdateController.Phase.DOWNLOADING -> R.string.update_downloading
                                    UpdateController.Phase.VERIFYING -> R.string.update_verifying
                                    else -> R.string.update_download
                                }
                            )
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
                    Text(
                        text = stringResource(R.string.update_auto_check),
                        fontWeight = FontWeight.Medium,
                    )
                    Text(
                        // 「未設定」與「開」在行為上相同，但顯示要分得出來 ——
                        // 使用者才知道自己有沒有動過這一項。
                        // 這一項是**連網開關底下的**子設定：開關關著時它一律不生效，
                        // 說明必須寫清楚，否則使用者會以為自己被偷偷檢查了。
                        text = stringResource(
                            when (autoCheck) {
                                null -> R.string.update_auto_check_default
                                true -> R.string.update_auto_check_set_on
                                false -> R.string.update_auto_check_set_off
                            }
                        ) + if (networkOn) "" else stringResource(R.string.update_auto_check_blocked),
                        fontSize = 12.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                if (autoCheck != null) {
                    TextButton(onClick = { onAutoCheckChange(null) }) {
                        Text(stringResource(R.string.reset_to_default), fontSize = 12.sp)
                    }
                }
                Switch(
                    checked = autoCheck ?: true,
                    onCheckedChange = { onAutoCheckChange(it) },
                )
            }

            /* ── 安全性說明 ── */
            Text(
                text = stringResource(R.string.update_security_note),
                fontSize = 11.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 10.dp),
            )
        }
    }
}

/**
 * 「這一版裝不上去，你要自己搬過去」。
 *
 * ══════════════════════════════════════════════════════════════════════
 *  ⚠ 這張卡片存在的理由，就是使用者 2026-08-09 看到的那個畫面
 * ══════════════════════════════════════════════════════════════════════
 *
 * 他當時拿到的是「APK 檔案無效或已損毀」加一行系統原文。檔案沒有壞：
 * 產品改名換了 app 的身分，而 Android 不允許覆蓋安裝。那句話把他指向
 * 「下載」—— 他會重試、每次都一樣，然後放棄。
 *
 * 所以這張卡片必須同時回答三件事，缺一不可：
 *
 *   1. **這是什麼**（不是你的錯，也不是檔案壞了）；
 *   2. **你會失去什麼** —— 詞典與設定不會自動轉移。這是唯一一件他
 *      真的會失去東西的事，而系統的任何一則訊息都不會提到它，
 *      所以它用 error 配色、放在步驟之前，不可以縮小、不可以變灰；
 *   3. **他做得到的下一步** —— 兩顆按鈕，不是一段「請至官網下載」。
 *
 * 步驟 1 與 5 的字面直接引用備份區塊那兩列的標題（`backup_export` /
 * `backup_import`）：叫使用者去按一個**畫面上真的存在、而且一字不差**的
 * 東西。自己另寫一句「請先匯出資料」的話，那兩行遲早會分岔，而分岔的
 * 症狀是使用者在畫面上找不到我們叫他按的那個東西。
 */
@Composable
private fun MigrationCard(
    controller: UpdateController,
    mig: UpdateController.Migration,
) {
    Card(
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.errorContainer
        ),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(12.dp)) {
            Text(
                text = stringResource(R.string.upgrade_blocked_title),
                fontWeight = FontWeight.SemiBold,
                color = MaterialTheme.colorScheme.onErrorContainer,
            )
            Spacer(Modifier.height(6.dp))
            Text(
                text = stringResource(
                    R.string.upgrade_blocked_why,
                    mig.toPackage,
                    mig.fromPackage,
                ),
                fontSize = 13.sp,
                color = MaterialTheme.colorScheme.onErrorContainer,
            )

            // 沒有宣告「我取代的是你」時語氣要保守：那可能根本不是我們改名，
            // 而是版本資訊的網址指到了別的地方。
            if (!mig.declared) {
                Spacer(Modifier.height(6.dp))
                Text(
                    text = stringResource(R.string.upgrade_blocked_unverified),
                    fontSize = 13.sp,
                    color = MaterialTheme.colorScheme.onErrorContainer,
                )
            }

            Spacer(Modifier.height(8.dp))
            Text(
                text = stringResource(R.string.upgrade_blocked_data_warning),
                fontSize = 13.sp,
                fontWeight = FontWeight.SemiBold,
                color = MaterialTheme.colorScheme.onErrorContainer,
            )

            Spacer(Modifier.height(8.dp))
            listOf(
                stringResource(R.string.upgrade_step_save, stringResource(R.string.backup_export)),
                stringResource(R.string.upgrade_step_get),
                stringResource(R.string.upgrade_step_uninstall),
                stringResource(R.string.upgrade_step_install),
                stringResource(
                    R.string.upgrade_step_restore,
                    stringResource(R.string.backup_import),
                ),
            ).forEach {
                Text(
                    text = it,
                    fontSize = 13.sp,
                    color = MaterialTheme.colorScheme.onErrorContainer,
                    modifier = Modifier.padding(top = 3.dp),
                )
            }

            Spacer(Modifier.height(10.dp))
            OutlinedButton(
                onClick = { controller.openDownloadPage() },
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(stringResource(R.string.upgrade_open_page))
            }
            OutlinedButton(
                onClick = { controller.copyDownloadLink() },
                modifier = Modifier.fillMaxWidth().padding(top = 6.dp),
            ) {
                Text(stringResource(R.string.upgrade_copy_link))
            }
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
