/**
 * @file log.h
 * @brief Ядро движка: базовые типы, математика, планировщик задач, аллокаторы.
 */
#pragma once
#include <android/log.h>
#include <cstdio>

#include "crashlog.h"

#define LOG_TAG "VoxelRPG"

// Каждая строка уходит и в logcat, и в файл. На телефоне без отладчика
// logcat чужого приложения недоступен, а файл читается из Termux —
// иначе причину падения не узнать вообще.
#define LOGI(...) do { __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__); \
                       crash::write('I', __VA_ARGS__); } while (0)
#define LOGW(...) do { __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__); \
                       crash::write('W', __VA_ARGS__); } while (0)
#define LOGE(...) do { __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__); \
                       crash::write('E', __VA_ARGS__); } while (0)
#define LOGD(...) do { __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__); \
                       crash::write('D', __VA_ARGS__); } while (0)

#define ASSERT_MSG(cond, msg) do { if (!(cond)) { LOGE("Assert failed: %s (%s:%d)", msg, __FILE__, __LINE__); __builtin_trap(); } } while(0)
#define ASSERT(cond) ASSERT_MSG(cond, #cond)
