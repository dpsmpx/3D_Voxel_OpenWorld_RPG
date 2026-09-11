#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct AAssetManager AAssetManager;
typedef struct AAsset AAsset;
enum { AASSET_MODE_UNKNOWN=0, AASSET_MODE_RANDOM=1, AASSET_MODE_STREAMING=2, AASSET_MODE_BUFFER=3 };
AAsset* AAssetManager_open(AAssetManager* mgr, const char* filename, int mode);
int AAsset_read(AAsset* asset, void* buf, size_t count);
off_t AAsset_getLength(AAsset* asset);
long AAsset_getLength64(AAsset* asset);
const void* AAsset_getBuffer(AAsset* asset);
void AAsset_close(AAsset* asset);
#ifdef __cplusplus
}
#endif
