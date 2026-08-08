/*
 * probe_host.c — Lua 沙盒探針的宿主
 *
 * 用**真的 Lua 5.4.8 直譯器**（librime-lua 出貨的那一份原始碼）重現
 * librime-lua 安裝沙盒的順序，然後把 probe.lua 丟進去跑：
 *
 *     luaL_openlibs()                      ← 上游：完整標準函式庫
 *     [stage >= 1] 第一層（src/lib/lua.cc 的 kRimeQuadSandbox）
 *     [stage >= 2] 第二層（src/modules.cc 的 kRimeQuadPathSandbox，帶兩個目錄）
 *     luaL_loadfile(probe.lua) + lua_pcall  ← 相當於 modules.cc 的 luaL_dofile(rime.lua)
 *
 * 兩段沙盒的內容**不是**複製過來的，是 verify_lua_sandbox.sh 從
 * patches/librime-lua@sandbox.patch 套用後的原始碼裡抽出來的。抽錯或抽空
 * 都會讓 stage 1/2 的斷言變成「和 stage 0 一樣」而整批失敗，不會靜靜通過。
 *
 * 為什麼 probe.lua 用 luaL_loadfile 而不是 Lua 的 dofile：第二層把 dofile
 * 收斂到資料目錄裡了，探針自己不在那裡面。modules.cc 載入 rime.lua 走的
 * 也是 C API（luaL_dofile），所以這裡的路徑和真的一樣。
 *
 * 回傳值 = probe.lua 留在全域 RIMEQUAD_FAILURES 的失敗數（>125 一律回 125）。
 * 用全域而不是 os.exit：os.exit 在 stage>=1 已經被拿掉了。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static char *slurp(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "probe_host: 讀不到 %s\n", path); exit(2); }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = (char *)malloc((size_t)n + 1);
  if (!buf) { fprintf(stderr, "probe_host: out of memory\n"); exit(2); }
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
    fprintf(stderr, "probe_host: 讀 %s 失敗\n", path); exit(2);
  }
  buf[n] = '\0';
  fclose(f);
  *len = (size_t)n;
  return buf;
}

static void setglobal_str(lua_State *L, const char *k, const char *v) {
  lua_pushstring(L, v);
  lua_setglobal(L, k);
}

int main(int argc, char **argv) {
  if (argc != 8) {
    fprintf(stderr,
      "用法: %s <stage 0|1|2> <stage1.lua> <stage2.lua> <user_dir> "
      "<shared_dir> <outside_dir> <probe.lua>\n", argv[0]);
    return 2;
  }
  int stage = atoi(argv[1]);
  const char *s1_path = argv[2], *s2_path = argv[3];
  const char *user_dir = argv[4], *shared_dir = argv[5];
  const char *outside_dir = argv[6], *probe = argv[7];

  lua_State *L = luaL_newstate();
  if (!L) { fprintf(stderr, "probe_host: luaL_newstate 失敗\n"); return 2; }
  luaL_openlibs(L);

  if (stage >= 1) {
    size_t n = 0;
    char *src = slurp(s1_path, &n);
    if (n == 0) {
      fprintf(stderr, "probe_host: 第一層沙盒是空的 —— 抽取失敗\n");
      return 2;
    }
    if (luaL_loadbuffer(L, src, n, "=rimequad-sandbox") != LUA_OK ||
        lua_pcall(L, 0, 0, 0) != LUA_OK) {
      fprintf(stderr, "probe_host: 第一層裝不上: %s\n", lua_tostring(L, -1));
      return 2;
    }
    free(src);
  }

  if (stage >= 2) {
    size_t n = 0;
    char *src = slurp(s2_path, &n);
    if (n == 0) {
      fprintf(stderr, "probe_host: 第二層沙盒是空的 —— 抽取失敗\n");
      return 2;
    }
    if (luaL_loadbuffer(L, src, n, "=rimequad-path-sandbox") != LUA_OK) {
      fprintf(stderr, "probe_host: 第二層載入失敗: %s\n", lua_tostring(L, -1));
      return 2;
    }
    lua_pushstring(L, user_dir);
    lua_pushstring(L, shared_dir);
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
      fprintf(stderr, "probe_host: 第二層裝不上: %s\n", lua_tostring(L, -1));
      return 2;
    }
    free(src);
  }

  lua_pushinteger(L, stage);
  lua_setglobal(L, "RIMEQUAD_STAGE");
  setglobal_str(L, "RIMEQUAD_USER", user_dir);
  setglobal_str(L, "RIMEQUAD_SHARED", shared_dir);
  setglobal_str(L, "RIMEQUAD_OUTSIDE", outside_dir);

  if (luaL_loadfile(L, probe) != LUA_OK ||
      lua_pcall(L, 0, 0, 0) != LUA_OK) {
    fprintf(stderr, "probe_host: probe.lua 爆掉: %s\n", lua_tostring(L, -1));
    return 2;
  }

  lua_getglobal(L, "RIMEQUAD_FAILURES");
  if (!lua_isinteger(L, -1)) {
    fprintf(stderr, "probe_host: probe.lua 沒有留下 RIMEQUAD_FAILURES —— "
                    "它可能中途就結束了，不算跑過\n");
    return 2;
  }
  lua_Integer fails = lua_tointeger(L, -1);
  lua_close(L);
  if (fails < 0) fails = 1;
  if (fails > 125) fails = 125;
  return (int)fails;
}
