; windows/installer/luminakey.iss — 安裝程式腳本(產物名見 SetupBaseName)
;
; ══ 為什麼是 Inno Setup 而不是 WiX ══════════════════════════════════
;
; 需求是「使用者下載、雙擊、下一步、裝好、能用」,也就是**一個 .exe**。
;
;   · WiX 的原生產物是 .msi。要變成單一 .exe 得再套一層 Burn bootstrapper
;     (bundle),等於為了同一個結果多一層工具鏈與多一種失敗模式。
;   · Inno 直接產生自解的 .exe,而且 UAC 提權是寫在它自己的 manifest 裡
;     (PrivilegesRequired=admin)—— 雙擊就跳提權對話框,不必教使用者按右鍵。
;     這正是使用者這一輪回報的問題:「雙擊沒反應、管理員執行也沒反應」。
;   · 解除安裝要做的事(停掉服務、反註冊、**但保留使用者詞典**)需要真的
;     跑一段邏輯。Inno 的 Pascal script 直接做得到;MSI 的 custom action
;     要另外編一個 DLL。
;   · windows-latest runner 內建 Inno Setup,CI 不必多裝東西。
;
; MSI 唯一贏的地方是網域環境的 GPO 派送。那不是這一輪的目標,而且
; 日後真的需要時,把同一批檔案再包一份 MSI 並不會推翻現在的任何決定。
;
; ══ 編碼 ═══════════════════════════════════════════════════════════
;
; ⚠ 這個檔案裡有中文。Inno Setup 6 只有在 .iss **帶 UTF-8 BOM** 時才會
;   當成 UTF-8 讀,否則走系統 ANSI 代碼頁 —— 在英文的 runner 上(cp1252)
;   那些中文會在編譯階段就變成亂碼,而安裝程式看起來「編成功了」。
;   BOM 由 windows/make_installer.sh 在複製到暫存目錄時加上,
;   版控裡這一份保持乾淨的 UTF-8。**不要直接把這個檔案餵給 ISCC。**
;
; ══ 由 make_installer.sh 以 /D 傳進來的 ════════════════════════════
;   PayloadDir       已經備好的檔案樹(bin + data)
;   AppVersion       顯示用版本字串
;   VersionInfo      x.x.x.x 的數字版本(給 exe 的版本資源)
;   ArchDirective    x64compatible(Inno 6.3+)或 x64(更舊的)

#ifndef PayloadDir
  #error 必須以 /DPayloadDir=<目錄> 呼叫;請用 windows/make_installer.sh
#endif
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef VersionInfo
  #define VersionInfo "0.0.0.0"
#endif
#ifndef ArchDirective
  #define ArchDirective "x64compatible"
#endif

; ── 產品識別:**不在這個檔案裡** ───────────────────────────────────
;
; 產品名、中文顯示名、識別碼字根、安裝程式檔名的單一來源是
; scripts/lib/product.env。windows/make_installer.sh 把它讀出來,用 ISCC
; 的 /D 傳進來。這裡刻意**沒有預設值**:少傳一個就編不過。
;
; 為什麼不寫 `#ifndef X / #define X "<某個寫死的名字>"` 那種退路 —— 那正好是改名
; 會漏掉的形狀。有退路的話,傳參那一條哪天斷了(改了旗標名、換了呼叫者),
; 安裝程式照樣編得出來,只是悄悄地用回一個寫死的舊名字,而每一關都是綠的。
; 沒有退路的話它會在四分鐘的 lint 那一步就死,訊息還指得出是哪一個。
#ifndef ProductName
  #error 少了 ProductName。這份 .iss 只能由 windows/make_installer.sh 編(它從 scripts/lib/product.env 傳值進來)。
#endif
#ifndef ProductNameZh
  #error 少了 ProductNameZh(來源同上)。
#endif
#ifndef ProductIdRoot
  #error 少了 ProductIdRoot(來源同上)。
#endif
#ifndef SetupBaseName
  #error 少了 SetupBaseName(來源同上,對應 product.env 的 CI_ARTIFACT_WINDOWS_SETUP)。
#endif

