#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef int32_t aaudio_result_t;
typedef int32_t aaudio_format_t;
typedef int32_t aaudio_direction_t;
typedef int32_t aaudio_performance_mode_t;
typedef int32_t aaudio_data_callback_result_t;
enum { AAUDIO_OK = 0, AAUDIO_ERROR_BASE = -900 };
enum { AAUDIO_FORMAT_PCM_I16 = 1, AAUDIO_FORMAT_PCM_FLOAT = 2 };
enum { AAUDIO_DIRECTION_OUTPUT = 0, AAUDIO_DIRECTION_INPUT = 1 };
enum { AAUDIO_PERFORMANCE_MODE_NONE = 10, AAUDIO_PERFORMANCE_MODE_POWER_SAVING = 11, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY = 12 };
enum { AAUDIO_CALLBACK_RESULT_CONTINUE = 0, AAUDIO_CALLBACK_RESULT_STOP = 1 };
typedef struct AAudioStreamStruct AAudioStream;
typedef struct AAudioStreamBuilderStruct AAudioStreamBuilder;
typedef aaudio_data_callback_result_t (*AAudioStream_dataCallback)(AAudioStream*, void*, void*, int32_t);
typedef void (*AAudioStream_errorCallback)(AAudioStream*, void*, aaudio_result_t);
aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder** b);
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder* b, int32_t v);
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder* b, int32_t v);
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder* b, aaudio_format_t v);
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder* b, aaudio_direction_t v);
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder* b, aaudio_performance_mode_t v);
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder* b, AAudioStream_dataCallback cb, void* ud);
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder* b, AAudioStream_errorCallback cb, void* ud);
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder* b, AAudioStream** s);
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder* b);
aaudio_result_t AAudioStream_requestStart(AAudioStream* s);
aaudio_result_t AAudioStream_requestStop(AAudioStream* s);
aaudio_result_t AAudioStream_close(AAudioStream* s);
int32_t AAudioStream_getSampleRate(AAudioStream* s);
int32_t AAudioStream_getChannelCount(AAudioStream* s);
int32_t AAudioStream_getFramesPerBurst(AAudioStream* s);
const char* AAudio_convertResultToText(aaudio_result_t r);
#ifdef __cplusplus
}
#endif
