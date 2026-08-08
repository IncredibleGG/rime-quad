// windows/tests/check.h — 極小的測試框架
//
// 為什麼不用 gtest:CI 上多一個要下載、要編譯、要釘版本的相依,
// 而這裡需要的功能是「跑一堆函式,有一個沒過就非零結束」。
//
// ⚠ 這個專案抓過**會靜靜跳過自己的測試**:發布關卡的升級測試因為步驟順序
//   寫反被判「略過」,報出一片全綠;LayoutEscapeTest 的清單寫死四份,
//   12 份裡有 8 份從沒被檢查過。所以本框架刻意做了三件事:
//
//   1. 沒有 SKIP。要嘛跑、要嘛不存在。
//   2. 結束時印出**跑了幾個斷言**,而且斷言數為 0 就算失敗 ——
//      「測試函式存在但裡面什麼都沒斷言」在報表上與通過長得一樣。
//   3. 有一個 --self-check 模式:故意跑一個必定失敗的斷言。
//      CI 會執行它並要求**非零結束**。反過來驗這個框架真的會紅。
//
#ifndef RIMEWIN_TESTS_CHECK_H_
#define RIMEWIN_TESTS_CHECK_H_

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace rimewin_test {

struct Case {
  const char* name;
  void (*fn)();
};

inline std::vector<Case>& Registry() {
  static std::vector<Case> r;
  return r;
}

inline int& Failures() {
  static int n = 0;
  return n;
}
inline long& Assertions() {
  static long n = 0;
  return n;
}
inline const char*& CurrentCase() {
  static const char* n = "(none)";
  return n;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { Registry().push_back({name, fn}); }
};

inline void Fail(const char* file, int line, const std::string& msg) {
  ++Failures();
  std::fprintf(stderr, "  !! [%s] %s:%d\n     %s\n", CurrentCase(), file, line,
               msg.c_str());
}

template <typename A, typename B>
inline void CheckEq(const char* file, int line, const char* ea, const char* eb,
                    const A& a, const B& b) {
  ++Assertions();
  if (!(a == b)) {
    std::string m = std::string(ea) + " == " + eb;
    Fail(file, line, m + "\n     實際不相等");
  }
}

}  // namespace rimewin_test

#define RW_CONCAT_(a, b) a##b
#define RW_CONCAT(a, b) RW_CONCAT_(a, b)

#define TEST(name)                                                       \
  static void name();                                                    \
  static ::rimewin_test::Registrar RW_CONCAT(reg_, name)(#name, &name); \
  static void name()

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++::rimewin_test::Assertions();                                          \
    if (!(cond)) ::rimewin_test::Fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
  } while (0)

#define CHECK_EQ(a, b)                                                       \
  do {                                                                       \
    ++::rimewin_test::Assertions();                                          \
    if (!((a) == (b))) {                                                     \
      char buf[512];                                                         \
      std::snprintf(buf, sizeof(buf), "CHECK_EQ(%s, %s)", #a, #b);           \
      ::rimewin_test::Fail(__FILE__, __LINE__, buf);                         \
    }                                                                        \
  } while (0)

// 整數專用:失敗時把兩邊的值印出來。查錯時「不相等」四個字沒有用。
#define CHECK_INT(a, b)                                                        \
  do {                                                                         \
    ++::rimewin_test::Assertions();                                            \
    const long long va_ = (long long)(a);                                      \
    const long long vb_ = (long long)(b);                                      \
    if (va_ != vb_) {                                                          \
      char buf[512];                                                           \
      std::snprintf(buf, sizeof(buf), "%s == %s  →  %lld (0x%llX) != %lld (0x%llX)", \
                    #a, #b, va_, (unsigned long long)va_, vb_,                 \
                    (unsigned long long)vb_);                                  \
      ::rimewin_test::Fail(__FILE__, __LINE__, buf);                           \
    }                                                                          \
  } while (0)

#define CHECK_STR(a, b)                                                      \
  do {                                                                       \
    ++::rimewin_test::Assertions();                                          \
    const std::string sa_ = (a);                                             \
    const std::string sb_ = (b);                                             \
    if (sa_ != sb_) {                                                        \
      char buf[1024];                                                        \
      std::snprintf(buf, sizeof(buf), "%s == %s  →  \"%s\" != \"%s\"", #a,   \
                    #b, sa_.c_str(), sb_.c_str());                           \
      ::rimewin_test::Fail(__FILE__, __LINE__, buf);                         \
    }                                                                        \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                \
  do {                                                                       \
    ++::rimewin_test::Assertions();                                          \
    const double da_ = (double)(a);                                          \
    const double db_ = (double)(b);                                          \
    const double d_ = da_ > db_ ? da_ - db_ : db_ - da_;                     \
    if (!(d_ <= (eps))) {                                                    \
      char buf[512];                                                         \
      std::snprintf(buf, sizeof(buf), "%s ≈ %s  →  %f vs %f", #a, #b, da_,   \
                    db_);                                                    \
      ::rimewin_test::Fail(__FILE__, __LINE__, buf);                         \
    }                                                                        \
  } while (0)

#endif  // RIMEWIN_TESTS_CHECK_H_