[Setup]
; ⚠ AppId 一旦發布出去就不能改。它決定「新增或移除程式」裡那一筆的登錄檔
;   鍵名,也決定升級時 Inno 認不認得舊版。改了的話,舊版會永遠留在
;   「新增或移除程式」裡,而且解除安裝不掉。
;
; ══ 2026-08-09:產品定名時**刻意**換了一個新的 ═══════════════════
;   舊值是 {7A033CF7-CB91-408E-A653-EF639F4173DB}。
;
;   為什麼非換不可:AppId 與 tsf/guids.h 的 CLSID 必須一起換或一起不換。
;   CLSID 換了(理由見那個檔的檔頭)而 AppId 沒換的話,新版的安裝程式會
;   被 Inno 當成「同一個產品的升級」,於是它會去跑**舊版**留下的解除安裝
;   邏輯 —— 而那份邏輯反註冊的是舊 CLSID,新的那一份沒有人管。
;
;   換掉的後果,以及使用者的正確升級步驟,寫在 windows/README.md 的
;   升級章節與 tsf/guids.h 檔頭。摘要:先用舊版解除安裝並重新開機,再裝新版。
;   **不要**讓新版自己去刪舊版的登錄檔 —— 那是在猜另一個產品的東西。
AppId={{4D16C4D6-444A-40A7-953D-57BF873E8689}
AppName={#ProductNameZh}
AppVerName={#ProductNameZh} {#AppVersion}
AppVersion={#AppVersion}
VersionInfoVersion={#VersionInfo}
AppPublisher={#ProductName}
; ⚠ 這一行的 rime-quad 是 **GitHub repo 名**,不是產品名,刻意不改 ——
;   見 scripts/lib/product.env 的 GITHUB_REPO(改它會牽動所有推送腳本與 CI)。
AppPublisherURL=https://github.com/IncredibleGG/rime-quad
DefaultDirName={autopf}\{#ProductName}
DefaultGroupName={#ProductNameZh}
OutputBaseFilename={#SetupBaseName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

; ── 提權 ──────────────────────────────────────────────────────────
; 這一行就是「雙擊就跳提權對話框」的來源:Inno 會把
; requestedExecutionLevel=requireAdministrator 寫進安裝程式自己的 manifest。
; 需要提權是因為 COM 與 TSF 的註冊寫的是 HKLM,而且要裝進 Program Files。
PrivilegesRequired=admin

; ── 架構 ──────────────────────────────────────────────────────────
; 兩行都要。第一行擋掉在 32 位元 Windows 上執行(我們只有 x64 的 DLL);
; 第二行讓 {autopf} 展開成 C:\Program Files 而不是 Program Files (x86),
; 而且讓 Inno 用 64 位元的登錄檔檢視 —— 少了它,任何登錄檔動作都會落進
; WOW6432Node,而 64 位元的宿主一個都找不到這個輸入法。
ArchitecturesAllowed={#ArchDirective}
ArchitecturesInstallIn64BitMode={#ArchDirective}

; ToUnicodeEx 的「不要改動鍵盤狀態」旗標需要 Windows 10 1607+。
; 少了它,使用者按過法文的死鍵 ^ 之後那個狀態會被我們吃掉 ——
; 症狀是「裝了這個輸入法之後別的語言的重音字打不出來」。
; 與其在舊系統上安靜地壞掉,不如明著擋。
MinVersion=10.0

; ── 解除安裝 ──────────────────────────────────────────────────────
UninstallDisplayName={#ProductNameZh}
UninstallDisplayIcon={app}\rime_service.exe

; 不去關使用者正在用的程式。TSF 的 DLL 會被載入到每一個接受文字輸入的
; 進程裡,讓 Inno 的重新啟動管理員去關它們等於「安裝輸入法會關掉你的 Word」。
; 檔案被佔用的情況由下面 [Files] 段的 restartreplace 承接。
CloseApplications=no
SetupMutex={#ProductName}SetupMutex

; 安裝完成頁不推銷任何東西。程式集資料夾裡只放兩個捷徑(見 [Icons]),
; 所以不必讓使用者選資料夾名字。
DisableProgramGroupPage=yes

; 歡迎頁要顯示。Inno 6 的 modern 樣式**預設把它藏起來**,但這裡它有實際用途:
; 使用者裝完之後還得自己去切輸入法,那件事得先講。
DisableWelcomePage=no

; 安裝位置固定,不讓使用者選。
; 服務進程靠「與執行檔同目錄的 data\shared」找執行期資料,而登錄檔裡存的是
; 絕對路徑 —— 使用者事後搬動資料夾就會壞,而症狀是「輸入法忽然不見了」。
; 少一個頁面,也少一種弄壞它的方法。
DisableDirPage=yes

[Icons]
; 只有兩個,而且兩個都真的做事。
;
; ⚠ 「診斷」那一個是上一輪加的,而且它是**修「使用者只能說不能用」這件事
;   本身**的一半:另一半是 doctor 那支程式,這一半是「他找得到它」。
;   一個要開命令列才跑得起來的診斷工具,對絕大多數使用者等於不存在。
;
; ⚠ 名字在 2026-08-09 前面補上了產品名,理由不是好看:
;
;   「開始」功能表的搜尋是**比對捷徑的名字**。叫「輸入法設定」的話,
;   使用者打「LuminaKey」搜不到它 —— 而他知道的名字只有這一個
;   (他不會記得我們把設定叫「輸入法設定」)。這一輪要修的正是
;   「使用者問 UI 在哪裡,而我們給了他一行命令列指令」,
;   搜不到的捷徑與不存在的捷徑對他是同一件事。
;
; ⚠ 這兩個捷徑的**存在、目標與參數**由 windows/verify_installer.sh §12 斷言。
;   捷徑指向錯的執行檔或錯的參數,症狀是「點了沒反應」——
;   而這個專案抓過四次那種鍵。目標與參數寫在這裡,斷言在那裡,
;   兩邊對不上時 CI 紅。
Name: "{group}\{#ProductName} 設定"; Filename: "{app}\rime_service.exe"; Parameters: "--settings"; Comment: "開啟 {#ProductNameZh} 的設定視窗(方案、簡繁、候選字大小)"
Name: "{group}\{#ProductName} 診斷:輸入法為什麼不能用"; Filename: "{app}\rime_ime_setup.exe"; Parameters: "doctor --report"; Comment: "檢查安裝、註冊、服務、引擎,並把結果用記事本打開"

[Files]
; restartreplace + uninsrestartdelete:
;   rime_tsf.dll 在升級的當下可能正被宿主進程載入著而換不掉。
;   這兩個旗標讓它排到下次開機再換 / 再刪,而不是讓整個安裝失敗。
Source: "{#PayloadDir}\rime_tsf.dll";       DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#PayloadDir}\rime_service.exe";   DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#PayloadDir}\rime_ime_setup.exe"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
; ⚠ rime_console.exe 是**給使用者的診斷工具**,不是開發者的東西。
;   它完全不經過 TSF、不經過管道,直接驅動 librime + 資料 —— 也就是
;   「引擎層通不通」那一刀。`rime_ime_setup.exe doctor` 的第 7 節呼叫它。
;   使用者回報「不能用」時,有沒有這一刀,決定我們是一次問清楚
;   還是來回五輪。
Source: "{#PayloadDir}\rime_console.exe";    DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete

; ⚠ 執行期資料。**少了這一段,前面每一步都會成功,服務起得來、輸入法
;   註冊得上,然後一個字都打不出來,而且沒有任何錯誤訊息。**
;   data\shared 是方案、詞庫與 opencc 詞典;data\user 是首次執行要補進
;   %APPDATA%\{#ProductName} 的範本(default.custom.yaml 把 schema_list 限縮成
;   我們真的有詞庫的四個方案)。
;   make_installer.sh 在編譯之前會逐項點名檢查這棵樹,缺了就不出安裝程式。
Source: "{#PayloadDir}\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs

; ⚠ 解除安裝要做的事(停服務、停用、反註冊)**不在 [UninstallRun]**,
;   在底下 [Code] 的 CurUninstallStepChanged(usUninstall)。
;
;   原因:那三件事裡有兩件必須以**登入者**的身分做(服務是使用者自己那一支,
;   HKCU 也是使用者自己的),而 runasoriginaluser 是 [Run] 專屬的旗標 ——
;   放進 [UninstallRun] 會讓 ISCC 直接拒絕編譯:
;     「Parameter "Flags" includes a flag that is not supported in this section.」
;   (實測過,CI run 上就是這個錯。)
;   搬到 [Code] 之後順序、身分、以及失敗時的記錄都在我們手上,反而更清楚。

; ⚠ 沒有 [UninstallDelete] 去碰使用者資料目錄,而且**不可以加**。
;   那裡是使用者的詞典、自訂短語與設定 —— 是使用者的資料,不是我們的檔案。
;   預設一律留下;重新安裝時使用者的詞會原封不動地回來。
;
;   使用者真的不想要了的話,有一條**明確的**路:解除安裝時會問一次
;   (預設「否」),或靜默解除安裝時傳 /PURGEUSERDATA。見底下 [Code] 的
;   DecidePurge / DoPurge。用 [UninstallDelete] 做不到「預設不刪、
;   問過才刪」,而且它也沒辦法在刪之前向產品確認路徑。
;
;   CI 兩條路都驗:
;     · 不帶旗標的靜默解除安裝 → %APPDATA% 底下的檔案必須還在(§9)
;     · 帶 /PURGEUSERDATA 的   → 必須整個消失(§10)
;   只驗一條等於沒驗到這個功能。

[Messages]
; ⚠⚠ 這一整段是**完整**覆寫 Inno 內建訊息,不是「翻我們自己寫的那幾句」。
;
; 使用者走完整個解除安裝流程,在最後一個對話框撞到一句英文:
;   "To complete the uninstallation of ..., your computer must be restarted."
; 而標題與按鈕是中文。原因是我們原本只覆寫了 34 句,Inno 內建的其餘 247 句
; 照樣是英文 —— 它們平常不會出現,所以「前面沒撞到」完全不代表沒有。
;
; 現在這一段覆蓋 Default.isl 的每一個訊息 ID,而且
; **windows/check_installer_messages.sh 會逐句對帳**:
;   · 少翻一句 → 紅(並列出是哪幾句)
;   · 覆寫了一個不存在的 ID → 紅(ISCC 對打錯的 ID 是安靜忽略的,
;     那一句會永遠是英文,而 .iss 裡看起來明明翻過了)
;   · %1 / [name] 這些帶資料的佔位符對不上 → 紅
;   · %n 比原文少 → 紅(少掉的換行會把兩句話黏在一起)
;
; 順序照 Default.isl,方便日後 Inno 升版時逐行對照。
; 產品名一律用 [name] / [name/ver],不要寫死 —— 那是 Inno 從
; [Setup] 的 AppName 展開的,改名時只要改那一處。
;
; ⚠ 這裡面**不可以有 Markdown**。這些字串是直接畫在對話框上的,
;   `**強調**` 在使用者眼裡就是四個星號。本檔的註解裡用得很兇,
;   訊息值裡一個都不准有 —— windows/check_installer_messages.sh 會擋。
SetupAppTitle=安裝
SetupWindowTitle=安裝 - %1
UninstallAppTitle=移除
UninstallAppFullTitle=移除 %1
InformationTitle=資訊
ConfirmTitle=確認
ErrorTitle=錯誤
SetupLdrStartupMessage=即將安裝 %1。要繼續嗎?
LdrCannotCreateTemp=建立不了暫存檔,安裝已中止
LdrCannotExecTemp=無法在暫存資料夾裡執行檔案,安裝已中止
LastErrorMessage=%1。%n%n錯誤 %2:%3
SetupFileMissing=安裝資料夾裡少了檔案 %1。請修正這個問題,或重新取得一份安裝程式。
SetupFileCorrupt=安裝檔已損毀。請重新取得一份安裝程式。
SetupFileCorruptOrWrongVer=安裝檔已損毀,或與這個版本的安裝程式不相容。請修正這個問題,或重新取得一份安裝程式。
InvalidParameter=命令列上有一個無效的參數:%n%n%1
SetupAlreadyRunning=安裝程式已經在執行中。
WindowsVersionNotSupported=本輸入法需要 Windows 10 1607 或更新的版本。
WindowsServicePackRequired=本程式需要 %1 Service Pack %2 或更新的版本。
NotOnThisPlatform=本程式無法在 %1 上執行。
OnlyOnThisPlatform=本程式必須在 %1 上執行。
OnlyOnTheseArchitectures=本輸入法只提供 64 位元版本,無法安裝在這台電腦上。%n%n它只能安裝在為下列處理器架構設計的 Windows 上:%n%n%1
WinVersionTooLowError=本程式需要 %1 %2 或更新的版本。
WinVersionTooHighError=本程式無法安裝在 %1 %2 或更新的版本上。
AdminPrivilegesRequired=安裝本程式時必須以系統管理員身分登入。
PowerUserPrivilegesRequired=安裝本程式時必須以系統管理員、或 Power Users 群組成員的身分登入。
SetupAppRunningError=安裝程式偵測到 %1 正在執行中。%n%n請先關閉它的所有視窗,然後按「確定」繼續,或按「取消」結束安裝。
UninstallAppRunningError=移除程式偵測到 %1 正在執行中。%n%n請先關閉它的所有視窗,然後按「確定」繼續,或按「取消」結束移除。
PrivilegesRequiredOverrideTitle=選擇安裝模式
PrivilegesRequiredOverrideInstruction=選擇安裝模式
PrivilegesRequiredOverrideText1=%1 可以安裝給這台電腦的所有使用者(需要系統管理員權限),或只安裝給你自己。
PrivilegesRequiredOverrideText2=%1 可以只安裝給你自己,或安裝給這台電腦的所有使用者(需要系統管理員權限)。
PrivilegesRequiredOverrideAllUsers=安裝給所有使用者(&A)
PrivilegesRequiredOverrideAllUsersRecommended=安裝給所有使用者(&A)(建議)
PrivilegesRequiredOverrideCurrentUser=只安裝給我自己(&M)
PrivilegesRequiredOverrideCurrentUserRecommended=只安裝給我自己(&M)(建議)
ErrorCreatingDir=安裝程式建立不了資料夾「%1」
ErrorTooManyFilesInDir=無法在資料夾「%1」裡建立檔案,因為它裡面的檔案太多了
ExitSetupTitle=結束安裝
ExitSetupMessage=安裝尚未完成。現在結束的話,本輸入法不會被安裝。%n%n你可以之後再執行一次安裝程式把它裝完。%n%n要結束安裝嗎?
AboutSetupMenuItem=關於安裝程式(&A)…
AboutSetupTitle=關於安裝程式
AboutSetupMessage=%1 %2%n%3%n%n%1 首頁:%n%4
ButtonBack=< 上一步(&B)
ButtonNext=下一步(&N) >
ButtonInstall=安裝(&I)
ButtonOK=確定
ButtonCancel=取消
ButtonYes=是(&Y)
ButtonYesToAll=全部都是(&A)
ButtonNo=否(&N)
ButtonNoToAll=全部都否(&O)
ButtonFinish=完成(&F)
ButtonBrowse=瀏覽(&B)…
ButtonWizardBrowse=瀏覽(&R)…
ButtonNewFolder=新增資料夾(&M)
SelectLanguageTitle=選擇安裝語言
SelectLanguageLabel=選擇安裝過程中要使用的語言。
ClickNext=按「下一步」繼續,或按「取消」結束安裝。
BrowseDialogTitle=瀏覽資料夾
BrowseDialogLabel=請在下面的清單裡選一個資料夾,然後按「確定」。
NewFolderName=新增資料夾
WelcomeLabel1=歡迎安裝 [name]
WelcomeLabel2=這將在您的電腦上安裝 [name/ver]。%n%n安裝完成後,請在工作列的輸入法切換清單(Win + 空白鍵)裡選擇它;設定在「開始」功能表裡。%n%n建議先關閉其他程式再繼續。
WizardPassword=密碼
PasswordLabel1=這份安裝程式有密碼保護。
PasswordLabel3=請輸入密碼,然後按「下一步」繼續。密碼區分大小寫。
PasswordEditLabel=密碼(&P):
IncorrectPassword=你輸入的密碼不正確,請再試一次。
WizardLicense=授權條款
LicenseLabel=繼續之前請先閱讀下面的重要資訊。
LicenseLabel3=請閱讀下面的授權條款。你必須接受這些條款才能繼續安裝。
LicenseAccepted=我接受這些條款(&A)
LicenseNotAccepted=我不接受這些條款(&D)
WizardInfoBefore=資訊
InfoBeforeLabel=繼續之前請先閱讀下面的重要資訊。
InfoBeforeClickLabel=準備好要繼續安裝時,請按「下一步」。
WizardInfoAfter=資訊
InfoAfterLabel=繼續之前請先閱讀下面的重要資訊。
InfoAfterClickLabel=準備好要繼續安裝時,請按「下一步」。
WizardUserInfo=使用者資訊
UserInfoDesc=請輸入你的資訊。
UserInfoName=使用者名稱(&U):
UserInfoOrg=組織(&O):
UserInfoSerial=序號(&S):
UserInfoNameRequired=必須輸入名稱。
WizardSelectDir=選擇安裝位置
SelectDirDesc=要把 [name] 安裝到哪裡?
SelectDirLabel3=安裝程式會把 [name] 安裝到下面這個資料夾。
SelectDirBrowseLabel=按「下一步」繼續。若要換一個資料夾,請按「瀏覽」。
DiskSpaceGBLabel=至少需要 [gb] GB 的可用磁碟空間。
DiskSpaceMBLabel=至少需要 [mb] MB 的可用磁碟空間。
CannotInstallToNetworkDrive=安裝程式無法安裝到網路磁碟機。
CannotInstallToUNCPath=安裝程式無法安裝到 UNC 路徑。
InvalidPath=必須輸入含磁碟機代號的完整路徑,例如:%n%nC:\App%n%n或是這種形式的 UNC 路徑:%n%n\\server\share
InvalidDrive=你選的磁碟機或 UNC 共用不存在、或無法存取,請換一個。
DiskSpaceWarningTitle=磁碟空間不足
DiskSpaceWarning=安裝至少需要 %1 KB 的可用空間,而所選磁碟機只剩 %2 KB。%n%n仍然要繼續嗎?
DirNameTooLong=資料夾名稱或路徑太長。
InvalidDirName=資料夾名稱無效。
BadDirName32=資料夾名稱不可以包含下列任何字元:%n%n%1
DirExistsTitle=資料夾已存在
DirExists=資料夾:%n%n%1%n%n已經存在。仍然要安裝到那個資料夾嗎?
DirDoesntExistTitle=資料夾不存在
DirDoesntExist=資料夾:%n%n%1%n%n不存在。要建立它嗎?
WizardSelectComponents=選擇元件
SelectComponentsDesc=要安裝哪些元件?
SelectComponentsLabel2=勾選要安裝的元件,取消勾選不要安裝的元件。準備好之後按「下一步」。
FullInstallation=完整安裝
CompactInstallation=精簡安裝
CustomInstallation=自訂安裝
NoUninstallWarningTitle=元件已存在
NoUninstallWarning=安裝程式偵測到這台電腦上已經裝了下列元件:%n%n%1%n%n取消勾選它們並不會把它們移除。%n%n仍然要繼續嗎?
ComponentSize1=%1 KB
ComponentSize2=%1 MB
ComponentsDiskSpaceGBLabel=目前的選擇至少需要 [gb] GB 的磁碟空間。
ComponentsDiskSpaceMBLabel=目前的選擇至少需要 [mb] MB 的磁碟空間。
WizardSelectTasks=選擇附加工作
SelectTasksDesc=要執行哪些附加工作?
SelectTasksLabel2=選擇安裝 [name] 時要一併執行的附加工作,然後按「下一步」。
WizardSelectProgramGroup=選擇「開始」功能表資料夾
SelectStartMenuFolderDesc=要把捷徑放在哪裡?
SelectStartMenuFolderLabel3=安裝程式會把捷徑建立在下面這個「開始」功能表資料夾裡。
SelectStartMenuFolderBrowseLabel=按「下一步」繼續。若要換一個資料夾,請按「瀏覽」。
MustEnterGroupName=必須輸入資料夾名稱。
GroupNameTooLong=資料夾名稱或路徑太長。
InvalidGroupName=資料夾名稱無效。
BadGroupName=資料夾名稱不可以包含下列任何字元:%n%n%1
NoProgramGroupCheck2=不要建立「開始」功能表資料夾(&D)
WizardReady=準備安裝
ReadyLabel1=安裝程式已準備好將 [name] 安裝到您的電腦。
ReadyLabel2a=按「安裝」開始安裝,或按「上一步」檢視或變更設定。
ReadyLabel2b=按「安裝」開始安裝。
ReadyMemoUserInfo=使用者資訊:
ReadyMemoDir=安裝位置:
ReadyMemoType=安裝類型:
ReadyMemoComponents=已選元件:
ReadyMemoGroup=「開始」功能表資料夾:
ReadyMemoTasks=附加工作:
DownloadingLabel2=正在下載檔案…
ButtonStopDownload=停止下載(&S)
StopDownload=確定要停止下載嗎?
ErrorDownloadAborted=下載已中止
ErrorDownloadFailed=下載失敗:%1 %2
ErrorDownloadSizeFailed=取得檔案大小失敗:%1 %2
ErrorProgress=進度無效:%1 / %2
ErrorFileSize=檔案大小無效:預期 %1,實際 %2
ExtractingLabel=正在解開檔案…
ButtonStopExtraction=停止解壓(&S)
StopExtraction=確定要停止解壓嗎?
ErrorExtractionAborted=解壓已中止
ErrorExtractionFailed=解壓失敗:%1
ArchiveIncorrectPassword=密碼不正確
ArchiveIsCorrupted=壓縮檔已損毀
ArchiveUnsupportedFormat=不支援這種壓縮格式
WizardPreparing=準備中
PreparingDesc=安裝程式正在準備安裝 [name]。
PreviousInstallNotCompleted=上一次的安裝或移除還沒有做完 —— 有幾個檔案正在等待重新啟動之後才會被換掉或刪掉。%n%n在那之前不能安裝,否則等到下次開機,系統會把剛裝好的檔案一起清掉。%n%n請先重新啟動電腦,然後再執行一次安裝程式,把 [name] 裝完。
CannotContinue=安裝無法繼續,請按「取消」結束。
ApplicationsFound=下列程式正在使用安裝程式需要更新的檔案。建議讓安裝程式自動關閉它們。
ApplicationsFound2=下列程式正在使用安裝程式需要更新的檔案。建議讓安裝程式自動關閉它們;安裝完成後,安裝程式會試著把它們重新開啟。
CloseApplications=自動關閉這些程式(&A)
DontCloseApplications=不要關閉這些程式(&D)
ErrorCloseApplications=安裝程式沒有辦法自動關閉所有程式。建議先自行關閉那些正在使用相關檔案的程式,再繼續。
PrepareToInstallNeedsRestart=安裝程式必須重新啟動電腦。重新啟動之後,請再執行一次安裝程式,把 [name] 裝完。%n%n要現在重新啟動嗎?
WizardInstalling=安裝中
InstallingLabel=請稍候,正在安裝 [name]…
FinishedHeadingLabel=[name] 安裝完成
FinishedLabelNoIcons=[name] 已安裝在您的電腦上。
FinishedLabel=[name] 已安裝在您的電腦上。%n%n切換輸入法:按 Win + 空白鍵,選擇「[name]」。清單上只會有一格 —— 它掛在您原本就有的那一個中文語言底下。%n%n開啟設定:「開始」功能表 →「[name] 設定」。方案、簡繁、候選字大小都在那裡。%n也可以用工作列右下角的系統匣圖示(Windows 11 預設把新圖示收在「^」裡面,要先點開那個箭頭),或語言列上的「設定」。%n%n簡體 / 繁體在設定的「文字」分頁切換,系統匣圖示按右鍵也切得到。%n%n首次使用時輸入法會在背景編譯詞庫,可能需要一到數分鐘;在那之前打出來的是英文,這是正常的。%n%n若打不出中文或看不到設定視窗,請執行「開始」功能表裡的「[name] 診斷:輸入法為什麼不能用」,它會把原因寫成一份報告並用記事本打開。
ClickFinish=按「完成」結束安裝程式。
FinishedRestartLabel=[name] 已經裝好了,但有幾個檔案要等重新啟動之後才換得掉 —— 舊版還被某些正在執行的程式(檔案總管、瀏覽器、Office…)握著。%n%n在重新啟動之前,您用到的還是舊版。要現在重新啟動嗎?
FinishedRestartMessage=[name] 已經裝好了,但有幾個檔案要等重新啟動之後才換得掉 —— 舊版還被某些正在執行的程式(檔案總管、瀏覽器、Office…)握著。%n%n在重新啟動之前,您用到的還是舊版。%n%n要現在重新啟動嗎?
ShowReadmeCheck=是,我想閱讀說明檔
YesRadio=是,立刻重新啟動電腦(&Y)
NoRadio=否,我稍後自己重新啟動(&N)
RunEntryExec=執行 %1
RunEntryShellExec=檢視 %1
ChangeDiskTitle=安裝程式需要下一張磁片
SelectDiskLabel2=請插入磁片 %1 再按「確定」。%n%n如果那張磁片上的檔案不在下面顯示的資料夾裡,請輸入正確的路徑或按「瀏覽」。
PathLabel=路徑(&P):
FileNotInDir2=在「%2」裡找不到檔案「%1」。請插入正確的磁片,或選擇別的資料夾。
SelectDirectoryLabel=請指定下一張磁片的位置。
SetupAborted=安裝沒有完成。%n%n請修正問題之後再執行一次安裝程式。
AbortRetryIgnoreSelectAction=選擇動作
AbortRetryIgnoreRetry=再試一次(&T)
AbortRetryIgnoreIgnore=忽略這個錯誤並繼續(&I)
AbortRetryIgnoreCancel=取消安裝
RetryCancelSelectAction=選擇動作
RetryCancelRetry=再試一次(&T)
RetryCancelCancel=取消
StatusClosingApplications=正在關閉程式…
StatusCreateDirs=正在建立資料夾…
StatusExtractFiles=正在解開檔案…
StatusDownloadFiles=正在下載檔案…
StatusCreateIcons=正在建立捷徑…
StatusCreateIniEntries=正在建立 INI 項目…
StatusCreateRegistryEntries=正在建立登錄檔項目…
StatusRegisterFiles=正在註冊檔案…
StatusSavingUninstall=正在儲存移除資訊…
StatusRunProgram=正在完成安裝…
StatusRestartingApplications=正在重新開啟程式…
StatusRollback=正在復原變更…
ErrorInternal2=內部錯誤:%1
ErrorFunctionFailedNoCode=%1 失敗
ErrorFunctionFailed=%1 失敗;錯誤碼 %2
ErrorFunctionFailedWithMessage=%1 失敗;錯誤碼 %2。%n%3
ErrorExecutingProgram=無法執行檔案:%n%1
ErrorRegOpenKey=開啟登錄檔機碼時發生錯誤:%n%1\%2
ErrorRegCreateKey=建立登錄檔機碼時發生錯誤:%n%1\%2
ErrorRegWriteKey=寫入登錄檔機碼時發生錯誤:%n%1\%2
ErrorIniEntry=在檔案「%1」裡建立 INI 項目時發生錯誤。
FileAbortRetryIgnoreSkipNotRecommended=略過這個檔案(&S)(不建議)
FileAbortRetryIgnoreIgnoreNotRecommended=忽略這個錯誤並繼續(&I)(不建議)
SourceIsCorrupted=來源檔已損毀
SourceDoesntExist=來源檔「%1」不存在
SourceVerificationFailed=來源檔驗證失敗:%1
VerificationSignatureDoesntExist=簽章檔「%1」不存在
VerificationSignatureInvalid=簽章檔「%1」無效
VerificationKeyNotFound=簽章檔「%1」用的是不認得的金鑰
VerificationFileNameIncorrect=檔名不正確
VerificationFileTagIncorrect=檔案標記不正確
VerificationFileSizeIncorrect=檔案大小不正確
VerificationFileHashIncorrect=檔案雜湊值不正確
ExistingFileReadOnly2=無法取代現有的檔案,因為它被標記為唯讀。
ExistingFileReadOnlyRetry=拿掉唯讀屬性再試一次(&R)
ExistingFileReadOnlyKeepExisting=保留現有的檔案(&K)
ErrorReadingExistingDest=讀取現有檔案時發生錯誤:
FileExistsSelectAction=選擇動作
FileExists2=這個檔案已經存在。
FileExistsOverwriteExisting=覆蓋現有的檔案(&O)
FileExistsKeepExisting=保留現有的檔案(&K)
FileExistsOverwriteOrKeepAll=接下來遇到相同情況時都這樣做(&D)
ExistingFileNewerSelectAction=選擇動作
ExistingFileNewer2=現有的檔案比安裝程式要裝的那一份還新。
ExistingFileNewerOverwriteExisting=覆蓋現有的檔案(&O)
ExistingFileNewerKeepExisting=保留現有的檔案(&K)(建議)
ExistingFileNewerOverwriteOrKeepAll=接下來遇到相同情況時都這樣做(&D)
ErrorChangingAttr=變更現有檔案的屬性時發生錯誤:
ErrorCreatingTemp=在目的資料夾裡建立檔案時發生錯誤:
ErrorReadingSource=讀取來源檔時發生錯誤:
ErrorCopying=複製檔案時發生錯誤:
ErrorDownloading=下載檔案時發生錯誤:
ErrorExtracting=解開壓縮檔時發生錯誤:
ErrorReplacingExistingFile=取代現有檔案時發生錯誤:
ErrorRestartReplace=排定重新啟動後取代檔案失敗:
ErrorRenamingTemp=在目的資料夾裡重新命名檔案時發生錯誤:
ErrorRegisterServer=無法註冊 DLL/OCX:%1
ErrorRegSvr32Failed=RegSvr32 失敗,結束碼 %1
ErrorRegisterTypeLib=無法註冊型別程式庫:%1
UninstallDisplayNameMark=%1(%2)
UninstallDisplayNameMarks=%1(%2、%3)
UninstallDisplayNameMark32Bit=32 位元
UninstallDisplayNameMark64Bit=64 位元
UninstallDisplayNameMarkAllUsers=所有使用者
UninstallDisplayNameMarkCurrentUser=目前的使用者
ErrorOpeningReadme=開啟說明檔時發生錯誤。
ErrorRestartingComputer=安裝程式沒有辦法重新啟動電腦,請自行重新啟動。
UninstallNotFound=檔案「%1」不存在,無法移除。
UninstallOpenError=開啟不了檔案「%1」,無法移除
UninstallUnsupportedVer=移除記錄檔「%1」的格式是這個版本的移除程式不認得的,無法移除
UninstallUnknownEntry=移除記錄裡有一筆不認得的項目(%1)
ConfirmUninstall=您確定要完整移除 %1 嗎?
UninstallOnlyOnWin64=這份安裝只能在 64 位元的 Windows 上移除。
OnlyAdminCanUninstall=這份安裝只能由具有系統管理員權限的使用者移除。
UninstallStatusLabel=請稍候,正在從您的電腦移除 %1…
UninstalledAll=%1 已從您的電腦順利移除。
UninstalledMost=%1 移除完成。%n%n有部分項目無法移除,重新啟動電腦後會自動清除。
UninstalledAndNeedsRestart=%1 已經移除完成,只剩安裝資料夾裡幾個檔案還刪不掉。%n%n【為什麼】這個輸入法要能用,就必須被載入到每一個可以打字的程式裡。您現在還開著的程式(檔案總管、瀏覽器、Office…)仍然握著那幾個檔案,所以它們現在刪不掉。這不是安裝程式出錯。%n%n【不重新啟動會怎樣】輸入法本身已經完全停用了 —— 它不會再出現在輸入法清單裡,也不會再被任何程式載入。留下的只是幾個佔幾 MB 的檔案,系統已經排好在下次開機時自動清掉。但在重新啟動之前您裝不回來:再執行一次安裝程式時它會擋下來,請您先重新啟動。%n%n【可以晚點再重新啟動嗎】可以,現在選「否」不會有任何問題,下次您正常關機或重新啟動時就會清乾淨。但請注意:登出再登入是不夠的,那份清單只有在開機時才會被處理。%n%n要現在重新啟動嗎?
UninstallDataCorrupted=檔案「%1」已損毀,無法移除
ConfirmDeleteSharedFileTitle=要移除共用檔案嗎?
ConfirmDeleteSharedFile2=系統顯示下面這個共用檔案已經沒有任何程式在使用。要讓移除程式把它刪掉嗎?%n%n如果還有程式在用它而它被刪掉,那些程式可能會不正常。不確定的話請選「否」——把它留在系統上不會造成任何問題。
SharedFileNameLabel=檔名:
SharedFileLocationLabel=位置:
WizardUninstalling=移除進度
StatusUninstalling=正在移除 %1…
ShutdownBlockReasonInstallingApp=正在安裝 %1。
ShutdownBlockReasonUninstallingApp=正在移除 %1。

[CustomMessages]
RegisterFailed=註冊輸入法失敗(rime_ime_setup.exe %1,結束碼 %2)。%n%n輸入法沒有被系統接受,現在就算裝完了也不會出現在輸入法清單上。%n安裝已中止,不會留下一個裝了卻用不了的狀態。
UninstallKeptUserData=您的詞典與設定保留在:%n%n%1%n%n那是您自己的資料(學過的詞、自訂短語),移除輸入法時刻意不刪。%n重新安裝時會原封不動地回來;確定不要了再自行刪除該資料夾。
UninstallAskPurge=要順便刪除您的詞典與設定嗎?%n%n%1%n%n那裡面是您使用期間學會的詞、自訂短語與設定。%n%n⚠ 刪除之後無法復原 —— 重新安裝也救不回來。%n%n選「否」會保留它(建議);之後改變主意再自行刪除該資料夾即可。%n選「是」會立刻永久刪除。
UninstallPurgeDone=您的詞典與設定已刪除:%n%n%1
UninstallPurgeFailed=有部分資料沒有刪掉(可能有檔案正被使用):%n%n%1%n%n重新開機後手動刪除該資料夾即可。

[Code]

const
  SetupExeName = 'rime_ime_setup.exe';
  // 安裝記錄(/LOG)裡我們自己那幾行的字首。
  // ⚠ 只在這一個地方由 ISPP 展開產品名 —— 原本是十五處各寫一份字面值,
  //   而 windows/verify_installer.sh 靠這個字首把我們的行從 Inno 自己的
  //   幾百行裡撈出來。十五份裡漏改一份,撈出來的就會少幾行,
  //   而「少了幾行」在報表上看起來跟「那一步沒跑」一模一樣。
  LogTag = '{#ProductName}: ';

// 停掉舊的服務進程。
//
// 兩件事:升級時 rime_service.exe 被佔用著換不掉;而且它持有使用者詞庫的
// LevelDB,不能直接被砍。rime_ime_setup.exe stop-service 會先送結束事件、
// 等它收尾,真的沒反應才強制結束。
//
// 兩種身分都跑一次是刻意的:服務是**登入者**那一支(所以 runasoriginaluser),
// 但使用者也可能是以另一個帳號登入後提權的,那時提權那一側才找得到。
procedure StopExistingService;
var
  OldExe: String;
  Rc: Integer;
begin
  OldExe := ExpandConstant('{app}\' + SetupExeName);
  if not FileExists(OldExe) then Exit;
  ExecAsOriginalUser(OldExe, 'stop-service --dir "' + ExpandConstant('{app}') + '"',
                     '', SW_HIDE, ewWaitUntilTerminated, Rc);
  Exec(OldExe, 'stop-service --dir "' + ExpandConstant('{app}') + '"',
       '', SW_HIDE, ewWaitUntilTerminated, Rc);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  // 在複製檔案**之前**。跑到 [Run] 才停就已經太遲了 —— 那時檔案早就換過。
  StopExistingService;
  Result := '';
end;

procedure RunSetupVerbOrFail(Verb: String);
var
  Exe: String;
  Rc: Integer;
begin
  Exe := ExpandConstant('{app}\' + SetupExeName);
  if (not Exec(Exe, Verb, '', SW_HIDE, ewWaitUntilTerminated, Rc)) or (Rc <> 0) then
    RaiseException(FmtMessage(CustomMessage('RegisterFailed'), [Verb, IntToStr(Rc)]));
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Exe: String;
  Rc: Integer;
begin
  if CurStep <> ssPostInstall then Exit;
  Exe := ExpandConstant('{app}\' + SetupExeName);

  // 註冊失敗必須讓整個安裝失敗。
  //
  // 這個專案吃過太多次「一片全綠而其實沒在做事」的虧,而在安裝程式上,
  // 那的具體長相就是「顯示安裝成功,使用者的輸入法清單裡什麼都沒有」。
  // 寧可中止並留下訊息。
  RunSetupVerbOrFail('register');

  // ── 使用者那一側 ────────────────────────────────────────────────
  //
  // **兩種身分都跑**,而且都 best-effort。
  //
  //   · ExecAsOriginalUser 是正確的那一條:使用者用系統管理員帳號提權安裝時,
  //     提權後的 HKCU 是**那個管理員的**,真正在用電腦的人什麼都沒拿到。
  //   · 但它需要拿得到「原始使用者」的權杖,而那來自 shell(Explorer)。
  //     沒有互動式桌面的環境(例如 CI runner)上它拿不到,於是整步靜靜地
  //     什麼都沒做 —— 實測就是這樣:全機註冊全綠,HKCU 底下一片空白。
  //   · 所以提權那一條也跑一次。它寫的是執行安裝那個帳號的 HKCU,
  //     對「自己就是管理員」的一般情況正好是對的。
  //
  // 失敗不中止安裝:輸入法已經全機註冊好了,使用者仍然可以自己從「設定」
  // 把它加進來。為了這一步讓整個安裝回滾是不成比例的。
  //
  // 結果寫進安裝記錄(/LOG)。沒有它的話,「enable-user 沒跑到」與
  // 「跑了但 EnableLanguageProfile 回錯」在報表上長得一模一樣,
  // 而要分辨得再等一輪 CI。
  try
    if ExecAsOriginalUser(Exe, 'enable-user', '', SW_HIDE, ewWaitUntilTerminated, Rc) then
      Log(LogTag + 'enable-user(登入者)rc=' + IntToStr(Rc))
    else
      Log(LogTag + 'enable-user(登入者)**啟動失敗** —— 多半是沒有互動式 shell');
  except
    Log(LogTag + 'ExecAsOriginalUser 不可用');
  end;

  if Exec(Exe, 'enable-user', '', SW_HIDE, ewWaitUntilTerminated, Rc) then
    Log(LogTag + 'enable-user(目前身分)rc=' + IntToStr(Rc))
  else
    Log(LogTag + 'enable-user(目前身分)啟動失敗');

  // ── 最後才自我檢查 ──────────────────────────────────────────────
  //
  // ⚠ 順序是踩出來的,不是隨便排的。原本 check 排在 enable-user **之前**,
  //   而 check 失敗時 RaiseException 會讓整個 CurStepChanged 中止 ——
  //   於是 enable-user 從來沒被執行過。症狀:全機註冊全綠、
  //   使用者清單一片空白,而錯誤訊息講的是「註冊失敗」。
  //   一個判斷失誤造成兩個看起來無關的症狀。
  //
  // ⚠ --no-enum:剛註冊完的當下 CTF 還看不到新的設定檔(實測 register 回傳
  //   成功之後 0.12 秒時 EnumLanguageProfiles 看不到,22 秒後同一支程式跑
  //   同一段就全部看得到)。登錄檔是同步的,CTF 的可見性不是。
  //   所以這裡只驗登錄檔;「系統接受了嗎」由 CI 事後不帶 --no-enum 再問一次。
  //
  // ⚠ 而且要誠實說一件事:在 /SUPPRESSMSGBOXES 之下,RaiseException 只會被
  //   記進安裝記錄、對話框自動按掉,**Setup 仍然以 0 結束**。
  //   所以這一步對「靜默安裝」不是真正的閘門 —— 真正的閘門是 CI 的
  //   windows/verify_installer.sh(它會斷言安裝記錄裡沒有 raised an exception)。
  //   互動安裝時使用者會看到錯誤訊息,那一條是成立的。
  RunSetupVerbOrFail('check --no-enum');
end;

// 解除安裝時的收尾。
//
// usUninstall 是**刪檔案之前**,所以 {app}\rime_ime_setup.exe 還在 ——
// 這三件事全都要靠它。
//
// 身分很重要,而且兩種都要跑:
//   · 服務是**登入者**那一支,結束事件在他的工作階段命名空間裡;
//     HKCU 也是他的。這兩件要 ExecAsOriginalUser。
//   · 但使用者也可能是以另一個帳號提權來解除安裝的,那時提權那一側
//     才找得到東西。所以提權身分也跑一次。
//   · unregister 寫的是 HKLM,只有提權那一側做得到。
//
// 全部 best-effort:解除安裝不該因為服務停不掉就失敗。真的清乾淨了沒有,
// 由 CI 的 windows/verify_installer.sh 去斷言,不是靠這裡的結束碼。
procedure UninstallCleanup;
var
  Exe, AppDir: String;
  Rc: Integer;
begin
  AppDir := ExpandConstant('{app}');
  Exe := AppDir + '\' + SetupExeName;
  if not FileExists(Exe) then begin
    Log(LogTag + '找不到 ' + Exe + ',跳過反註冊');
    Exit;
  end;

  // ⚠ ExecAsOriginalUser 包在 try 裡。
  //
  // Inno 有一部分的支援函式在**解除安裝**的情境下不能呼叫,叫了會丟例外。
  // 我沒有辦法在這台機器上確認它屬不屬於那一類,而賭錯的後果是
  // 「解除安裝走到一半丟出例外」—— 比停不掉服務嚴重得多。
  // 包起來之後,最壞情況只是退回下面提權那一次,清理照樣完成。
  try
    if ExecAsOriginalUser(Exe, 'stop-service --dir "' + AppDir + '"', '',
                          SW_HIDE, ewWaitUntilTerminated, Rc) then
      Log(LogTag + 'stop-service(登入者)rc=' + IntToStr(Rc))
    else
      Log(LogTag + 'stop-service(登入者)啟動失敗');
  except
    Log(LogTag + 'ExecAsOriginalUser 在解除安裝情境下不可用,改走提權那一條');
  end;

  if Exec(Exe, 'stop-service --dir "' + AppDir + '"', '',
          SW_HIDE, ewWaitUntilTerminated, Rc) then
    Log(LogTag + 'stop-service(提權)rc=' + IntToStr(Rc));

  try
    if ExecAsOriginalUser(Exe, 'disable-user', '', SW_HIDE, ewWaitUntilTerminated, Rc) then
      Log(LogTag + 'disable-user(登入者)rc=' + IntToStr(Rc))
    else
      Log(LogTag + 'disable-user(登入者)啟動失敗');
  except
    Log(LogTag + 'disable-user(登入者)無法執行');
  end;
  // 提權那一側的 HKCU 也清一次(unregister 會刪掉整棵 HKCU CTF 子樹)。
  if Exec(Exe, 'disable-user', '', SW_HIDE, ewWaitUntilTerminated, Rc) then
    Log(LogTag + 'disable-user(提權)rc=' + IntToStr(Rc));

  if Exec(Exe, 'unregister', '', SW_HIDE, ewWaitUntilTerminated, Rc) then
    Log(LogTag + 'unregister rc=' + IntToStr(Rc))
  else
    Log(LogTag + 'unregister 啟動失敗 —— 登錄檔會留下殘骸');
end;

// ══════════════════════════════════════════════════════════════════
//  「連我的資料一起刪」
// ══════════════════════════════════════════════════════════════════
//
// 使用者的要求:真的不用了的人,要能一次清乾淨,不必自己去翻 AppData。
//
// ⚠ 這是整個安裝程式裡**唯一一個不可回復**的動作,所以每一條規則都往
//   「寧可少刪」那一邊倒:
//
//   1. **預設保留。** 對話框的預設按鈕是「否」(MB_DEFBUTTON2),
//      而且被 /SUPPRESSMSGBOXES 壓掉時的答案也是 IDNO。
//      也就是說「什麼都不做」是每一條路徑的預設。
//   2. **靜默解除安裝永遠不刪**,除非明確傳 /PURGEUSERDATA。
//      這一條不是潔癖:windows/verify_installer.sh 第 9 步斷言
//      「解除安裝後 %APPDATA% 底下的檔案還在」,那道關卡守的是
//      「移除輸入法不會順手毀掉使用者的東西」。靜默模式若預設會刪,
//      那道斷言就會開始紅 —— 而最糟的情況是有人為了讓它變綠去改斷言。
//   3. **路徑向產品要,不自己拼。** GUserDataDir 來自
//      `rime_ime_setup.exe user-data-path`。這裡若寫死
//      {userappdata}\<資料夾名>,產品改名時就會漏掉這一處,
//      結果是「刪了一個不存在的資料夾然後回報成功」。
//   4. **在檔案還沒被刪掉之前就把路徑問出來。** usPostUninstall 的時候
//      {app}\rime_ime_setup.exe 已經不在了,問不到任何東西。
var
  GUserDataDir: String;
  GPurge: Boolean;
  GPurgeOk: Boolean;

// ⚠ 兩種找法都做。
//
// Inno 的解除安裝程式會**把自己複製到暫存目錄再跑一次**(_iu*.tmp),
// 而那一次的參數列是它自己組出來的(會多一個 /SECONDPHASE=...)。
// 只看 ParamStr 的話,旗標有可能在第二階段就不見了 —— 而症狀是
// 「帶了 /PURGEUSERDATA 卻沒有刪」,看起來像刪除功能壞掉。
// GetCmdTail 拿的是原始命令列字串,兩種都查一次是零成本的保險。
function CmdLineParamExists(const Value: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), Value) = 0 then begin
      Result := True;
      Exit;
    end;
  if Pos(Uppercase(Value), Uppercase(GetCmdTail)) > 0 then
    Result := True;
end;

// 問產品自己:使用者資料目錄在哪裡。
//
// 走 ExecAsOriginalUser 優先 —— %APPDATA% 是**登入者的**,而解除安裝程式
// 可能跑在另一個(提權的)帳號底下。拿不到就退回提權那一側。
//
// ⚠ 用「另一個帳號提權」解除安裝時,退回的那一條會拿到**提權帳號的**路徑,
//   與登入者的不同。要說清楚這件事的後果:
//     · 真正的刪除**不受影響** —— DoPurge 叫的是 rime_ime_setup.exe,
//       那支程式自己算它所在身分的 %APPDATA%,所以刪的一定是「跑它的那個人」的。
//     · 受影響的只有(a)對話框裡顯示的路徑,與(b)刪完之後的 DirExists 檢查。
//   兩者都往保守的方向偏(顯示別人的路徑、或報「沒刪乾淨」),
//   而不會往「刪掉不該刪的東西」偏。這是刻意接受的取捨。
function QueryUserDataDir(Exe: String): String;
var
  Tmp: String;
  Rc: Integer;
  Lines: TArrayOfString;
begin
  Result := '';
  Tmp := ExpandConstant('{tmp}\{#ProductIdRoot}-userdir.txt');
  // cmd /C 才能做輸出重導向。Exec 本身不會解析 > 。
  try
    if not ExecAsOriginalUser(ExpandConstant('{cmd}'),
         '/C ""' + Exe + '" user-data-path > "' + Tmp + '""',
         '', SW_HIDE, ewWaitUntilTerminated, Rc) then
      Rc := -1;
  except
    Rc := -1;
  end;
  if (Rc <> 0) or (not FileExists(Tmp)) then
    Exec(ExpandConstant('{cmd}'),
         '/C ""' + Exe + '" user-data-path > "' + Tmp + '""',
         '', SW_HIDE, ewWaitUntilTerminated, Rc);
  if FileExists(Tmp) and LoadStringsFromFile(Tmp, Lines) and
     (GetArrayLength(Lines) > 0) then
    Result := Trim(Lines[0]);
  DeleteFile(Tmp);
end;

procedure DecidePurge;
begin
  GPurge := CmdLineParamExists('/PURGEUSERDATA');
  if GPurge then begin
    Log(LogTag + '/PURGEUSERDATA —— 明確要求刪除使用者資料');
    Exit;
  end;
  // 靜默解除安裝時**不問也不刪**。沒有旗標就是不刪。
  if UninstallSilent then Exit;
  if (GUserDataDir = '') or (not DirExists(GUserDataDir)) then Exit;
  // MB_DEFBUTTON2:預設按鈕是「否」。
  // 最後那個 IDNO 是 /SUPPRESSMSGBOXES 之下採用的答案 —— 同樣是不刪。
  GPurge := SuppressibleMsgBox(
              FmtMessage(CustomMessage('UninstallAskPurge'), [GUserDataDir]),
              mbConfirmation, MB_YESNO or MB_DEFBUTTON2, IDNO) = IDYES;
  if GPurge then Log(LogTag + '使用者選擇「連資料一起刪」');
end;

procedure DoPurge(Exe: String);
var
  Rc: Integer;
begin
  GPurgeOk := False;
  if GUserDataDir = '' then begin
    Log(LogTag + '算不出使用者資料目錄,不刪');
    Exit;
  end;
  // ⚠ 身分很重要:要刪的是**登入者的** %APPDATA%。
  //   ExecAsOriginalUser 是正確的那一條;它在沒有互動式 shell 的環境
  //   (例如 CI runner)上拿不到權杖,那時才退回提權那一側 ——
  //   而提權那一側刪的是執行解除安裝那個帳號的資料夾,對「自己就是
  //   管理員」的一般情況正好也是對的。
  Rc := -1;
  try
    if not ExecAsOriginalUser(Exe,
         'purge-user-data --yes-delete-my-dictionary', '', SW_HIDE,
         ewWaitUntilTerminated, Rc) then
      Rc := -1;
  except
    Rc := -1;
  end;
  if Rc <> 0 then begin
    Log(LogTag + 'purge(登入者)rc=' + IntToStr(Rc) + ',改走提權那一條');
    if not Exec(Exe, 'purge-user-data --yes-delete-my-dictionary', '',
                SW_HIDE, ewWaitUntilTerminated, Rc) then
      Rc := -1;
  end;
  Log(LogTag + 'purge-user-data rc=' + IntToStr(Rc));
  GPurgeOk := (Rc = 0) and (not DirExists(GUserDataDir));
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Msg: String;
begin
  if CurUninstallStep = usUninstall then begin
    // 順序是刻意的:
    //   1. 先把路徑問出來(這時 {app}\rime_ime_setup.exe 還在)
    //   2. 再問使用者要不要刪(問的時候路徑已經是真的,能顯示給他看)
    //   3. 停服務 / 反註冊(UninstallCleanup)—— 服務還握著詞庫時刪不掉
    //   4. 最後才刪資料
    GUserDataDir := QueryUserDataDir(ExpandConstant('{app}\' + SetupExeName));
    Log(LogTag + '使用者資料目錄 = ' + GUserDataDir);
    DecidePurge;
    UninstallCleanup;
    if GPurge then DoPurge(ExpandConstant('{app}\' + SetupExeName));
    Exit;
  end;
  if CurUninstallStep <> usPostUninstall then Exit;
  if UninstallSilent then Exit;
  // ⚠ 底下這幾行刻意先把訊息組進一個變數,而不是把 [GUserDataDir] 直接
  //   斷行寫在 FmtMessage 的參數位置。
  //
  //   ISCC 是**逐行**判斷區段標籤的,而且它會先去掉行首的空白 ——
  //   所以一行縮排之後以 `[` 開頭(例如續行的 `[GUserDataDir]),`)
  //   會被當成一個區段標籤,錯誤訊息是
  //     「PreprocessingError ... Invalid section tag」
  //   而它指的行號還是**別的地方**。實測:CI run #62 就是這樣紅的,
  //   而 windows/make_installer.sh --lint 那一步四分鐘就抓到了
  //   (真正的安裝程式建置在二十分鐘之後)。
  if GPurge then begin
    if GPurgeOk then
      Msg := FmtMessage(CustomMessage('UninstallPurgeDone'), [GUserDataDir])
    else
      Msg := FmtMessage(CustomMessage('UninstallPurgeFailed'), [GUserDataDir]);
    SuppressibleMsgBox(Msg, mbInformation, MB_OK, IDOK);
    Exit;
  end;
  // 明著告訴使用者他的詞典還在、在哪裡。悄悄留下一個資料夾跟悄悄刪掉一樣糟。
  if (GUserDataDir <> '') and DirExists(GUserDataDir) then begin
    Msg := FmtMessage(CustomMessage('UninstallKeptUserData'), [GUserDataDir]);
    SuppressibleMsgBox(Msg, mbInformation, MB_OK, IDOK);
  end;
end;
