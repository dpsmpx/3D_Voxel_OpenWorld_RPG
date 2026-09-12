#!/usr/bin/env python3
"""Сверяет вызовы NDK с минимальным уровнем Android API.

Хост-проверка компилирует код обычным clang с заглушками, поэтому
атрибуты доступности из настоящих заголовков NDK до неё не доходят.
Из-за этого на устройство дважды уехали ошибки, которых на хосте не
видно:

  * ALooper_pollAll — помечен недоступным начиная с NDK r27;
  * AAudio_* — появился в API 26, а минимум по ТЗ 24: приложение на
    Android 7 не запускалось бы вовсе.

Скрипт разбирает исходники (без комментариев и строк, чтобы не
цепляться за упоминания в тексте), находит вызовы функций NDK и
сверяет их с таблицей ниже. Незнакомая функция — тоже ошибка: её
уровень нужно выяснить и записать, а не узнавать на телефоне.
"""
import os
import re
import sys

PROJ = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(PROJ, 'app', 'src', 'main', 'cpp', 'src')
BUILD_SH = os.path.join(PROJ, 'build.sh')

OBSOLETE = 'obsolete'

# Минимальный уровень API для каждой функции NDK.
API_LEVEL = {
    # Ассеты
    'AAssetManager_open': 9, 'AAssetManager_fromJava': 9,
    'AAssetManager_openDir': 9, 'AAssetDir_getNextFileName': 9,
    'AAssetDir_close': 9,
    'AAsset_close': 9, 'AAsset_getBuffer': 9, 'AAsset_getLength': 9,
    'AAsset_getLength64': 13, 'AAsset_read': 9, 'AAsset_seek': 9,
    'AAsset_getRemainingLength': 9,
    # Ввод
    'AInputEvent_getType': 9, 'AInputEvent_getSource': 9,
    'AInputEvent_getDeviceId': 9,
    'AKeyEvent_getAction': 9, 'AKeyEvent_getKeyCode': 9,
    'AKeyEvent_getMetaState': 9, 'AKeyEvent_getRepeatCount': 9,
    'AMotionEvent_getAction': 9, 'AMotionEvent_getAxisValue': 13,
    'AMotionEvent_getEventTime': 9, 'AMotionEvent_getPointerCount': 9,
    'AMotionEvent_getPointerId': 9, 'AMotionEvent_getX': 9,
    'AMotionEvent_getY': 9, 'AMotionEvent_getPressure': 9,
    'AInputQueue_getEvent': 9, 'AInputQueue_finishEvent': 9,
    'AInputQueue_preDispatchEvent': 9,
    # Цикл событий
    'ALooper_prepare': 9, 'ALooper_pollOnce': 1,
    'ALooper_pollAll': OBSOLETE,
    'ALooper_addFd': 9, 'ALooper_removeFd': 9, 'ALooper_wake': 9,
    # Окно и активность
    'ANativeActivity_finish': 9, 'ANativeActivity_setWindowFlags': 9,
    'ANativeActivity_showSoftInput': 9, 'ANativeActivity_hideSoftInput': 9,
    'ANativeWindow_acquire': 9, 'ANativeWindow_release': 9,
    'ANativeWindow_getWidth': 9, 'ANativeWindow_getHeight': 9,
    'ANativeWindow_getFormat': 9, 'ANativeWindow_setBuffersGeometry': 9,
    'ANativeWindow_lock': 9, 'ANativeWindow_unlockAndPost': 9,
    'ANativeWindow_fromSurface': 9,
    'ANativeWindow_setBuffersTransform': 26,
    'ANativeWindow_setFrameRate': 30,
    # Конфигурация
    'AConfiguration_new': 9, 'AConfiguration_delete': 9,
    'AConfiguration_fromAssetManager': 9, 'AConfiguration_getDensity': 9,
    'AConfiguration_getOrientation': 9,
    # Прочее, чего в проекте пока нет, но что легко утащить по привычке
    'AChoreographer_getInstance': 24,
    'AChoreographer_postFrameCallback': 24,
    'AChoreographer_postFrameCallback64': 29,
    'AHardwareBuffer_allocate': 26, 'AHardwareBuffer_release': 26,
    'ATrace_beginSection': 23, 'ATrace_endSection': 23,
    'APerformanceHint_getManager': 33,
    'ASensorManager_getInstance': 9,
    # AAudio: API 26. В проекте берётся через dlopen/dlsym, поэтому
    # прямых вызовов быть не должно — иначе .so не загрузится на
    # Android 7.0/7.1 и приложение не стартует вовсе.
    'AAudio_createStreamBuilder': 26, 'AAudio_convertResultToText': 26,
    'AAudioStreamBuilder_setDirection': 26,
    'AAudioStreamBuilder_setPerformanceMode': 26,
    'AAudioStreamBuilder_setFormat': 26,
    'AAudioStreamBuilder_setChannelCount': 26,
    'AAudioStreamBuilder_setSampleRate': 26,
    'AAudioStreamBuilder_setDataCallback': 26,
    'AAudioStreamBuilder_setErrorCallback': 26,
    'AAudioStreamBuilder_openStream': 26,
    'AAudioStreamBuilder_delete': 26,
    'AAudioStream_requestStart': 26, 'AAudioStream_requestStop': 26,
    'AAudioStream_close': 26, 'AAudioStream_getSampleRate': 26,
    'AAudioStream_getChannelCount': 26, 'AAudioStream_getFramesPerBurst': 26,
    # Журнал
    '__android_log_print': 3, '__android_log_write': 3,
    '__android_log_vprint': 3, '__android_log_assert': 3,
}

