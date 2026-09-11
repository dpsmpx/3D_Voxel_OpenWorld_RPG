#pragma once
#include <android/log.h>
#include <cstdio>

#define LOG_TAG "VoxelRPG"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

#define ASSERT_MSG(cond, msg) do { if (!(cond)) { LOGE("Assert failed: %s (%s:%d)", msg, __FILE__, __LINE__); __builtin_trap(); } } while(0)
#define ASSERT(cond) ASSERT_MSG(cond, #cond)
