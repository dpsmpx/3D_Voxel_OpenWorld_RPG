/**
 * @file crashlog.cpp
 * @brief Журнал и отчёт о падении в файл, доступный без adb.
 */
#include "crashlog.h"

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unwind.h>

namespace crash {
namespace {

constexpr int  MAX_FRAMES  = 32;
constexpr long MAX_LOG_SIZE = 512 * 1024;   ///< дальше файл начинается заново

int  gFd       = -1;          ///< внутренний каталог, есть всегда
int  gSharedFd = -1;          ///< /sdcard/Android/media/<пакет>
char gPath[512]       = {0};
char gSharedPath[512] = {0};
char gLastStep[128]   = "(ни одного этапа)";

/// Пишет как есть в оба файла. Только write(): FILE* буферизует, и при
/// падении последние — самые нужные — строки не доходят до диска.
void raw(const char* data, unsigned len) {
    if (gFd >= 0)       { ssize_t r = ::write(gFd, data, len); (void)r; }
    if (gSharedFd >= 0) { ssize_t r = ::write(gSharedFd, data, len); (void)r; }
}

void rawStr(const char* s) { raw(s, (unsigned)std::strlen(s)); }

/// Создаёт каталог вместе с родителями. mkdir -p, но без вызова оболочки.
bool makeDirs(const char* path) {
    char tmp[512];
    std::snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        ::mkdir(tmp, 0775);
        *p = '/';
    }
    return ::mkdir(tmp, 0775) == 0 || errno == EEXIST;
}

int openLog(const char* path) {
    // Файл не должен расти без предела: журнал ведётся каждую сессию.
    struct stat st;
    int flags = O_WRONLY | O_CREAT | O_APPEND;
    if (::stat(path, &st) == 0 && st.st_size > MAX_LOG_SIZE) flags |= O_TRUNC;
    return ::open(path, flags, 0664);
}

/// Обход стека. _Unwind_Backtrace есть и в NDK, и на хосте, и работает
/// без дополнительных библиотек — лишь бы были -funwind-tables.
struct BacktraceState { void** current; void** end; };

_Unwind_Reason_Code unwindCallback(_Unwind_Context* ctx, void* arg) {
    auto* state = static_cast<BacktraceState*>(arg);
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc && state->current != state->end) {
        *state->current++ = reinterpret_cast<void*>(pc);
    }
    return _URC_NO_REASON;
}

void writeBacktrace() {
    void* frames[MAX_FRAMES];
    BacktraceState state{frames, frames + MAX_FRAMES};
    _Unwind_Backtrace(unwindCallback, &state);
    const int count = (int)(state.current - frames);

    char line[512];
    for (int i = 0; i < count; ++i) {
        Dl_info info;
        const char* symbol = "";
        const char* object = "";
        uintptr_t offset = 0;
        if (dladdr(frames[i], &info) && info.dli_fname) {
            object = info.dli_fname;
            if (info.dli_sname) symbol = info.dli_sname;
            offset = (uintptr_t)frames[i] - (uintptr_t)info.dli_fbase;
        }
        std::snprintf(line, sizeof(line), "  #%02d  pc %012lx  %s  %s\n",
                      i, (unsigned long)offset, object, symbol);
        rawStr(line);
    }
}

const char* signalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (обращение по недопустимому адресу)";
        case SIGBUS:  return "SIGBUS (невыровненный или недоступный адрес)";
        case SIGFPE:  return "SIGFPE (ошибка арифметики)";
        case SIGILL:  return "SIGILL (недопустимая инструкция)";
        case SIGABRT: return "SIGABRT (abort)";
        case SIGTRAP: return "SIGTRAP (__builtin_trap — не прошёл ASSERT)";
        default:      return "неизвестный сигнал";
    }
}

struct sigaction gOld[NSIG];

