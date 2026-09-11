#pragma once
#include <stdint.h>
#include <jni.h>
#include <android/asset_manager.h>
#include <android/native_window.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ANativeActivityCallbacks ANativeActivityCallbacks;
typedef struct ANativeActivity {
    ANativeActivityCallbacks* callbacks;
    JavaVM* vm;
    JNIEnv* env;
    jobject clazz;
    const char* internalDataPath;
    const char* externalDataPath;
    int32_t sdkVersion;
    void* instance;
    AAssetManager* assetManager;
    const char* obbPath;
} ANativeActivity;
struct ANativeActivityCallbacks {
    void (*onStart)(ANativeActivity*);
    void (*onResume)(ANativeActivity*);
    void* (*onSaveInstanceState)(ANativeActivity*, size_t*);
    void (*onPause)(ANativeActivity*);
    void (*onStop)(ANativeActivity*);
    void (*onDestroy)(ANativeActivity*);
    void (*onWindowFocusChanged)(ANativeActivity*, int);
    void (*onNativeWindowCreated)(ANativeActivity*, ANativeWindow*);
    void (*onNativeWindowResized)(ANativeActivity*, ANativeWindow*);
    void (*onNativeWindowRedrawNeeded)(ANativeActivity*, ANativeWindow*);
    void (*onNativeWindowDestroyed)(ANativeActivity*, ANativeWindow*);
    void (*onInputQueueCreated)(ANativeActivity*, struct AInputQueue*);
    void (*onInputQueueDestroyed)(ANativeActivity*, struct AInputQueue*);
    void (*onContentRectChanged)(ANativeActivity*, const struct ARect*);
    void (*onConfigurationChanged)(ANativeActivity*);
    void (*onLowMemory)(ANativeActivity*);
};
void ANativeActivity_finish(ANativeActivity* activity);
void ANativeActivity_setWindowFlags(ANativeActivity*, uint32_t, uint32_t);
#ifdef __cplusplus
}
#endif
