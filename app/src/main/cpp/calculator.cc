#include <jni.h>

extern "C" JNIEXPORT jstring JNICALL
Java_io_nava_calculator_MainActivity_getHello(JNIEnv* env, jobject obj) {
  return env->NewStringUTF("Hello from C++");
};
