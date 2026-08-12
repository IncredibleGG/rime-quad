/*
 * test_host.c — 跑 charset_guard/test.lua 的最小 Lua 宿主
 *
 * librime-lua 的 `thirdparty/lua5.4` 只有**函式庫**，沒有上游的 lua.c
 * （那一份 CLI 不在他們打包的範圍裡）。所以這裡自己給一個 main：
 * 開標準函式庫、擺好 `arg`、把測試檔跑掉，結束碼就是測試的結束碼。
 *
 * 為什麼不裝系統的 lua：要測的是**出貨的那一份直譯器**的行為
 * （utf8 函式庫、長字串、coroutine 的語意都可能隨版本變）。
 * 這與 scripts/lua_sandbox/probe_host.c 是同一個理由、同一個作法。
 *
 * 用法：test_host <script.lua> [args…]
 */
#include <stdio.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "用法: %s <script.lua> [args…]\n", argv[0]);
    return 2;
  }
  lua_State *L = luaL_newstate();
  if (!L) { fprintf(stderr, "test_host: out of memory\n"); return 2; }
  luaL_openlibs(L);

  /* arg[1] = 第一個腳本參數，與 lua CLI 一致。 */
  lua_newtable(L);
  for (int i = 2; i < argc; i++) {
    lua_pushstring(L, argv[i]);
    lua_rawseti(L, -2, i - 1);
  }
  lua_pushstring(L, argv[1]);
  lua_rawseti(L, -2, 0);
  lua_setglobal(L, "arg");

  int rc = 0;
  if (luaL_loadfile(L, argv[1]) != LUA_OK ||
      lua_pcall(L, 0, 0, 0) != LUA_OK) {
    const char *e = lua_tostring(L, -1);
    fprintf(stderr, "test_host: %s\n", e ? e : "(未知錯誤)");
    rc = 1;
  }
  lua_close(L);
  return rc;
}
