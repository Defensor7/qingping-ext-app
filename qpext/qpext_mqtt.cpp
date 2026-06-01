// qpext_mqtt.cpp — Minimal MQTT 3.1.1 client + Home Assistant Discovery.
//
// Reuses broker creds from /data/etc/setting.ini ([host] section). Publishes
// discovery configs at startup so HA auto-registers the device, then a
// telemetry thread pushes sensor states every few seconds. Subscribes to a
// command topic for actions (switch_tab, reboot, etc.).
//
// Pure TCP, no TLS. Single thread.

#include <pthread.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <stdint.h>

#include <string>
#include <map>
#include <vector>

#define JSMN_STATIC
#include "jsmn.h"

extern "C" void qplog_c(const char* fmt, ...);

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

// ---------------------------------------------------------------------------
// Discovery + main loop
// ---------------------------------------------------------------------------

struct MqttCfg {
    std::string host, user, pass, client_id, mac, mac_norm;
    int port = 1883;
};

static bool read_cfg(MqttCfg& c) {
    auto ini = parse_ini(slurp("/data/etc/setting.ini"));
    auto& h = ini.s["host"];
    auto& d = ini.s["device"];
    if (h["host"].empty()) return false;
    c.host = h["host"];
    c.port = atoi(h["port"].c_str()); if (c.port <= 0) c.port = 1883;
    c.user = h["username"];
    c.pass = h["password"];
    c.mac  = d["wifi_mac"];
    c.mac_norm = c.mac;
    std::string out;
    for (char ch : c.mac_norm) if (ch != ':') out += ch;
    c.mac_norm = out;
    c.client_id = "qpext-" + c.mac_norm;
    return true;
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
    const std::string dev_blk = "\"device\":{"
        "\"identifiers\":[\"" + dev_id + "\"],"
        "\"name\":\"Airmonitor (qpext)\","
        "\"manufacturer\":\"Qingping\","
        "\"model\":\"Air Monitor 2\","
        "\"sw_version\":\"qpext\","
        "\"connections\":[[\"mac\",\"" + c.mac + "\"]]"
        "}";

    auto sensor = [&](const char* key, const char* name, const char* unit,
                      const char* dev_class, const char* icon = "")
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
        payload += "\"state_class\":\"measurement\","+dev_blk+"}";
        mqtt_publish(fd, cfg_topic, payload, /*retain=*/true);
    };

    sensor("soc_temp",     "Airmonitor SoC temperature",     "°C",  "temperature");
    sensor("gpu_temp",     "Airmonitor GPU temperature",     "°C",  "temperature");
    sensor("battery_temp", "Airmonitor battery temperature", "°C",  "temperature");
    sensor("cpu",          "Airmonitor CPU usage",           "%",   "",            "mdi:chip");
    sensor("ram_free",     "Airmonitor RAM free",            "MiB", "data_size");
    sensor("uptime",       "Airmonitor uptime",              "s",   "duration");
    sensor("cam_status",   "Airmonitor camera status",       "",    "",            "mdi:cctv");

    // Helper: tab-navigation button. Publishes {"action":"switch_tab","name":"<tab>"}.
    auto tab_button = [&](const char* key, const char* label,
                          const char* tab_name, const char* icon)
    {
        std::string cfg_topic = "homeassistant/button/" + dev_id + "/" + key + "/config";
        std::string cmd_topic = "qpext/" + c.mac_norm + "/cmd";
        std::string payload =
            "{\"name\":\"Airmonitor "+std::string(label)+"\","
             "\"uniq_id\":\""+dev_id+"_"+key+"\","
             "\"command_topic\":\""+cmd_topic+"\","
             "\"payload_press\":\"{\\\"action\\\":\\\"switch_tab\\\",\\\"name\\\":\\\""+
                std::string(tab_name)+"\\\"}\","
             "\"icon\":\""+std::string(icon)+"\","+dev_blk+"}";
        mqtt_publish(fd, cfg_topic, payload, true);
    };

    // One button per visible tab in MainPage.qml's PathView model.
    tab_button("show_air",      "show air data",     "airDatasView",      "mdi:weather-cloudy");
    tab_button("show_summary",  "show summary",      "summaryView",       "mdi:chart-line");
    tab_button("show_settings", "show settings",     "settingView",       "mdi:cog");
    tab_button("show_app",      "show app",          "appView",           "mdi:apps");
    tab_button("show_ha",       "show HA dashboard", "qpextView",         "mdi:home-assistant");
    tab_button("show_camera",   "show camera",       "qpextCamerasView",  "mdi:cctv");

    // Button: reboot device. uses HA "button" component.
    {
        std::string cfg_topic = "homeassistant/button/" + dev_id + "/reboot/config";
        std::string cmd_topic = "qpext/" + c.mac_norm + "/cmd";
        std::string payload =
            "{\"name\":\"Airmonitor reboot\","
             "\"uniq_id\":\""+dev_id+"_reboot\","
             "\"command_topic\":\""+cmd_topic+"\","
             "\"payload_press\":\"{\\\"action\\\":\\\"reboot\\\"}\","
             "\"icon\":\"mdi:restart\","+dev_blk+"}";
        mqtt_publish(fd, cfg_topic, payload, true);
    }
    qplog_c("[qpext-mqtt] discovery published for device id=%s", dev_id.c_str());
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

