#include "ui_strings.h"

#include <cstddef>

namespace rimewin {
namespace {

// ── ⚠ 為什麼是 X 巨集,而不是三個並排的陣列 ──────────────────────
//
// §12.9.1 建議「每個語系一個陣列 + static_assert 長度相同」。長度相同
// 擋得住「少一條」,但**擋不住錯位**:在中間插一條而只補了兩個語系,
// 三個陣列的長度可以完全一樣,而第 57 條之後全部往下錯一格。
// 症狀是使用者切到英文之後,「刪掉全部」這一顆的說明變成別人的。
//
// X 巨集讓三個語系寫在**同一行**上,錯位在字面上不可能發生;
// 再加上下面那個 constexpr 的順序檢查,enum 與清單一旦分岔就**編不過**。
// 這是 §2-B2 誇 macOS 的那種「型別強制」,在 C++ 裡的等價物。
//
// 一行一條,`grep -c 'X(k'` 就是條數 —— §12.9.1 第 4 條要的掃描友善也還在。
//
// 欄位順序:enum 名, en-US, zh-Hant, zh-Hans
//
// ⚠ 禁用字(§6.7 第一層)一個都不准出現在下面:
//   rs_*／schema_list／page_size／simplification／ascii_punct／preedit／
//   librime／TSF／langid／部署／上屏／配置／候選字／詞庫／任何 id。
//   W9 在掃這件事,而 catalog 以外的地方由 W7 掃。
//
// ⚠ `中`／`En`／`简`／`繁` **不在**下面(§12.9.3 第 1 條)。它們是
//   §8.12 規範性的四端一致字面,直接寫在狀態列的繪製碼裡。W10 兩頭驗。

#define RIMEWIN_UI_STRINGS(X)                                                  \
  /* ── 視窗與側欄 ────────────────────────────────────────────── */          \
  X(kWindowTitle, L"LuminaKey Input Method Settings",                          \
    L"LuminaKey 輸入法 設定", L"LuminaKey 输入法 设置")                        \
  X(kNavSchemas, L"Typing method", L"輸入方案", L"输入方案")                   \
  X(kNavAppearance, L"Appearance", L"外觀", L"外观")                           \
  X(kNavText, L"Text", L"文字", L"文字")                                       \
  X(kNavAdvanced, L"Advanced", L"進階", L"高级")                               \
  X(kNavStatusReady, L"Ready to type", L"可以打字", L"可以打字")               \
  X(kNavStatusPreparing,                                                       \
    L"Getting ready - the first start takes a moment",                         \
    L"正在準備 —— 第一次啟動要等一下",                                         \
    L"正在准备 —— 第一次启动要等一下")                                         \
  X(kNavStatusPrepareFailed, L"Could not finish getting the words ready",      \
    L"字詞沒有整理成功", L"字词没有整理成功")                                  \
  X(kNavStatusNotRunning, L"Input method is not running", L"輸入法沒有在跑",   \
    L"输入法没有在跑")                                                         \
  X(kNavStatusOffline, L"Offline", L"離線", L"离线")                           \
  X(kClose, L"Close", L"關閉", L"关闭")                                        \
  /* ── 輸入方案 ──────────────────────────────────────────────── */          \
  X(kSchemasTitle, L"Typing method", L"輸入方案", L"输入方案")                 \
  X(kSchemasSubtitle,                                                          \
    L"Which ways of typing Chinese you can switch between.",                   \
    L"你可以用哪幾種方式打中文,以及切換的順序。",                             \
    L"你可以用哪几种方式打中文,以及切换的顺序。")                             \
  X(kSchemasListHeading, L"Turned on", L"啟用的方式", L"启用的方式")           \
  X(kSchemasListBlurb,                                                         \
    L"The order here is the switching order. Press Ctrl+` while typing to "    \
    L"switch.",                                                                \
    L"這個順序就是切換的順序。打字時按 Ctrl+` 或 F4 可以隨時換。",             \
    L"这个顺序就是切换的顺序。打字时按 Ctrl+` 或 F4 可以随时换。")             \
  X(kSchemasDefaultBadge, L"Default", L"預設", L"默认")                        \
  X(kSchemasCurrentDefaultPrefix, L"Right now the default is ",                \
    L"現在預設是", L"现在默认是")                                              \
  X(kSchemasMoveUp, L"Move up", L"上移", L"上移")                              \
  X(kSchemasMoveDown, L"Move down", L"下移", L"下移")                          \
  X(kSchemasApplyOrder, L"Use this order", L"套用這個順序", L"套用这个顺序")   \
  X(kSchemasFollowTitle,                                                       \
    L"Pick the typing method to match the language I chose",                   \
    L"跟著我選的輸入法語言,自動挑一種",                                       \
    L"跟着我选的输入法语言,自动挑一种")                                       \
  X(kSchemasFollowBlurb,                                                       \
    L"Pick Traditional in the language menu and you get a Traditional way of " \
    L"typing; pick Simplified and you get a Simplified one.",                  \
    L"你在系統裡選「繁體中文」就用繁體的那一種,選「簡體中文」就用簡體的。"    \
    L"不打勾的話,一律用你排在最前面的那一種。",                               \
    L"你在系统里选“繁体中文”就用繁体的那一种,选“简体中文”就用简体的。"       \
    L"不打勾的话,一律用你排在最前面的那一种。")                               \
  X(kSchemasEmptyTitle, L"No typing methods yet", L"目前一種都沒有",           \
    L"目前一种都没有")                                                         \
  X(kSchemasEmptyWhy,                                                          \
    L"This usually means the word data has not finished building yet.",        \
    L"多半是字詞還沒整理完 —— 第一次啟動要花十幾秒到幾分鐘。",                 \
    L"多半是字词还没整理完 —— 第一次启动要花十几秒到几分钟。")                 \
  X(kSchemasEmptyNext, L"Go to Advanced and press Rebuild words.",             \
    L"到「進階」按一下「重新整理字詞」。",                                     \
    L"到“高级”按一下“重新整理字词”。")                                         \
  X(kSchemasOrderChangedHint,                                                  \
    L"Order changed but not in use yet - press Use this order.",               \
    L"順序改了,還沒生效 —— 按「套用這個順序」。",                             \
    L"顺序改了,还没生效 —— 按“套用这个顺序”。")                               \
  /* ── 外觀 ──────────────────────────────────────────────────── */          \
  X(kAppearanceTitle, L"Appearance", L"外觀", L"外观")                         \
  X(kAppearanceSubtitle, L"How the window that picks characters looks.",       \
    L"你打字時跳出來的那個小窗,長什麼樣子。",                                 \
    L"你打字时跳出来的那个小窗,长什么样子。")                                 \
  X(kCountHeading, L"How many characters to show at once",                     \
    L"一次顯示幾個字", L"一次显示几个字")                                      \
  X(kCountBlurb,                                                               \
    L"More choices means less pressing of the next-page key, but a wider "     \
    L"window.",                                                                \
    L"多一點就少按幾次翻頁,但那個小窗會變寬。",                               \
    L"多一点就少按几次翻页,但那个小窗会变宽。")                               \
  X(kScaleHeading, L"Text size in that window", L"那個小窗的字大小",           \
    L"那个小窗的字大小")                                                       \
  X(kScaleBlurb, L"Takes effect right away, next time the window pops up.",    \
    L"改了立刻生效,下一次跳出來就看得到。",                                   \
    L"改了立刻生效,下一次跳出来就看得到。")                                   \
  X(kThemeHeading, L"Light or dark", L"淺色還是深色", L"浅色还是深色")         \
  X(kThemeBlurb,                                                               \
    L"Follow system means it changes when Windows changes.",                   \
    L"選「跟著系統」的話,Windows 換的時候這裡也跟著換。",                     \
    L"选“跟着系统”的话,Windows 换的时候这里也跟着换。")                       \
  X(kThemeFollowSystem, L"Follow system", L"跟著系統", L"跟着系统")            \
  X(kThemeLight, L"Light", L"淺色", L"浅色")                                   \
  X(kThemeDark, L"Dark", L"深色", L"深色")                                     \
  X(kStatusBarHeading, L"The little bar", L"那一小橫", L"那一小横")            \
  X(kStatusBarBlurb,                                                           \
    L"A small bar you can drag anywhere. It is the only place to switch "      \
    L"between Chinese and English mid-sentence.",                              \
    L"可以拖到任何地方的一小橫。**在句子中間切中英文,目前只有它做得到。**",   \
    L"可以拖到任何地方的一小横。**在句子中间切中英文,目前只有它做得到。**")   \
  X(kStatusBarShow, L"Show the little bar", L"顯示那一小橫",                   \
    L"显示那一小横")                                                           \
  X(kAppearanceHonestNote,                                                     \
    L"How many characters to show is decided by the engine, so it needs a "    \
    L"rebuild. Hiding them on screen alone would not work: the number keys "   \
    L"would still pick the ones you cannot see.",                              \
    L"「一次顯示幾個字」要重新整理字詞之後才會變(按下去時會問你)。"           \
    L"它不能只改畫面 —— 只藏起來的話,你按數字鍵仍然會選到看不見的那幾個。",   \
    L"“一次显示几个字”要重新整理字词之后才会变(按下去时会问你)。"           \
    L"它不能只改画面 —— 只藏起来的话,你按数字键仍然会选到看不见的那几个。")   \
  /* ── 文字 ──────────────────────────────────────────────────── */          \
  X(kTextTitle, L"Text", L"文字", L"文字")                                     \
  X(kTextSubtitle, L"Which characters and punctuation come out.",              \
    L"打出來的是哪一種字、哪一種標點。",                                       \
    L"打出来的是哪一种字、哪一种标点。")                                       \
  X(kVariantHeading, L"Traditional or Simplified characters",                  \
    L"打出繁體字還是簡體字", L"打出繁体字还是简体字")                          \
  X(kVariantBlurb,                                                             \
    L"Changing this applies to every window you already have open.",           \
    L"改了之後,你已經開著的每一個程式都立刻跟著變。",                         \
    L"改了之后,你已经开着的每一个程序都立刻跟着变。")                         \
  X(kVariantFollow, L"Follow the language this input method sits under",       \
    L"跟著輸入法所在的語言", L"跟着输入法所在的语言")                          \
  X(kVariantTraditional, L"Traditional", L"繁體字", L"繁体字")                 \
  X(kVariantSimplified, L"Simplified", L"簡體字", L"简体字")                   \
  X(kPunctHeading, L"Which comma and full stop", L"逗號句號的樣子",            \
    L"逗号句号的样子")                                                         \
  X(kPunctBlurb,                                                               \
    L"Chinese punctuation is wide, English is narrow. People who write code "  \
    L"usually want the English one.",                                          \
    L"中文標點是「,。」,英文標點是「, .」。寫程式的人通常要英文的。",         \
    L"中文标点是“,。”,英文标点是“, .”。写程序的人通常要英文的。")            \
  X(kPunctFollow, L"Leave it alone", L"不干預", L"不干预")                     \
  X(kPunctChinese, L"Chinese  ,  。", L"中文  ,  。", L"中文  ,  。")          \
  X(kPunctEnglish, L"English  ,  .", L"英文  ,  .", L"英文  ,  .")             \
  X(kTextHonestNote,                                                           \
    L"This typing method only produces Simplified characters. To type "        \
    L"Traditional, switch to a Traditional one.",                              \
    L"這個打字方式只有簡體字。要打繁體字,請換一個繁體的打字方式。",           \
    L"这个打字方式只有简体字。要打繁体字,请换一个繁体的打字方式。")           \
  X(kTextHonestAction, L"Change typing method", L"去換打字方式",               \
    L"去换打字方式")                                                           \
  /* ── 進階 ──────────────────────────────────────────────────── */          \
  X(kAdvancedTitle, L"Advanced", L"進階", L"高级")                             \
  X(kAdvancedSubtitle, L"Rebuilding words, your files, and diagnostics.",      \
    L"整理字詞、你的檔案,以及診斷。",                                         \
    L"整理字词、你的文件,以及诊断。")                                         \
  X(kRedeployHeading, L"Rebuild words", L"重新整理字詞", L"重新整理字词")      \
  X(kRedeployBlurb,                                                            \
    L"Do this after you change files by hand, or if typing stops finding "     \
    L"words. Takes ten seconds or so.",                                        \
    L"你手動改過檔案、或是打字時找不到詞了,就按一下。約十幾秒。",             \
    L"你手动改过文件、或是打字时找不到词了,就按一下。约十几秒。")             \
  X(kRedeployButton, L"Rebuild words", L"重新整理字詞", L"重新整理字词")       \
  X(kFilesHeading, L"Your files", L"你的檔案", L"你的文件")                    \
  X(kFilesBlurb,                                                               \
    L"Words you add and your settings live here. Backing up this folder "      \
    L"backs up everything you have taught it.",                                \
    L"你自己加的詞與設定都放在這裡。備份這個資料夾就備份了你教過它的一切。",   \
    L"你自己加的词与设置都放在这里。备份这个文件夹就备份了你教过它的一切。")   \
  X(kOpenUserDir, L"Open my folder", L"開啟我的資料夾", L"打开我的文件夹")     \
  X(kOpenSettingsFile, L"Open the settings file", L"開啟設定檔",               \
    L"打开设置文件")                                                           \
  X(kLanguageHeading, L"Language of this window", L"這個視窗的語言",           \
    L"这个窗口的语言")                                                         \
  X(kLanguageBlurb,                                                            \
    L"Only changes the words in this window. It does not change what you "     \
    L"type.",                                                                  \
    L"只改這個視窗上的字,不會改變你打出來的東西。",                           \
    L"只改这个窗口上的字,不会改变你打出来的东西。")                           \
  X(kLanguageSystem, L"Follow system", L"跟著系統", L"跟着系统")               \
  X(kLanguageEnglish, L"English", L"English", L"English")                      \
  X(kLanguageHant, L"Traditional Chinese", L"正體中文", L"繁体中文")           \
  X(kLanguageHans, L"Simplified Chinese", L"簡體中文", L"简体中文")            \
  X(kDiagnosticsHeading, L"Diagnostics", L"診斷", L"诊断")                     \
  X(kDiagnosticsNote,                                                          \
    L"These numbers are for us. You do not need to understand them - just "    \
    L"copy them into your report.",                                            \
    L"這些數字是給我們看的,你不用懂 —— 回報問題時複製過去就好。",             \
    L"这些数字是给我们看的,你不用懂 —— 回报问题时复制过去就好。")             \
  X(kDiagnosticsCopy, L"Copy", L"複製", L"复制")                               \
  X(kDiagnosticsCopied, L"Copied.", L"複製好了。", L"复制好了。")              \
  X(kResetHeading, L"Put the settings back to how they started", \
    L"把設定回復成一開始的樣子", L"把设置回复成一开始的样子") \
  X(kResetBlurb, \
    L"Everything on these four pages goes back to the way it shipped. " \
    L"Words you have added and the order you set are not touched.", \
    L"這四頁上的每一項都回到出廠的樣子。**你自己加的詞不會消失**," \
    L"你排的順序也不會變。", \
    L"这四页上的每一项都回到出厂的样子。**你自己加的词不会消失**," \
    L"你排的顺序也不会变。") \
  X(kResetButton, L"Put the settings back", L"把設定回復成預設", \
    L"把设置回复成默认") \
  X(kResetConfirmBody, \
    L"These four pages go back to how they started. What will NOT " \
    L"disappear: the words you have added, the order of typing methods, " \
    L"and anything you typed. This cannot be undone.", \
    L"這四頁上的設定會回到一開始的樣子。**不會消失的是**:你自己加的詞、" \
    L"打字方式的順序、以及你打過的任何東西。這件事沒辦法還原。", \
    L"这四页上的设置会回到一开始的样子。**不会消失的是**:你自己加的词、" \
    L"打字方式的顺序、以及你打过的任何东西。这件事没办法还原。") \
  X(kStatusResetDone, L"Settings are back to how they started.", \
    L"設定已經回復成一開始的樣子。", L"设置已经回复成一开始的样子。") \
  /* ── 共用的值 ──────────────────────────────────────────────── */          \
  X(kValueFollowSchema, L"Leave it alone", L"不干預", L"不干预")               \
  X(kCountThree, L"3", L"3 個", L"3 个")                                       \
  X(kCountFive, L"5", L"5 個", L"5 个")                                        \
  X(kCountSeven, L"7", L"7 個", L"7 个")                                       \
  X(kCountNine, L"9", L"9 個", L"9 个")                                        \
  X(kScaleSmall, L"Small", L"小", L"小")                                       \
  X(kScaleNormal, L"Normal", L"標準", L"标准")                                 \
  X(kScaleLarge, L"Large", L"大", L"大")                                       \
  X(kScaleHuge, L"Very large", L"很大", L"很大")                               \
  /* ── 確認對話框 ────────────────────────────────────────────── */          \
  X(kCancel, L"Cancel", L"取消", L"取消")                                      \
  X(kQuitServiceTitle, L"Stop the input method", L"結束輸入法服務",            \
    L"结束输入法服务")                                                         \
  X(kQuitServiceBody,                                                          \
    L"It will start again by itself the next time you type. Words you have "   \
    L"already added stay where they are.",                                     \
    L"下一次打字時它會自動再啟動。你已經加過的詞不會消失,設定也不會。",       \
    L"下一次打字时它会自动再启动。你已经加过的词不会消失,设置也不会。")       \
  X(kQuitServiceConfirm, L"Stop it now", L"現在結束它", L"现在结束它")         \
  X(kApplyCountTitle, L"How many characters to show at once",                  \
    L"一次顯示幾個字", L"一次显示几个字")                                      \
  X(kApplyCountBody,                                                           \
    L"This one needs the words rebuilt before it changes - about ten "         \
    L"seconds. Nothing you typed is lost.",                                    \
    L"這一項要重新整理字詞之後才會變,約十幾秒。你打過的東西不會受影響。",     \
    L"这一项要重新整理字词之后才会变,约十几秒。你打过的东西不会受影响。")     \
  X(kApplyCountConfirm, L"Rebuild now", L"現在就整理", L"现在就整理")          \
  X(kApplyCountLater, L"Later", L"晚點再說", L"晚点再说")                      \
  /* ── 視窗底部那一行 ────────────────────────────────────────── */          \
  X(kStatusApplied, L"Done.", L"已套用。", L"已套用。")                        \
  X(kStatusSaveFailed,                                                         \
    L"Could not save - the change works now but will not survive a restart.",  \
    L"設定存不起來 —— 現在有效,但不會留到下次開機。",                         \
    L"设置存不起来 —— 现在有效,但不会留到下次开机。")                         \
  X(kStatusOrderNotApplied, L"The order was not applied.",                     \
    L"順序沒有套用。", L"顺序没有套用。")                                      \
  X(kStatusRedeployRunning, L"Rebuilding words... %u seconds so far",          \
    L"正在整理字詞…已耗時 %u 秒", L"正在整理字词…已耗时 %u 秒")                \
  X(kStatusRedeployDone, L"%s finished in %.1f seconds.",                      \
    L"%s完成(耗時 %.1f 秒)。", L"%s完成(耗时 %.1f 秒)。")                  \
  X(kStatusRedeployFailed, L"Rebuilding failed; your settings were restored.", \
    L"整理字詞失敗,設定已還原。", L"整理字词失败,设置已还原。")               \
  X(kStatusFollowOn, L"It will follow the language you choose.",               \
    L"會跟著你選的輸入法語言自動挑。", L"会跟着你选的输入法语言自动挑。")      \
  X(kStatusFollowOff,                                                          \
    L"It will always use the one at the top; takes effect in new windows.",    \
    L"一律用排在最前面的那一種,下次開新的視窗時生效。",                       \
    L"一律用排在最前面的那一种,下次开新的窗口时生效。")                       \
  X(kStatusPunctFollow, L"Back to leaving it alone; new windows will use it.", \
    L"改回不干預,下次開新的視窗時生效。",                                     \
    L"改回不干预,下次开新的窗口时生效。")                                     \
  X(kStatusDefaultCleared,                                                     \
    L"No longer pinned; it will choose again in new windows.",                 \
    L"取消指定,下次開新的視窗時重新挑。",                                     \
    L"取消指定,下次开新的窗口时重新挑。")                                     \
  X(kStatusScaleApplied, L"Done - you will see it next time it pops up.",      \
    L"已套用,下一次跳出來就看得到。", L"已套用,下一次跳出来就看得到。")      \
  /* ── 重新整理字詞的失敗訊息 ────────────────────────────────── */          \
  X(kRedeployFailTitle, L"Rebuilding words", L"重新整理字詞",                  \
    L"重新整理字词")                                                           \
  X(kRedeployFailNoReason,                                                     \
    L"The engine did not say why. The usual cause is a missing word file for " \
    L"one of the typing methods.",                                             \
    L"引擎沒有給原因。常見的成因是某一種打字方式的字詞檔案缺席。",             \
    L"引擎没有给原因。常见的成因是某一种打字方式的字词文件缺席。")             \
  X(kRedeployFailRolledBack, L"Your settings were put back the way they were.",\
    L"設定已經還原成改之前的樣子。", L"设置已经还原成改之前的样子。")          \
  X(kRedeployFailRollbackFailed,                                               \
    L"Putting them back failed too. Please check the file by hand.",           \
    L"而且還原也失敗了。請手動檢查你的資料夾裡那個設定檔。",                   \
    L"而且还原也失败了。请手动检查你的文件夹里那个设置文件。")                 \
  X(kRedeployRefused, L"Did not start: ", L"沒有開始整理:", L"没有开始整理:") \
  X(kOrderPatchUnreadable,                                                     \
    L"I cannot read your settings file, so nothing was changed. Please edit "  \
    L"it by hand.",                                                            \
    L"你的設定檔我看不懂,為了不弄壞你改過的東西,這次什麼都沒動 ——"           \
    L"請手動編輯那個檔案。",                                                   \
    L"你的设置文件我看不懂,为了不弄坏你改过的东西,这次什么都没动 ——"         \
    L"请手动编辑那个文件。")                                                   \
  X(kOrderPatchInvalid,                                                        \
    L"The list has an entry I do not recognise; nothing was changed.",         \
    L"清單裡有不合法的項目,這次什麼都沒動。",                                 \
    L"列表里有不合法的项目,这次什么都没动。")                                 \
  X(kOrderPatchWriteFailed, L"Could not write the settings file.",             \
    L"設定檔寫不進去。", L"设置文件写不进去。")                                \
  X(kOrderPatchMissing,                                                        \
    L"The settings file is missing. It is copied into your folder the first "  \
    L"time this runs.",                                                        \
    L"找不到那個設定檔。它會在第一次啟動時複製到你的資料夾裡。",               \
    L"找不到那个设置文件。它会在第一次启动时复制到你的文件夹里。")             \
  /* ── 系統匣 ────────────────────────────────────────────────── */          \
  X(kTraySettings, L"Settings...", L"設定(&S)…", L"设置(&S)…")                 \
  X(kTrayRedeploy, L"Rebuild words", L"重新整理字詞(&R)", L"重新整理字词(&R)") \
  X(kTrayQuit, L"Stop the input method", L"結束輸入法服務(&X)",                \
    L"结束输入法服务(&X)")                                                     \
  X(kTrayTip, L"LuminaKey Input Method", L"LuminaKey 輸入法",                  \
    L"LuminaKey 输入法")                                                       \
  /* ── 語言列 ────────────────────────────────────────────────── */          \
  X(kLangBarButton, L"Settings", L"設定", L"设置")                             \
  X(kLangBarTooltip, L"LuminaKey Input Method settings",                       \
    L"LuminaKey 輸入法設定", L"LuminaKey 输入法设置")                          \
  X(kLangBarNotRunning, L"Not running", L"未啟動", L"未启动")                  \
  /* ── 懸浮狀態列 ────────────────────────────────────────────── */          \
  X(kBarSettings, L"Settings", L"設定", L"设置")                               \
  X(kBarPreparing, L"Getting ready, one moment", L"正在準備,馬上就好",        \
    L"正在准备,马上就好")                                                     \
  X(kBarPrepareFailed, L"The words are not ready - click here",                \
    L"字詞沒有整理完 —— 點這裡", L"字词没有整理完 —— 点这里")                  \
  X(kBarNotRunning, L"Input method is not running", L"輸入法沒有在跑",         \
    L"输入法没有在跑")                                                         \
  X(kBarPickSchema, L"Choose a typing method", L"選一種打字方式",              \
    L"选一种打字方式")                                                         \
  /* ── 提權政策 ──────────────────────────────────────────────── */          \
  X(kElevatedBlocked,                                                          \
    L"LuminaKey: this window runs as an administrator, so the input method "   \
    L"will not start from here. That is deliberate - it would leave the "      \
    L"words you add owned by the administrator. Type one character in an "     \
    L"ordinary window first and this one will work too.",                      \
    L"LuminaKey:這個視窗是以系統管理員身分執行的,輸入法服務不會從這裡啟動"    \
    L"(這是刻意的:那會把你自己加的詞換成系統管理員所有)。"                   \
    L"請在一般的視窗裡先打一個字,服務起來之後這個視窗也能用。",               \
    L"LuminaKey:这个窗口是以系统管理员身分执行的,输入法服务不会从这里启动"    \
    L"(这是刻意的:那会把你自己加的词换成系统管理员所有)。"                   \
    L"请在一般的窗口里先打一个字,服务起来之后这个窗口也能用。")               \
  X(kNotUserBlocked,                                                           \
    L"LuminaKey: this process does not run as you, so the input method will "  \
    L"not start from here.",                                                   \
    L"LuminaKey:這個程式不是以你的身分執行的,輸入法服務不會從這裡啟動。",     \
    L"LuminaKey:这个程序不是以你的身分执行的,输入法服务不会从这里启动。")     \
  X(kUnknownTokenBlocked,                                                      \
    L"LuminaKey: I could not tell what permissions this program has, so to "   \
    L"be safe the input method was not started. Run the setup tool's doctor "  \
    L"for a diagnosis.",                                                       \
    L"LuminaKey:問不出這個程式的權限狀態,保守起見沒有啟動輸入法服務。"        \
    L"請執行安裝工具的 doctor 取得診斷。",                                     \
    L"LuminaKey:问不出这个程序的权限状态,保守起见没有启动输入法服务。"        \
    L"请执行安装工具的 doctor 取得诊断。")                                     \
  /* ── 關於 ──────────────────────────────────────────────────── */          \
  X(kAboutOffline,                                                             \
    L"This input method is offline by default: the network switch starts "  \
    L"off, and while it is off not one byte goes out. Every connection that "  \
    L"really happens is written into the log on the Network page.",            \
    L"這個輸入法預設離線:連網開關一開始是關的,關著的時候一個位元組都不會"     \
    L"送出去。每一次真的連過網,都寫在「連網」那一頁的紀錄裡。",                \
    L"这个输入法默认离线:连网开关一开始是关的,关着的时候一个字节都不会"       \
    L"送出去。每一次真的连过网,都写在“连网”那一页的纪录里。")                  \
  /* ── 連網 ──────────────────────────────────────────────────── */          \
  X(kNavNetwork, L"Network", L"連網", L"连网")                                 \
  X(kNetworkTitle, L"Network", L"連網", L"连网")                               \
  X(kNetworkSubtitle,                                                          \
    L"When this may go online, and every time it really did.",                 \
    L"這個輸入法什麼時候會連上網路,以及每一次真的連過的紀錄。",                \
    L"这个输入法什么时候会连上网络,以及每一次真的连过的纪录。")                \
  X(kNetworkSwitch, L"Network", L"連網", L"连网")                              \
  X(kNetworkOnSummary,                                                         \
    L"On: it can go online, and only when you press a button that says so. "   \
    L"What you type never leaves this computer.",                              \
    L"已開啟:現在可以連網,而且只有你按下明說要連的那顆按鈕時才會。"           \
    L"你打的字永遠不會離開這台電腦。",                                          \
    L"已开启:现在可以连网,而且只有你按下明说要连的那颗按钮时才会。"           \
    L"你打的字永远不会离开这台电脑。")                                          \
  X(kNetworkOffSummary,                                                        \
    L"Off (the default): fully offline. Nothing reaches the internet, and "    \
    L"the buttons that would need it say so instead of failing quietly.",      \
    L"已關閉(預設):完全離線。什麼都不會連出去;需要連網的按鈕會直接告訴你"    \
    L"開關是關的,而不是安靜地失敗。",                                          \
    L"已关闭(默认):完全离线。什么都不会连出去;需要连网的按钮会直接告诉你"    \
    L"开关是关的,而不是安静地失败。")                                          \
  X(kNetworkOnDetail,                                                          \
    L"The honest cost: the moment a connection really happens, your network "  \
    L"learns that this computer reached a domain. Our requests do not carry "  \
    L"this program's name, but which domain was reached travels in the clear " \
    L"by design. We cannot hide that and will not pretend otherwise.",         \
    L"誠實的代價:只要真的連了一次,你的網路(電信業者、這個無線網路的主人、"   \
    L"中間的任何人)就會知道這台電腦連到了某個網域。我們送出去的請求不帶這個"  \
    L"程式的名字,但「連到哪個網域」在設計上就是明文的 —— 我們藏不了,"        \
    L"也不會假裝藏得了。",                                                      \
    L"诚实的代价:只要真的连了一次,你的网络(电信业者、这个无线网络的主人、"   \
    L"中间的任何人)就会知道这台电脑连到了某个网域。我们送出去的请求不带这个"  \
    L"程序的名字,但“连到哪个网域”在设计上就是明文的 —— 我们藏不了,"          \
    L"也不会假装藏得了。")                                                      \
  X(kNetworkTurnedOn, L"Network is on now.", L"連網已經打開。",                \
    L"连网已经打开。")                                                          \
  X(kNetworkTurnedOff, L"Network is off - fully offline again.",               \
    L"連網已經關掉 —— 又回到完全離線。",                                        \
    L"连网已经关掉 —— 又回到完全离线。")                                        \
  X(kNetLogHeading, L"Connection log", L"連網紀錄", L"连网纪录")                \
  X(kNetLogBlurb,                                                              \
    L"Every connection that really happened is in here, one line each, "       \
    L"redirect hops included. Only the host name and the reason - nothing "    \
    L"you typed, and no part of the address after the host.",                  \
    L"每一次真的連過網都在裡面,轉址的每一跳也各記一筆。"                       \
    L"裡面只有主機名與原因 —— 沒有你打過的任何東西,"                           \
    L"也沒有網址主機後面那一段。",                                              \
    L"每一次真的连过网都在里面,转址的每一跳也各记一笔。"                       \
    L"里面只有主机名与原因 —— 没有你打过的任何东西,"                           \
    L"也没有网址主机后面那一段。")                                              \
  X(kNetLogColumns, L"Time  ·  Host  ·  Reason  ·  Result",                    \
    L"時間  ·  主機  ·  原因  ·  結果", L"时间  ·  主机  ·  原因  ·  结果")     \
  X(kNetLogCount, L"%d connections, newest first.",                            \
    L"共 %d 次連線,由新到舊。", L"共 %d 次连线,由新到旧。")                    \
  X(kNetLogEmptyTitle, L"Nothing has ever connected.", L"一次都沒有連過。",     \
    L"一次都没有连过。")                                                        \
  X(kNetLogEmptyWhy,                                                           \
    L"If the switch has never been on, this is supposed to be empty - that "   \
    L"is exactly the result you came here to see. The log file itself has "    \
    L"not been created either.",                                               \
    L"開關從來沒開過的話,這裡本來就會是空的 —— 這正是你要看到的結果。"        \
    L"紀錄檔本身也還沒有被建立。",                                              \
    L"开关从来没开过的话,这里本来就会是空的 —— 这正是你要看到的结果。"        \
    L"纪录文件本身也还没有被建立。")                                            \
  X(kNetLogFileLine, L"Log file: ", L"紀錄檔:", L"纪录文件:")                 \
  X(kNetLogClearHeading, L"Clear the connection log", L"清除連網紀錄",          \
    L"清除连网纪录")                                                            \
  X(kNetLogClearBlurb,                                                         \
    L"Once cleared, the lines above cannot be brought back. Your settings "    \
    L"and the words you added are not touched.",                               \
    L"清掉之後,上面那些紀錄就找不回來了。你的設定與你自己加的詞不受影響。",     \
    L"清掉之后,上面那些纪录就找不回来了。你的设置与你自己加的词不受影响。")     \
  X(kNetLogClearButton, L"Clear the connection log", L"清除連網紀錄",           \
    L"清除连网纪录")                                                            \
  X(kNetLogCleared, L"The connection log is empty now.",                       \
    L"連網紀錄已經清空。", L"连网纪录已经清空。")                               \
  X(kNetPurposeIndex, L"Browsing the store", L"瀏覽市集", L"浏览市集")          \
  X(kNetPurposePackage, L"Downloading a typing method", L"下載打字方式",        \
    L"下载打字方式")                                                            \
  X(kNetOutcomeOk, L"succeeded", L"成功", L"成功")                             \
  X(kNetOutcomeFailed, L"failed", L"失敗", L"失败")                            \
  X(kNetOutcomeRedirected, L"redirected", L"轉址", L"转址")                     \
  X(kUpdateHeading,                                                           \
    L"Updates",                                                               \
    L"更新",                                                                    \
    L"更新")                                                                    \
  X(kUpdateBlurb,                                                             \
    L"See whether a newer version exists. If you update, we download the "    \
    L"same installer that sits on the download page and hand over to it "     \
    L"- exactly what happens when you fetch it yourself.",                    \
    L"看看有沒有新版本。要更新的話,我們會下載下載頁上的同一支安裝程式,然後把事情交給它 —— 跟你自己去下載一次完全一樣。",           \
    L"看看有没有新版本。要更新的话,我们会下载下载页上的同一支安装程序,然后把事情交给它 —— 跟你自己去下载一次完全一样。")           \
  X(kUpdateTrustAnchor,                                                       \
    L"This program carries no signature. The only thing protecting the "      \
    L"download is the encrypted connection itself. We do compare a checksum " \
    L"afterwards, but that catches a cut-off or damaged download - not "      \
    L"someone who can swap the file and the checksum together. You can "      \
    L"always fetch it from the download page yourself instead.",              \
    L"這個程式沒有數位簽章。保護這次下載的只有加密連線本身。下載完我們會核對摘要,但那擋得住斷線與壞掉的檔案,擋不住一個能同時換掉檔案"       \
    L"與摘要的人。你也可以自己去下載頁拿,結果一樣。",                                               \
    L"这个程序没有数字签名。保护这次下载的只有加密连线本身。下载完我们会核对摘要,但那挡得住断线与坏掉的文件,挡不住一个能同时换掉文件"       \
    L"与摘要的人。你也可以自己去下载页拿,结果一样。")                                               \
  X(kUpdateWhatHappens,                                                       \
    L"While it updates: the input method stops, the files are replaced, "     \
    L"and it starts again by itself. No restart, no signing out. Programs "   \
    L"you already have open pick up the new one the next time you open "      \
    L"them.",                                                                 \
    L"更新的時候:輸入法會先停下來,換掉檔案,然後自己起來。不必重新開機,也不必登出。已經開著的程式要等你下一次開它,才會換成新的。",       \
    L"更新的时候:输入法会先停下来,换掉文件,然后自己起来。不必重新开机,也不必登出。已经开着的程序要等你下一次开它,才会换成新的。")       \
  X(kUpdateCheckButton,                                                       \
    L"Check for a newer version",                                             \
    L"看看有沒有新版本",                                                              \
    L"看看有没有新版本")                                                              \
  X(kUpdateInstallButton,                                                     \
    L"Download and update",                                                   \
    L"下載並更新",                                                                 \
    L"下载并更新")                                                                 \
  X(kUpdateInstallNowButton,                                                  \
    L"Update now",                                                            \
    L"現在更新",                                                                  \
    L"现在更新")                                                                  \
  X(kUpdateOpenPageButton,                                                    \
    L"Open the download page",                                                \
    L"開啟下載頁",                                                                 \
    L"打开下载页")                                                                 \
  X(kUpdateStatusIdle,                                                        \
    L"Not checked yet.",                                                      \
    L"還沒查過。",                                                                 \
    L"还没查过。")                                                                 \
  X(kUpdateStatusChecking,                                                    \
    L"Checking...",                                                           \
    L"正在查…",                                                                  \
    L"正在查…")                                                                  \
  X(kUpdateStatusUpToDate,                                                    \
    L"You already have the newest one.",                                      \
    L"你已經是最新的了。",                                                             \
    L"你已经是最新的了。")                                                             \
  X(kUpdateStatusAvailable,                                                   \
    L"There is a newer version: %s",                                          \
    L"有新版本:%s",                                                               \
    L"有新版本:%s")                                                               \
  X(kUpdateStatusDowngrade,                                                   \
    L"The one online is older than the one you have, so there is nothing "    \
    L"to install. That means something went wrong where we publish, not "     \
    L"here.",                                                                 \
    L"線上那一份比你手上這一份還舊,所以沒有東西可以裝。那代表發布那一端出了事,不是你這邊。",                           \
    L"线上那一份比你手上这一份还旧,所以没有东西可以装。那代表发布那一端出了事,不是你这边。")                           \
  X(kUpdateStatusDownloading,                                                 \
    L"Downloading... (%s)",                                               \
    L"正在下載…(%s)",                                                          \
    L"正在下载…(%s)")                                                          \
  X(kUpdateStatusVerifying,                                                   \
    L"Checking what came down...",                                            \
    L"正在核對下載回來的檔案…",                                                          \
    L"正在核对下载回来的文件…")                                                          \
  X(kUpdateStatusReady,                                                       \
    L"Downloaded and checked. Once you press update, the input method "       \
    L"stops for a moment and comes back by itself.",                          \
    L"下載好了,也核對過了。按下更新之後,輸入法會停一下,然後自己回來。",                                     \
    L"下载好了,也核对过了。按下更新之后,输入法会停一下,然后自己回来。")                                     \
  X(kUpdateStatusHandedOff,                                                   \
    L"The installer is running now. This window closes and comes back "       \
    L"when it is finished.",                                                  \
    L"安裝程式已經在跑了。這個視窗會關掉,做完會自己回來。",                                            \
    L"安装程序已经在跑了。这个窗口会关掉,做完会自己回来。")                                            \
  X(kUpdateStatusInstalled,                                                   \
    L"Updated to %s.",                                                        \
    L"已經更新到 %s。",                                                             \
    L"已经更新到 %s。")                                                             \
  X(kUpdateErrSwitchOff,                                                      \
    L"The switch above is off, so not one byte went out. Turn it on first "   \
    L"if you want to check.",                                                 \
    L"上面那個開關是關的,所以一個位元組都沒有出去。要查的話,先把它打開。",                                    \
    L"上面那个开关是关的,所以一个字节都没有出去。要查的话,先把它打开。")                                     \
  X(kUpdateErrUnreachable,                                                    \
    L"Could not reach the place that holds the version information. The "     \
    L"network may be down, or that server may not be answering right now.",   \
    L"連不到放版本資訊的地方。可能是網路不通,也可能是那台伺服器暫時沒有回應。",                                  \
    L"连不到放版本信息的地方。可能是网络不通,也可能是那台服务器暂时没有回应。")                                  \
  X(kUpdateErrUnreadable,                                                     \
    L"The version information came back but does not make sense, so nothing " \
    L"was downloaded.",                                                       \
    L"版本資訊拿到了,但是讀不懂,所以什麼都沒有下載。",                                              \
    L"版本信息拿到了,但是读不懂,所以什么都没有下载。")                                              \
  X(kUpdateErrOwnVersionUnknown,                                              \
    L"Cannot tell which version you are running, so there is no way to "      \
    L"say whether the one online is newer. Install once from the download "   \
    L"page and it will work from then on.",                                   \
    L"查不出你現在裝的是哪一版,所以沒辦法判斷線上那一份是新是舊。從下載頁自己裝一次之後就查得到了。",                       \
    L"查不出你现在装的是哪一版,所以没办法判断线上那一份是新是旧。从下载页自己装一次之后就查得到了。")                       \
  X(kUpdateErrDownloadInterrupted,                                            \
    L"The download stopped part way and what had come down was thrown "       \
    L"away. Nothing on your computer was touched - press again to start "     \
    L"over.",                                                                 \
    L"下載中途停了,已經下載的部分丟掉了。你的電腦上一個檔案都沒有被動到,再按一次就重來。",                            \
    L"下载中途停了,已经下载的部分丢掉了。你的电脑上一个文件都没有被动到,再按一次就重来。")                            \
  X(kUpdateErrTooLarge,                                                       \
    L"More came down than the version information said there would be, "      \
    L"so it was stopped and thrown away. The one online does not match "      \
    L"its own description.",                                                  \
    L"收到的比版本資訊宣告的還多,所以停下來並且丟掉了。那代表線上那一份跟它自己的說明對不起來。",                         \
    L"收到的比版本信息宣告的还多,所以停下来并且丢掉了。那代表线上那一份跟它自己的说明对不起来。")                         \
  X(kUpdateErrSha256,                                                         \
    L"What came down is not the file the version information describes, "     \
    L"so the whole thing was thrown away and nothing was installed.",         \
    L"下載回來的檔案跟版本資訊說的不是同一個,整包丟掉了,沒有安裝任何東西。",                                   \
    L"下载回来的文件跟版本信息说的不是同一个,整包丢掉了,没有安装任何东西。")                                   \
  X(kUpdateErrStagingWrite,                                                   \
    L"Could not save what was downloaded. The disk may be full, or another "  \
    L"program may be holding the file.",                                      \
    L"存不下下載回來的檔案。可能是磁碟滿了,也可能是有別的程式正握著它。",                                     \
    L"存不下下载回来的文件。可能是磁盘满了,也可能是有别的程序正握着它。")                                     \
  X(kUpdateErrProductChanged,                                                 \
    L"The version online calls itself a different product. Installing "       \
    L"it would leave you with two copies instead of replacing this one, "     \
    L"so it was not downloaded. Open the download page to see what to "       \
    L"do.",                                                                   \
    L"線上那一版把自己標成另一個產品。裝下去不會蓋掉你現在這一套,而是在旁邊多出一套,所以我們沒有下載它。請開下載頁看說明。",           \
    L"线上那一版把自己标成另一个产品。装下去不会盖掉你现在这一套,而是在旁边多出一套,所以我们没有下载它。请开下载页看说明。")           \
  X(kUpdateErrBusy,                                                           \
    L"The words are being sorted out right now, so this cannot stop cleanly. " \
    L"Wait for that to finish and try again - stopping half way would "       \
    L"leave the work unfinished.",                                            \
    L"現在正在整理字詞,停不下來。等它做完再更新 —— 中途停掉會留下做到一半的東西。",                              \
    L"现在正在整理字词,停不下来。等它做完再更新 —— 中途停掉会留下做到一半的东西。")                              \
  X(kUpdateErrElevationDeclined,                                              \
    L"You answered no to the administrator prompt, so nothing was done. "     \
    L"Replacing the installed files needs administrator rights.",             \
    L"你在系統管理員的確認視窗按了否,所以什麼都沒有做。換掉已經裝好的檔案需要系統管理員權限。",                          \
    L"你在系统管理员的确认窗口按了否,所以什么都没有做。换掉已经装好的文件需要系统管理员权限。")                          \
  X(kUpdateErrHandoffFailed,                                                  \
    L"Could not start the installer. What was downloaded is still there, "    \
    L"and you can open it yourself.",                                         \
    L"叫不起安裝程式。下載回來的那一份還在,你可以自己把它打開。",                                         \
    L"叫不起安装程序。下载回来的那一份还在,你可以自己把它打开。")                                         \
  X(kUpdateErrFileLocked,                                                     \
    L"The update did not go in: something is still holding the installed "    \
    L"files. Close those programs and try again. We will not push it to "     \
    L"your next restart instead.",                                            \
    L"更新沒有裝上去:還有程式握著已經裝好的檔案。把那些程式關掉再試一次。我們不會把它排到你下一次開機才做。",                   \
    L"更新没有装上去:还有程序握着已经装好的文件。把那些程序关掉再试一次。我们不会把它排到你下一次开机才做。")                   \
  X(kUpdateErrNotInstalled,                                                   \
    L"The update did not go in, and we cannot tell why. The version you "     \
    L"had is untouched.",                                                     \
    L"更新沒有裝上去,而我們看不出原因。你原本那一版一點都沒有被動到。",                                      \
    L"更新没有装上去,而我们看不出原因。你原本那一版一点都没有被动到。")                                      \

// ── 由同一份清單長出 enum 順序表與三個語系 ──────────────────────

#define X(name, en, hant, hans) UiString::name,
constexpr UiString kOrder[] = {RIMEWIN_UI_STRINGS(X)};
#undef X

constexpr int kCount = static_cast<int>(sizeof(kOrder) / sizeof(kOrder[0]));

static_assert(kCount == static_cast<int>(UiString::kUiStringCount),
              "ui_strings.h 的 enum 與 RIMEWIN_UI_STRINGS 清單條數不一致");

// ⚠ 這一段才是重點:長度相同**擋不住錯位**。逐項比對 enum 的序數,
//   在中間插一條而忘了改 enum 的話,這裡就編不過。
constexpr bool OrderMatchesEnum() {
  for (int i = 0; i < kCount; ++i)
    if (static_cast<int>(kOrder[i]) != i) return false;
  return true;
}
static_assert(OrderMatchesEnum(),
              "RIMEWIN_UI_STRINGS 的順序與 UiString 的宣告順序不一致 —— "
              "有人在中間插了一條卻沒有同步改 enum");

#define X(name, en, hant, hans) en,
const wchar_t* const kEnUs[] = {RIMEWIN_UI_STRINGS(X)};
#undef X

#define X(name, en, hant, hans) hant,
const wchar_t* const kZhHant[] = {RIMEWIN_UI_STRINGS(X)};
#undef X

#define X(name, en, hant, hans) hans,
const wchar_t* const kZhHans[] = {RIMEWIN_UI_STRINGS(X)};
#undef X

// W8:三個語系的陣列長度 == kUiStringCount,編譯期。
static_assert(sizeof(kEnUs) / sizeof(kEnUs[0]) == static_cast<size_t>(kCount),
              "英文少了一條");
static_assert(sizeof(kZhHant) / sizeof(kZhHant[0]) ==
                  static_cast<size_t>(kCount),
              "正體中文少了一條");
static_assert(sizeof(kZhHans) / sizeof(kZhHans[0]) ==
                  static_cast<size_t>(kCount),
              "簡體中文少了一條");

UiLang g_lang = UiLang::kZhHant;

}  // namespace

UiLang CurrentUiLang() { return g_lang; }

void SetUiLang(UiLang lang) {
  if (lang == UiLang::kLangCount) return;
  g_lang = lang;
}

UiLang ResolveUiLang(const std::string& pref, uint32_t langid) {
  if (pref == "en") return UiLang::kEnUs;
  if (pref == "zh-Hant") return UiLang::kZhHant;
  if (pref == "zh-Hans") return UiLang::kZhHans;
  // "system" 或不認得的值:依這個輸入法註冊在哪個語言底下推。
  // ⚠ 用 langid 而不是系統地區設定 —— 使用者可能在英文版 Windows 上
  //   用繁體輸入法,那時他要的是繁體介面,不是英文介面。
  switch (langid & 0xFFFF) {
    case 0x0804:  // zh-Hans-CN
    case 0x1004:  // zh-Hans-SG
      return UiLang::kZhHans;
    case 0x0404:  // zh-Hant-TW
    case 0x0C04:  // zh-Hant-HK
    case 0x1404:  // zh-Hant-MO
      return UiLang::kZhHant;
    case 0:
      // 不知道。回正體中文 —— 隨附方案的主語言,而且是現況的行為。
      return UiLang::kZhHant;
    default:
      return UiLang::kEnUs;
  }
}

const char* UiLangPrefValue(UiLang lang) {
  switch (lang) {
    case UiLang::kEnUs:
      return "en";
    case UiLang::kZhHant:
      return "zh-Hant";
    case UiLang::kZhHans:
      return "zh-Hans";
    default:
      return "system";
  }
}

const wchar_t* UiTextIn(UiLang lang, UiString s) {
  const int i = static_cast<int>(s);
  // ⚠ 越界回空字串,不崩潰。這支會在 WM_PAINT 裡被呼叫,
  //   而繪製路徑不可以是崩潰來源 —— 崩在那裡的話,使用者看到的是
  //   「打字打到一半整個程式不見了」。
  if (i < 0 || i >= kCount) return L"";
  switch (lang) {
    case UiLang::kEnUs:
      return kEnUs[i];
    case UiLang::kZhHans:
      return kZhHans[i];
    case UiLang::kZhHant:
    default:
      return kZhHant[i];
  }
}

const wchar_t* UiText(UiString s) { return UiTextIn(g_lang, s); }

int UiStringCount() { return kCount; }

}  // namespace rimewin