# Семейства функций NDK. Всё, что начинается с этих префиксов и
# вызывается как функция, обязано быть в таблице.
FAMILY = re.compile(
    r'\b((?:AAsset|AAudio|AInput|AKey|ALooper|AMotion|ANative|AConfiguration'
    r'|ASensor|AChoreographer|AHardwareBuffer|ASurface|AMidi|ATrace|AFont'
    r'|APerformanceHint)[A-Za-z0-9_]*|__android_log_[a-z]+)\s*\(')


def strip_noise(text):
    """Убирает строки и комментарии: упоминание в тексте — не вызов."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            i = text.find('\n', i)
            if i < 0:
                break
        elif c == '/' and i + 1 < n and text[i + 1] == '*':
            end = text.find('*/', i + 2)
            i = n if end < 0 else end + 2
        elif c in '"\'':
            quote, i = c, i + 1
            while i < n and text[i] != quote:
                i += 2 if text[i] == '\\' else 1
            i += 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def min_api():
    with open(BUILD_SH, encoding='utf-8') as f:
        m = re.search(r'^API=(\d+)', f.read(), re.M)
    return int(m.group(1)) if m else 0


# Библиотеки, которых не должно быть в target_link_libraries: они
# появились позже минимального API, и запись DT_NEEDED на них делает
# .so незагружаемым — приложение не стартует вовсе, ещё до main.
LATE_LIBS = {'aaudio': 26, 'mediandk': 21, 'nativewindow': 26,
             'sync': 26, 'camera2ndk': 24}


def check_linked_libs(target):
    path = os.path.join(PROJ, 'app', 'src', 'main', 'cpp', 'CMakeLists.txt')
    with open(path, encoding='utf-8') as f:
        text = re.sub(r'#[^\n]*', '', f.read())
    bad = []
    for block in re.findall(r'target_link_libraries\s*\((.*?)\)', text, re.S):
        for word in block.split():
            level = LATE_LIBS.get(word.strip('"'))
            if level is not None and level > target:
                bad.append('лишняя зависимость lib%s.so: она появилась в API %d, '
                           'а минимум проекта %d' % (word, level, target))
    return bad


def main():
    target = min_api()
    if target == 0:
        print('  ✗ не удалось прочитать API= из build.sh')
        return 1

    problems, unknown, checked = check_linked_libs(target), set(), 0
    for root, _dirs, files in os.walk(SRC):
        for name in files:
            if not name.endswith(('.cpp', '.h', '.hpp')):
                continue
            path = os.path.join(root, name)
            with open(path, encoding='utf-8') as f:
                code = strip_noise(f.read())
            rel = os.path.relpath(path, PROJ)
            for line_no, line in enumerate(code.splitlines(), 1):
                for sym in FAMILY.findall(line):
                    checked += 1
                    level = API_LEVEL.get(sym)
                    if level is None:
                        unknown.add((rel, line_no, sym))
                    elif level == OBSOLETE:
                        problems.append(
                            '%s:%d: %s объявлен устаревшим и убран из свежих NDK'
                            % (rel, line_no, sym))
                    elif level > target:
                        problems.append(
                            '%s:%d: %s появился в API %d, а минимум проекта %d'
                            % (rel, line_no, sym, level, target))

    for rel, line_no, sym in sorted(unknown):
        problems.append(
            '%s:%d: %s нет в таблице уровней API '
            '(tools/hostcheck/check_android_api.py)' % (rel, line_no, sym))

    if problems:
        print('  ✗ вызовы NDK не сходятся с минимальным API %d:' % target)
        for p in problems:
            print('      ' + p)
        return 1

    print('  вызовов NDK проверено: %d, минимальный API %d — расхождений нет'
          % (checked, target))
    return 0


if __name__ == '__main__':
    sys.exit(main())
