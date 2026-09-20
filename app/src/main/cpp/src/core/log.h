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
//
// Журнал можно выключить целиком (настройка «Вести журнал»), и тогда
// не пишется НИ ОДНА из двух половин: строка, отданная в logcat, — это
// тоже системный вызов, и «выключено» обязано означать выключено.
//
// Проверка стоит ПЕРЕД форматированием: аргументы LOG* сплошь и рядом
// считаются на месте — позиции, счётчики, имена из реестров, — и при
// выключенном журнале за них не платят ничего.
#define VOXEL_LOG(lvl, prio, ...)                                   \
    do {                                                            \
        if (crash::enabled()) {                                     \
            __android_log_print(prio, LOG_TAG, __VA_ARGS__);        \
            crash::write(lvl, __VA_ARGS__);                         \
        }                                                           \
    } while (0)

#define LOGI(...) VOXEL_LOG('I', ANDROID_LOG_INFO,  __VA_ARGS__)
#define LOGW(...) VOXEL_LOG('W', ANDROID_LOG_WARN,  __VA_ARGS__)
#define LOGE(...) VOXEL_LOG('E', ANDROID_LOG_ERROR, __VA_ARGS__)
#define LOGD(...) VOXEL_LOG('D', ANDROID_LOG_DEBUG, __VA_ARGS__)

// Предсмертная записка идёт МИМО выключателя.
//
// Ассерт — это не «ведение журнала», а объяснение, почему процесса
// больше нет. Выключенный журнал не должен забирать единственную
// строку, которая называет причину: следом идёт __builtin_trap, и
// после него писать уже некому.
#define ASSERT_MSG(cond, msg)                                              \
    do {                                                                   \
        if (!(cond)) {                                                     \
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,                \
                                "Assert failed: %s (%s:%d)",               \
                                msg, __FILE__, __LINE__);                  \
            crash::writeFatal("Assert failed: %s (%s:%d)",                 \
                              msg, __FILE__, __LINE__);                    \
            __builtin_trap();                                              \
        }                                                                  \
    } while (0)
#define ASSERT(cond) ASSERT_MSG(cond, #cond)
