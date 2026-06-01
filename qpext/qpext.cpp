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

// When true, the getaddrinfo hook substitutes "127.0.0.1" for every name
// the `/bin/ping` process tries to resolve. Localhost replies to ICMP
// echo for free, so ping exits with "1 received, 0% packet loss" and the
// stock app's `[WIFI模块] checkPing` sees the network as reachable —
// regardless of whether 1.1.1.1 / 180.76.76.76 / etc. are actually
// reachable from the device's network. Useful in regions where some of
// those public DNS targets are filtered (Russia → Baidu 180.76.76.76,
// CN → Cloudflare 1.0.0.1, etc.). State is mirrored in
// `/data/qpext/ping_stub.txt` for persistence across reboots and
// controlled from HA via a separate switch entity (see qpext_mqtt.cpp).
static volatile bool g_ping_stub_enabled = false;

extern "C" void qpext_set_ping_stub_enabled(bool enabled) {
    g_ping_stub_enabled = enabled;
}
extern "C" bool qpext_get_ping_stub_enabled(void) {
    return g_ping_stub_enabled;
}

// Three additional egress-filter toggles. Each gates a class of outbound
// connect() calls made by the stock app (i.e. originated from
// libQt5Network) WITHOUT touching outbound traffic from our own shim
// (qpext.so) or from the `ping` subprocess (covered by ping_stub above).
//
//   block_cloud_https  — refuse `connect(*, *:443)` from Qt. Kills cloud
//                        HTTPS to qing.cleargrass.com (weather, location,
//                        cooperation/companies, device/pairStatus, …).
//   block_stock_mqtt   — refuse `connect(*, *:1883)` from Qt. Kills the
//                        stock app's MQTT push to the user's home broker
//                        (10.7.1.7 / setting.ini [host]).
//   block_miio_ipc     — refuse `connect(*, 127.0.0.1:54322)` from Qt.
//                        Kills the xiaomi miio_client local socket (the
//                        miio_helper.sh integration that mirrors sensor
//                        data into Mi Home cloud via /tmp UDS bridge).
//
// All three default to OFF and are persisted in their own `.txt` files;
// the MQTT plumbing mirrors the update_check / ping_stub pattern.
static volatile bool g_block_cloud_https = false;
static volatile bool g_block_stock_mqtt  = false;
static volatile bool g_block_miio_ipc    = false;

extern "C" void qpext_set_block_cloud_https(bool b) { g_block_cloud_https = b; }
extern "C" bool qpext_get_block_cloud_https(void)   { return g_block_cloud_https; }
extern "C" void qpext_set_block_stock_mqtt(bool b)  { g_block_stock_mqtt  = b; }
extern "C" bool qpext_get_block_stock_mqtt(void)    { return g_block_stock_mqtt; }
extern "C" void qpext_set_block_miio_ipc(bool b)    { g_block_miio_ipc    = b; }
extern "C" bool qpext_get_block_miio_ipc(void)      { return g_block_miio_ipc; }

