#pragma once
/* Заглушка JNI для host-сборки.
 *
 * На хосте настоящих заголовков NDK нет, а компилировать нативный код
 * надо весь — иначе файл, который собирается только на устройстве,
 * ломается незамеченным до самой сборки APK.
 *
 * Поэтому здесь повторён C++-интерфейс JNI в том объёме, который
 * использует игра: методы-члены JNIEnv и JavaVM, типы дескрипторов и
 * коды возврата. Тела ничего не делают и ничего не значат — код
 * отсюда не запускается, только компилируется. */
#include <stdint.h>
#include <stddef.h>

typedef int32_t jint;
typedef int64_t jlong;
typedef unsigned char jboolean;
typedef void* jobject;
typedef jobject jclass;
typedef jobject jstring;
typedef jobject jthrowable;
typedef void* jmethodID;
typedef void* jfieldID;
typedef void* jvalue;

#define JNIEXPORT
#define JNICALL

#define JNI_OK         0
#define JNI_ERR        (-1)
#define JNI_EDETACHED  (-2)
#define JNI_EVERSION   (-3)
#define JNI_VERSION_1_6 0x00010006

struct JNIInvokeInterface;

/* Методы вызываются как env->Method(...), поэтому нужны именно
 * члены, а не свободные функции: расхождение поймается компилятором. */
struct _JNIEnv {
    jint     GetEnv(void**, jint)                         { return JNI_OK; }
    jclass   GetObjectClass(jobject)                      { return nullptr; }
    jclass   FindClass(const char*)                       { return nullptr; }
    jmethodID GetMethodID(jclass, const char*, const char*)       { return nullptr; }
    jmethodID GetStaticMethodID(jclass, const char*, const char*) { return nullptr; }
    jstring  NewStringUTF(const char*)                    { return nullptr; }
    void     ReleaseStringUTFChars(jstring, const char*)  {}
    jobject  CallObjectMethod(jobject, jmethodID, ...)             { return nullptr; }
    jobject  CallStaticObjectMethod(jclass, jmethodID, ...)        { return nullptr; }
    void     CallVoidMethod(jobject, jmethodID, ...)               {}
    jboolean ExceptionCheck()                             { return 0; }
    void     ExceptionClear()                             {}
    void     ExceptionDescribe()                          {}
    jint     PushLocalFrame(jint)                         { return JNI_OK; }
    jobject  PopLocalFrame(jobject)                       { return nullptr; }
    void     DeleteLocalRef(jobject)                      {}
};

struct _JavaVM {
    jint GetEnv(void**, jint)                    { return JNI_OK; }
    jint AttachCurrentThread(_JNIEnv**, void*)   { return JNI_OK; }
    jint DetachCurrentThread()                   { return JNI_OK; }
};

typedef struct _JNIEnv JNIEnv;
typedef struct _JavaVM JavaVM;
