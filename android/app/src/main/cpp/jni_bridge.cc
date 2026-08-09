/*
 * jni_bridge.cc — rime_shell.h ↔ Kotlin 的 JNI 橋接
 *
 * 設計要點
 * ────────
 * 1. 用 RegisterNatives 而非 Java_xxx 命名慣例，套件名由建置系統以
 *    -DRIME_JNI_CLASS 傳進來（見 gradle.properties 的 rime.jniClass）。
 *    日後改 applicationId 時，這個檔案一個字都不用動。
 *
 * 2. rime_shell.h 的記憶體約定：rs_snapshot_acquire() 回傳的指標與其中
 *    所有字串，只在下一次 acquire / release 之前有效。因此
 *    nativeSnapshot() 在「同一次 JNI 呼叫內」完成
 *        acquire → 全部複製成 JVM 物件 → release
 *    絕不把 const rs_snapshot* 存起來跨呼叫使用。
 *
 * 3. 為了避免大量 NewObject 的簽章錯誤風險，快照以兩個平坦陣列回傳
 *    （int[] + String[]），由 Kotlin 端 RimeSnapshot.decode() 組裝。
 *    編碼格式在下方 kSnapshot* 註解與 RimeModels.kt 中同步描述。
 */

#include <jni.h>
#include <android/log.h>

#include <cstring>
#include <string>
#include <vector>

#include "rime_shell.h"

#ifndef RIME_JNI_CLASS
#define RIME_JNI_CLASS "org/luminakey/ime/core/RimeCore"
#endif

#define LOG_TAG "RimeJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

JavaVM* g_vm = nullptr;
jclass g_core_class = nullptr;   // global ref，RIME_JNI_CLASS
jmethodID g_on_deploy = nullptr; // static void onDeployStatusFromNative(int)

// rs_setup 的字串由呼叫端擁有；不確定 rs_init 是否複製，這裡自己留一份。
std::string g_user_data_dir;
std::string g_shared_data_dir;
std::string g_log_dir;
std::string g_app_name;
bool g_has_log_dir = false;

std::string FromJString(JNIEnv* env, jstring s) {
  if (s == nullptr) return std::string();
  const char* chars = env->GetStringUTFChars(s, nullptr);
  std::string out(chars ? chars : "");
  if (chars) env->ReleaseStringUTFChars(s, chars);
  return out;
}

jstring ToJString(JNIEnv* env, const char* s) {
  return env->NewStringUTF(s ? s : "");
}

void DeployCallback(rs_deploy_status status, void* /*userdata*/) {
  if (g_vm == nullptr || g_core_class == nullptr || g_on_deploy == nullptr) {
    return;
  }
  JNIEnv* env = nullptr;
  bool attached = false;
  jint rc = g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (rc == JNI_EDETACHED) {
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
      LOGE("DeployCallback: AttachCurrentThread 失敗");
      return;
    }
    attached = true;
  } else if (rc != JNI_OK) {
    return;
  }
  env->CallStaticVoidMethod(g_core_class, g_on_deploy,
                            static_cast<jint>(status));
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
  if (attached) g_vm->DetachCurrentThread();
}

/* ── 快照編碼 ─────────────────────────────────────────────────────────────
 *
 * ints  長度固定 14：
 *   [0] composition.sel_start
 *   [1] composition.sel_end
 *   [2] composition.caret
 *   [3] menu.count
 *   [4] menu.page_no
 *   [5] menu.highlighted
 *   [6] menu.is_last_page
 *   [7] status.is_composing
 *   [8] status.is_ascii_mode
 *   [9] status.is_full_shape
 *  [10] status.is_simplified
 *  [11] status.is_ascii_punct
 *  [12] status.is_disabled
 *  [13] commit_text 是否為非 NULL
 *
 * strs  長度 4 + 3 * count：
 *   [0] composition.preedit
 *   [1] commit_text（NULL 時為 Java null）
 *   [2] status.schema_id
 *   [3] status.schema_name
 *   之後每 3 個一組：text, comment, label
 * ─────────────────────────────────────────────────────────────────────── */
constexpr jsize kSnapshotIntCount = 14;
constexpr jsize kSnapshotStringHeader = 4;

/* ── native 方法實作 ─────────────────────────────────────────────────── */

jint nativeAbiVersion(JNIEnv*, jclass) {
  return static_cast<jint>(rs_abi_version());
}

jint nativeExpectedAbiVersion(JNIEnv*, jclass) {
  // 編譯期常數，供 Kotlin 端與執行期版本比對。
  return static_cast<jint>(RIME_SHELL_ABI_VERSION);
}

