// qpext_mqtt.cpp — Minimal MQTT 3.1.1 client + Home Assistant Discovery.
//
// Reuses broker creds from /data/etc/setting.ini ([host] section). Publishes
// discovery configs at startup so HA auto-registers the device, then a
// telemetry thread pushes sensor states every few seconds. Subscribes to a
// command topic for actions (switch_tab, reboot, etc.).
//
// Pure TCP, no TLS. Single thread.

#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/un.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include <dlfcn.h>

#include <string>
#include <map>
#include <vector>

#define JSMN_STATIC
#include "jsmn.h"

extern "C" void qplog_c(const char* fmt, ...);

// EINTR-safe sleep. The host QingSnow2App process forks short-lived
// children (wpa_cli, miio scripts, analyze_noise.sh, …) whose SIGCHLD
// reaches our background threads and cuts plain sleep() short. Left
// unchecked the retry loops spun at ~kHz on extended network outages,
// starving the Qt main thread's watchdog feed and triggering the
// script-level `reboot -f` after >4 self-exits in 60 s.
static void qpext_sleep_safe(unsigned seconds) {
    struct timespec req{(time_t)seconds, 0}, rem{};
    while (nanosleep(&req, &rem) < 0 && errno == EINTR)
        req = rem;
}

// Provided at compile time by build.sh; fall back to "dev" if the build
// script didn't set it (e.g. someone invokes zig c++ directly).
#ifndef QPEXT_VERSION
#define QPEXT_VERSION "dev"
#endif

namespace {

// ---------------------------------------------------------------------------
// Tiny utilities
// ---------------------------------------------------------------------------
static std::string slurp(const char* path) {
    int fd = open(path, O_RDONLY); if (fd < 0) return {};
    std::string out; char buf[4096]; ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) out.append(buf, (size_t)n);
    close(fd); return out;
}

// Trivial INI parser: section -> key -> value. Comments (#, ;) stripped.
struct Ini { std::map<std::string, std::map<std::string, std::string>> s; };
static Ini parse_ini(const std::string& body) {
    Ini ini;
    std::string section;
    size_t i = 0;
    while (i < body.size()) {
        size_t nl = body.find('\n', i);
        std::string line = body.substr(i, (nl == std::string::npos ? body.size() : nl) - i);
        i = (nl == std::string::npos) ? body.size() : nl + 1;
        // trim
        size_t a = line.find_first_not_of(" \t\r");
        size_t b = line.find_last_not_of(" \t\r");
        if (a == std::string::npos) continue;
        line = line.substr(a, b - a + 1);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!k.empty() && (k.back()==' '||k.back()=='\t')) k.pop_back();
        while (!v.empty() && (v.front()==' '||v.front()=='\t')) v.erase(v.begin());
        ini.s[section][k] = v;
    }
    return ini;
}

// ---------------------------------------------------------------------------
// MQTT 3.1.1 wire format (we only implement what we use)
// ---------------------------------------------------------------------------

static bool write_all(int fd, const void* p, size_t n) {
    const char* b = (const char*)p; while (n) {
        ssize_t w = write(fd, b, n);
        if (w <= 0) { if (errno == EINTR) continue; return false; }
        b += w; n -= (size_t)w;
    } return true;
}
static bool read_all(int fd, void* p, size_t n) {
    char* b = (char*)p; while (n) {
        ssize_t r = read(fd, b, n);
        if (r <= 0) { if (errno == EINTR) continue; return false; }
        b += r; n -= (size_t)r;
    } return true;
}

// Encode a remaining-length field (1..4 bytes, base-128 varint).
static void enc_len(std::string& out, uint32_t n) {
    do { uint8_t b = n & 0x7F; n >>= 7; if (n) b |= 0x80; out += (char)b; } while (n);
}
// Decode incoming remaining-length.
static bool dec_len(int fd, uint32_t& n) {
    n = 0; int shift = 0;
    for (int i = 0; i < 4; ++i) {
        uint8_t b; if (!read_all(fd, &b, 1)) return false;
        n |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) return true;
        shift += 7;
    }
    return false;
}
static void enc_u16(std::string& out, uint16_t v) {
    out += (char)((v >> 8) & 0xFF); out += (char)(v & 0xFF);
}
static void enc_str(std::string& out, const std::string& s) {
    enc_u16(out, (uint16_t)s.size()); out.append(s);
}

// Send CONNECT packet. Returns true on CONNACK ok.
static bool mqtt_connect(int fd, const std::string& cid,
                         const std::string& user, const std::string& pass) {
    std::string vh;
    enc_str(vh, "MQTT");                              // protocol name
    vh += (char)4;                                    // protocol level 3.1.1
    uint8_t flags = 0xC2;                             // user|pass|clean session
    vh += (char)flags;
    enc_u16(vh, 30);                                  // keepalive 30s

    std::string payload;
    enc_str(payload, cid);
    enc_str(payload, user);
    enc_str(payload, pass);

    std::string pkt;
    pkt += (char)0x10;                                // CONNECT
    enc_len(pkt, (uint32_t)(vh.size() + payload.size()));
    pkt += vh; pkt += payload;

    if (!write_all(fd, pkt.data(), pkt.size())) return false;

    uint8_t hdr; if (!read_all(fd, &hdr, 1)) return false;
    if (hdr != 0x20) { qplog_c("[qpext-mqtt] expected CONNACK, got 0x%02x", hdr); return false; }
    uint32_t rl; if (!dec_len(fd, rl)) return false;
    if (rl < 2) return false;
    uint8_t ack[2]; if (!read_all(fd, ack, 2)) return false;
    // Drain any remaining bytes.
    for (uint32_t i = 2; i < rl; ++i) { uint8_t x; read_all(fd, &x, 1); }
    if (ack[1] != 0) { qplog_c("[qpext-mqtt] CONNACK code=%u", ack[1]); return false; }
    return true;
}

static bool mqtt_publish(int fd, const std::string& topic, const std::string& payload,
                         bool retain = false) {
    std::string vh; enc_str(vh, topic);                // QoS 0 — no packet id
    std::string pkt;
    pkt += (char)(0x30 | (retain ? 1 : 0));            // PUBLISH | retain
    enc_len(pkt, (uint32_t)(vh.size() + payload.size()));
    pkt += vh; pkt += payload;
    return write_all(fd, pkt.data(), pkt.size());
}

