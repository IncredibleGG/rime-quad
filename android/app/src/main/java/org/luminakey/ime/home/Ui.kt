package org.luminakey.ime.home

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.luminakey.ime.R

/*
 * App 這一側的共用零件。
 *
 * 三條規矩，整個 App 都照著走：
 *   · **每屏最多一顆實心按鈕**，其餘一律外框或純文字 —— 不必讀字就知道該點哪裡。
 *   · **一張卡，不是一堆卡**：同一組東西放進同一張卡用 1px 分隔線切開；
 *     不同組之間用留白與小標題分開，不用第二張卡。
 *   · 第二層一定是終點：那一層裡不再有任何 `›`。
 */

/** 小標題（灰、小、字距略開）。 */
@Composable
fun SectionLabel(text: String, modifier: Modifier = Modifier) {
    Text(
        text = text,
        fontSize = 12.5.sp,
        fontWeight = FontWeight.Medium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = modifier.padding(start = 2.dp, bottom = 8.dp),
    )
}

@Composable
fun Hairline(modifier: Modifier = Modifier) {
    Box(
        modifier
            .fillMaxWidth()
            .height(1.dp)
            .background(MaterialTheme.colorScheme.outline)
    )
}

/** 一張卡；裡面的列由呼叫端自己用 [RowDivider] 切開。 */
@Composable
fun PlainCard(
    modifier: Modifier = Modifier,
    content: @Composable ColumnScope.() -> Unit,
) {
    Card(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(16.dp),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
        content = { Column(content = content) },
    )
}

@Composable
fun RowDivider() {
    Box(
        Modifier
            .fillMaxWidth()
            .padding(start = 16.dp)
            .height(1.dp)
            .background(MaterialTheme.colorScheme.outline)
    )
}

/**
 * 第一層的一列：**只有名字，和現在的值**。
 *
 * 刻意沒有說明文字 —— 說明文字是給不確定的人看的，而「注音 · 大千」
 * 「淺色 · 標準」這一行灰字本身就是最好的說明。大多數人來設定只是想
 * 確認一件事，看一眼就走，不必點進去。
 */