jboolean nativeIsStub(JNIEnv*, jclass) {
#ifdef RIME_HAVE_LIBRIME
  return JNI_FALSE;
#else
  return JNI_TRUE;
#endif
}

jboolean nativeInit(JNIEnv* env, jclass, jstring userDataDir,
                    jstring sharedDataDir, jstring logDir, jstring appName) {
  g_user_data_dir = FromJString(env, userDataDir);
  g_shared_data_dir = FromJString(env, sharedDataDir);
  g_has_log_dir = (logDir != nullptr);
  g_log_dir = FromJString(env, logDir);
  g_app_name = FromJString(env, appName);

  rs_setup setup;
  std::memset(&setup, 0, sizeof(setup));
  setup.user_data_dir = g_user_data_dir.c_str();
  setup.shared_data_dir = g_shared_data_dir.c_str();
  setup.log_dir = g_has_log_dir ? g_log_dir.c_str() : nullptr;
  setup.app_name = g_app_name.c_str();
  setup.on_deploy = &DeployCallback;
  setup.userdata = nullptr;

  bool ok = rs_init(&setup);
  LOGI("rs_init(user=%s, shared=%s) -> %d", g_user_data_dir.c_str(),
       g_shared_data_dir.c_str(), ok ? 1 : 0);
  return ok ? JNI_TRUE : JNI_FALSE;
}

void nativeFinalize(JNIEnv*, jclass) { rs_finalize(); }

jboolean nativeDeploy(JNIEnv*, jclass) {
  return rs_deploy() ? JNI_TRUE : JNI_FALSE;
}

jstring nativeLastError(JNIEnv* env, jclass) {
  return ToJString(env, rs_last_error());
}

jlong nativeSessionCreate(JNIEnv*, jclass) {
  return static_cast<jlong>(rs_session_create());
}

void nativeSessionDestroy(JNIEnv*, jclass, jlong s) {
  rs_session_destroy(static_cast<rs_session>(s));
}

jboolean nativeSessionAlive(JNIEnv*, jclass, jlong s) {
  return rs_session_alive(static_cast<rs_session>(s)) ? JNI_TRUE : JNI_FALSE;
}

jboolean nativeProcessKey(JNIEnv*, jclass, jlong s, jint keysym,
                          jint modifiers) {
  return rs_process_key(static_cast<rs_session>(s), keysym,
                        static_cast<uint32_t>(modifiers))
             ? JNI_TRUE
             : JNI_FALSE;
}

jboolean nativeSelectCandidate(JNIEnv*, jclass, jlong s, jint index) {
  return rs_select_candidate(static_cast<rs_session>(s), index) ? JNI_TRUE
                                                                : JNI_FALSE;
}

jboolean nativeDeleteCandidate(JNIEnv*, jclass, jlong s, jint index) {
  return rs_delete_candidate(static_cast<rs_session>(s), index) ? JNI_TRUE
                                                                : JNI_FALSE;
}

jboolean nativeChangePage(JNIEnv*, jclass, jlong s, jboolean backward) {
  return rs_change_page(static_cast<rs_session>(s), backward == JNI_TRUE)
             ? JNI_TRUE
             : JNI_FALSE;
}

jboolean nativeClearComposition(JNIEnv*, jclass, jlong s) {
  return rs_clear_composition(static_cast<rs_session>(s)) ? JNI_TRUE : JNI_FALSE;
}

jboolean nativeCommitComposition(JNIEnv*, jclass, jlong s) {
  return rs_commit_composition(static_cast<rs_session>(s)) ? JNI_TRUE : JNI_FALSE;
}

