/**
 * @file crashlog.cpp
 * @brief Журнал и отчёт о падении в файл, доступный без adb.
 */
#include "crashlog.h"
#include "shared_dir.h"

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

/// Ведётся ли журнал. По умолчанию да: до чтения settings.cfg
/// успевает произойти половина запуска, и молчать в это время
/// нельзя — именно там и ломается то, что ломается.
bool gEnabled = true;
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

namespace {

/// Пробует открыть журнал в каталоге; при успехе запоминает путь.
bool tryShared(const char* dir) {
    if (!dir || !*dir) return false;
    if (!makeDirs(dir)) return false;
    char path[512];
    std::snprintf(path, sizeof(path), "%s/voxelrpg.log", dir);
    const int fd = openLog(path);
    if (fd < 0) return false;
    gSharedFd = fd;
    std::snprintf(gSharedPath, sizeof(gSharedPath), "%s", path);
    return true;
}

} // namespace

void init(const char* internalDataPath, const char* externalDataPath) {
    if (internalDataPath && *internalDataPath) {
        std::snprintf(gPath, sizeof(gPath), "%s/voxelrpg.log", internalDataPath);
        gFd = openLog(gPath);
        if (gFd < 0) gPath[0] = '\0';
    }

    // Журнал во внешнем хранилище — единственный способ прочитать его с
    // телефона: logcat чужого приложения недоступен. Куда именно —
    // решает sys::sharedDirCandidates: тот же список, по которому
    // уходят наружу выгруженные миры, и одно правило на обоих.
    char pkg[256];
    sys::packageFromPath(externalDataPath, pkg, sizeof(pkg));
    if (!pkg[0]) sys::packageFromPath(internalDataPath, pkg, sizeof(pkg));

    char dirs[8][sys::SHARED_PATH_CAP];
    const u32 count = sys::sharedDirCandidates(internalDataPath,
                                               externalDataPath, dirs, 8);
    for (u32 i = 0; i < count && gSharedFd < 0; ++i) tryShared(dirs[i]);
    if (gSharedFd < 0) gSharedPath[0] = '\0';

    installHandlers();

    char head[640];
    const std::time_t t = std::time(nullptr);
    std::snprintf(head, sizeof(head),
                  "\n======== VoxelRPG: запуск, unix-время %ld ========\n",
                  (long)t);
    rawStr(head);

    // Пути — в сам журнал: если до нас дойдёт только внутренняя копия,
    // будет видно, куда ещё пытались писать.
    write('I', "внутренний журнал: %s", gPath[0] ? gPath : "(нет)");
    write('I', "общий журнал: %s", gSharedPath[0] ? gSharedPath : "(нет)");
    write('I', "internalDataPath=%s externalDataPath=%s пакет=%s",
          internalDataPath ? internalDataPath : "(null)",
          externalDataPath ? externalDataPath : "(null)",
          pkg[0] ? pkg : "(не определён)");
}

namespace {

/// Общее тело write/writeFatal: разница между ними ровно в одной
/// проверке выше по стеку, и дублировать ради неё разбор va_list
/// незачем.
void writeLine(char level, const char* fmt, va_list ap) {
    if (gFd < 0 && gSharedFd < 0) return;
    char msg[1024];
    const int n = std::vsnprintf(msg, sizeof(msg) - 2, fmt, ap);
    if (n < 0) return;

    char line[1100];
    const int len = std::snprintf(line, sizeof(line), "%c: %s\n", level, msg);
    if (len > 0) raw(line, (unsigned)len);
}

} // namespace

void write(char level, const char* fmt, ...) {
    if (!gEnabled) return;
    va_list ap;
    va_start(ap, fmt);
    writeLine(level, fmt, ap);
    va_end(ap);
}

void writeFatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    writeLine('F', fmt, ap);
    va_end(ap);
}

void setEnabled(bool on) {
    if (on == gEnabled) return;
    // Переход отмечается в файле, и отмечается ДО выключения: иначе
    // оборвавшийся журнал не отличить от упавшей игры. Сообщение
    // идёт мимо gEnabled — оно и есть объяснение, почему дальше
    // тишина.
    if (on) {
        gEnabled = true;
        write('I', "--- журнал включён ---");
    } else {
        writeFatal("--- журнал выключен настройкой; "
                   "отчёт о падении пишется по-прежнему ---");
        gEnabled = false;
    }
}

void step(const char* name) {
    if (!name) return;
    // Отметка запоминается ВСЕГДА, даже при выключенном журнале:
    // её печатает отчёт о падении, а он пишется независимо.
    std::snprintf(gLastStep, sizeof(gLastStep), "%s", name);
    write('S', "--- этап: %s", name);
}

const char* logPath()       { return gPath; }
const char* sharedLogPath() { return gSharedPath; }

} // namespace crash