void handler(int sig, siginfo_t* info, void* ctx) {
    char line[512];
    rawStr("\n================ ПАДЕНИЕ ================\n");
    std::snprintf(line, sizeof(line), "сигнал: %d — %s\n", sig, signalName(sig));
    rawStr(line);
    std::snprintf(line, sizeof(line), "адрес: %p, код: %d\n",
                  info ? info->si_addr : nullptr, info ? info->si_code : 0);
    rawStr(line);
    std::snprintf(line, sizeof(line), "последний этап запуска: %s\n", gLastStep);
    rawStr(line);
    rawStr("стек вызовов:\n");
    writeBacktrace();
    rawStr("=========================================\n");
    if (gFd >= 0)       ::fsync(gFd);
    if (gSharedFd >= 0) ::fsync(gSharedFd);

    // Отдаём сигнал системе: пусть Android запишет свой tombstone и
    // покажет привычный диалог, а не молча завершит процесс.
    if (sig > 0 && sig < NSIG && gOld[sig].sa_sigaction) {
        ::sigaction(sig, &gOld[sig], nullptr);
    } else {
        struct sigaction dfl{};
        dfl.sa_handler = SIG_DFL;
        ::sigaction(sig, &dfl, nullptr);
    }
    ::raise(sig);
    (void)ctx;
}

void installHandlers() {
    // Свой стек: при переполнении основного обработчику негде работать.
    // Размер фиксированный: в новых версиях glibc SIGSTKSZ уже не
    // константа времени компиляции, а вызов sysconf.
    static char altStack[64 * 1024];
    stack_t ss{};
    ss.ss_sp = altStack;
    ss.ss_size = sizeof(altStack);
    ::sigaltstack(&ss, nullptr);

    struct sigaction sa{};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    ::sigemptyset(&sa.sa_mask);

    const int signals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGTRAP};
    for (int sig : signals) ::sigaction(sig, &sa, &gOld[sig]);
}

} // namespace

void init(const char* internalDataPath, const char* externalDataPath) {
    if (internalDataPath && *internalDataPath) {
        std::snprintf(gPath, sizeof(gPath), "%s/voxelrpg.log", internalDataPath);
        gFd = openLog(gPath);
        if (gFd < 0) gPath[0] = '\0';
    }

    // externalDataPath выглядит как /storage/emulated/0/Android/data/<пакет>/files.
    // Каталог Android/data с Android 11 закрыт для других приложений, а
    // Android/media — нет, поэтому журнал кладём туда: из Termux он
    // читается после termux-setup-storage.
    if (externalDataPath && *externalDataPath) {
        const char* marker = "/Android/data/";
        const char* found = std::strstr(externalDataPath, marker);
        if (found) {
            const size_t prefix = (size_t)(found - externalDataPath);
            char pkg[256] = {0};
            const char* rest = found + std::strlen(marker);
            size_t n = 0;
            while (rest[n] && rest[n] != '/' && n + 1 < sizeof(pkg)) { pkg[n] = rest[n]; ++n; }
            pkg[n] = '\0';

            char dir[512];
            std::snprintf(dir, sizeof(dir), "%.*s/Android/media/%s",
                          (int)prefix, externalDataPath, pkg);
            if (makeDirs(dir)) {
                std::snprintf(gSharedPath, sizeof(gSharedPath), "%s/voxelrpg.log", dir);
                gSharedFd = openLog(gSharedPath);
            }
            if (gSharedFd < 0) gSharedPath[0] = '\0';
        }
    }

    installHandlers();

    char head[640];
    const std::time_t t = std::time(nullptr);
    std::snprintf(head, sizeof(head),
                  "\n======== VoxelRPG: запуск, unix-время %ld ========\n",
                  (long)t);
    rawStr(head);
}

void write(char level, const char* fmt, ...) {
    if (gFd < 0 && gSharedFd < 0) return;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(msg, sizeof(msg) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;

    char line[1100];
    const int len = std::snprintf(line, sizeof(line), "%c: %s\n", level, msg);
    if (len > 0) raw(line, (unsigned)len);
}

void step(const char* name) {
    if (!name) return;
    std::snprintf(gLastStep, sizeof(gLastStep), "%s", name);
    write('S', "--- этап: %s", name);
}

const char* logPath()       { return gPath; }
const char* sharedLogPath() { return gSharedPath; }

} // namespace crash
