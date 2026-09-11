#pragma once
/* Коды клавиш Android. В настоящем NDK этот заголовок подключается
   из <android/input.h>; здесь повторена та же структура. */
#ifdef __cplusplus
extern "C" {
#endif
/* Клавиатура и геймпад */
enum { AKEY_EVENT_ACTION_DOWN = 0, AKEY_EVENT_ACTION_UP = 1, AKEY_EVENT_ACTION_MULTIPLE = 2 };
enum {
    AKEYCODE_BACK = 4,
    AKEYCODE_DPAD_UP = 19, AKEYCODE_DPAD_DOWN = 20,
    AKEYCODE_DPAD_LEFT = 21, AKEYCODE_DPAD_RIGHT = 22,
    AKEYCODE_BUTTON_A = 96, AKEYCODE_BUTTON_B = 97, AKEYCODE_BUTTON_C = 98,
    AKEYCODE_BUTTON_X = 99, AKEYCODE_BUTTON_Y = 100, AKEYCODE_BUTTON_Z = 101,
    AKEYCODE_BUTTON_L1 = 102, AKEYCODE_BUTTON_R1 = 103,
    AKEYCODE_BUTTON_L2 = 104, AKEYCODE_BUTTON_R2 = 105,
    AKEYCODE_BUTTON_THUMBL = 106, AKEYCODE_BUTTON_THUMBR = 107,
    AKEYCODE_BUTTON_START = 108, AKEYCODE_BUTTON_SELECT = 109,
};
#ifdef __cplusplus
}
#endif