static bool mqtt_subscribe(int fd, const std::string& topic, uint16_t pkt_id) {
    std::string vh; enc_u16(vh, pkt_id);
    std::string payload; enc_str(payload, topic); payload += (char)0;  // QoS 0
    std::string pkt;
    pkt += (char)0x82;
    enc_len(pkt, (uint32_t)(vh.size() + payload.size()));
    pkt += vh; pkt += payload;
    if (!write_all(fd, pkt.data(), pkt.size())) return false;
    // Read SUBACK.
    uint8_t hdr; if (!read_all(fd, &hdr, 1)) return false;
    uint32_t rl; if (!dec_len(fd, rl)) return false;
    for (uint32_t i = 0; i < rl; ++i) { uint8_t x; read_all(fd, &x, 1); }
    return hdr == 0x90;
}

static bool mqtt_ping(int fd) {
    uint8_t p[2] = {0xC0, 0x00};
    return write_all(fd, p, 2);
}

// Read one incoming packet. Returns {type, payload} where type is the high
// nibble. Used to handle PUBLISH from broker (for SUB) and PINGRESP.
struct InPkt { uint8_t type; std::string payload; std::string topic; };
static bool mqtt_read_pkt(int fd, InPkt& out) {
    uint8_t hdr; if (!read_all(fd, &hdr, 1)) return false;
    out.type = (hdr >> 4) & 0x0F;
    uint32_t rl; if (!dec_len(fd, rl)) return false;
    std::string body(rl, 0);
    if (rl && !read_all(fd, &body[0], rl)) return false;

    if (out.type == 0x3) {  // PUBLISH
        if (body.size() < 2) return true;
        uint16_t tlen = ((uint8_t)body[0] << 8) | (uint8_t)body[1];
        if ((size_t)tlen + 2 > body.size()) return true;
        out.topic = body.substr(2, tlen);
        size_t skip = 2 + tlen;
        bool qos = (hdr & 0x06) != 0;
        if (qos && body.size() >= skip + 2) skip += 2;  // packet id for QoS>0
        out.payload = body.substr(skip);
    } else {
        out.payload = body;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Firmware / air-data introspection
// ---------------------------------------------------------------------------

// Read the Qingping firmware version once at startup. It lives in
// /qingping/etc/os-release as `CLEARGRASS_VERSION=4.5.6_0167`. Used in
// MQTT discovery's `sw_version` so HA shows both the device's stock
// firmware and our shim version side by side.
static std::string g_fw_version;

static void read_fw_version() {
    std::string raw = slurp("/qingping/etc/os-release");
    if (raw.empty()) { g_fw_version = "unknown"; return; }
    static const char* KEY = "CLEARGRASS_VERSION=";
    auto pos = raw.find(KEY);
    if (pos == std::string::npos) { g_fw_version = "unknown"; return; }
    pos += strlen(KEY);
    auto end = raw.find_first_of("\r\n", pos);
    g_fw_version = raw.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

// Live air-data pulled straight from QML's `airdataController` QObject —
// the same one bound into AirDatasView.qml etc. Populated by poll_airdata()
// each telemetry cycle via direct Qt introspection (QObject::property
// chains; see qpext.cpp's setContextProperty hook for how we get the
// controller pointer). No log parsing, no DB scrape.
static pthread_mutex_t g_air_mu = PTHREAD_MUTEX_INITIALIZER;
static std::map<std::string, double> g_air;
static void* g_airdata_controller = nullptr;
static pthread_mutex_t g_ctrl_mu = PTHREAD_MUTEX_INITIALIZER;

// Layout-compatible stand-in for QVariant (Qt 5.14, aarch64). QVariant has
// non-trivial copy-ctor + dtor, so per AAPCS64 it is returned via the hidden
// `x8` sret pointer — NOT in {x0,x1}. To force the calling compiler to use
// the same convention as the real QObject::property() implementation, we
// declare our proxy with a user-provided dtor + copy-ctor (which makes the
// type "non-trivial for calls"). With that in place, declaring a function
// pointer of type `FakeQVariant (*)(...)` produces the correct sret call.
//
// Memory layout matches QVariant 5.14:
//   0..7   data union (raw double / int / pointer)
//   8..11  type:30 | is_shared:1 | is_null:1
//   12..15 padding
//
// Heads up: leaks refcount when the returned QVariant owns shared resources
// (QString, QByteArray, …). Fine for primitives + QObjectStar, which is all
// we read here.
class FakeQVariant {
public:
    uint64_t data;
    uint64_t flags;
    FakeQVariant() : data(0), flags(0) {}
    ~FakeQVariant() {}                              // user-provided → sret
    FakeQVariant(const FakeQVariant&) {}            // user-provided → sret
    FakeQVariant& operator=(const FakeQVariant&) { return *this; }
};
static_assert(sizeof(FakeQVariant) == 16, "QVariant size mismatch");

// QMetaType ids we care about.
enum {
    QMT_Bool    = 1,
    QMT_Int     = 2,
    QMT_UInt    = 3,
    QMT_Double  = 6,
    QMT_Float   = 38,
    QMT_QObjectStar = 39,
};

using qobj_property_fn = FakeQVariant (*)(const void* self, const char* name);
static qobj_property_fn p_qobj_property = nullptr;

static void resolve_qt_introspection() {
    if (p_qobj_property) return;
    p_qobj_property = (qobj_property_fn)dlsym(RTLD_NEXT, "_ZNK7QObject8propertyEPKc");
    if (!p_qobj_property)
        qplog_c("[qpext-air] FATAL: dlsym QObject::property failed");
}

static uint32_t qvar_type(const FakeQVariant& v) { return v.flags & 0x3FFFFFFF; }

static void* qvar_to_qobject(const FakeQVariant& v) {
    return (qvar_type(v) == QMT_QObjectStar) ? (void*)v.data : nullptr;
}

static bool qvar_to_double(const FakeQVariant& v, double& out) {
    switch (qvar_type(v)) {
        case QMT_Double: { memcpy(&out, &v.data, sizeof(double)); return true; }
        case QMT_Float:  { float f; memcpy(&f, &v.data, sizeof(float)); out = f; return true; }
        case QMT_Int:    { int32_t i; memcpy(&i, &v.data, sizeof(int32_t)); out = i; return true; }
        case QMT_UInt:   { uint32_t u; memcpy(&u, &v.data, sizeof(uint32_t)); out = u; return true; }
        default: return false;
    }
}

// Walk airdataController.air<X>.value for every metric we know about, snap
// the values into g_air. Called every telemetry tick; cheap (one Qt
// property() call per air metric — ~10 reads).
static void poll_airdata() {
    resolve_qt_introspection();
    if (!p_qobj_property) return;
    pthread_mutex_lock(&g_ctrl_mu);
    void* ctrl = g_airdata_controller;
    pthread_mutex_unlock(&g_ctrl_mu);
    if (!ctrl) return;

    static const struct { const char* qml_child; const char* topic; } air_map[] = {
        {"airTEMP",  "temperature"},
        {"airHUMI",  "humidity"},
        {"airCO2",   "co2"},
        {"airPM10",  "pm10"},
        {"airPM25",  "pm25"},
        {"airTVOC",  "tvoc"},
        {"airNoise", "noise"},
        {"airPMV",   "pmv"},
        {"airPOA",   "poa"},
        {"airAQI",   "aqi"},
    };
    pthread_mutex_lock(&g_air_mu);
    for (auto& m : air_map) {
        FakeQVariant child_var = p_qobj_property(ctrl, m.qml_child);
        // QObject*-derived custom types register their own metatype id but
        // QVariant's storage IS the raw pointer (verified empirically; grep
        // QingSnow2App for AirData* and Q_PROPERTY).
        void* child = nullptr;
        uint32_t t = qvar_type(child_var);
        if (t == QMT_QObjectStar) child = (void*)child_var.data;
        else if (t > 0 && child_var.data > 0x1000) child = (void*)child_var.data;
        if (!child) continue;
        // AirData exposes `rawValue` (qreal) for the numeric reading; the
        // QML side uses `valueString` for the already-formatted display.
        FakeQVariant val_var = p_qobj_property(child, "rawValue");
        double d;
        if (!qvar_to_double(val_var, d)) continue;
        // QingSnow2App uses 99999 (and similar runaway integers) as a
        // sentinel for "metric not yet computed / sensor warming up" —
        // most visible on airAQI before PM/CO2 have stabilised. Drop these
        // so HA shows the entity as `unknown` rather than a misleading
        // five-digit number.
        if (d > 99000) continue;
        g_air[m.topic] = d;
    }
    pthread_mutex_unlock(&g_air_mu);
}

// Light is updated by the kernel driver — read directly from sysfs since
// QingSnow2App doesn't seem to log it through the updateValu channel.
static int read_light_lux() {
    std::string s = slurp("/sys/devices/platform/ff190000.i2c/i2c-1/1-0045/light_opt3004_read");
    auto eq = s.find('=');
    if (eq == std::string::npos) return 0;
    return atoi(s.c_str() + eq + 1);
}

} // namespace

// Called from qpext.cpp's hooked QQmlContext::setContextProperty —
// captures the live `airdataController` QObject pointer once QingSnow2App
// registers it on the QML root context. poll_airdata() reads from it on
// every telemetry tick.
extern "C" void qpext_set_airdata_controller(void* obj) {
    pthread_mutex_lock(&g_ctrl_mu);
    g_airdata_controller = obj;
    pthread_mutex_unlock(&g_ctrl_mu);
    qplog_c("[qpext-air] airdataController QObject = %p", obj);
}

namespace {

// ---------------------------------------------------------------------------
// Sensor sampling
// ---------------------------------------------------------------------------

struct CpuPrev { uint64_t total = 0, idle = 0; };
static CpuPrev g_cpu_prev;

static int read_int(const char* path) {
    std::string s = slurp(path);
    return s.empty() ? 0 : atoi(s.c_str());
}

static double cpu_percent() {
    std::string st = slurp("/proc/stat");
    if (st.empty()) return 0;
    size_t nl = st.find('\n');
    std::string line = st.substr(0, nl);
    uint64_t v[10] = {0};
    int n = sscanf(line.c_str(), "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
        (unsigned long long*)&v[0],(unsigned long long*)&v[1],(unsigned long long*)&v[2],
        (unsigned long long*)&v[3],(unsigned long long*)&v[4],(unsigned long long*)&v[5],
        (unsigned long long*)&v[6],(unsigned long long*)&v[7],(unsigned long long*)&v[8],
        (unsigned long long*)&v[9]);
    if (n < 5) return 0;
    uint64_t total = 0; for (int i = 0; i < n; ++i) total += v[i];
    uint64_t idle = v[3] + v[4];
    double pct = 0;
    if (g_cpu_prev.total && total > g_cpu_prev.total) {
        uint64_t dt = total - g_cpu_prev.total;
        uint64_t di = idle  - g_cpu_prev.idle;
        pct = 100.0 * (double)(dt - di) / (double)dt;
    }
    g_cpu_prev.total = total; g_cpu_prev.idle = idle;
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    return pct;
}

static int ram_free_mb() {
    std::string mi = slurp("/proc/meminfo");
    // "MemAvailable: NNN kB"
    size_t p = mi.find("MemAvailable:");
    if (p == std::string::npos) return 0;
    return atoi(mi.c_str() + p + 13) / 1024;
}

static long uptime_seconds() {
    std::string up = slurp("/proc/uptime");
    return up.empty() ? 0 : (long)atof(up.c_str());
}

// Wi-Fi info via wpa_supplicant's local-socket ctrl interface. SAME
// transport wpa_cli uses; the protocol is text — send "STATUS" and read
// back a key=value\n block (BSSID / SSID / ip_address / wpa_state / …).
// Avoids fork(): popen() blocks indefinitely when invoked from inside a
// Qt LD_PRELOAD'd application, and SIOCGIWESSID returns nothing on
// nl80211-only stacks (this device).
static std::string wpa_ctrl_cmd(const char* iface, const char* cmd) {
    int sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock < 0) return {};

    // Bind to a unique abstract Linux socket so wpa_supplicant's reply
    // datagram has a return address. Abstract == leading null byte;
    // avoids the filesystem-cleanup hazard of a real path.
    struct sockaddr_un me{};
    me.sun_family = AF_UNIX;
    int nlen = snprintf(me.sun_path + 1, sizeof(me.sun_path) - 1,
                        "qpext-wpa-%d-%ld", (int)getpid(), (long)time(nullptr));
    if (nlen <= 0) { close(sock); return {}; }
    socklen_t me_len = offsetof(struct sockaddr_un, sun_path) + 1 + (socklen_t)nlen;
    if (bind(sock, (struct sockaddr*)&me, me_len) < 0) { close(sock); return {}; }

    struct sockaddr_un dst{};
    dst.sun_family = AF_UNIX;
    snprintf(dst.sun_path, sizeof(dst.sun_path),
             "/var/run/wpa_supplicant/%s", iface);

    struct timeval tv{1, 0};                    // 1 s read timeout
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (sendto(sock, cmd, strlen(cmd), 0,
               (struct sockaddr*)&dst, sizeof(dst)) < 0) {
        close(sock); return {};
    }
    char buf[4096];
    ssize_t n = recv(sock, buf, sizeof(buf), 0);
    close(sock);
    if (n <= 0) return {};
    return std::string(buf, (size_t)n);
}

// Pull `key=` value out of a wpa_supplicant key=value\n block.
static std::string kv_extract(const std::string& body, const char* key) {
    std::string needle = key;
    needle += "=";
    size_t p;
    if (body.compare(0, needle.size(), needle) == 0) {
        p = needle.size();                       // first line
    } else {
        std::string nl_needle = "\n" + needle;
        p = body.find(nl_needle);
        if (p == std::string::npos) return {};
        p += nl_needle.size();
    }
    size_t q = body.find('\n', p);
    if (q == std::string::npos) q = body.size();
    std::string v = body.substr(p, q - p);
    while (!v.empty() && (v.back() == '\r' || v.back() == ' ' || v.back() == '\t'))
        v.pop_back();
    return v;
}

// IPv4 address of the interface via SIOCGIFADDR. Returns empty if the
// interface has no v4 lease yet (DHCP still in progress, link down, etc.).
static std::string read_ipv4(const char* iface = "wlan0") {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return {};
    struct ifreq ifr{};
    ifr.ifr_addr.sa_family = AF_INET;
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    int rc = ioctl(sock, SIOCGIFADDR, &ifr);
    close(sock);
    if (rc < 0) return {};
    struct sockaddr_in* sa = (struct sockaddr_in*)&ifr.ifr_addr;
    char buf[INET_ADDRSTRLEN] = {0};
    if (!inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf))) return {};
    return std::string(buf);
}