// Cheap once-per-process check whether we're running inside the `/bin/ping`
// subprocess that the stock app spawns to test internet connectivity. Our
// LD_PRELOAD is inherited across fork/exec so the same qpext.so is loaded
// into ping too; we just need to look at /proc/self/comm to tell where
// we are. Cached because the answer never changes within a process.
static bool qpext_in_ping_process(void) {
    static int cached = 0;   // 0=unknown, +1=yes, -1=no
    if (cached) return cached > 0;
    int fd = open("/proc/self/comm", O_RDONLY);
    if (fd < 0) { cached = -1; return false; }
    char buf[32] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = 0;
    cached = (strcmp(buf, "ping") == 0) ? 1 : -1;
    return cached > 0;
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
using ssl_read_fn  = int (*)(void* ssl, void* buf, int num);

static dlsym_fn     real_dlsym     = nullptr;
static ssl_write_fn real_ssl_write = nullptr;
static ssl_read_fn  real_ssl_read  = nullptr;

// Tiny LRU mapping `SSL* ctx` → last URL path we saw it write. Used by the
// SSL_read hook to log the matching response body, so we can reverse-
// engineer the JSON schema of `/daily/weatherNow`, `/daily/dailyForecasts`
// etc. Lock-free — racy on concurrent writes, but the worst case is a
// mis-tagged log line during capture, which is fine for one-off
// reverse engineering.
struct SslUrlTag {
    void* ctx;
    char  path[96];
    int   reads_remaining;   // how many more reads to log for this ctx
};
#define QPEXT_SSL_URL_SLOTS 16
static SslUrlTag g_ssl_tags[QPEXT_SSL_URL_SLOTS] = {};
static int       g_ssl_tag_next = 0;

static void qpext_tag_ssl_url(void* ctx, const char* http_request_line) {
    // Extract just the path (between first and second space): GET <path> HTTP/1.1
    const char* sp1 = strchr(http_request_line, ' ');
    if (!sp1) return;
    const char* path = sp1 + 1;
    const char* sp2 = strchr(path, ' ');
    if (!sp2) return;
    size_t plen = (size_t)(sp2 - path);
    SslUrlTag* slot = &g_ssl_tags[g_ssl_tag_next++ % QPEXT_SSL_URL_SLOTS];
    slot->ctx = ctx;
    if (plen >= sizeof(slot->path)) plen = sizeof(slot->path) - 1;
    memcpy(slot->path, path, plen);
    slot->path[plen] = 0;
    slot->reads_remaining = 2;     // log first two read chunks (headers + body)
}

static SslUrlTag* qpext_find_ssl_tag(void* ctx) {
    for (int i = 0; i < QPEXT_SSL_URL_SLOTS; ++i) {
        if (g_ssl_tags[i].ctx == ctx) return &g_ssl_tags[i];
    }
    return nullptr;
}

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

// Extract the URL path out of a plaintext HTTP request the caller is about
// to encrypt — for the `[qpext-net]` audit log. Best-effort: copies up to
// `cap-1` bytes from the start of the first line of the buffer, stopping at
// the second space (HTTP version) or any control char. Empty on miss.
static void qpext_extract_request_line(const void* buf, int num,
                                       char* out, int cap) {
    out[0] = 0;
    if (!buf || num < 20 || cap < 8) return;
    const char* p = (const char*)buf;
    // Require an HTTP verb at the start so we don't log random binary noise.
    if (!(num > 4 &&
          (memcmp(p, "GET ", 4) == 0 ||
           memcmp(p, "POST", 4) == 0 ||
           memcmp(p, "HEAD", 4) == 0 ||
           memcmp(p, "PUT ", 4) == 0)))
        return;
    int n = 0;
    while (n < num && n < cap - 1 && p[n] != '\r' && p[n] != '\n') ++n;
    memcpy(out, p, (size_t)n);
    out[n] = 0;
}

// Wrapper Qt actually calls when it think it's calling OpenSSL's SSL_write.
extern "C" int qpext_ssl_write_hook(void* ssl, const void* buf, int num) {
    if (num > 30 && buf != nullptr) {
        char line[256];
        qpext_extract_request_line(buf, num, line, sizeof(line));
        if (line[0]) {
            qplog("[qpext-net] https %s (%d B)", line, num);
            // Remember which URL this SSL ctx is fetching, so the SSL_read
            // hook can tag the matching response body — see below.
            qpext_tag_ssl_url(ssl, line);
        }

        if (!g_update_check_allowed) {
            // memmem() is a glibc extension; available in this device's libc.
            if (memmem(buf, (size_t)num, "/firmware/checkUpdate", 21) != nullptr) {
                qplog("[qpext] firmware update check blocked (dropped %d B SSL_write)", num);
                return num;
            }
        }
    }
    if (real_ssl_write) return real_ssl_write(ssl, buf, num);
    return num;   // best-effort no-op rather than -1 (which would tear down the SSL session)
}

// Wrapper Qt calls when it thinks it's calling OpenSSL's SSL_read. Logs the
// first couple of plaintext chunks of each HTTPS response so we can capture
// the JSON schema the Qingping cloud returns — needed for the upcoming
// "shim feeds cached weather from HA back into the stock app" project.
extern "C" int qpext_ssl_read_hook(void* ssl, void* buf, int num) {
    if (!real_ssl_read) return -1;
    int n = real_ssl_read(ssl, buf, num);
    if (n > 0 && buf) {
        SslUrlTag* tag = qpext_find_ssl_tag(ssl);
        if (tag && tag->reads_remaining > 0) {
            // Single-line log: replace control chars with '.' and truncate
            // so the line stays under qplog's bounded buffer (~2 KiB total).
            char snippet[1600];
            int cap = (int)sizeof(snippet) - 1;
            int copy = n < cap ? n : cap;
            const char* src = (const char*)buf;
            for (int i = 0; i < copy; ++i) {
                unsigned char c = (unsigned char)src[i];
                snippet[i] = (c == '\n' || c == '\r' || c == '\t' || c >= 32) ? c : '.';
            }
            snippet[copy] = 0;
            qplog("[qpext-net] resp %s (%d/%d B) chunk: %s",
                  tag->path, copy, n, snippet);
            --tag->reads_remaining;
        }
    }
    return n;
}

// --------------------------------------------------------------------------
// Net-audit hooks: log every connect() + getaddrinfo() + sendto() the host
// app issues. Same `.symver name@@GLIBC_2.17` plumbing as the SSL_write
// interception above — each function uses a `qpext_<name>_impl` C symbol
// aliased to the versioned glibc name the binary actually links against,
// so libQt5Network and friends route through us instead of glibc.
//
// We bootstrap the real glibc copies via `dlvsym(RTLD_NEXT, ..., "GLIBC_2.17")`
// rather than `dlsym` to avoid recursing through our own dlsym hook above.
// --------------------------------------------------------------------------

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>

using connect_fn = int (*)(int, const struct sockaddr*, socklen_t);
using getaddrinfo_fn = int (*)(const char*, const char*,
                               const struct addrinfo*, struct addrinfo**);
using sendto_fn = ssize_t (*)(int, const void*, size_t, int,
                              const struct sockaddr*, socklen_t);

static connect_fn     real_connect     = nullptr;
static getaddrinfo_fn real_getaddrinfo = nullptr;
static sendto_fn      real_sendto      = nullptr;

namespace { static void resolve_net_originals(void) {
    if (!real_connect)
        real_connect = (connect_fn)dlvsym(RTLD_NEXT, "connect", "GLIBC_2.17");
    if (!real_getaddrinfo)
        real_getaddrinfo = (getaddrinfo_fn)dlvsym(RTLD_NEXT, "getaddrinfo", "GLIBC_2.17");
    if (!real_sendto)
        real_sendto = (sendto_fn)dlvsym(RTLD_NEXT, "sendto", "GLIBC_2.17");
}}

// Format `addr` as `ip:port` for the audit log. Quietly skips AF_UNIX /
// AF_NETLINK / unknown families (those aren't outbound network traffic).
static void qpext_format_sockaddr(const struct sockaddr* addr, socklen_t len,
                                  char* out, size_t cap) {
    out[0] = 0;
    if (!addr || len < (socklen_t)sizeof(sa_family_t)) return;
    if (addr->sa_family == AF_INET && len >= (socklen_t)sizeof(struct sockaddr_in)) {
        const struct sockaddr_in* a = (const struct sockaddr_in*)addr;
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
        snprintf(out, cap, "%s:%u", ip, (unsigned)ntohs(a->sin_port));
    } else if (addr->sa_family == AF_INET6 &&
               len >= (socklen_t)sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6* a = (const struct sockaddr_in6*)addr;
        char ip[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof(ip));
        snprintf(out, cap, "[%s]:%u", ip, (unsigned)ntohs(a->sin6_port));
    }
}

// Format the caller's location for the audit log. Uses
// `__builtin_return_address(0)` (the address the hook will return to,
// i.e. the instruction immediately after the call) and `dladdr()` to map
// it to the enclosing module + the closest exported symbol. For QingSnow's
// internal functions `dli_sname` is usually empty (file-static linkage) —
// we fall back to a `module+offset` form that's still useful for
// correlating across calls and feeding `addr2line`.
static void qpext_format_caller(void* ret_addr, char* out, size_t cap) {
    out[0] = 0;
    if (!ret_addr) return;
    Dl_info info{};
    if (!dladdr(ret_addr, &info)) {
        snprintf(out, cap, "?+%p", ret_addr);
        return;
    }
    const char* base = info.dli_fname ? strrchr(info.dli_fname, '/') : nullptr;
    base = base ? base + 1 : (info.dli_fname ? info.dli_fname : "?");
    if (info.dli_sname) {
        long off = (long)((char*)ret_addr - (char*)info.dli_saddr);
        snprintf(out, cap, "%s!%s+0x%lx", base, info.dli_sname, off);
    } else {
        long off = (long)((char*)ret_addr - (char*)info.dli_fbase);
        snprintf(out, cap, "%s+0x%lx", base, off);
    }
}

// Return non-zero when `addr_in_module` falls inside a shared object whose
// basename matches `needle` (substring match against the dl_fname path).
// Used so the connect-block toggles can ignore traffic our own shim
// initiates without having to plumb thread-local flags everywhere.
static int qpext_caller_module_matches(void* ret_addr, const char* needle) {
    if (!ret_addr || !needle) return 0;
    Dl_info info{};
    if (!dladdr(ret_addr, &info) || !info.dli_fname) return 0;
    return strstr(info.dli_fname, needle) != nullptr;
}

extern "C" {

__asm__(".symver qpext_connect_impl,connect@@GLIBC_2.17");
__attribute__((visibility("default")))
int qpext_connect_impl(int sock, const struct sockaddr* addr, socklen_t len) {
    resolve_net_originals();
    void* ret_addr = __builtin_return_address(0);
    char where[80] = {0};
    qpext_format_sockaddr(addr, len, where, sizeof(where));
    if (where[0]) {
        char who[160] = {0};
        qpext_format_caller(ret_addr, who, sizeof(who));
        qplog("[qpext-net] connect %s  <- %s", where, who);
    }

    // Egress filters. Only fire on AF_INET/AF_INET6 destinations from a
    // libQt5Network caller — we never want to block our own shim's
    // outbound traffic, and AF_UNIX local sockets aren't relevant here.
    if (addr && len >= (socklen_t)sizeof(sa_family_t) &&
        (addr->sa_family == AF_INET || addr->sa_family == AF_INET6) &&
        qpext_caller_module_matches(ret_addr, "libQt5Network")) {

        unsigned port = 0;
        bool is_localhost = false;
        if (addr->sa_family == AF_INET) {
            const struct sockaddr_in* a = (const struct sockaddr_in*)addr;
            port = ntohs(a->sin_port);
            is_localhost = (ntohl(a->sin_addr.s_addr) == 0x7f000001u); // 127.0.0.1
        } else {
            const struct sockaddr_in6* a = (const struct sockaddr_in6*)addr;
            port = ntohs(a->sin6_port);
            // IN6ADDR_LOOPBACK_INIT = ::1
            static const unsigned char loop6[16] = {
                0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1
            };
            is_localhost = memcmp(&a->sin6_addr, loop6, 16) == 0;
        }

        if (g_block_cloud_https && port == 443 && !is_localhost) {
            qplog("[qpext-net] block_cloud_https → drop connect %s", where);
            errno = ECONNREFUSED;
            return -1;
        }
        if (g_block_stock_mqtt && port == 1883 && !is_localhost) {
            qplog("[qpext-net] block_stock_mqtt → drop connect %s", where);
            errno = ECONNREFUSED;
            return -1;
        }
        if (g_block_miio_ipc && is_localhost && port == 54322) {
            qplog("[qpext-net] block_miio_ipc → drop connect %s", where);
            errno = ECONNREFUSED;
            return -1;
        }
    }

    if (!real_connect) { errno = ENOSYS; return -1; }
    return real_connect(sock, addr, len);
}

__asm__(".symver qpext_getaddrinfo_impl,getaddrinfo@@GLIBC_2.17");
__attribute__((visibility("default")))
int qpext_getaddrinfo_impl(const char* node, const char* service,
                           const struct addrinfo* hints, struct addrinfo** res) {
    resolve_net_originals();
    if (node) {
        char who[160] = {0};
        qpext_format_caller(__builtin_return_address(0), who, sizeof(who));
        qplog("[qpext-net] dns   %s%s%s  <- %s", node,
              service ? ":" : "", service ? service : "", who);
    }
    if (!real_getaddrinfo) { return EAI_SYSTEM; }
    // Ping-stub: if we're inside the stock app's connectivity-probe `ping`
    // subprocess and the user has enabled the stub, route every lookup to
    // 127.0.0.1 so the ping packet bounces off the loopback interface and
    // returns "0% packet loss" regardless of the actual reachability of
    // 1.1.1.1 / 180.76.76.76 / etc.
    if (g_ping_stub_enabled && node && qpext_in_ping_process()) {
        qplog("[qpext-net] ping stub: substituting %s -> 127.0.0.1", node);
        return real_getaddrinfo("127.0.0.1", service, hints, res);
    }
    return real_getaddrinfo(node, service, hints, res);
}

__asm__(".symver qpext_sendto_impl,sendto@@GLIBC_2.17");
__attribute__((visibility("default")))
ssize_t qpext_sendto_impl(int sock, const void* buf, size_t len, int flags,
                          const struct sockaddr* addr, socklen_t addrlen) {
    resolve_net_originals();
    if (addr && addrlen >= (socklen_t)sizeof(sa_family_t) &&
        (addr->sa_family == AF_INET || addr->sa_family == AF_INET6)) {
        char where[80] = {0};
        qpext_format_sockaddr(addr, addrlen, where, sizeof(where));
        if (where[0]) {
            char who[160] = {0};
            qpext_format_caller(__builtin_return_address(0), who, sizeof(who));
            qplog("[qpext-net] udp   %s (%zu B)  <- %s", where, len, who);
        }
    }
    if (!real_sendto) { errno = ENOSYS; return -1; }
    return real_sendto(sock, buf, len, flags, addr, addrlen);
}

}  // extern "C"

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
    } else if (sym && name && strcmp(name, "SSL_read") == 0) {
        real_ssl_read = (ssl_read_fn)sym;
        sym = (void*)&qpext_ssl_read_hook;
        qplog("[qpext] dlsym SSL_read intercepted → wrapper at %p (real %p)",
              sym, (void*)real_ssl_read);
    }
    reentry = 0;
    return sym;
}

}  // extern "C"


}  // extern "C"
