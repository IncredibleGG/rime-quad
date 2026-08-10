// windows/common/net_gate_core.cc — 連網出口的流程。**不含任何網路 API。**
//
// ⚠ 這個檔案的名字裡有 net,但它一個 socket 都開不了:它只認得
//   NetTransport 這個介面。真的開連線的是 windows/service/net_gate.cc。

#include "net_gate_core.h"

#include <cstdio>

namespace rimewin {
namespace {

std::string Lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

// 只取 scheme。取不到回空字串。
std::string SchemeOf(const std::string& url) {
  const size_t c = url.find("://");
  if (c == std::string::npos || c == 0) return std::string();
  return Lower(url.substr(0, c));
}

std::string Num(long long v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lld", v);
  return std::string(buf);
}

}  // namespace

const char* NetResultText(NetResult r) {
  switch (r) {
    case NetResult::kOk: return "成功";
    case NetResult::kBlocked: return "連網開關是關閉的";
    case NetResult::kBadUrl: return "網址無效";
    case NetResult::kBadScheme: return "只支援 http / https";
    case NetResult::kDowngraded: return "轉址想把加密連線換成明文,已中止";
    case NetResult::kTooManyRedirects: return "轉址次數過多";
    case NetResult::kTooLarge: return "超過大小上限";
    case NetResult::kHttpError: return "伺服器回應不是 200";
    case NetResult::kTransportError: return "連線失敗";
  }
  return "?";
}

bool NetResultIsBlocked(NetResult r) { return r == NetResult::kBlocked; }

bool LocationSchemeAllowed(const std::string& location) {
  const size_t c = location.find("://");
  if (c == std::string::npos || c == 0) return true;  // 相對網址,交給 ResolveUrl
  // "://" 之前必須整段都是 scheme 合法字元,否則那個 "://" 是路徑或
  // 查詢字串裡剛好長這樣的東西,這一則 Location 仍然是相對網址。
  for (size_t i = 0; i < c; ++i) {
    const char ch = location[i];
    const bool scheme_char = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                             (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' ||
                             ch == '.';
    if (!scheme_char) return true;
  }
  const std::string s = Lower(location.substr(0, c));
  return s == "http" || s == "https";
}

bool NoDowngrade(const std::string& from, const std::string& to) {
  // 來源不是 https:本來就沒有錨可以掉。
  if (SchemeOf(from) != "https") return true;
  // ⚠ 只對 http **這一個** scheme 說話。https → ftp:// / file:// 也該擋,
  //   但那不是「降級」而是「scheme 不准」,該由下一跳的 CheckHop 回答 ——
  //   net_policy.cc 為了同一個理由把 scheme 檢查排在 authority 之前:
  //   要對使用者說的那句話不一樣,而說錯話比不說更糟。
  return SchemeOf(to) != "http";
}

NetReport RunFetch(const NetRequest& req, const NetSwitchFn& is_enabled,
                   NetTransport* transport, NetSink* sink,
                   const NetRecordFn& record_fn, const NetClockFn& now_ms) {
  NetReport rep;
  if (transport == nullptr || sink == nullptr) {
    rep.result = NetResult::kTransportError;
    rep.message = "內部錯誤:沒有傳輸層";
    rep.host = HostOf(req.url);
    return rep;
  }

  // ⚠ **整個檔案只有這一個地方會產生紀錄,而且它只在 ++rep.hops 之後被呼叫。**
  //   把它留在這裡(而不是散在各個分支)是刻意的:要驗「有沒有人在沒連線
  //   的路徑上偷記一筆」,只要看這個 lambda 的呼叫點都在不在那條線之後。
  const auto record = [&](const std::string& host, NetOutcome outcome,
                          int64_t bytes, const std::string& detail) {
    if (!record_fn) return;
    NetLogEntry e;
    e.at_ms = now_ms ? now_ms() : 0;
    e.host = host;
    e.purpose = req.purpose;
    e.label = req.label;
    e.outcome = outcome;
    e.bytes = bytes;
    e.detail = detail;
    record_fn(e);
  };

  std::string url = req.url;
  int redirects = 0;

  for (;;) {
    // ⚠ 每一跳都重新問開關。使用者可能在下載到一半時把它關掉,
    //   那一下必須真的中斷後續連線,而不是等這一輪跑完。
    const bool enabled = is_enabled ? is_enabled() : false;
    const HopVerdict v = CheckHop(enabled, url);
    if (v != HopVerdict::kProceed) {
      // ══════════════════════════════════════════════════════════
      //  這條路上**一個字都不記**。見 net_policy.h 檔頭。
      //  被擋下的嘗試不是一次連線;記了的話,「開關從沒開過所以
      //  紀錄是空的」這句話就不成立,而那正是使用者驗證我們的方式。
      // ══════════════════════════════════════════════════════════
      sink->Discard();
      rep.host = HostOf(url);
      switch (v) {
        case HopVerdict::kBlockedBySwitch: rep.result = NetResult::kBlocked; break;
        case HopVerdict::kBadUrl:          rep.result = NetResult::kBadUrl; break;
        case HopVerdict::kBadScheme:       rep.result = NetResult::kBadScheme; break;
        case HopVerdict::kProceed:         break;  // 到不了
      }
      rep.message = NetResultText(rep.result);
      return rep;
    }

    const std::string host = HostOf(url);
    rep.host = host;

    NetTransport::Head h;
    transport->Begin(url, &h);

    if (!h.sent) {
      // 連請求都沒送出去(session / connection handle 開不起來)。
      // 網路上什麼都沒發生 —— 沒有 DNS 查詢,沒有封包。所以不記。
      transport->End();
      sink->Discard();
      rep.result = NetResult::kTransportError;
      rep.message = h.error.empty() ? NetResultText(rep.result) : h.error;
      return rep;
    }

    // ══════════════════════════════════════════════════════════════
    //  過了這一行,這一跳**一定**會產生恰好一筆紀錄。
    //  下面每一條 return 之前都有一次 record(),continue 那一條也有。
    // ══════════════════════════════════════════════════════════════
    ++rep.hops;

    if (!h.ok) {
      const std::string why = h.error.empty() ? "連線失敗" : h.error;
      record(host, NetOutcome::kFailed, 0, why);
      transport->End();
      sink->Discard();
      rep.result = NetResult::kTransportError;
      rep.message = why;
      return rep;
    }

    if (h.status >= 300 && h.status <= 399) {
      if (h.location.empty()) {
        record(host, NetOutcome::kFailed, 0, "HTTP " + Num(h.status) + " 無 Location");
        transport->End();
        sink->Discard();
        rep.result = NetResult::kHttpError;
        rep.message = "HTTP " + Num(h.status) + ",但沒有 Location 標頭";
        return rep;
      }
      if (++redirects > kMaxRedirects) {
        record(host, NetOutcome::kFailed, 0, "轉址過多");
        transport->End();
        sink->Discard();
        rep.result = NetResult::kTooManyRedirects;
        rep.message = NetResultText(rep.result);
        return rep;
      }
      if (!LocationSchemeAllowed(h.location)) {
        // ⚠ 這一條不能省,也不能交給下一跳的 CheckHop 代勞。理由見
        //   net_gate_core.h 的 LocationSchemeAllowed:ResolveUrl 只認得
        //   http(s):// 開頭的絕對網址,`file:///x` 會被它接成
        //   `https://<原主機>/file:///x`,於是 scheme 檢查根本看不到那個 file。
        record(host, NetOutcome::kFailed, 0,
               "HTTP " + Num(h.status) + " 想轉去不支援的 scheme,已中止");
        transport->End();
        sink->Discard();
        rep.result = NetResult::kBadScheme;
        rep.message = NetResultText(rep.result);
        return rep;
      }
      const std::string next = ResolveUrl(url, h.location);
      if (!NoDowngrade(url, next)) {
        // 加密連線被導去明文。Windows 端沒有程式碼簽章,TLS 憑證是唯一的
        // 信任錨,跟過去等於把它丟掉。見 net_gate_core.h 的 NoDowngrade。
        record(host, NetOutcome::kFailed, 0,
               "HTTP " + Num(h.status) + " 想降級成明文,已中止");
        transport->End();
        sink->Discard();
        rep.result = NetResult::kDowngraded;
        rep.message = NetResultText(rep.result);
        return rep;
      }
      // 轉址也是一次真的連線。不記的話,紀錄就會漏掉使用者最想知道的
      // 那一件事:它到底連了哪幾台主機。
      record(host, NetOutcome::kRedirected, 0,
             "HTTP " + Num(h.status) + " → " + HostOf(next));
      transport->End();
      url = next;
      continue;
    }

    if (h.status != 200) {
      record(host, NetOutcome::kFailed, 0, "HTTP " + Num(h.status));
      transport->End();
      sink->Discard();
      rep.result = NetResult::kHttpError;
      rep.message = "HTTP " + Num(h.status);
      return rep;
    }

    // 宣告的大小只是**提早收手**用的,真正的牆在下面邊收邊數 ——
    // Content-Length 是對方給的,它可以撒謊,也可以整個不給。
    if (h.declared_bytes > 0 && h.declared_bytes > req.max_bytes) {
      record(host, NetOutcome::kFailed, 0, "宣告大小超過上限");
      transport->End();
      sink->Discard();
      rep.result = NetResult::kTooLarge;
      rep.message = "回應宣告 " + Num(h.declared_bytes) + " 位元組,超過上限 " +
                    Num(req.max_bytes);
      return rep;
    }

    int64_t read = 0;
    bool over = false;
    std::string err;
    const bool body_ok = transport->ReadBody(sink, req.max_bytes, &read, &over, &err);
    transport->End();

    if (over) {
      sink->Discard();
      record(host, NetOutcome::kFailed, read, "超過大小上限,已中止");
      rep.result = NetResult::kTooLarge;
      rep.bytes = read;
      rep.message = "下載量超過上限 " + Num(req.max_bytes) + " 位元組,已中止";
      return rep;
    }
    if (!body_ok) {
      sink->Discard();
      const std::string why = err.empty() ? "下載中斷" : err;
      record(host, NetOutcome::kFailed, read, why);
      rep.result = NetResult::kTransportError;
      rep.bytes = read;
      rep.message = why;
      return rep;
    }

    record(host, NetOutcome::kOk, read, std::string());
    rep.result = NetResult::kOk;
    rep.bytes = read;
    rep.message = NetResultText(NetResult::kOk);
    return rep;
  }
}

}  // namespace rimewin