// Strip trailing whitespace/newline from a sysfs single-line read. The
// power_supply class files all end in '\n' so without the trim every
// MQTT payload would carry a stray newline.
static std::string trim_trailing(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' '  || s.back() == '\t'))
        s.pop_back();
    return s;
}

// ---------------------------------------------------------------------------
// Discovery + main loop
// ---------------------------------------------------------------------------

struct MqttCfg {
    std::string host, user, pass, client_id, mac, mac_norm;
    int port = 1883;
};

// Forward-declared; the implementations live further down (originally added
// for the dashboard/set handler). read_cfg needs obj_find here.
static bool jsoneq(const char* j, const jsmntok_t* t, const char* s);
static int  skip_tok(const jsmntok_t* toks, int nt, int i);
static int  obj_find(const char* j, const jsmntok_t* toks, int nt, int obj_idx, const char* key);

// Read MQTT broker credentials. Source priority:
//   1. /data/qpext/mqtt.json   — qpext-owned config, written by install.sh.
//      Schema: {"host","port","username","password"[,"client_id"]}.
//   2. /data/etc/setting.ini   — the stock Qingping app's config; fallback
//      so the device works the first time before mqtt.json exists.
// The MAC address is always read from setting.ini — it's the only on-device
// source of truth for the wifi MAC and the user shouldn't need to type it.
static bool read_cfg(MqttCfg& c) {
    auto ini = parse_ini(slurp("/data/etc/setting.ini"));
    c.mac = ini.s["device"]["wifi_mac"];

    auto tok_extract = [](const std::string& j,
                          const jsmntok_t* toks, int nt, const char* key) -> std::string {
        int x = obj_find(j.c_str(), toks, nt, 0, key);
        if (x < 0) return "";
        return j.substr(toks[x].start, toks[x].end - toks[x].start);
    };

    // 1) qpext-owned mqtt.json
    std::string mj = slurp("/data/qpext/mqtt.json");
    bool from_json = false;
    if (!mj.empty()) {
        jsmn_parser p;
        std::vector<jsmntok_t> toks(64);
        int nt;
        for (;;) {
            jsmn_init(&p);
            nt = jsmn_parse(&p, mj.data(), mj.size(), toks.data(), (int)toks.size());
            if (nt == JSMN_ERROR_NOMEM) { toks.resize(toks.size() * 2); continue; }
            break;
        }
        if (nt > 0 && toks[0].type == JSMN_OBJECT) {
            c.host = tok_extract(mj, toks.data(), nt, "host");
            std::string port = tok_extract(mj, toks.data(), nt, "port");
            c.user = tok_extract(mj, toks.data(), nt, "username");
            c.pass = tok_extract(mj, toks.data(), nt, "password");
            std::string cid = tok_extract(mj, toks.data(), nt, "client_id");
            c.port = port.empty() ? 1883 : atoi(port.c_str());
            if (c.port <= 0) c.port = 1883;
            // Reject placeholder values that the .example template ships with.
            if (!c.host.empty() && c.host != "10.0.0.0" &&
                c.pass != "PUT_MQTT_PASSWORD_HERE") {
                from_json = true;
                if (!cid.empty()) c.client_id = cid;
                qplog_c("[qpext-mqtt] cfg source: /data/qpext/mqtt.json");
            }
        }
    }

    // 2) Fallback to stock setting.ini
    if (!from_json) {
        auto& h = ini.s["host"];
        if (h["host"].empty()) return false;
        c.host = h["host"];
        c.port = atoi(h["port"].c_str()); if (c.port <= 0) c.port = 1883;
        c.user = h["username"];
        c.pass = h["password"];
        qplog_c("[qpext-mqtt] cfg source: /data/etc/setting.ini (fallback)");
    }

    // mac_norm = MAC without separators, uppercase.
    std::string norm;
    for (char ch : c.mac) if (ch != ':' && ch != '-') norm += (char)toupper(ch);
    c.mac_norm = norm;
    if (c.client_id.empty()) c.client_id = "qpext-" + c.mac_norm;
    return !c.host.empty();
}

