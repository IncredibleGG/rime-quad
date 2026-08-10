// windows/common/update_flow.cc — 純邏輯。不連網、不碰檔案、不叫任何進程。

#include "update_flow.h"

namespace rimewin {

const char* UpdateFailureTag(UpdateFailure f) {
  switch (f) {
    case UpdateFailure::kNone: return "NONE";
    case UpdateFailure::kSwitchOff: return "SWITCH_OFF";
    case UpdateFailure::kUnreachable: return "UNREACHABLE";
    case UpdateFailure::kUnreadable: return "UNREADABLE";
    case UpdateFailure::kOwnVersionUnknown: return "OWN_VERSION_UNKNOWN";
    case UpdateFailure::kDownloadInterrupted: return "DOWNLOAD_INTERRUPTED";
    case UpdateFailure::kDownloadTooLarge: return "DOWNLOAD_TOO_LARGE";
    case UpdateFailure::kSha256Mismatch: return "SHA256_MISMATCH";
    case UpdateFailure::kStagingWriteFailed: return "STAGING_WRITE_FAILED";
    case UpdateFailure::kProductChanged: return "PRODUCT_CHANGED";
    case UpdateFailure::kBusyCannotStop: return "BUSY_CANNOT_STOP";
    case UpdateFailure::kElevationDeclined: return "ELEVATION_DECLINED";
    case UpdateFailure::kHandoffFailed: return "HANDOFF_FAILED";
    case UpdateFailure::kFileLocked: return "FILE_LOCKED";
    case UpdateFailure::kNotInstalled: return "NOT_INSTALLED";
    case UpdateFailure::kFailureCount: break;
  }
  return "UNKNOWN";
}

UiString UpdateFailureText(UpdateFailure f) {
  switch (f) {
    // kNone 沒有訊息 —— 呼叫端不該把它畫出來。
    case UpdateFailure::kNone: return UiString::kUiStringCount;
    case UpdateFailure::kSwitchOff: return UiString::kUpdateErrSwitchOff;
    case UpdateFailure::kUnreachable: return UiString::kUpdateErrUnreachable;
    case UpdateFailure::kUnreadable: return UiString::kUpdateErrUnreadable;
    case UpdateFailure::kOwnVersionUnknown:
      return UiString::kUpdateErrOwnVersionUnknown;
    case UpdateFailure::kDownloadInterrupted:
      return UiString::kUpdateErrDownloadInterrupted;
    case UpdateFailure::kDownloadTooLarge: return UiString::kUpdateErrTooLarge;
    case UpdateFailure::kSha256Mismatch: return UiString::kUpdateErrSha256;
    case UpdateFailure::kStagingWriteFailed:
      return UiString::kUpdateErrStagingWrite;
    case UpdateFailure::kProductChanged:
      return UiString::kUpdateErrProductChanged;
    case UpdateFailure::kBusyCannotStop: return UiString::kUpdateErrBusy;
    case UpdateFailure::kElevationDeclined:
      return UiString::kUpdateErrElevationDeclined;
    case UpdateFailure::kHandoffFailed: return UiString::kUpdateErrHandoffFailed;
    case UpdateFailure::kFileLocked: return UiString::kUpdateErrFileLocked;
    case UpdateFailure::kNotInstalled: return UiString::kUpdateErrNotInstalled;
    case UpdateFailure::kFailureCount: break;
  }
  return UiString::kUiStringCount;
}

bool UpdateFailureCanRetry(UpdateFailure f) {
  switch (f) {
    // 再按一次不會有任何不同 —— 給重試等於請使用者重複做一件註定失敗的事。
    case UpdateFailure::kNone:
    case UpdateFailure::kSwitchOff:
    case UpdateFailure::kProductChanged:
    case UpdateFailure::kOwnVersionUnknown:
      return false;
    default:
      return true;
  }
}

bool UpdateFailureNeedsSwitch(UpdateFailure f) {
  return f == UpdateFailure::kSwitchOff;
}

UpdateFailure ClassifyManifestFetch(NetResult r) {
  switch (r) {
    case NetResult::kOk: return UpdateFailure::kNone;
    case NetResult::kBlocked: return UpdateFailure::kSwitchOff;
    // 網址壞掉 / scheme 不准 / 降級轉址 → 這一份清單本身有問題,
    // 不是「連不上」。使用者要做的事是回報,不是重試。
    case NetResult::kBadUrl:
    case NetResult::kBadScheme:
    case NetResult::kDowngraded:
      return UpdateFailure::kUnreadable;
    case NetResult::kTooLarge: return UpdateFailure::kDownloadTooLarge;
    case NetResult::kTooManyRedirects:
    case NetResult::kHttpError:
    case NetResult::kTransportError:
      return UpdateFailure::kUnreachable;
  }
  return UpdateFailure::kUnreachable;
}

UpdateFailure ClassifyDownload(NetResult r) {
  switch (r) {
    case NetResult::kOk: return UpdateFailure::kNone;
    // ⚠ 下載到一半使用者把開關關掉,走的也是這一條。訊息要說
    //   「已經下載的部分丟掉了、你的輸入法一點都沒有動」——
    //   而不是一句看起來像我們壞掉的紅字。
    case NetResult::kBlocked: return UpdateFailure::kSwitchOff;
    case NetResult::kTooLarge: return UpdateFailure::kDownloadTooLarge;
    case NetResult::kBadUrl:
    case NetResult::kBadScheme:
    case NetResult::kDowngraded:
      return UpdateFailure::kUnreadable;
    case NetResult::kTooManyRedirects:
    case NetResult::kHttpError:
    case NetResult::kTransportError:
      return UpdateFailure::kDownloadInterrupted;
  }
  return UpdateFailure::kDownloadInterrupted;
}

UpdateFailure ClassifyHandoff(bool shell_ok, unsigned long win32_error) {
  if (shell_ok) return UpdateFailure::kNone;
  if (win32_error == kWin32ErrorCancelled) return UpdateFailure::kElevationDeclined;
  return UpdateFailure::kHandoffFailed;
}

bool MayHandOff(const HandoffPreflight& pre, UpdateFailure* why) {
  UpdateFailure dummy = UpdateFailure::kNone;
  UpdateFailure& out = why ? *why : dummy;
  out = UpdateFailure::kNone;

  // ⚠ **這一條排在最前面,而且不可以往下移。** 沒有驗過摘要的檔案
  //   一律不交出去 —— 其餘每一格都是「要不要做」,只有這一格是
  //   「做了會怎樣」。反向測試把它拿掉時,布林立方體那條會紅。
  if (!pre.sha256_verified) {
    out = UpdateFailure::kSha256Mismatch;
    return false;
  }
  if (!pre.file_present) {
    out = UpdateFailure::kStagingWriteFailed;
    return false;
  }
  if (!pre.installed_version_known) {
    out = UpdateFailure::kOwnVersionUnknown;
    return false;
  }
  if (!pre.have_manifest) {
    out = UpdateFailure::kUnreachable;
    return false;
  }
  // 換了身分的那一版:裝下去會多出第二套,而不是蓋掉這一套。不給按鈕。
  if (pre.app_id == AppIdVerdict::kChanged) {
    out = UpdateFailure::kProductChanged;
    return false;
  }
  if (pre.verdict != UpdateVerdict::kUpdateAvailable) {
    // 沒有東西要裝,不是錯誤。
    out = UpdateFailure::kNone;
    return false;
  }
  if (pre.deploy_running) {
    out = UpdateFailure::kBusyCannotStop;
    return false;
  }
  return true;
}

UpdateOutcome ReconcileHandoff(bool have_record, int64_t handed_code,
                               int64_t installed_code,
                               bool queued_in_install_dir, UpdateFailure* why) {
  UpdateFailure dummy = UpdateFailure::kNone;
  UpdateFailure& out = why ? *why : dummy;
  out = UpdateFailure::kNone;

  if (!have_record) return UpdateOutcome::kNoHandoff;

  // ⚠ 用 >= 而不是 ==:交棒之後、我們回來之前,使用者完全可以自己再裝
  //   一個更新的版本(下載頁就在那裡)。那時 == 會判成「沒裝成」,
  //   然後對一個剛剛才手動更新完的人說「更新沒有裝上去」。
  if (installed_code > 0 && handed_code > 0 && installed_code >= handed_code)
    return UpdateOutcome::kInstalled;

  out = queued_in_install_dir ? UpdateFailure::kFileLocked
                              : UpdateFailure::kNotInstalled;
  return UpdateOutcome::kNotInstalled;
}

UpdateCard DescribeUpdateCard(const UpdateUiState& s) {
  UpdateCard c;
  // 信任錨那一句在每一條路徑上都在。不是「錯誤時才說」。
  c.trust = UiString::kUpdateTrustAnchor;

  // 失敗優先:它是使用者剛剛按下那一下的結果,不該被進度訊息蓋掉。
  if (s.failure != UpdateFailure::kNone) {
    c.status = UpdateFailureText(s.failure);
    c.show_check_button = !UpdateFailureNeedsSwitch(s.failure) &&
                          UpdateFailureCanRetry(s.failure);
    c.show_page_button = (s.failure == UpdateFailure::kProductChanged) ||
                         (s.failure == UpdateFailure::kOwnVersionUnknown);
    c.busy = false;
    return c;
  }

  switch (s.stage) {
    case UpdateStage::kChecking:
      c.status = UiString::kUpdateStatusChecking;
      c.busy = true;
      c.show_check_button = false;
      return c;
    case UpdateStage::kDownloading:
      c.status = UiString::kUpdateStatusDownloading;
      c.busy = true;
      c.show_check_button = false;
      return c;
    case UpdateStage::kVerifying:
      c.status = UiString::kUpdateStatusVerifying;
      c.busy = true;
      c.show_check_button = false;
      return c;
    case UpdateStage::kHandedOff:
      c.status = UiString::kUpdateStatusHandedOff;
      c.busy = true;
      c.show_check_button = false;
      return c;
    case UpdateStage::kReady:
      c.status = UiString::kUpdateStatusReady;
      // ⚠ 沒驗過就沒有按鈕。這是 MayHandOff 那條硬條件在畫面上的一半:
      //   使用者連按都按不到。
      if (s.file_verified) c.action = UiString::kUpdateInstallNowButton;
      return c;
    case UpdateStage::kIdle:
      break;
  }

  if (!s.have_manifest) {
    c.status = UiString::kUpdateStatusIdle;
    return c;
  }
  if (!s.installed_version_known) {
    c.status = UiString::kUpdateErrOwnVersionUnknown;
    c.show_page_button = true;
    return c;
  }
  if (s.app_id == AppIdVerdict::kChanged) {
    c.status = UiString::kUpdateErrProductChanged;
    c.show_page_button = true;
    return c;
  }
  switch (s.verdict) {
    case UpdateVerdict::kUpToDate:
      c.status = UiString::kUpdateStatusUpToDate;
      return c;
    case UpdateVerdict::kDowngrade:
      // 線上比本機舊 = 發布端出了事。給一顆「更新」等於請使用者用一個
      // 更舊的版本蓋掉自己。
      c.status = UiString::kUpdateStatusDowngrade;
      return c;
    case UpdateVerdict::kUpdateAvailable:
      c.status = UiString::kUpdateStatusAvailable;
      c.action = UiString::kUpdateInstallButton;
      c.show_page_button = true;
      return c;
  }
  c.status = UiString::kUpdateStatusIdle;
  return c;
}

}  // namespace rimewin
