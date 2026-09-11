#pragma once
#include <stdint.h>
typedef int32_t jint; typedef int64_t jlong; typedef unsigned char jboolean;
typedef void* jobject; typedef void* jclass; typedef void* jstring;
struct _JNIEnv; struct _JavaVM;
typedef struct _JNIEnv JNIEnv; typedef struct _JavaVM JavaVM;
#define JNIEXPORT
#define JNICALL