static int tcp_open(const char* host, int port) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    char ps[8]; snprintf(ps, sizeof(ps), "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);
    return fd;
}

// Discovery topic for one sensor.
static void publish_discovery(int fd, const MqttCfg& c) {
    const std::string dev_id  = "qpext_" + c.mac_norm;
    const std::string sw_version = "fw " + g_fw_version + " · qpext " QPEXT_VERSION;
    const std::string dev_blk = "\"device\":{"
        "\"identifiers\":[\"" + dev_id + "\"],"
        "\"name\":\"Airmonitor App Extension\","
        "\"manufacturer\":\"Qingping\","
        "\"model\":\"Air Monitor 2\","
        "\"sw_version\":\"" + sw_version + "\","
        "\"connections\":[[\"mac\",\"" + c.mac + "\"]]"
        "}";

    auto sensor = [&](const char* key, const char* name, const char* unit,
                      const char* dev_class, const char* icon = "",
                      bool numeric = true)
    {
        std::string cfg_topic = "homeassistant/sensor/" + dev_id + "/" + key + "/config";
        std::string state_topic = "qpext/" + c.mac_norm + "/" + key;
        std::string payload =
            "{\"name\":\""+std::string(name)+"\","
             "\"uniq_id\":\""+dev_id+"_"+key+"\","
             "\"obj_id\":\""+dev_id+"_"+key+"\","
             "\"state_topic\":\""+state_topic+"\",";
        if (unit && *unit) payload += "\"unit_of_measurement\":\""+std::string(unit)+"\",";
        if (dev_class && *dev_class) payload += "\"device_class\":\""+std::string(dev_class)+"\",";
        if (icon && *icon) payload += "\"icon\":\""+std::string(icon)+"\",";
        // state_class=measurement only makes sense for numeric sensors;
        // skip for text values like version strings — HA logs a warning
        // otherwise and refuses to graph them as numbers (which they aren't).
        if (numeric) payload += "\"state_class\":\"measurement\",";
        payload += dev_blk + "}";
        mqtt_publish(fd, cfg_topic, payload, /*retain=*/true);
    };

    // Entity friendly names are short on purpose — the HA device card
    // already shows "Airmonitor App Extension <MAC>" above them, so
    // repeating the prefix on every entity is just noise.

    // Versions (text — non-numeric).
    sensor("fw_version",    "Firmware",          "",  "",  "mdi:tag-outline",       /*numeric=*/false);
    sensor("qpext_version", "Extension version", "",  "",  "mdi:package-variant",   /*numeric=*/false);

    // System telemetry
    sensor("soc_temp",     "SoC temperature",     "°C",  "temperature");
    sensor("gpu_temp",     "GPU temperature",     "°C",  "temperature");
    sensor("battery_temp", "Battery temperature", "°C",  "temperature");
    sensor("cpu",          "CPU usage",           "%",   "",            "mdi:chip");
    sensor("ram_free",     "RAM free",            "MiB", "data_size");
    sensor("uptime",       "Uptime",              "s",   "duration");
    sensor("cam_status",   "Camera status",       "",    "",            "mdi:cctv", /*numeric=*/false);

    // Connectivity (text). Pulled from `wpa_cli -i wlan0 status` each tick.
    sensor("ssid",         "Wi-Fi SSID",          "",    "",            "mdi:wifi",            /*numeric=*/false);
    sensor("ip",           "IP address",          "",    "",            "mdi:ip-network",      /*numeric=*/false);

    // Battery. status is a free-form string ("Full"/"Charging"/"Discharging"/
    // "Not charging") read straight from /sys/class/power_supply/battery/status;
    // capacity is the matching numeric %.
    sensor("battery_status",   "Battery status",   "",    "",       "mdi:battery-charging", /*numeric=*/false);
    sensor("battery_capacity", "Battery",          "%",   "battery");

    // Air quality (calibrated values pulled from QingSnow2App's own
    // SQLite store — same numbers the device's UI displays).
    sensor("temperature",  "Temperature",         "°C",  "temperature");
    sensor("humidity",     "Humidity",            "%",   "humidity");
    sensor("co2",          "CO₂ equivalent",      "ppm", "carbon_dioxide");
    sensor("pm10",         "PM10",                "µg/m³","pm10");
    sensor("pm25",         "PM2.5",               "µg/m³","pm25");
    sensor("tvoc",         "TVOC index",          "",    "",            "mdi:molecule");
    sensor("noise",        "Noise",               "dB",  "sound_pressure");
    sensor("light",        "Illuminance",         "lx",  "illuminance");
    sensor("pmv",          "Thermal comfort (PMV)", "",  "",            "mdi:thermometer-lines");
    sensor("poa",          "Predicted comfort",     "%", "",            "mdi:gauge");
    sensor("aqi",          "Air Quality Index",     "",  "aqi");

    // Helper: tab-navigation button. Publishes {"action":"switch_tab","name":"<tab>"}.
    auto tab_button = [&](const char* key, const char* label,
                          const char* tab_name, const char* icon)
    {
        std::string cfg_topic = "homeassistant/button/" + dev_id + "/" + key + "/config";
        std::string cmd_topic = "qpext/" + c.mac_norm + "/cmd";
        std::string payload =
            "{\"name\":\""+std::string(label)+"\","
             "\"uniq_id\":\""+dev_id+"_"+key+"\","
             "\"command_topic\":\""+cmd_topic+"\","
             "\"payload_press\":\"{\\\"action\\\":\\\"switch_tab\\\",\\\"name\\\":\\\""+
                std::string(tab_name)+"\\\"}\","
             "\"icon\":\""+std::string(icon)+"\","+dev_blk+"}";
        mqtt_publish(fd, cfg_topic, payload, true);
    };

    // One button per stock tab in MainPage.qml's PathView. User-defined
    // tabs (widgets / camera) get their own per-tab discovery buttons
    // published by the qpext_airmonitor integration — see
    // ha_integration/qpext_airmonitor/__init__.py::_publish_tab_buttons.
    tab_button("show_air",      "Show air data",     "airDatasView",      "mdi:weather-cloudy");
    tab_button("show_summary",  "Show summary",      "summaryView",       "mdi:chart-line");
    tab_button("show_settings", "Show settings",     "settingView",       "mdi:cog");
    tab_button("show_app",      "Show app",          "appView",           "mdi:apps");

    // Button: reboot device. uses HA "button" component.
    {
        std::string cfg_topic = "homeassistant/button/" + dev_id + "/reboot/config";
        std::string cmd_topic = "qpext/" + c.mac_norm + "/cmd";
        std::string payload =
            "{\"name\":\"Reboot\","
             "\"uniq_id\":\""+dev_id+"_reboot\","
             "\"command_topic\":\""+cmd_topic+"\","
             "\"payload_press\":\"{\\\"action\\\":\\\"reboot\\\"}\","
             "\"icon\":\"mdi:restart\","+dev_blk+"}";
        mqtt_publish(fd, cfg_topic, payload, true);
    }

    // Switch: gate the stock app's firmware update check. Hook in qpext.cpp
    // intercepts UpdateController::checkUpdate(bool); when the toggle is
    // OFF the hook no-ops the call. State is persisted in
    // /data/qpext/update_check.txt so the choice survives reboots.
    {
        std::string cfg_topic   = "homeassistant/switch/" + dev_id + "/update_check/config";
        std::string state_topic = "qpext/" + c.mac_norm + "/update_check";
        std::string cmd_topic   = "qpext/" + c.mac_norm + "/update_check/set";
        std::string payload =
            "{\"name\":\"Allow firmware update check\","
             "\"uniq_id\":\""+dev_id+"_update_check\","
             "\"obj_id\":\""+dev_id+"_update_check\","
             "\"state_topic\":\""+state_topic+"\","
             "\"command_topic\":\""+cmd_topic+"\","
             "\"payload_on\":\"1\",\"payload_off\":\"0\","
             "\"state_on\":\"1\",\"state_off\":\"0\","
             "\"icon\":\"mdi:cloud-search\","+dev_blk+"}";
        mqtt_publish(fd, cfg_topic, payload, true);
    }

    qplog_c("[qpext-mqtt] discovery published for device id=%s", dev_id.c_str());
}

