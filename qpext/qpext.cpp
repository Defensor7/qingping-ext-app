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

#define _GNU_SOURCE
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

// --------------------------------------------------------------------------
// Hook QQmlContext::setContextProperty(const QString&, QObject*)
//
// The original QingSnow2App registers ~20 controllers on the root context —
// `airdataController` (sensor model), `screenManager`, `wifiManager`, etc.
// We want a live, programmatic handle to `airdataController` for telemetry,
// so we override the C++ overload, save the QObject* when name matches, and
// forward to the real implementation. Everything else goes through unchanged.
// --------------------------------------------------------------------------
using set_ctx_prop_fn = void (*)(void* self, const QString* name, void* obj);
static set_ctx_prop_fn original_set_ctx_prop = nullptr;
extern "C" void qpext_set_airdata_controller(void* obj);  // in qpext_mqtt.cpp

namespace { static void resolve_set_ctx_prop() {
    if (original_set_ctx_prop) return;
    original_set_ctx_prop = (set_ctx_prop_fn)dlsym(RTLD_NEXT,
        "_ZN11QQmlContext18setContextPropertyERK7QStringP7QObject");
    if (!original_set_ctx_prop)
        qplog("[qpext] FATAL: dlsym QQmlContext::setContextProperty failed");
}}

extern "C" {

__attribute__((visibility("default")))
void _ZN11QQmlContext18setContextPropertyERK7QStringP7QObject(
    void* self, const QString* name, void* obj)
{
    resolve_set_ctx_prop();
    if (original_set_ctx_prop) original_set_ctx_prop(self, name, obj);
    char buf[64];
    qstring_to_utf8(name, buf, sizeof(buf));
    if (strcmp(buf, "airdataController") == 0) {
        qpext_set_airdata_controller(obj);
    }
}

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

// --------------------------------------------------------------------------
// Hook HttpRunner::send(QString)
//
// QingSnow2App periodically polls Qingping's `/firmware/checkUpdate`
// endpoint to see whether a new device firmware is available; when one is,
// the QML HeaderBar shows a "Firmware update available" notification banner.
//
// Direct `UpdateController::checkUpdate(bool)` interposition would be the
// natural hook point, but although that symbol is exported as GLOBAL
// DEFAULT, all of its callers live inside QingSnow2App.real itself — the
// linker resolves those calls via internal addressing so LD_PRELOAD never
// gets a look in.
//
// Instead we hook the stock app's HTTP wrapper `HttpRunner::send(QString)`
// (in the same binary, but called via Qt signal-slot dispatch which DOES
// go through the dynamic linker). We read the URL string, and if the
// shim's `g_update_check_allowed` is false AND the path contains
// `/firmware/checkUpdate`, we no-op the call. All other URLs (weather,
// location, …) pass through.
//
// Storage for the toggle. Written by qpext_mqtt.cpp via the setter below
// when an MQTT command arrives, and on startup from /data/qpext/update_check.txt.
// `volatile` because it's read on the calling (Qt main) thread and written
// from the MQTT background thread without any other synchronisation — a
// torn bool read is harmless here.
// --------------------------------------------------------------------------
static volatile bool g_update_check_allowed = true;

extern "C" void qpext_set_update_check_allowed(bool allowed) {
    g_update_check_allowed = allowed;
}
extern "C" bool qpext_get_update_check_allowed(void) {
    return g_update_check_allowed;
}

// QingSnow2App.real does NOT link libssl directly — Qt5Network dlopens
// `libssl.so.1.1` lazily and then dlsym()s the OpenSSL functions it needs
// (`SSL_write`, `SSL_read`, …) by name. Those lookups happen against a
// specific dlopen handle, NOT through the global symbol scope, so a plain
// LD_PRELOAD override of `SSL_write` never gets called — Qt grabs the real
// address straight out of libssl and bypasses us entirely.
//
// To intercept anyway we override `dlsym` itself. When Qt asks for
// `SSL_write` we hand back a pointer to our wrapper, which inspects the
// plaintext buffer Qt is about to encrypt. If the toggle is off AND the
// buffer contains a `/firmware/checkUpdate` HTTP request we drop the write
// (return num, no data forwarded); the network reply eventually surfaces
// to UpdateController as an error and no "Firmware update available"
// banner is shown.
//
// All other HTTPS traffic — weather, location, telemetry — uses the same
// SSL connection but different URL paths and so passes through unmodified.

using dlsym_fn   = void* (*)(void*, const char*);
using ssl_write_fn = int (*)(void* ssl, const void* buf, int num);

static dlsym_fn   real_dlsym   = nullptr;
static ssl_write_fn real_ssl_write = nullptr;

namespace { static void resolve_real_dlsym(void) {
    if (real_dlsym) return;
    // glibc on this device exports `dlsym` versioned as `GLIBC_2.17`
    // (aarch64 base version). dlvsym resolves the versioned symbol from
    // the next library along the search order — i.e. the real glibc one,
    // skipping our own override below. Without this bootstrap our hook
    // would recursively call itself.
    real_dlsym = (dlsym_fn)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.17");
    if (!real_dlsym)
        qplog("[qpext] dlvsym dlsym/GLIBC_2.17 failed — update filter inert");
}}

// Wrapper Qt actually calls when it think it's calling OpenSSL's SSL_write.
extern "C" int qpext_ssl_write_hook(void* ssl, const void* buf, int num) {
    if (!g_update_check_allowed && num > 30 && buf != nullptr) {
        // memmem() is a glibc extension; available in this device's libc.
        if (memmem(buf, (size_t)num, "/firmware/checkUpdate", 21) != nullptr) {
            qplog("[qpext] firmware update check blocked (dropped %d B SSL_write)", num);
            return num;
        }
    }
    if (real_ssl_write) return real_ssl_write(ssl, buf, num);
    return num;   // best-effort no-op rather than -1 (which would tear down the SSL session)
}

extern "C" {

// Export our override as `dlsym@@GLIBC_2.17` (the default GLIBC version
// alias). libQt5Network's `dlsym` PLT relocation requests exactly that
// versioned symbol; without the `.symver` alias the loader's versioned
// lookup would skip our unversioned/Qt_5-tagged export and fall through
// to glibc's real dlsym, leaving Qt's SSL_write resolution untouched.
// The double-`@@` form marks our copy as the *default* version, so
// callers requesting unversioned `dlsym` also resolve to us.
__asm__(".symver qpext_dlsym_impl,dlsym@@GLIBC_2.17");

// Intercept dlsym so that when Qt's SSL backend asks for `SSL_write` (after
// dlopening libssl) we hand it our wrapper. Other lookups pass through
// unchanged. A thread-local recursion guard keeps us safe if anything in
// our log path or libdl internals ends up calling dlsym again.
__attribute__((visibility("default")))
void* qpext_dlsym_impl(void* handle, const char* name) {
    static __thread int reentry = 0;
    resolve_real_dlsym();
    if (!real_dlsym) return nullptr;
    if (reentry) return real_dlsym(handle, name);
    reentry = 1;
    void* sym = real_dlsym(handle, name);
    if (sym && name && strcmp(name, "SSL_write") == 0) {
        real_ssl_write = (ssl_write_fn)sym;
        sym = (void*)&qpext_ssl_write_hook;
        qplog("[qpext] dlsym SSL_write intercepted → wrapper at %p (real %p)",
              sym, (void*)real_ssl_write);
    }
    reentry = 0;
    return sym;
}

}  // extern "C"


}  // extern "C"
