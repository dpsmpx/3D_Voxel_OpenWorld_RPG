#pragma once
#include <stdint.h>
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
int32_t AMotionEvent_getAction(const AInputEvent* e);
size_t  AMotionEvent_getPointerCount(const AInputEvent* e);
int32_t AMotionEvent_getPointerId(const AInputEvent* e, size_t idx);
float   AMotionEvent_getX(const AInputEvent* e, size_t idx);
float   AMotionEvent_getY(const AInputEvent* e, size_t idx);
int64_t AMotionEvent_getEventTime(const AInputEvent* e);
typedef struct ALooper ALooper;
ALooper* ALooper_prepare(int opts);
int ALooper_pollAll(int timeoutMillis, int* outFd, int* outEvents, void** outData);
int ALooper_addFd(ALooper* looper, int fd, int ident, int events, int (*callback)(int, int, void*), void* data);
enum { ALOOPER_PREPARE_ALLOW_NON_CALLBACKS = 1, ALOOPER_EVENT_INPUT = 1, ALOOPER_POLL_WAKE = -1 };
#ifdef __cplusplus
}
#endif
