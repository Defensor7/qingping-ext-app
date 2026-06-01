// qpext.so — LD_PRELOAD shim for QingSnow2App (Qt 5.14.2, aarch64).
//
// Intercepts QQmlApplicationEngine::load(const QUrl&) and forwards to the
// String overload of load() with our own URL, so we can serve a modified
// QML tree from /data/qpext without touching the binary.
//
// We deliberately avoid:
//   - return-by-value (Qt non-trivial-for-calls types use x8 sret, but our
//     opaque struct {void* d;} is trivial and the compiler would use x0)
//   - QString destructor (not exported in Qt 5.14, would have to be inlined)
//
// Both problems disappear by:
//   - using QString::QString(const QChar*, int) which takes the QString by
//     pointer (no sret),
//   - using QQmlApplicationEngine::load(const QString&) which also takes by
//     pointer,
//   - leaking ~24 bytes of QStringData per call (only one call ever per
//     process start — fine).
//
// QChar is a thin wrapper over ushort, so a UTF-16 char16_t[] literal lays
// out the same in memory.

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/time.h>

// We log to /data/qpext/qpext.log because stderr -> /dev/console -> UART
// and the device's UART is not hooked up to anything we can read.
// extern "C" so qpext_ha.cpp can call it directly.
static int log_fd = -1;
static void log_open() {
    if (log_fd >= 0) return;
    log_fd = open("/data/qpext/qpext.log", O_WRONLY|O_CREAT|O_APPEND, 0644);
}
extern "C" __attribute__((format(printf, 1, 2)))
void qplog_c(const char* fmt, ...) {
    log_open();
    if (log_fd < 0) return;
    char buf[1024];
    struct timeval tv; gettimeofday(&tv, nullptr);
    int n = snprintf(buf, sizeof(buf), "[%ld.%03d] ",
                     (long)tv.tv_sec, (int)(tv.tv_usec/1000));
    va_list ap; va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
    if (m > 0) n += m;
    if (n > (int)sizeof(buf)-1) n = sizeof(buf)-1;
    if (buf[n-1] != '\n' && n < (int)sizeof(buf)-1) { buf[n++] = '\n'; }
    ssize_t _ = write(log_fd, buf, n); (void)_;
}
#define qplog qplog_c

