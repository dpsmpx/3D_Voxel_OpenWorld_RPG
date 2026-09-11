#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ANativeWindow ANativeWindow;
int32_t ANativeWindow_getWidth(ANativeWindow* w);
int32_t ANativeWindow_getHeight(ANativeWindow* w);
int32_t ANativeWindow_getFormat(ANativeWindow* w);
void ANativeWindow_acquire(ANativeWindow* w);
void ANativeWindow_release(ANativeWindow* w);
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow* w, int32_t width, int32_t height, int32_t format);
#ifdef __cplusplus
}
#endif