// Apply a `dashboard/set` MQTT message — merge into widgets.json, preserving
// the local `ha` block (so the HA token never has to travel over MQTT). The
// payload is a JSON object with arbitrary top-level keys (`widgets`, `events`,
// ...); each replaces the corresponding key in widgets.json.
static void handle_dashboard_set(const std::string& payload) {
    // Parse payload.
    jsmn_parser pp;
    std::vector<jsmntok_t> ptoks(512);
    int pnt;
    for (;;) {
        jsmn_init(&pp);
        pnt = jsmn_parse(&pp, payload.data(), payload.size(),
                         ptoks.data(), (int)ptoks.size());
        if (pnt == JSMN_ERROR_NOMEM) { ptoks.resize(ptoks.size() * 2); continue; }
        break;
    }
    if (pnt < 1 || ptoks[0].type != JSMN_OBJECT) {
        qplog_c("[qpext-mqtt] dashboard/set: not a JSON object (nt=%d)", pnt);
        return;
    }

    // Read current widgets.json to preserve the ha block.
    std::string cur = slurp("/data/qpext/widgets.json");
    std::string ha_slice = "{}";
    if (!cur.empty()) {
        jsmn_parser cp;
        std::vector<jsmntok_t> ctoks(512);
        int cnt;
        for (;;) {
            jsmn_init(&cp);
            cnt = jsmn_parse(&cp, cur.data(), cur.size(),
                             ctoks.data(), (int)ctoks.size());
            if (cnt == JSMN_ERROR_NOMEM) { ctoks.resize(ctoks.size() * 2); continue; }
            break;
        }
        if (cnt > 0 && ctoks[0].type == JSMN_OBJECT) {
            int ch = obj_find(cur.c_str(), ctoks.data(), cnt, 0, "ha");
            if (ch >= 0) ha_slice = cur.substr(ctoks[ch].start,
                                               ctoks[ch].end - ctoks[ch].start);
        }
    }

    // Rebuild: {"ha": <preserved>, ...all-other-keys-from-payload}
    std::string out;
    out.reserve(payload.size() + ha_slice.size() + 64);
    out = "{\n  \"ha\": " + ha_slice;
    int n_keys = ptoks[0].size;
    int i = 1;
    int extra_keys = 0;
    for (int k = 0; k < n_keys && i < pnt; ++k) {
        const jsmntok_t& key_tok = ptoks[i];
        if (i + 1 >= pnt) break;
        const jsmntok_t& val_tok = ptoks[i + 1];
        std::string key_str = payload.substr(key_tok.start,
                                             key_tok.end - key_tok.start);
        if (key_str != "ha") {
            std::string val_str = payload.substr(val_tok.start,
                                                 val_tok.end - val_tok.start);
            out += ",\n  \"" + key_str + "\": " + val_str;
            ++extra_keys;
        }
        i = skip_tok(ptoks.data(), pnt, i + 1);
    }
    out += "\n}\n";

    if (write_file_atomic("/data/qpext/widgets.json", out)) {
        qplog_c("[qpext-mqtt] dashboard/set applied: %d top-level keys, %zu bytes",
                extra_keys, out.size());
    } else {
        qplog_c("[qpext-mqtt] dashboard/set: write failed");
    }
}

// Very small JSON extract for {"action":"X","name":"Y"} payloads.
static std::string json_str(const std::string& j, const char* key) {
    std::string pat = "\""; pat += key; pat += "\"";
    size_t p = j.find(pat); if (p == std::string::npos) return "";
    p = j.find('"', p + pat.size()); if (p == std::string::npos) return "";
    size_t q = j.find('"', p + 1); if (q == std::string::npos) return "";
    return j.substr(p + 1, q - p - 1);
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
    MqttCfg cfg;
    while (!read_cfg(cfg)) sleep(2);
    qplog_c("[qpext-mqtt] cfg host=%s:%d user=%s mac=%s",
            cfg.host.c_str(), cfg.port, cfg.user.c_str(), cfg.mac.c_str());

    int backoff = 1;
    for (;;) {
        int fd = tcp_open(cfg.host.c_str(), cfg.port);
        if (fd < 0) {
            qplog_c("[qpext-mqtt] tcp connect failed: %s", strerror(errno));
            sleep(backoff); backoff = std::min(backoff * 2, 30); continue;
        }
        if (!mqtt_connect(fd, cfg.client_id, cfg.user, cfg.pass)) {
            close(fd); sleep(backoff); backoff = std::min(backoff * 2, 30); continue;
        }
        backoff = 1;
        qplog_c("[qpext-mqtt] connected, publishing discovery");
        publish_discovery(fd, cfg);
        std::string cmd_topic = "qpext/" + cfg.mac_norm + "/cmd";
        std::string dashboard_topic = "qpext/" + cfg.mac_norm + "/dashboard/set";
        mqtt_subscribe(fd, cmd_topic, 1);
        mqtt_subscribe(fd, dashboard_topic, 2);

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
                }
                // type 0x9 = SUBACK, 0xD = PINGRESP — ignore
            }

            // Telemetry every 10s.
            if (now - last_telem >= 10) {
                last_telem = now;
                char buf[64];
                std::string base = "qpext/" + cfg.mac_norm + "/";
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
            }

            // Keepalive ping every 20s.
            if (now - last_ping >= 20) {
                if (!mqtt_ping(fd)) { ok = false; break; }
                last_ping = now;
            }
        }

        close(fd);
        qplog_c("[qpext-mqtt] disconnected, reconnect in %d s", backoff);
        sleep(backoff); backoff = std::min(backoff * 2, 30);
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