jobjectArray nativeSnapshot(JNIEnv* env, jclass, jlong session) {
  rs_session s = static_cast<rs_session>(session);
  const rs_snapshot* snap = rs_snapshot_acquire(s);
  if (snap == nullptr) {
    return nullptr;  // session 失效
  }

  // ↓↓↓ 從這裡到 rs_snapshot_release 之間，只做「複製」，不做任何
  //     可能重入 rime_shell 的事。
  jint ints[kSnapshotIntCount];
  ints[0] = snap->composition.sel_start;
  ints[1] = snap->composition.sel_end;
  ints[2] = snap->composition.caret;
  ints[3] = snap->menu.count;
  ints[4] = snap->menu.page_no;
  ints[5] = snap->menu.highlighted;
  ints[6] = snap->menu.is_last_page ? 1 : 0;
  ints[7] = snap->status.is_composing ? 1 : 0;
  ints[8] = snap->status.is_ascii_mode ? 1 : 0;
  ints[9] = snap->status.is_full_shape ? 1 : 0;
  ints[10] = snap->status.is_simplified ? 1 : 0;
  ints[11] = snap->status.is_ascii_punct ? 1 : 0;
  ints[12] = snap->status.is_disabled ? 1 : 0;
  ints[13] = snap->commit_text != nullptr ? 1 : 0;

  int32_t count = snap->menu.count;
  if (count < 0) count = 0;
  if (snap->menu.items == nullptr) count = 0;

  // 先把 C 字串複製成 std::string，之後才建 JVM 物件；
  // 這樣即使 NewStringUTF 觸發 GC 也不會回頭讀到已失效的指標。
  std::string preedit(snap->composition.preedit ? snap->composition.preedit : "");
  bool has_commit = snap->commit_text != nullptr;
  std::string commit(has_commit ? snap->commit_text : "");
  std::string schema_id(snap->status.schema_id ? snap->status.schema_id : "");
  std::string schema_name(snap->status.schema_name ? snap->status.schema_name : "");

  std::vector<std::string> cand;
  cand.reserve(static_cast<size_t>(count) * 3);
  for (int32_t i = 0; i < count; ++i) {
    const rs_candidate& c = snap->menu.items[i];
    cand.emplace_back(c.text ? c.text : "");
    cand.emplace_back(c.comment ? c.comment : "");
    cand.emplace_back(c.label ? c.label : "");
  }

  rs_snapshot_release(s);
  snap = nullptr;  // 這行以後 snap 已失效，任何解參考都是 use-after-free
  // ↑↑↑ 記憶體約定窗口結束

  jintArray jints = env->NewIntArray(kSnapshotIntCount);
  if (jints == nullptr) return nullptr;
  env->SetIntArrayRegion(jints, 0, kSnapshotIntCount, ints);

  jclass string_class = env->FindClass("java/lang/String");
  jsize str_len = kSnapshotStringHeader + static_cast<jsize>(cand.size());
  jobjectArray jstrs = env->NewObjectArray(str_len, string_class, nullptr);
  if (jstrs == nullptr) return nullptr;

  env->SetObjectArrayElement(jstrs, 0, ToJString(env, preedit.c_str()));
  if (has_commit) {
    env->SetObjectArrayElement(jstrs, 1, ToJString(env, commit.c_str()));
  }  // 否則保持 Java null
  env->SetObjectArrayElement(jstrs, 2, ToJString(env, schema_id.c_str()));
  env->SetObjectArrayElement(jstrs, 3, ToJString(env, schema_name.c_str()));
  for (size_t i = 0; i < cand.size(); ++i) {
    env->SetObjectArrayElement(jstrs, kSnapshotStringHeader + static_cast<jsize>(i),
                               ToJString(env, cand[i].c_str()));
  }

  jclass object_class = env->FindClass("java/lang/Object");
  jobjectArray result = env->NewObjectArray(2, object_class, nullptr);
  if (result == nullptr) return nullptr;
  env->SetObjectArrayElement(result, 0, jints);
  env->SetObjectArrayElement(result, 1, jstrs);
  return result;
}

/* 回傳扁平陣列：[id0, name0, id1, name1, ...] */
jobjectArray nativeSchemaList(JNIEnv* env, jclass) {
  int32_t total = rs_schema_list(nullptr, nullptr, 0);
  if (total < 0) total = 0;

  std::vector<std::string> flat;
  if (total > 0) {
    std::vector<const char*> ids(static_cast<size_t>(total), nullptr);
    std::vector<const char*> names(static_cast<size_t>(total), nullptr);
    int32_t got = rs_schema_list(ids.data(), names.data(), total);
    if (got > total) got = total;
    if (got < 0) got = 0;
    flat.reserve(static_cast<size_t>(got) * 2);
    for (int32_t i = 0; i < got; ++i) {
      flat.emplace_back(ids[i] ? ids[i] : "");
      flat.emplace_back(names[i] ? names[i] : "");
    }
  }

  jclass string_class = env->FindClass("java/lang/String");
  jobjectArray out =
      env->NewObjectArray(static_cast<jsize>(flat.size()), string_class, nullptr);
  for (size_t i = 0; i < flat.size(); ++i) {
    env->SetObjectArrayElement(out, static_cast<jsize>(i),
                               ToJString(env, flat[i].c_str()));
  }
  return out;
}

jboolean nativeSelectSchema(JNIEnv* env, jclass, jlong s, jstring schemaId) {
  std::string id = FromJString(env, schemaId);
  return rs_select_schema(static_cast<rs_session>(s), id.c_str()) ? JNI_TRUE
                                                                  : JNI_FALSE;
}