@Composable
fun NavRow(
    title: String,
    value: String? = null,
    subtitle: String? = null,
    onClick: () -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .heightIn(min = 58.dp)
            .padding(horizontal = 16.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(
                text = title,
                fontSize = 16.sp,
                fontWeight = FontWeight.Medium,
                // ⚠ 這兩行是在地化之後補的，而且是實測撞出來的。
                //   右邊那行現值以前沒有寬度上限，中文短所以看不出來；換成英文
                //   （“Characters and punctuation follow the keyboard”）之後它把
                //   整列吃光，左邊的「Text」被擠成兩行的「Te / xt」—— 一個標題
                //   從中間斷開，看起來像畫面壞了。名字永遠一行，長了就截。
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            if (subtitle != null) {
                Text(
                    text = subtitle,
                    fontSize = 12.5.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        if (value != null) {
            Text(
                text = value,
                fontSize = 13.5.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                // 現值可以折行，但不能超過一半的寬度 —— 它是補充，名字才是主體。
                textAlign = TextAlign.End,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier
                    .weight(1f, fill = false)
                    .padding(start = 12.dp, end = 8.dp),
            )
        }
        // 這個箭頭走資源，不寫在程式裡：它是**有方向的**字元，RTL 語系要換成
        // 反向的那一個。留在 Kotlin 裡就等於把「往前是往右」寫死。
        Text(
            text = stringResource(R.string.nav_chevron),
            fontSize = 18.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

/** 開關列。用在「鍵上顯示小提示」這種純二選一。 */
@Composable
fun SwitchRow(
    title: String,
    subtitle: String? = null,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 58.dp)
            .padding(horizontal = 16.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(text = title, fontSize = 16.sp, fontWeight = FontWeight.Medium)
            if (subtitle != null) {
                Text(
                    text = subtitle,
                    fontSize = 12.5.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        Switch(checked = checked, onCheckedChange = onCheckedChange)
    }
}

/**
 * 分段控制：**看得到全部選項、當場改完**。
 *
 * 第二層一律用它而不是滑桿或 `›`。滑桿要在數值與預覽之間來回猜；
 * 而使用者要的是「再大一點」，不是 `1.15`。所以這一版的 App 設定畫面上
 * 不出現任何百分比、毫秒或倍數。
 */
@Composable
fun <T> Segmented(
    options: List<Pair<T, String>>,
    selected: T,
    onSelect: (T) -> Unit,
    modifier: Modifier = Modifier,
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(11.dp))
            .background(MaterialTheme.colorScheme.surfaceVariant)
            .padding(3.dp),
        horizontalArrangement = Arrangement.spacedBy(3.dp),
    ) {
        for ((value, label) in options) {
            val on = value == selected
            Box(
                modifier = Modifier
                    .weight(1f)
                    .clip(RoundedCornerShape(9.dp))
                    .background(
                        if (on) MaterialTheme.colorScheme.surface
                        else MaterialTheme.colorScheme.surfaceVariant
                    )
                    .clickable { onSelect(value) }
                    .padding(vertical = 9.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    text = label,
                    fontSize = 13.sp,
                    maxLines = 1,
                    fontWeight = if (on) FontWeight.SemiBold else FontWeight.Normal,
                    color = if (on) MaterialTheme.colorScheme.onSurface
                    else MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

/** 一組「小標題 + 分段控制」，第二層的基本單位。 */
@Composable
fun <T> SettingGroup(
    label: String,
    options: List<Pair<T, String>>,
    selected: T,
    onSelect: (T) -> Unit,
) {
    Column(Modifier.fillMaxWidth().padding(vertical = 8.dp)) {
        SectionLabel(label)
        Segmented(options = options, selected = selected, onSelect = onSelect)
    }
}

/**
 * 「隨手試一下」。
 *
 * 「打出第一個字」不該需要離開 App、去找一個聊天室、再點輸入框。
 * 這一格就是能打字的地方，而且**打壞也沒關係**。
 */
@Composable
fun TryField(
    placeholder: String = stringResource(R.string.try_field_placeholder),
    modifier: Modifier = Modifier,
    focusRequester: FocusRequester? = null,
) {
    var text by remember { mutableStateOf("") }
    OutlinedTextField(
        value = text,
        onValueChange = { text = it },
        placeholder = { Text(placeholder, fontSize = 15.sp) },
        singleLine = false,
        minLines = 2,
        shape = RoundedCornerShape(14.dp),
        modifier = modifier
            .fillMaxWidth()
            .let { if (focusRequester != null) it.focusRequester(focusRequester) else it },
    )
}

/**
 * 單選晶片。用在選項數量不固定的地方（配色有 6 個，而且日後會變多），
 * 分段控制在那種情況下會擠成一團看不懂。
 */
@Composable
fun Chip(
    label: String,
    selected: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Box(
        modifier = modifier
            .clip(RoundedCornerShape(11.dp))
            .background(
                if (selected) MaterialTheme.colorScheme.primaryContainer
                else MaterialTheme.colorScheme.surfaceVariant
            )
            .clickable(onClick = onClick)
            .padding(horizontal = 12.dp, vertical = 11.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = label,
            fontSize = 13.sp,
            maxLines = 1,
            fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Normal,
            color = if (selected) MaterialTheme.colorScheme.onPrimaryContainer
            else MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

/** 分隔線之後的那一行小灰字入口。刻意與上面的列**不等重**。 */
@Composable
fun QuietRow(text: String, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(vertical = 14.dp, horizontal = 2.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = text,
            fontSize = 13.5.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.weight(1f),
        )
        Text(
            text = stringResource(R.string.nav_chevron),
            fontSize = 16.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

/** 第二層頁面的頂端導覽列。**每一個第二層都有它**，出口不靠使用者記得按返回鍵。 */
@Composable
fun PageHeader(title: String, onBack: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 52.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        TextButton(onClick = onBack) { Text(stringResource(R.string.back), fontSize = 15.sp) }
        Spacer(Modifier.padding(horizontal = 2.dp))
        Text(text = title, fontSize = 17.sp, fontWeight = FontWeight.SemiBold)
    }
}
