// saf_bridge.cc — JNI glue to the Java MainActivity for SAF file I/O.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
#include "saf_bridge.hh"

#include <android_native_app_glue.h>
#include <jni.h>

#include <mutex>

namespace {

std::mutex  g_mu;
std::string g_importJson;
bool        g_haveImport = false;

// Attach the (native) calling thread to the JVM and return its JNIEnv.
JNIEnv* attach(android_app* app, bool& didAttach) {
    didAttach = false;
    JavaVM* vm = app->activity->vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK) { didAttach = true; return env; }
    return nullptr;
}

void callActivityVoid(android_app* app, const char* method, const char* sig,
                      jstring a0 = nullptr, jstring a1 = nullptr) {
    bool didAttach = false;
    JNIEnv* env = attach(app, didAttach);
    if (!env) return;
    jobject act = app->activity->clazz;
    jclass  cls = env->GetObjectClass(act);
    jmethodID mid = env->GetMethodID(cls, method, sig);
    if (mid) {
        if (a0 && a1)      env->CallVoidMethod(act, mid, a0, a1);
        else               env->CallVoidMethod(act, mid);
    }
    env->DeleteLocalRef(cls);
    if (didAttach) app->activity->vm->DetachCurrentThread();
}

}  // namespace

namespace saf {

void requestExport(android_app* app, const std::string& json, const std::string& suggestedName) {
    bool didAttach = false;
    JNIEnv* env = attach(app, didAttach);
    if (!env) return;
    jobject act = app->activity->clazz;
    jclass  cls = env->GetObjectClass(act);
    jmethodID mid = env->GetMethodID(cls, "exportWorkspace",
                                     "(Ljava/lang/String;Ljava/lang/String;)V");
    if (mid) {
        jstring jj = env->NewStringUTF(json.c_str());
        jstring jn = env->NewStringUTF(suggestedName.c_str());
        env->CallVoidMethod(act, mid, jj, jn);
        env->DeleteLocalRef(jj);
        env->DeleteLocalRef(jn);
    }
    env->DeleteLocalRef(cls);
    if (didAttach) app->activity->vm->DetachCurrentThread();
}

void requestImport(android_app* app) {
    callActivityVoid(app, "importWorkspace", "()V");
}

bool pollImport(std::string& out) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_haveImport) return false;
    out = std::move(g_importJson);
    g_importJson.clear();
    g_haveImport = false;
    return true;
}

void haptic(android_app* app) {
    callActivityVoid(app, "hapticTick", "()V");
}

}  // namespace saf

// Called from MainActivity.onActivityResult (JVM main thread) with the file text.
extern "C" JNIEXPORT void JNICALL
Java_io_nava_calculator_MainActivity_nativeDeliverImport(JNIEnv* env, jobject, jstring json) {
    const char* c = json ? env->GetStringUTFChars(json, nullptr) : nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_importJson = c ? c : "";
        g_haveImport = true;
    }
    if (c) env->ReleaseStringUTFChars(json, c);
}
