/**
 * @file file_picker.cpp
 * @brief Системный выбор файла через активность Android.
 */
#include "file_picker.h"
#include "log.h"

#include <android/native_activity.h>
#include <jni.h>

#include <mutex>
#include <string>

namespace sys {

namespace {

/// Что выбрал игрок.
///
/// Ответ приходит из потока Java, а забирают его из игрового цикла:
/// это два разных потока, и между ними нужен замок. Без него
/// строка читалась бы наполовину записанной.
std::mutex  gPickMutex;
std::string gPickedPath;
bool        gPending = false;

/// Присоединение к виртуальной машине — то же, что у буфера обмена.
class JniScope {
public:
    explicit JniScope(ANativeActivity* activity) {
        if (!activity || !activity->vm) return;
        vm_ = activity->vm;
        const jint got = vm_->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6);
        if (got == JNI_EDETACHED) {
            if (vm_->AttachCurrentThread(&env_, nullptr) != JNI_OK) {
                env_ = nullptr;
                return;
            }
            attached_ = true;
        } else if (got != JNI_OK) {
            env_ = nullptr;
        }
    }
    ~JniScope() { if (attached_ && vm_) vm_->DetachCurrentThread(); }
    JniScope(const JniScope&)            = delete;
    JniScope& operator=(const JniScope&) = delete;
    JNIEnv* env() const { return env_; }

private:
    JavaVM* vm_       = nullptr;
    JNIEnv* env_      = nullptr;
    bool    attached_ = false;
};

/// Проверяет и ГАСИТ исключение Java: непогашенное роняет процесс
/// при следующем же вызове JNI.
bool failed(JNIEnv* env, const char* what) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    LOGW("выбор файла: %s — исключение Java", what);
    return true;
}

} // namespace

bool openWorldPicker(ANativeActivity* activity) {
    if (!activity || !activity->clazz) {
        LOGW("выбор файла: активности нет");
        return false;
    }

    JniScope scope(activity);
    JNIEnv* env = scope.env();
    if (!env) {
        LOGW("выбор файла: не вышло присоединиться к виртуальной машине");
        return false;
    }

    jclass cls = env->GetObjectClass(activity->clazz);
    if (failed(env, "класс активности") || !cls) return false;

    jmethodID m = env->GetMethodID(cls, "openWorldPicker", "()V");
    if (failed(env, "метод openWorldPicker") || !m) {
        // Так бывает при запуске поверх голой NativeActivity: игра
        // работает, а выбора файла в ней нет. Это не падение.
        env->DeleteLocalRef(cls);
        return false;
    }

    env->CallVoidMethod(activity->clazz, m);
    const bool bad = failed(env, "вызов openWorldPicker");
    env->DeleteLocalRef(cls);
    if (bad) return false;

    {
        std::lock_guard<std::mutex> lk(gPickMutex);
        gPending = true;
        gPickedPath.clear();
    }
    return true;
}

std::string takePickedFile() {
    std::lock_guard<std::mutex> lk(gPickMutex);
    std::string out;
    out.swap(gPickedPath);
    return out;
}

bool pickerPending() {
    std::lock_guard<std::mutex> lk(gPickMutex);
    return gPending;
}

} // namespace sys

// ============================================================
// Обратный вызов из Java
// ============================================================
//
// Имя собирается по правилам JNI из пакета и класса: любая правка
// имени класса в Java обязана прийти и сюда, иначе метод не
// свяжется и игра упадёт при первом же выборе файла.
extern "C" JNIEXPORT void JNICALL
Java_com_voxelrpg_game_MainActivity_nativeWorldPicked(JNIEnv* env, jclass,
                                                      jstring path)
{
    std::lock_guard<std::mutex> lk(sys::gPickMutex);
    sys::gPending = false;
    sys::gPickedPath.clear();
    if (!path) {
        LOGI("выбор файла: отменён");
        return;
    }
    const char* chars = env->GetStringUTFChars(path, nullptr);
    if (chars) {
        sys::gPickedPath = chars;
        env->ReleaseStringUTFChars(path, chars);
        LOGI("выбор файла: %s", sys::gPickedPath.c_str());
    }
}