jboolean nativeSetOption(JNIEnv* env, jclass, jlong s, jstring option,
                         jboolean value) {
  std::string opt = FromJString(env, option);
  return rs_set_option(static_cast<rs_session>(s), opt.c_str(),
                       value == JNI_TRUE)
             ? JNI_TRUE
             : JNI_FALSE;
}

jboolean nativeGetOption(JNIEnv* env, jclass, jlong s, jstring option) {
  std::string opt = FromJString(env, option);
  return rs_get_option(static_cast<rs_session>(s), opt.c_str()) ? JNI_TRUE
                                                                : JNI_FALSE;
}

const JNINativeMethod kMethods[] = {
    {"nativeAbiVersion", "()I", reinterpret_cast<void*>(nativeAbiVersion)},
    {"nativeExpectedAbiVersion", "()I",
     reinterpret_cast<void*>(nativeExpectedAbiVersion)},
    {"nativeIsStub", "()Z", reinterpret_cast<void*>(nativeIsStub)},
    {"nativeInit",
     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z",
     reinterpret_cast<void*>(nativeInit)},
    {"nativeFinalize", "()V", reinterpret_cast<void*>(nativeFinalize)},
    {"nativeDeploy", "()Z", reinterpret_cast<void*>(nativeDeploy)},
    {"nativeLastError", "()Ljava/lang/String;",
     reinterpret_cast<void*>(nativeLastError)},
    {"nativeSessionCreate", "()J", reinterpret_cast<void*>(nativeSessionCreate)},
    {"nativeSessionDestroy", "(J)V",
     reinterpret_cast<void*>(nativeSessionDestroy)},
    {"nativeSessionAlive", "(J)Z", reinterpret_cast<void*>(nativeSessionAlive)},
    {"nativeProcessKey", "(JII)Z", reinterpret_cast<void*>(nativeProcessKey)},
    {"nativeSelectCandidate", "(JI)Z",
     reinterpret_cast<void*>(nativeSelectCandidate)},
    {"nativeDeleteCandidate", "(JI)Z",
     reinterpret_cast<void*>(nativeDeleteCandidate)},
    {"nativeChangePage", "(JZ)Z", reinterpret_cast<void*>(nativeChangePage)},
    {"nativeClearComposition", "(J)Z",
     reinterpret_cast<void*>(nativeClearComposition)},
    {"nativeCommitComposition", "(J)Z",
     reinterpret_cast<void*>(nativeCommitComposition)},
    {"nativeSnapshot", "(J)[Ljava/lang/Object;",
     reinterpret_cast<void*>(nativeSnapshot)},
    {"nativeSchemaList", "()[Ljava/lang/String;",
     reinterpret_cast<void*>(nativeSchemaList)},
    {"nativeSelectSchema", "(JLjava/lang/String;)Z",
     reinterpret_cast<void*>(nativeSelectSchema)},
    {"nativeSetOption", "(JLjava/lang/String;Z)Z",
     reinterpret_cast<void*>(nativeSetOption)},
    {"nativeGetOption", "(JLjava/lang/String;)Z",
     reinterpret_cast<void*>(nativeGetOption)},
};

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
  g_vm = vm;
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    LOGE("JNI_OnLoad: GetEnv 失敗");
    return JNI_ERR;
  }

  jclass local = env->FindClass(RIME_JNI_CLASS);
  if (local == nullptr) {
    LOGE("JNI_OnLoad: 找不到類別 %s", RIME_JNI_CLASS);
    return JNI_ERR;
  }
  g_core_class = static_cast<jclass>(env->NewGlobalRef(local));

  jint rc = env->RegisterNatives(
      g_core_class, kMethods,
      static_cast<jint>(sizeof(kMethods) / sizeof(kMethods[0])));
  if (rc != JNI_OK) {
    LOGE("JNI_OnLoad: RegisterNatives 失敗 rc=%d", rc);
    return JNI_ERR;
  }

  g_on_deploy =
      env->GetStaticMethodID(g_core_class, "onDeployStatusFromNative", "(I)V");
  if (g_on_deploy == nullptr) {
    LOGE("JNI_OnLoad: 找不到 onDeployStatusFromNative(int)");
    env->ExceptionClear();
  }

  env->DeleteLocalRef(local);
  LOGI("JNI_OnLoad ok，class=%s，stub=%d", RIME_JNI_CLASS,
#ifdef RIME_HAVE_LIBRIME
       0
#else
       1
#endif
  );
  return JNI_VERSION_1_6;
}
