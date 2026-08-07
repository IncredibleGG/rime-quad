// librime Android 交叉編譯的連結煙霧測試。
//
// 目的不是在裝置上跑,而是證明:
//   1. prebuilt 的公開標頭可以被 NDK clang++ 編譯;
//   2. 用 manifest.json 記載的連結順序可以把 librime.a 與所有相依 .a
//      連成 Android 執行檔 / .so,且沒有 undefined symbol。
//
// 由 scripts/build_native.sh 自動編譯,不需要手動執行。

#include <rime_api.h>
#include <rime_api_deprecated.h>
#include <rime_levers_api.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void rime_smoke_notification(void* /*context*/,
                                        RimeSessionId /*session_id*/,
                                        const char* message_type,
                                        const char* message_value) {
  std::printf("notification: %s = %s\n", message_type, message_value);
}

extern "C" int rime_smoke_main(void) {
  // --- 新版 API(rime_get_api)---
  RimeApi* api = rime_get_api();
  if (!api) {
    std::fprintf(stderr, "rime_get_api() returned null\n");
    return 1;
  }

  RIME_STRUCT(RimeTraits, traits);
  traits.shared_data_dir = "/data/local/tmp/rime/shared";
  traits.user_data_dir = "/data/local/tmp/rime/user";
  traits.log_dir = "/data/local/tmp/rime/log";
  traits.distribution_name = "Rime";
  traits.distribution_code_name = "rime-android-smoke";
  traits.distribution_version = "0.0.0";
  traits.app_name = "rime.android.smoke";

  api->setup(&traits);
  api->set_notification_handler(&rime_smoke_notification, nullptr);
  api->initialize(&traits);

  Bool full_check = False;
  api->start_maintenance(full_check);
  api->join_maintenance_thread();

  RimeSessionId session = api->create_session();
  if (session) {
    RIME_STRUCT(RimeCommit, commit);
    RIME_STRUCT(RimeContext, context);
    RIME_STRUCT(RimeStatus, status);
    api->process_key(session, 'a', 0);
    api->get_commit(session, &commit);
    api->free_commit(&commit);
    api->get_context(session, &context);
    api->free_context(&context);
    api->get_status(session, &status);
    api->free_status(&status);
    api->destroy_session(session);
  }

  // --- levers 模組(rime-levers 的符號要在 librime.a 裡)---
  RimeModule* levers_module = RimeFindModule("levers");
  if (levers_module && levers_module->get_api) {
    RimeLeversApi* levers =
        reinterpret_cast<RimeLeversApi*>(levers_module->get_api());
    if (levers) {
      RimeCustomSettings* settings =
          levers->custom_settings_init("default", "rime_android_smoke");
      if (settings) {
        levers->load_settings(settings);
        levers->custom_settings_destroy(settings);
      }
    }
  }

  // --- 舊版 deprecated C API(確認這些符號也在 .a 裡)---
  if (getenv("RIME_SMOKE_NEVER") != nullptr) {
    RimeSetup(&traits);
    RimeInitialize(&traits);
    RimeSyncUserData();
    RimeFinalize();
  }

  api->finalize();
  std::printf("rime %s smoke ok\n", api->get_version());
  return 0;
}

// 連成執行檔時需要 main;連成 .so 時這個 main 只是普通符號,無害。
int main(void) {
  return rime_smoke_main();
}