// Presence / auto-discovery payload for the HA custom_component
// (qpext_airmonitor). Published retained on (re)connect — when the user
// adds the integration, HA's MQTT discovery routes this message to the
// integration's async_step_mqtt and the device shows up "to be added"
// in Settings → Devices & Services without any MAC typing.
//
// We deliberately put this on a non-`homeassistant/...` topic so it doesn't
// clash with the regular HA MQTT discovery routing.
static void publish_presence(int fd, const MqttCfg& c) {
    const std::string topic = "qpext/" + c.mac_norm + "/info";
    std::string payload =
        "{\"mac\":\""+c.mac+"\","
         "\"mac_norm\":\""+c.mac_norm+"\","
         "\"device_id\":\"qpext_"+c.mac_norm+"\","
         "\"manufacturer\":\"Qingping\","
         "\"model\":\"Air Monitor 2\","
         "\"sw\":\"qpext\","
         "\"version\":\"" QPEXT_VERSION "\","
         "\"dashboard_topic\":\"qpext/"+c.mac_norm+"/dashboard/set\","
         "\"cmd_topic\":\"qpext/"+c.mac_norm+"/cmd\"}";
    mqtt_publish(fd, topic, payload, /*retain=*/true);
    qplog_c("[qpext-mqtt] presence published on %s", topic.c_str());
}

