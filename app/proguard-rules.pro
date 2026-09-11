# VoxelRPG ProGuard rules
#
# Игра полностью нативная: Java-код отсутствует, RTTI/exceptions
# выключены в C++. ProGuard здесь — минимальный.

# Не трогать NativeActivity
-keep class android.app.NativeActivity { *; }
-keep class android.app.NativeActivity* { *; }

# Не удалять классы, к которым может обратиться JNI
-keepclasseswithmembernames class * {
    native <methods>;
}

# Заглушки для отладки (не удаляем атрибуты)
-keepattributes SourceFile,LineNumberTable
-renamesourcefileattribute SourceFile