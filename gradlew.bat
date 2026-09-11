@rem Gradle wrapper для Windows.
@rem gradle-wrapper.jar в репозиторий не коммитится — если его нет,
@rem установите Gradle вручную и запускайте "gradle" напрямую.
@if "%DEBUG%"=="" @echo off
setlocal
set DIR=%~dp0
if exist "%DIR%gradle\wrapper\gradle-wrapper.jar" (
    java -classpath "%DIR%gradle\wrapper\gradle-wrapper.jar" org.gradle.wrapper.GradleWrapperMain %*
) else (
    echo gradle-wrapper.jar отсутствует. Установите Gradle 8.4 и запустите: gradle %*
    exit /b 1
)