// Camera status from shim's internal state.
extern "C" void qpext_get_cam_status_into(std::string* out);

// Write tab_event for QML (same channel doorbell automation uses).
static void write_tab_event(const std::string& tab) {
    char body[256];
    snprintf(body, sizeof(body), "{\"switch_to\":\"%s\",\"ts\":%ld}\n",
             tab.c_str(), (long)time(nullptr));
    int fd = open("/tmp/qpext/tab_event.tmp", O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) return;
    ssize_t w = write(fd, body, strlen(body)); (void)w;
    close(fd);
    rename("/tmp/qpext/tab_event.tmp", "/tmp/qpext/tab_event");
}

// Tiny jsmn helpers (mirrors of qpext_ha.cpp's; duplicated to keep the TUs
// independent — both files share the JSMN_STATIC single-header).
static bool jsoneq(const char* j, const jsmntok_t* t, const char* s) {
    int n = t->end - t->start;
    return (t->type == JSMN_STRING) && (int)strlen(s) == n &&
           strncmp(j + t->start, s, n) == 0;
}
static int skip_tok(const jsmntok_t* toks, int nt, int i) {
    int end = toks[i].end;
    int n = 1;
    while ((i + n) < nt && toks[i + n].start < end) ++n;
    return i + n;
}
static int obj_find(const char* j, const jsmntok_t* toks, int nt, int obj_idx, const char* key) {
    if (obj_idx >= nt || toks[obj_idx].type != JSMN_OBJECT) return -1;
    int n = toks[obj_idx].size;
    int i = obj_idx + 1;
    for (int k = 0; k < n; ++k) {
        if (i >= nt) return -1;
        if (jsoneq(j, &toks[i], key)) return (i + 1 < nt) ? (i + 1) : -1;
        i = skip_tok(toks, nt, i + 1);
    }
    return -1;
}
static bool write_file_atomic(const char* path, const std::string& data) {
    std::string tmp = std::string(path) + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) return false;
    ssize_t n = write(fd, data.data(), data.size());
    close(fd);
    if (n != (ssize_t)data.size()) { unlink(tmp.c_str()); return false; }
    return rename(tmp.c_str(), path) == 0;
}

