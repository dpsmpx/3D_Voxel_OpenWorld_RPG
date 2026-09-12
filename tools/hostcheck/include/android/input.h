#pragma once
#include <stdint.h>
#include <android/keycodes.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct AInputEvent AInputEvent;
enum { AINPUT_EVENT_TYPE_KEY = 1, AINPUT_EVENT_TYPE_MOTION = 2 };
enum {
    AMOTION_EVENT_ACTION_MASK = 0xff,
    AMOTION_EVENT_ACTION_POINTER_INDEX_MASK = 0xff00,
    AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT = 8,
    AMOTION_EVENT_ACTION_DOWN = 0,
    AMOTION_EVENT_ACTION_UP = 1,
    AMOTION_EVENT_ACTION_MOVE = 2,
    AMOTION_EVENT_ACTION_CANCEL = 3,
    AMOTION_EVENT_ACTION_POINTER_DOWN = 5,
    AMOTION_EVENT_ACTION_POINTER_UP = 6
};
int32_t AInputEvent_getType(const AInputEvent* e);
int32_t AInputEvent_getSource(const AInputEvent* e);

/* Классы и источники ввода */
enum {
    AINPUT_SOURCE_CLASS_MASK     = 0x000000ff,
    AINPUT_SOURCE_CLASS_BUTTON   = 0x00000001,
    AINPUT_SOURCE_CLASS_POINTER  = 0x00000002,
    AINPUT_SOURCE_CLASS_NAVIGATION = 0x00000004,
    AINPUT_SOURCE_CLASS_POSITION = 0x00000008,
    AINPUT_SOURCE_CLASS_JOYSTICK = 0x00000010,
};
enum {
    AINPUT_SOURCE_KEYBOARD  = 0x00000101,
    AINPUT_SOURCE_DPAD      = 0x00000201,
    AINPUT_SOURCE_GAMEPAD   = 0x00000401,
    AINPUT_SOURCE_TOUCHSCREEN = 0x00001002,
    AINPUT_SOURCE_JOYSTICK  = 0x01000010,
};

/* Оси motion-события */
enum {
    AMOTION_EVENT_AXIS_X = 0,
    AMOTION_EVENT_AXIS_Y = 1,
    AMOTION_EVENT_AXIS_Z = 11,
    AMOTION_EVENT_AXIS_RZ = 14,
    AMOTION_EVENT_AXIS_HAT_X = 15,
    AMOTION_EVENT_AXIS_HAT_Y = 16,
    AMOTION_EVENT_AXIS_LTRIGGER = 17,
    AMOTION_EVENT_AXIS_RTRIGGER = 18,
};
float AMotionEvent_getAxisValue(const AInputEvent* e, int32_t axis, size_t idx);

int32_t AKeyEvent_getAction(const AInputEvent* e);
int32_t AKeyEvent_getKeyCode(const AInputEvent* e);
int32_t AMotionEvent_getAction(const AInputEvent* e);
size_t  AMotionEvent_getPointerCount(const AInputEvent* e);
int32_t AMotionEvent_getPointerId(const AInputEvent* e, size_t idx);
float   AMotionEvent_getX(const AInputEvent* e, size_t idx);
float   AMotionEvent_getY(const AInputEvent* e, size_t idx);
int64_t AMotionEvent_getEventTime(const AInputEvent* e);
typedef struct ALooper ALooper;
ALooper* ALooper_prepare(int opts);
int ALooper_pollAll(int timeoutMillis, int* outFd, int* outEvents, void** outData);
int ALooper_pollOnce(int timeoutMillis, int* outFd, int* outEvents, void** outData);
int ALooper_addFd(ALooper* looper, int fd, int ident, int events, int (*callback)(int, int, void*), void* data);
enum { ALOOPER_PREPARE_ALLOW_NON_CALLBACKS = 1, ALOOPER_EVENT_INPUT = 1 };
enum { ALOOPER_POLL_WAKE = -1, ALOOPER_POLL_CALLBACK = -2,
       ALOOPER_POLL_TIMEOUT = -3, ALOOPER_POLL_ERROR = -4 };
#ifdef __cplusplus
}
#endif
