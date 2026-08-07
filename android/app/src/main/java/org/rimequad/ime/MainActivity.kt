package org.rimequad.ime

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.provider.Settings
import android.view.inputmethod.InputMethodManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.rimequad.ime.core.RimeCore
import org.rimequad.ime.core.RimeDeployStatus
import org.rimequad.ime.core.RimeRuntime
import org.rimequad.ime.ui.RimeTheme

/**
 * 設定／診斷畫面，同時也是 method.xml 指定的 settingsActivity。
 *
 * 現階段的職責很窄：告訴人「輸入法有沒有啟用」「native 那條線通了沒」。
 */
class MainActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        RimeRuntime.ensureInitialized(applicationContext)
        setContent {
            RimeTheme {
                Scaffold { padding ->
                    SetupScreen(
                        modifier = Modifier
                            .fillMaxSize()
                            .padding(padding)
                            .padding(16.dp)
                    )
                }
            }
        }
    }
}

@Composable
private fun SetupScreen(modifier: Modifier = Modifier) {
    val context = LocalContext.current
    var deployStatus by remember { mutableStateOf(RimeCore.lastDeployStatus) }

    DisposableEffect(Unit) {
        val listener: (RimeDeployStatus) -> Unit = { deployStatus = it }
        RimeCore.addDeployListener(listener)
        onDispose { RimeCore.removeDeployListener(listener) }
    }

    Column(
        modifier = modifier.verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text(
            text = "RIME 四端輸入法（Android）",
            style = MaterialTheme.typography.headlineSmall,
        )

        if (RimeCore.isStub()) {
            InfoCard(
                title = "⟦STUB⟧ 目前跑的是假實作",
                body = "librime 尚未接上，候選字全是佔位資料。\n" +
                    "third_party/prebuilt/<abi>/lib/librime.a 與 core/src/rime_shell.cc " +
                    "齊備後重新建置，會自動切換成真的。",
            )
        }

        InfoCard(
            title = "原生層狀態",
            body = buildString {
                appendLine("so 載入: ${if (RimeCore.libraryLoaded) "成功" else "失敗 — ${RimeCore.libraryLoadError}"}")
                appendLine("rime_shell ABI（執行期）: ${RimeCore.abiVersion()}")
                appendLine("rime_shell ABI（so 編譯期）: ${RimeCore.compiledAbiVersion()}")
                appendLine("上層要求 ABI: ${RimeCore.EXPECTED_ABI_VERSION}")
                appendLine("ABI 相容: ${if (RimeCore.abiCompatible()) "是" else "否"}")
                appendLine("實作: ${if (RimeCore.isStub()) "stub 假實作" else "真 librime"}")
                appendLine("rs_init: ${if (RimeRuntime.isInitialized) "成功" else "失敗 — ${RimeRuntime.initError}"}")
                append("部署狀態: $deployStatus")
            },
        )

        InfoCard(
            title = "資料目錄",
            body = RimeRuntime.describeDataDirs(),
        )

        val schemas = remember { RimeCore.schemaList() }
        InfoCard(
            title = "Schema（${schemas.size}）",
            body = if (schemas.isEmpty()) {
                "尚無可用 schema"
            } else {
                schemas.joinToString("\n") { "${it.id}  —  ${it.name}" }
            },
        )

        Button(
            onClick = {
                context.startActivity(
                    Intent(Settings.ACTION_INPUT_METHOD_SETTINGS)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                )
            },
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text("① 到系統設定啟用本輸入法")
        }

        Button(
            onClick = {
                val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
                imm.showInputMethodPicker()
            },
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text("② 切換到本輸入法")
        }

        OutlinedButton(
            onClick = { RimeCore.deploy() },
            enabled = RimeRuntime.isInitialized,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text("重新部署（rs_deploy）")
        }
    }
}

@Composable
private fun InfoCard(title: String, body: String) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(14.dp)) {
            Text(text = title, fontWeight = FontWeight.SemiBold)
            Text(
                text = body,
                fontSize = 13.sp,
                fontFamily = FontFamily.Monospace,
                modifier = Modifier.padding(top = 6.dp),
            )
        }
    }
}