// Validate a JSON payload, atomically write to `path`. Used for both the
// dashboard/set and cameras/set topics — same pattern, different target file.
static void handle_set_payload(const std::string& payload, const char* path,
                                const char* tag) {
    jsmn_parser pp;
    std::vector<jsmntok_t> ptoks(256);
    int pnt;
    for (;;) {
        jsmn_init(&pp);
        pnt = jsmn_parse(&pp, payload.data(), payload.size(),
                         ptoks.data(), (int)ptoks.size());
        if (pnt == JSMN_ERROR_NOMEM) { ptoks.resize(ptoks.size() * 2); continue; }
        break;
    }
    if (pnt < 1 || ptoks[0].type != JSMN_OBJECT) {
        qplog_c("[qpext-mqtt] %s: not a JSON object (nt=%d)", tag, pnt);
        return;
    }
    if (write_file_atomic(path, payload)) {
        qplog_c("[qpext-mqtt] %s applied: %zu bytes (%d top-level keys)",
                tag, payload.size(), ptoks[0].size);
    } else {
        qplog_c("[qpext-mqtt] %s: write failed", tag);
    }
}

// Apply a `dashboard/set` MQTT message: validate it's a JSON object, then
// write it verbatim to /data/qpext/widgets.json. After the ha-config split
// (ha.* lives in /data/qpext/ha.json now) widgets.json is purely the
// HA-integration-managed cache of {widgets, events} — no merging required.
static void handle_dashboard_set(const std::string& payload) {
    handle_set_payload(payload, "/data/qpext/widgets.json", "dashboard/set");
}

// `cameras/set`: same shape, different file. The shim's cam_thread polls
// /data/qpext/cameras.json each second via mtime, restarts gst-launch
// pipelines when the contents change, so writing here is all we need.
static void handle_cameras_set(const std::string& payload) {
    handle_set_payload(payload, "/data/qpext/cameras.json", "cameras/set");
}

// Very small JSON extract for {"action":"X","name":"Y"} payloads.
static std::string json_str(const std::string& j, const char* key) {
    std::string pat = "\""; pat += key; pat += "\"";
    size_t p = j.find(pat); if (p == std::string::npos) return "";
    p = j.find('"', p + pat.size()); if (p == std::string::npos) return "";
    size_t q = j.find('"', p + 1); if (q == std::string::npos) return "";
    return j.substr(p + 1, q - p - 1);
}

// Set by the qpext.cpp UpdateController::checkUpdate hook to read its toggle
// state. The shim caches the bool here; this file owns persistence (the
// /data/qpext/update_check.txt file) and the MQTT pub/sub plumbing.
extern "C" void qpext_set_update_check_allowed(bool allowed);
extern "C" bool qpext_get_update_check_allowed(void);

static const char* UPDATE_CHECK_FILE = "/data/qpext/update_check.txt";

// Read the persisted toggle on shim startup. Missing file ⇒ allow (the
// stock behaviour). Anything other than "0" is treated as enabled.
static void load_update_check_state() {
    std::string s = slurp(UPDATE_CHECK_FILE);
    bool allowed = !(s.size() >= 1 && s[0] == '0');
    qpext_set_update_check_allowed(allowed);
    qplog_c("[qpext-mqtt] update_check loaded: %s", allowed ? "ALLOW" : "BLOCK");
}

// Persist the toggle so the choice survives reboots.
static void save_update_check_state(bool allowed) {
    write_file_atomic(UPDATE_CHECK_FILE, std::string(allowed ? "1" : "0") + "\n");
}

// Publish the current toggle on its state topic (retained). HA reads the
// switch state from the retained value on (re)subscribe.
static void publish_update_check_state(int fd, const std::string& mac_norm) {
    std::string topic = "qpext/" + mac_norm + "/update_check";
    mqtt_publish(fd, topic, qpext_get_update_check_allowed() ? "1" : "0",
                 /*retain=*/true);
}

static void handle_update_check_set(int fd, const std::string& mac_norm,
                                    const std::string& payload) {
    // Tolerate "1"/"0", "ON"/"OFF", "true"/"false" — HA usually sends the
    // discovery-declared payload_on/payload_off pair, but other dashboards
    // (Node-RED, manual mosquitto_pub) often send the textual form.
    bool allowed;
    if (payload == "0" || payload == "OFF" || payload == "off" ||
        payload == "false" || payload == "False") {
        allowed = false;
    } else {
        allowed = true;
    }
    qpext_set_update_check_allowed(allowed);
    save_update_check_state(allowed);
    publish_update_check_state(fd, mac_norm);
    qplog_c("[qpext-mqtt] update_check set: %s (payload='%s')",
            allowed ? "ALLOW" : "BLOCK", payload.c_str());
}

static void handle_cmd(const std::string& payload) {
    std::string action = json_str(payload, "action");
    qplog_c("[qpext-mqtt] cmd action='%s' payload='%s'",
            action.c_str(), payload.c_str());
    if (action == "reboot") {
        sync();
        if (fork() == 0) execl("/sbin/reboot", "reboot", (char*)nullptr);
    } else if (action == "switch_tab") {
        std::string nm = json_str(payload, "name");
        if (!nm.empty()) write_tab_event(nm);
    }
}