namespace {

struct QString { void* d; };  // opaque, only ever passed by pointer

using load_qstring_fn  = void (*)(void* self, const QString* path);
using qstr_qchar_ctor  = void (*)(QString* self, const uint16_t* utf16, int len);

// QtMessageHandler signature: void(QtMsgType, const QMessageLogContext&, const QString&)
using qt_msg_handler_t = void (*)(int type, const void* ctx, const QString* msg);
using install_handler_t = qt_msg_handler_t (*)(qt_msg_handler_t h);

static load_qstring_fn  load_qstring = nullptr;
static qstr_qchar_ctor  qstr_ctor    = nullptr;
static install_handler_t qt_install_handler = nullptr;

// Read a QString (Qt 5.14 layout) and copy it into a UTF-8 buffer.
// QArrayData header on aarch64:
//   off 0..3   : ref (int)
//   off 4..7   : size (int)
//   off 8..11  : alloc/capacity bitfield (uint)
//   off 12..15 : padding for 8-byte alignment of next member
//   off 16..23 : offset (qptrdiff, signed long)
// followed by UTF-16 data at (d + offset).
static int qstring_to_utf8(const QString* s, char* out, int out_max) {
    if (!s || !s->d || out_max <= 1) {
        if (out_max > 0) out[0] = 0;
        return 0;
    }
    const char* d = (const char*)s->d;
    int size      = *(const int*)(d + 4);
    long offset   = *(const long*)(d + 16);
    if (size < 0 || size > 65536) { out[0] = 0; return 0; }
    const uint16_t* u = (const uint16_t*)(d + offset);
    int n = 0;
    for (int i = 0; i < size && n < out_max-4; ++i) {
        uint16_t c = u[i];
        if (c < 0x80) {
            out[n++] = (char)c;
        } else if (c < 0x800) {
            out[n++] = (char)(0xC0 | (c >> 6));
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[n++] = (char)(0xE0 | (c >> 12));
            out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[n] = 0;
    return n;
}

static void qpext_msg_handler(int type, const void* /*ctx*/, const QString* msg) {
    static const char* lvls[] = {"DBG","WRN","CRT","FTL","INF"};
    const char* lvl = (type >= 0 && type <= 4) ? lvls[type] : "???";
    char buf[2048];
    qstring_to_utf8(msg, buf, sizeof(buf));
    qplog("[qt-%s] %s\n", lvl, buf);
}

static void resolve(void) {
    if (load_qstring && qstr_ctor) return;

    // QQmlApplicationEngine::load(const QString&)
    load_qstring = (load_qstring_fn)dlsym(RTLD_NEXT,
        "_ZN21QQmlApplicationEngine4loadERK7QString");

    // QString::QString(const QChar*, int)   (C1/C2 both work; pick whichever
    // happens to be exported)
    qstr_ctor = (qstr_qchar_ctor)dlsym(RTLD_NEXT,
        "_ZN7QStringC1EPK5QChari");
    if (!qstr_ctor)
        qstr_ctor = (qstr_qchar_ctor)dlsym(RTLD_NEXT,
            "_ZN7QStringC2EPK5QChari");

    // qInstallMessageHandler — pipe QML console.log + warnings to qpext.log.
    qt_install_handler = (install_handler_t)dlsym(RTLD_NEXT,
        "_Z22qInstallMessageHandlerPFv9QtMsgTypeRK18QMessageLogContextRK7QStringE");
    if (qt_install_handler) qt_install_handler(qpext_msg_handler);

    qplog("[qpext] resolved load_qstring=%p qstr_ctor=%p install=%p\n",
            (void*)load_qstring, (void*)qstr_ctor, (void*)qt_install_handler);
}

// Encoded once per process. ENTRY can be overridden by QPEXT_ENTRY env.
static const QString* build_entry(void) {
    static QString entry = {nullptr};
    static int built = 0;
    if (built) return &entry;
    built = 1;

    // UTF-16 little-endian literal. char16_t and QChar share layout.
    // u"..." gives a NUL-terminated char16_t array — we slice off the NUL.
    static const char16_t default_path[] = u"file:///data/qpext/main.qml";

    const char* override = getenv("QPEXT_ENTRY");
    if (override && *override) {
        // Convert ASCII override to UTF-16 on the fly. Bounded to 256 chars,
        // which is plenty for /data/... paths.
        static char16_t buf[256];
        int n = 0;
        while (n < 255 && override[n]) { buf[n] = (uint16_t)override[n]; ++n; }
        buf[n] = 0;
        qstr_ctor(&entry, (const uint16_t*)buf, n);
        qplog("[qpext] entry (env): %s\n", override);
    } else {
        int n = sizeof(default_path)/sizeof(default_path[0]) - 1;  // strip NUL
        qstr_ctor(&entry, (const uint16_t*)default_path, n);
        qplog("[qpext] entry (default): /data/qpext/main.qml\n");
    }
    return &entry;
}

}  // namespace

extern "C" {

// Quick check: are we actually inside QingSnow2App? LD_PRELOAD is inherited
// by every child process the app spawns (wpa_cli, ping, mosquitto_sub, ...).
// We don't want to pollute the log from those.
static bool in_target_process(void) {
    char buf[64] = {0};
    int fd = open("/proc/self/comm", O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, sizeof(buf)-1);
    close(fd);
    if (n <= 0) return false;
    // Strip trailing newline.
    if (buf[n-1] == '\n') buf[n-1] = 0;
    // Linux truncates comm at 15 chars: "QingSnow2App.re"
    return (buf[0] == 'Q' && strstr(buf, "QingSnow2App") != nullptr);
}

extern void qpext_start_ha_thread();
extern void qpext_start_cam_thread();
extern void qpext_start_mqtt_thread();

__attribute__((constructor))
static void qpext_init(void) {
    if (!in_target_process()) return;
    qplog("[qpext] loaded in pid=%d comm=%s\n", (int)getpid(), "QingSnow2App");
    qpext_start_ha_thread();
    qpext_start_cam_thread();
    qpext_start_mqtt_thread();
}

// Hook: QQmlApplicationEngine::load(const QUrl&)
__attribute__((visibility("default")))
void _ZN21QQmlApplicationEngine4loadERK4QUrl(void* self, const void* /*url*/) {
    resolve();
    if (!load_qstring || !qstr_ctor) {
        qplog("[qpext] FATAL: dlsym failed, app will likely crash\n");
        return;
    }
    const QString* s = build_entry();
    load_qstring(self, s);
}

}  // extern "C"
