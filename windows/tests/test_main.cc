// windows/tests/test_main.cc — 測試進入點
//
// 用法:
//   rime_tests                # 跑全部
//   rime_tests --list         # 列出案例
//   rime_tests --filter=<子字串>
//   rime_tests --self-check   # 反向測試:故意失敗。CI 要求它**非零結束**

#include <cstdio>
#include <cstring>
#include <string>

#include "check.h"

using rimewin_test::Assertions;
using rimewin_test::CurrentCase;
using rimewin_test::Failures;
using rimewin_test::Registry;

int main(int argc, char** argv) {
  const char* filter = nullptr;
  bool list_only = false;
  bool self_check = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--list") == 0) list_only = true;
    else if (std::strcmp(argv[i], "--self-check") == 0) self_check = true;
    else if (std::strncmp(argv[i], "--filter=", 9) == 0) filter = argv[i] + 9;
    else {
      std::fprintf(stderr, "未知參數: %s\n", argv[i]);
      return 2;
    }
  }

  if (self_check) {
    // 反向測試。這不是湊數:macOS 端已經因為「斷言沒有錨定」而被中途輸出騙過,
    // 這個專案也有過「測試是綠的,因為它沒在測」。這一步證明框架真的會紅。
    std::printf("--self-check:故意讓一個斷言失敗,預期本進程以非零結束\n");
    CurrentCase() = "self_check";
    CHECK_INT(1, 2);
    if (Failures() == 0) {
      std::fprintf(stderr,
                   "!! 故意失敗的斷言竟然通過了 —— 這個測試框架不會紅,"
                   "它報出來的所有綠燈都不算數\n");
      return 3;
    }
    std::printf("框架有正確判定失敗(failures=%d)\n", Failures());
    return 1;  // 非零 = CI 那一步預期的結果
  }

  if (list_only) {
    for (const auto& c : Registry()) std::printf("%s\n", c.name);
    return 0;
  }

  int ran = 0;
  for (const auto& c : Registry()) {
    if (filter && std::strstr(c.name, filter) == nullptr) continue;
    CurrentCase() = c.name;
    const int before = Failures();
    const long before_a = Assertions();
    c.fn();
    ++ran;
    const bool ok = Failures() == before;
    std::printf("%s %-44s (%ld 個斷言)\n", ok ? "  ok" : "FAIL", c.name,
                Assertions() - before_a);
    // 「測試函式存在但一個斷言都沒有」在報表上與通過長得一樣。明著擋掉。
    if (Assertions() == before_a) {
      std::fprintf(stderr, "  !! [%s] 這個案例一個斷言都沒跑 —— 它不是在測試\n",
                   c.name);
      ++Failures();
    }
  }

  std::printf("\n跑了 %d 個案例,共 %ld 個斷言,失敗 %d\n", ran, Assertions(),
              Failures());
  if (ran == 0) {
    // 篩選字串打錯 / 沒有任何案例被註冊 —— 這種情形報「全過」是騙人的。
    std::fprintf(stderr, "!! 一個案例都沒跑\n");
    return 2;
  }
  return Failures() == 0 ? 0 : 1;
}