static void* mqtt_thread_fn(void*) {
    read_fw_version();
    qplog_c("[qpext-mqtt] fw=%s qpext=%s", g_fw_version.c_str(), QPEXT_VERSION);
    load_update_check_state();
    MqttCfg cfg;
    while (!read_cfg(cfg)) qpext_sleep_safe(2);
    qplog_c("[qpext-mqtt] cfg host=%s:%d user=%s mac=%s",
            cfg.host.c_str(), cfg.port, cfg.user.c_str(), cfg.mac.c_str());

    int backoff = 1;
    for (;;) {
        int fd = tcp_open(cfg.host.c_str(), cfg.port);
        if (fd < 0) {
            qplog_c("[qpext-mqtt] tcp connect failed: %s", strerror(errno));
            qpext_sleep_safe(backoff); backoff = std::min(backoff * 2, 30); continue;
        }
        if (!mqtt_connect(fd, cfg.client_id, cfg.user, cfg.pass)) {
            close(fd); qpext_sleep_safe(backoff); backoff = std::min(backoff * 2, 30); continue;
        }
        backoff = 1;
        qplog_c("[qpext-mqtt] connected, publishing discovery");
        publish_discovery(fd, cfg);
        publish_presence(fd, cfg);
        std::string cmd_topic = "qpext/" + cfg.mac_norm + "/cmd";
        std::string dashboard_topic = "qpext/" + cfg.mac_norm + "/dashboard/set";
        std::string cameras_topic   = "qpext/" + cfg.mac_norm + "/cameras/set";
        std::string upd_set_topic   = "qpext/" + cfg.mac_norm + "/update_check/set";
        mqtt_subscribe(fd, cmd_topic, 1);
        mqtt_subscribe(fd, dashboard_topic, 2);
        mqtt_subscribe(fd, cameras_topic, 3);
        mqtt_subscribe(fd, upd_set_topic, 4);
        publish_update_check_state(fd, cfg.mac_norm);

        time_t last_telem = 0, last_ping = time(nullptr);
        bool ok = true;
        while (ok) {
            // Set socket to non-blocking briefly to multiplex read/idle.
            fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
            struct timeval tv{1, 0};
            int sr = select(fd + 1, &rfds, nullptr, nullptr, &tv);
            time_t now = time(nullptr);

            if (sr > 0 && FD_ISSET(fd, &rfds)) {
                InPkt pkt;
                if (!mqtt_read_pkt(fd, pkt)) { ok = false; break; }
                if (pkt.type == 0x3 && !pkt.topic.empty()) {
                    if (pkt.topic == cmd_topic) handle_cmd(pkt.payload);
                    else if (pkt.topic == dashboard_topic) handle_dashboard_set(pkt.payload);
                    else if (pkt.topic == cameras_topic)   handle_cameras_set(pkt.payload);
                    else if (pkt.topic == upd_set_topic)
                        handle_update_check_set(fd, cfg.mac_norm, pkt.payload);
                }
                // type 0x9 = SUBACK, 0xD = PINGRESP — ignore
            }

            // Telemetry every 3s. All publishes are local (~17 small messages
            // per tick) and the air-data introspection is just ~20 cheap
            // QObject::property calls — no measurable overhead.
            if (now - last_telem >= 3) {
                last_telem = now;
                char buf[64];
                std::string base = "qpext/" + cfg.mac_norm + "/";

                // Versions — static for the life of the process, but cheap
                // and lets HA pick them up immediately after subscribing.
                mqtt_publish(fd, base + "fw_version",    g_fw_version);
                mqtt_publish(fd, base + "qpext_version", QPEXT_VERSION);

                // System
                snprintf(buf, sizeof(buf), "%.1f", read_int("/sys/class/thermal/thermal_zone0/temp")/1000.0);
                mqtt_publish(fd, base + "soc_temp", buf);
                snprintf(buf, sizeof(buf), "%.1f", read_int("/sys/class/thermal/thermal_zone1/temp")/1000.0);
                mqtt_publish(fd, base + "gpu_temp", buf);
                snprintf(buf, sizeof(buf), "%.1f", read_int("/sys/class/thermal/thermal_zone2/temp")/1000.0);
                mqtt_publish(fd, base + "battery_temp", buf);
                snprintf(buf, sizeof(buf), "%.0f", cpu_percent());
                mqtt_publish(fd, base + "cpu", buf);
                snprintf(buf, sizeof(buf), "%d", ram_free_mb());
                mqtt_publish(fd, base + "ram_free", buf);
                snprintf(buf, sizeof(buf), "%ld", uptime_seconds());
                mqtt_publish(fd, base + "uptime", buf);
                std::string camst;
                qpext_get_cam_status_into(&camst);
                mqtt_publish(fd, base + "cam_status", camst);

                // Air quality — snap straight from `airdataController.air*.value`
                // via Qt introspection. Only metrics whose QObject is reachable
                // get published; HA leaves the others at "unknown" until the
                // QML side wires them up.
                poll_airdata();
                std::map<std::string, double> snap;
                pthread_mutex_lock(&g_air_mu);
                snap = g_air;
                pthread_mutex_unlock(&g_air_mu);
                auto pub_air = [&](const char* topic, int prec) {
                    auto it = snap.find(topic);
                    if (it == snap.end()) return;
                    snprintf(buf, sizeof(buf), "%.*f", prec, it->second);
                    mqtt_publish(fd, base + topic, buf);
                };
                pub_air("temperature", 1);
                pub_air("humidity",    1);
                pub_air("co2",         0);
                pub_air("pm10",        1);
                pub_air("pm25",        1);
                pub_air("tvoc",        0);
                pub_air("noise",       0);
                pub_air("pmv",         2);
                pub_air("poa",         0);
                pub_air("aqi",         0);
                snprintf(buf, sizeof(buf), "%d", read_light_lux());
                mqtt_publish(fd, base + "light", buf);

                // Slow-cadence sensors (Wi-Fi SSID + IP + battery state).
                // We publish these once every 30 s rather than on every
                // 3 s telemetry tick: a 3 s cadence noticeably aggravated
                // the stock QingSnow2App's existing ~30 s self-restart
                // cycle on this device (root cause TBD — likely Qt main-
                // loop pressure from the extra wpa_supplicant ctrl chatter
                // + sysfs reads). 30 s is plenty fresh for SSID/IP/battery.
                static time_t last_slow = 0;
                if (now - last_slow >= 30) {
                    last_slow = now;
                    std::string wpa = wpa_ctrl_cmd("wlan0", "STATUS");
                    mqtt_publish(fd, base + "ssid", kv_extract(wpa, "ssid"));
                    mqtt_publish(fd, base + "ip",   read_ipv4());
                    mqtt_publish(fd, base + "battery_status",
                                 trim_trailing(slurp("/sys/class/power_supply/battery/status")));
                    mqtt_publish(fd, base + "battery_capacity",
                                 trim_trailing(slurp("/sys/class/power_supply/battery/capacity")));
                }
            }

            // Keepalive ping every 20s.
            if (now - last_ping >= 20) {
                if (!mqtt_ping(fd)) { ok = false; break; }
                last_ping = now;
            }
        }

        close(fd);
        qplog_c("[qpext-mqtt] disconnected, reconnect in %d s", backoff);
        qpext_sleep_safe(backoff); backoff = std::min(backoff * 2, 30);
    }
    return nullptr;
}

} // namespace

extern "C" void qpext_start_mqtt_thread() {
    static bool started = false;
    if (started) return;
    started = true;
    pthread_t t;
    pthread_create(&t, nullptr, mqtt_thread_fn, nullptr);
    pthread_detach(t);
    qplog_c("[qpext-mqtt] thread spawned");
}
