/**
 * @file clipboard.cpp
 * @brief Буфер обмена Android из нативного кода.
 */
#include "clipboard.h"
#include "log.h"

#include <android/native_activity.h>
#include <jni.h>

#include <cstdio>
#include <string>
#include <vector>

namespace sys {

namespace {

/// Держатель присоединения к виртуальной машине.
///
/// android_main крутится в СВОЁМ потоке, а не в том, где живёт Java.
/// Звать JNI оттуда можно только присоединившись; отсоединяться
/// обязаны мы же и ровно в том случае, если присоединялись сами —
/// иначе отвалится поток, который нам не принадлежит.
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
    ~JniScope() {
        if (attached_ && vm_) vm_->DetachCurrentThread();
    }
    JniScope(const JniScope&)            = delete;
    JniScope& operator=(const JniScope&) = delete;

    JNIEnv* env() const { return env_; }

private:
    JavaVM* vm_       = nullptr;
    JNIEnv* env_      = nullptr;
    bool    attached_ = false;
};

/// Проверяет и ГАСИТ исключение Java.
///
/// Оставить его висеть нельзя: следующий же вызов JNI с непогашенным
/// исключением — это аварийное завершение процесса. А случиться оно
/// здесь может буднично, от TransactionTooLargeException до отказа
/// прошивки писать в буфер из фона.
bool failed(JNIEnv* env, const char* what) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    LOGW("буфер обмена: %s — исключение Java", what);
    return true;
}

} // namespace

bool copyToClipboard(ANativeActivity* activity, const char* text) {
    if (!activity || !text) return false;

    JniScope scope(activity);
    JNIEnv* env = scope.env();
    if (!env) { LOGW("буфер обмена: нет доступа к виртуальной машине"); return false; }

    // Локальных ссылок по умолчанию отводится немного; здесь их
    // десяток, и просить кадр — дешевле, чем считать их руками.
    if (env->PushLocalFrame(16) != JNI_OK) {
        LOGW("буфер обмена: не отвести кадр локальных ссылок");
        return false;
    }
    struct FramePop {
        JNIEnv* e;
        ~FramePop() { e->PopLocalFrame(nullptr); }
    } pop{ env };

    // activity.getSystemService(Context.CLIPBOARD_SERVICE)
    jclass actCls = env->GetObjectClass(activity->clazz);
    if (!actCls || failed(env, "класс активности")) return false;
    jmethodID getService = env->GetMethodID(
        actCls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    if (!getService || failed(env, "getSystemService")) return false;

    jstring svcName = env->NewStringUTF("clipboard");
    if (!svcName || failed(env, "имя службы")) return false;
    jobject clipboard = env->CallObjectMethod(activity->clazz, getService, svcName);
    if (failed(env, "получение службы буфера") || !clipboard) {
        LOGW("буфер обмена: служба недоступна");
        return false;
    }

    // ClipData.newPlainText(label, text)
    jclass clipDataCls = env->FindClass("android/content/ClipData");
    if (!clipDataCls || failed(env, "класс ClipData")) return false;
    jmethodID newPlainText = env->GetStaticMethodID(
        clipDataCls, "newPlainText",
        "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;");
    if (!newPlainText || failed(env, "newPlainText")) return false;

    jstring label = env->NewStringUTF("VoxelRPG log");
    if (!label || failed(env, "подпись")) return false;
    // NewStringUTF ждёт модифицированный UTF-8. Наш журнал — ASCII и
    // кириллица, то есть один и два байта; расхождение начинается с
    // четырёхбайтных символов, которых мы не пишем.
    jstring payload = env->NewStringUTF(text);
    if (!payload || failed(env, "текст")) {
        LOGW("буфер обмена: текст не преобразовался в строку Java");
        return false;
    }

    jobject clipData = env->CallStaticObjectMethod(clipDataCls, newPlainText,
                                                   label, payload);
    if (failed(env, "создание ClipData") || !clipData) return false;

    // clipboard.setPrimaryClip(clipData)
    jclass cmCls = env->GetObjectClass(clipboard);
    if (!cmCls || failed(env, "класс ClipboardManager")) return false;
    jmethodID setPrimaryClip = env->GetMethodID(
        cmCls, "setPrimaryClip", "(Landroid/content/ClipData;)V");
    if (!setPrimaryClip || failed(env, "setPrimaryClip")) return false;

    env->CallVoidMethod(clipboard, setPrimaryClip, clipData);
    if (failed(env, "запись в буфер")) return false;

    return true;
}

std::string clipboardTextFromFile(const char* path) {
    if (!path || !path[0]) return {};

    std::FILE* f = std::fopen(path, "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    if (size <= 0) { std::fclose(f); return {}; }
    const usize total = (usize)size;

    std::string text;
    if (total <= CLIPBOARD_MAX_BYTES) {
        text.resize(total);
        std::fseek(f, 0, SEEK_SET);
        text.resize(std::fread(text.data(), 1, total, f));
    } else {
        // Начало плюс хвост: в начале условия запуска, в хвосте то, на
        // чём всё кончилось. Середина длинной сессии — это повторы
        // одной и той же сводки, и она не стоит транзакции.
        const usize tail = CLIPBOARD_MAX_BYTES - CLIPBOARD_HEAD_BYTES;
        std::string head(CLIPBOARD_HEAD_BYTES, '\0');
        std::fseek(f, 0, SEEK_SET);
        head.resize(std::fread(head.data(), 1, CLIPBOARD_HEAD_BYTES, f));

        std::string tailBuf(tail, '\0');
        std::fseek(f, (long)(total - tail), SEEK_SET);
        tailBuf.resize(std::fread(tailBuf.data(), 1, tail, f));

        char note[192];
        std::snprintf(note, sizeof(note),
                      "\n\n... пропущено %zu байт середины журнала "
                      "(всего %zu, в буфер вошло %zu) ...\n\n",
                      total - head.size() - tailBuf.size(), total,
                      head.size() + tailBuf.size());
        text = head + note + tailBuf;
    }
    std::fclose(f);
    return text;
}

bool copyFileToClipboard(ANativeActivity* activity, const char* path) {
    if (!activity) return false;

    const std::string text = clipboardTextFromFile(path);
    if (text.empty()) {
        LOGW("буфер обмена: нечего копировать из %s", path ? path : "(нет пути)");
        return false;
    }
    if (!copyToClipboard(activity, text.c_str())) return false;

    LOGI("Журнал скопирован в буфер обмена: %zu байт (%s)", text.size(), path);
    return true;
}

} // namespace sys
