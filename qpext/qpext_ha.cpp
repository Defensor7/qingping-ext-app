// qpext_ha.cpp — WebSocket client for Home Assistant.
//
// Runs in a background pthread spawned from the shim's constructor.
// Reads /data/qpext/widgets.json to extract ha.base_url + ha.token,
// then maintains a WebSocket connection:
//
//   1. HTTP/1.1 Upgrade → WebSocket (RFC 6455)
//   2. HA auth: expect {"type":"auth_required"}, send {"type":"auth", "access_token":...}
//   3. Subscribe: {"id":1, "type":"subscribe_events", "event_type":"state_changed"}
//   4. Snapshot: {"id":2, "type":"get_states"} (one-shot, fills initial cache)
//   5. Loop: parse incoming frames. On `event` / `result` with state list — update
//      the in-memory map (entity_id -> raw JSON for that entity). After every
//      update, atomically write /data/qpext/state.json so the QML side can pick
//      it up via cheap local-file polling.
//
// Reconnects with backoff on disconnect. No TLS — HA on plain http://host:8123.

#define _GNU_SOURCE
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <signal.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdint.h>

#include <string>
#include <map>
#include <vector>

#define JSMN_STATIC
#include "jsmn.h"

// Logging hook (defined in qpext.cpp).
extern "C" void qplog_c(const char* fmt, ...);

// ---------------------------------------------------------------------------
// Tiny utilities
// ---------------------------------------------------------------------------

static std::string read_file(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return {};
    std::string out;
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, (size_t)n);
    }
    close(fd);
    return out;
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

// Cheap entropy from /dev/urandom; pseudo-fallback to rand().
static void fill_random(unsigned char* out, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t got = 0;
        while (got < n) {
            ssize_t r = read(fd, out + got, n - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        close(fd);
        if (got == n) return;
    }
    for (size_t i = 0; i < n; ++i) out[i] = (unsigned char)(rand() & 0xFF);
}

static std::string b64(const unsigned char* in, size_t n) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = ((uint32_t)in[i]) << 16;
        if (i + 1 < n) v |= ((uint32_t)in[i + 1]) << 8;
        if (i + 2 < n) v |= (uint32_t)in[i + 2];
        out += tbl[(v >> 18) & 0x3F];
        out += tbl[(v >> 12) & 0x3F];
        out += (i + 1 < n) ? tbl[(v >> 6) & 0x3F] : '=';
        out += (i + 2 < n) ? tbl[v & 0x3F]        : '=';
    }
    return out;
}

// ---------------------------------------------------------------------------
// JSON helpers built on jsmn
// ---------------------------------------------------------------------------

// Returns true if token t equals literal s.
static bool jsoneq(const char* j, const jsmntok_t* t, const char* s) {
    int n = t->end - t->start;
    return (t->type == JSMN_STRING) && (int)strlen(s) == n &&
           strncmp(j + t->start, s, n) == 0;
}

// Skip a token and all its children. Returns index just past the subtree.
// `nt` is the total number of valid tokens — required to avoid reading past
// the end of the array.
static int skip_tok(const jsmntok_t* toks, int nt, int i) {
    int end = toks[i].end;
    int n = 1;
    while ((i + n) < nt && toks[i + n].start < end) ++n;
    return i + n;
}

// Find direct child object key inside object token `obj_idx`. Returns index of
// the VALUE token, or -1 if not found.
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

static std::string tok_str(const char* j, const jsmntok_t* t) {
    return std::string(j + t->start, t->end - t->start);
}

// ---------------------------------------------------------------------------
// Config: parse widgets.json -> {host, port, path, token}
// ---------------------------------------------------------------------------

struct HaConfig {
    std::string host;
    int         port = 8123;
    std::string path = "/api/websocket";
    std::string token;

    bool valid() const { return !host.empty() && !token.empty(); }
};

// Find the "ha" credentials: prefer the post-split /data/qpext/ha.json
// (top-level {base_url, token}); fall back to the legacy
// /data/qpext/widgets.json with an embedded {"ha": {...}} block, for
// devices that haven't been re-installed since the split. Returns the
// `base_url` and `token` strings as substring slices of `raw`, with
// `raw` being whichever file was actually read.
struct HaCreds { std::string base_url; std::string token; };

static bool read_ha_creds(HaCreds& out) {
    std::string raw = read_file("/data/qpext/ha.json");
    bool nested = false;
    if (raw.empty()) {
        raw = read_file("/data/qpext/widgets.json");
        if (raw.empty()) return false;
        nested = true;
    }
    jsmn_parser p; jsmn_init(&p);
    int cap = 256;
    std::vector<jsmntok_t> toks(cap);
    int nt;
    for (;;) {
        nt = jsmn_parse(&p, raw.data(), raw.size(), toks.data(), cap);
        if (nt == JSMN_ERROR_NOMEM) { cap *= 2; toks.resize(cap); jsmn_init(&p); continue; }
        break;
    }
    if (nt < 1 || toks[0].type != JSMN_OBJECT) return false;

    int root = 0;
    if (nested) {
        root = obj_find(raw.c_str(), toks.data(), nt, 0, "ha");
        if (root < 0) return false;
    }
    int uri = obj_find(raw.c_str(), toks.data(), nt, root, "base_url");
    int tok = obj_find(raw.c_str(), toks.data(), nt, root, "token");
    if (uri < 0 || tok < 0) return false;
    out.base_url = tok_str(raw.c_str(), &toks[uri]);
    out.token    = tok_str(raw.c_str(), &toks[tok]);
    return true;
}

static bool parse_config(HaConfig& cfg) {
    HaCreds creds;
    if (!read_ha_creds(creds)) return false;
    if (creds.token.empty() || creds.token.find("PUT_") == 0) return false;
    cfg.token = creds.token;

    // Parse http://host:port[/path]. We only support plain http.
    const char* prefix = "http://";
    if (creds.base_url.compare(0, 7, prefix) != 0) return false;
    std::string rest = creds.base_url.substr(7);
    size_t sl = rest.find('/');
    std::string hp = (sl == std::string::npos) ? rest : rest.substr(0, sl);
    size_t colon = hp.find(':');
    if (colon == std::string::npos) {
        cfg.host = hp;
    } else {
        cfg.host = hp.substr(0, colon);
        cfg.port = atoi(hp.c_str() + colon + 1);
        if (cfg.port <= 0) cfg.port = 8123;
    }
    return cfg.valid();
}

// ---------------------------------------------------------------------------
// WebSocket I/O (client-side, RFC 6455, no TLS)
// ---------------------------------------------------------------------------

// Wrap fd + persistent input buffer so handshake leftovers don't get lost.
struct WsConn {
    int fd = -1;
    std::string ibuf;     // bytes already read from socket but not consumed
};

static int tcp_connect(const char* host, int port) {
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

static bool write_all(int fd, const void* p, size_t n) {
    const char* b = (const char*)p;
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w <= 0) { if (errno == EINTR) continue; return false; }
        b += w; n -= (size_t)w;
    }
    return true;
}

static ssize_t read_some(int fd, void* p, size_t n) {
    for (;;) {
        ssize_t r = read(fd, p, n);
        if (r >= 0) return r;
        if (errno == EINTR) continue;
        return -1;
    }
}

// Read exactly `n` bytes from `c`, satisfying first from c.ibuf then from
// the socket. Returns false on EOF/error.
static bool ws_read_exact(WsConn& c, void* out, size_t n) {
    char* o = (char*)out;
    size_t off = 0;
    if (!c.ibuf.empty()) {
        size_t take = std::min(n, c.ibuf.size());
        memcpy(o, c.ibuf.data(), take);
        c.ibuf.erase(0, take);
        off = take;
    }
    while (off < n) {
        ssize_t r = read_some(c.fd, o + off, n - off);
        if (r <= 0) return false;
        off += (size_t)r;
    }
    return true;
}

// Perform RFC 6455 client handshake. Returns true on success.
// Any bytes read past the "\r\n\r\n" terminator are stashed in c.ibuf for the
// next ws_recv_text() — HA tends to send auth_required immediately and we
// MUST NOT drop it.
static bool ws_handshake(WsConn& c, const char* host, int port, const char* path) {
    int fd = c.fd;
    unsigned char key_raw[16];
    fill_random(key_raw, sizeof(key_raw));
    std::string key_b64 = b64(key_raw, sizeof(key_raw));

    char req[1024];
    int rn = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        path, host, port, key_b64.c_str());
    if (rn <= 0 || !write_all(fd, req, rn)) return false;

    // Read response headers (terminator: "\r\n\r\n").
    std::string resp;
    char buf[1024];
    size_t end_pos = std::string::npos;
    while ((end_pos = resp.find("\r\n\r\n")) == std::string::npos) {
        ssize_t r = read_some(fd, buf, sizeof(buf));
        if (r <= 0) return false;
        resp.append(buf, (size_t)r);
        if (resp.size() > 16384) return false;
    }
    // Naive: status 101 anywhere in first line.
    if (resp.compare(0, 12, "HTTP/1.1 101") != 0) {
        qplog_c("[qpext-ha] handshake bad response: %.80s", resp.c_str());
        return false;
    }
    // Stash any bytes that came AFTER \r\n\r\n into ibuf for ws_recv_text.
    size_t consumed = end_pos + 4;
    if (consumed < resp.size()) {
        c.ibuf.assign(resp, consumed, resp.size() - consumed);
        qplog_c("[qpext-ha] handshake left %zu bytes in ibuf", c.ibuf.size());
    }
    return true;
}

// Build and send a text frame (client → server, masked).
static bool ws_send_text(WsConn& c, const std::string& s) {
    int fd = c.fd;
    size_t n = s.size();
    unsigned char hdr[14];
    int hi = 0;
    hdr[hi++] = 0x81;  // FIN + opcode=1 (text)
    if (n < 126) {
        hdr[hi++] = (unsigned char)(0x80 | n);
    } else if (n < 65536) {
        hdr[hi++] = (unsigned char)(0x80 | 126);
        hdr[hi++] = (unsigned char)((n >> 8) & 0xFF);
        hdr[hi++] = (unsigned char)(n & 0xFF);
    } else {
        hdr[hi++] = (unsigned char)(0x80 | 127);
        for (int i = 7; i >= 0; --i) hdr[hi++] = (unsigned char)((n >> (i * 8)) & 0xFF);
    }
    unsigned char mask[4];
    fill_random(mask, 4);
    memcpy(hdr + hi, mask, 4); hi += 4;
    if (!write_all(fd, hdr, hi)) return false;
    // Mask payload.
    std::string masked(n, 0);
    for (size_t i = 0; i < n; ++i) masked[i] = (char)((unsigned char)s[i] ^ mask[i & 3]);
    return write_all(fd, masked.data(), n);
}

// Receive one text frame, defragmenting continuation frames if any.
// Returns the payload as string, or empty string on close/error (sets *closed).
static std::string ws_recv_text(WsConn& c, bool* closed) {
    *closed = false;
    int fd = c.fd;
    std::string out;

    for (;;) {
        unsigned char h[2];
        if (!ws_read_exact(c, h, 2)) { *closed = true; return {}; }
        bool fin     = (h[0] & 0x80) != 0;
        unsigned op  = (h[0] & 0x0F);
        bool masked  = (h[1] & 0x80) != 0;
        uint64_t len = (h[1] & 0x7F);
        if (len == 126) {
            unsigned char e[2];
            if (!ws_read_exact(c, e, 2)) { *closed = true; return {}; }
            len = ((uint64_t)e[0] << 8) | e[1];
        } else if (len == 127) {
            unsigned char e[8];
            if (!ws_read_exact(c, e, 8)) { *closed = true; return {}; }
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | e[i];
        }
        unsigned char mask[4] = {0,0,0,0};
        if (masked) {
            if (!ws_read_exact(c, mask, 4)) { *closed = true; return {}; }
        }
        std::string payload((size_t)len, 0);
        if (len && !ws_read_exact(c, &payload[0], (size_t)len)) { *closed = true; return {}; }
        if (masked) {
            for (size_t i = 0; i < (size_t)len; ++i)
                payload[i] = (char)((unsigned char)payload[i] ^ mask[i & 3]);
        }

        if (op == 0x8) { *closed = true; return {}; }   // close
        if (op == 0x9) {                                // ping → send pong
            unsigned char p[14]; int pi = 0;
            p[pi++] = 0x8A;
            if (len < 126) p[pi++] = (unsigned char)(0x80 | len);
            else { p[pi++] = (unsigned char)(0x80 | 126); p[pi++] = (unsigned char)((len>>8)&0xFF); p[pi++] = (unsigned char)(len&0xff); }
            unsigned char m[4]; fill_random(m, 4);
            memcpy(p+pi, m, 4); pi += 4;
            write_all(fd, p, pi);
            std::string mp((size_t)len, 0);
            for (size_t i = 0; i < (size_t)len; ++i)
                mp[i] = (char)((unsigned char)payload[i] ^ m[i & 3]);
            write_all(fd, mp.data(), (size_t)len);
            continue;
        }
        if (op == 0xA) continue;                        // pong → ignore
        if (op == 0x1 || op == 0x2 || op == 0x0) {
            out.append(payload);
            if (fin) return out;
            continue;
        }
        qplog_c("[qpext-ha] unknown opcode %u, closing", op);
        *closed = true;
        return {};
    }
}

// Defined further down (cameras section).
static void check_event_trigger(const std::string& eid, const std::string& new_state_json);

// ---------------------------------------------------------------------------
// HA state cache → /data/qpext/state.json
// ---------------------------------------------------------------------------

static pthread_mutex_t st_mu = PTHREAD_MUTEX_INITIALIZER;
static std::map<std::string, std::string> g_states;   // entity_id -> raw JSON object
static bool g_dirty = false;

static void flush_state_file() {
    pthread_mutex_lock(&st_mu);
    if (!g_dirty) { pthread_mutex_unlock(&st_mu); return; }
    std::string out = "{";
    bool first = true;
    for (auto& kv : g_states) {
        if (!first) out += ",";
        out += "\"";
        for (char c : kv.first) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        out += "\":";
        out += kv.second;
        first = false;
    }
    out += "}";
    g_dirty = false;
    pthread_mutex_unlock(&st_mu);
    write_file_atomic("/data/qpext/state.json", out);
}

// Apply one full entity object (as raw JSON substring).
static void cache_entity(const std::string& eid, std::string raw) {
    pthread_mutex_lock(&st_mu);
    g_states[eid] = std::move(raw);
    g_dirty = true;
    pthread_mutex_unlock(&st_mu);
}

// Slice the substring covered by a token (including outer braces / quotes).
static std::string tok_substr(const std::string& j, const jsmntok_t& t) {
    return j.substr(t.start, t.end - t.start);
}

// Walk one HA message and update cache.
//   - "auth_required"  -> caller handles
//   - "auth_ok"        -> caller handles
//   - {"type":"result","success":true,"result":[ states... ]}    (response to get_states)
//   - {"type":"event","event":{"event_type":"state_changed","data":{"new_state":{...}}}}
static void apply_message(const std::string& m) {
    jsmn_parser p; jsmn_init(&p);
    int cap = 8192;
    std::vector<jsmntok_t> toks(cap);
    int nt;
    for (;;) {
        nt = jsmn_parse(&p, m.data(), m.size(), toks.data(), cap);
        if (nt == JSMN_ERROR_NOMEM) { cap *= 2; toks.resize(cap); jsmn_init(&p); continue; }
        break;
    }
    if (nt < 1 || toks[0].type != JSMN_OBJECT) return;

    int ty = obj_find(m.c_str(), toks.data(), nt, 0, "type");
    if (ty < 0) return;
    std::string type = tok_str(m.c_str(), &toks[ty]);

    if (type == "result") {
        int success = obj_find(m.c_str(), toks.data(), nt, 0, "success");
        if (success < 0 || toks[success].type != JSMN_PRIMITIVE) return;
        if (m[toks[success].start] != 't') return;
        int result = obj_find(m.c_str(), toks.data(), nt, 0, "result");
        if (result < 0 || toks[result].type != JSMN_ARRAY) return;
        int n = toks[result].size;
        int i = result + 1;
        int updated = 0;
        for (int k = 0; k < n && i < nt; ++k) {
            if (toks[i].type == JSMN_OBJECT) {
                int ei = obj_find(m.c_str(), toks.data(), nt, i, "entity_id");
                if (ei > 0) {
                    cache_entity(tok_str(m.c_str(), &toks[ei]), tok_substr(m, toks[i]));
                    updated++;
                }
            }
            i = skip_tok(toks.data(), nt, i);
        }
        qplog_c("[qpext-ha] snapshot: %d entities cached", updated);
        flush_state_file();
    } else if (type == "event") {
        int ev = obj_find(m.c_str(), toks.data(), nt, 0, "event");
        if (ev < 0) return;
        int data = obj_find(m.c_str(), toks.data(), nt, ev, "data");
        if (data < 0) return;
        int ns = obj_find(m.c_str(), toks.data(), nt, data, "new_state");
        if (ns < 0 || toks[ns].type != JSMN_OBJECT) return;
        int ei = obj_find(m.c_str(), toks.data(), nt, ns, "entity_id");
        if (ei < 0) return;
        std::string eid = tok_str(m.c_str(), &toks[ei]);
        std::string ns_json = tok_substr(m, toks[ns]);
        cache_entity(eid, ns_json);
        flush_state_file();
        check_event_trigger(eid, ns_json);
        // Log first few events so we can confirm realtime flow.
        static int ev_logged = 0;
        if (ev_logged < 5) {
            qplog_c("[qpext-ha] event #%d: %s", ++ev_logged, eid.c_str());
        }
    } else if (type == "auth_required" || type == "auth_ok" || type == "auth_invalid") {
        // handled in main loop
    }
}

// ---------------------------------------------------------------------------
// Main thread
// ---------------------------------------------------------------------------

static void* ha_thread_fn(void*) {
    // Wait until widgets.json exists with a non-placeholder token.
    HaConfig cfg;
    while (!parse_config(cfg)) {
        sleep(2);
    }
    qplog_c("[qpext-ha] connecting to %s:%d (path=%s)", cfg.host.c_str(), cfg.port, cfg.path.c_str());

    int backoff = 1;
    for (;;) {
        WsConn c;
        c.fd = tcp_connect(cfg.host.c_str(), cfg.port);
        if (c.fd < 0) {
            qplog_c("[qpext-ha] tcp_connect failed: %s, retry in %d s", strerror(errno), backoff);
            sleep(backoff); backoff = backoff < 30 ? backoff * 2 : 30; continue;
        }
        if (!ws_handshake(c, cfg.host.c_str(), cfg.port, cfg.path.c_str())) {
            qplog_c("[qpext-ha] ws_handshake failed");
            close(c.fd); sleep(backoff); backoff = backoff < 30 ? backoff * 2 : 30; continue;
        }
        backoff = 1;
        qplog_c("[qpext-ha] ws connected, awaiting auth_required");

        bool authed = false;
        bool closed = false;
        int msg_count = 0;
        for (;;) {
            std::string msg = ws_recv_text(c, &closed);
            if (closed) break;
            msg_count++;

            jsmn_parser p; jsmn_init(&p);
            jsmntok_t toks[64];
            int nt = jsmn_parse(&p, msg.data(), msg.size(), toks, 64);
            std::string type;
            if (nt > 0 && toks[0].type == JSMN_OBJECT) {
                int ty = obj_find(msg.c_str(), toks, nt, 0, "type");
                if (ty >= 0) type = tok_str(msg.c_str(), &toks[ty]);
            }
            if (msg_count <= 3)
                qplog_c("[qpext-ha] msg#%d type=%s len=%zu", msg_count, type.c_str(), msg.size());

            if (type == "auth_required") {
                std::string auth = "{\"type\":\"auth\",\"access_token\":\"" + cfg.token + "\"}";
                if (!ws_send_text(c, auth)) { closed = true; break; }
                continue;
            }
            if (type == "auth_ok") {
                authed = true;
                qplog_c("[qpext-ha] auth_ok");
                ws_send_text(c, "{\"id\":1,\"type\":\"subscribe_events\",\"event_type\":\"state_changed\"}");
                ws_send_text(c, "{\"id\":2,\"type\":\"get_states\"}");
                continue;
            }
            if (type == "auth_invalid") {
                qplog_c("[qpext-ha] auth_invalid: %.200s", msg.c_str());
                closed = true; break;
            }
            if (authed) apply_message(msg);
        }

        close(c.fd);
        qplog_c("[qpext-ha] disconnected after %d msgs, retry in %d s", msg_count, backoff);
        sleep(backoff);
        backoff = backoff < 30 ? backoff * 2 : 30;
    }
    return nullptr;
}

extern "C" void qpext_start_ha_thread() {
    static bool started = false;
    if (started) return;
    started = true;
    pthread_t t;
    pthread_create(&t, nullptr, ha_thread_fn, nullptr);
    pthread_detach(t);
    qplog_c("[qpext-ha] thread spawned");
}

// ---------------------------------------------------------------------------
// Shared state for camera pause/resume + tab switch IPC
// ---------------------------------------------------------------------------

static pthread_mutex_t cam_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile time_t g_last_heartbeat = 0;        // updated by HTTP listener
struct EventTrigger { std::string entity; std::string switch_to; std::string last_marker; };
static std::vector<EventTrigger> g_triggers;        // from widgets.json "events"

// ---------------------------------------------------------------------------
// Camera pipelines (RTSP → JPEG frames via gst-launch + Rockchip MPP hw decode)
// ---------------------------------------------------------------------------
//
// For each camera in /data/qpext/cameras.json we spawn:
//   gst-launch-1.0 -q rtspsrc location=URL latency=200 ! rtph264depay
//     ! h264parse ! mppvideodec ! videoconvert ! videorate
//     ! video/x-raw,framerate=FPS/1 ! videoscale ! video/x-raw,width=WIDTH
//     ! jpegenc quality=75
//     ! multifilesink location=/tmp/qpext/cam/<name>-%d.jpg max-files=4 next-file=buffer
//
// max-files=4 + rolling indices give QML a safe-ish file to read while
// gst-launch rewrites the others. On crash we respawn with backoff.

struct CamSpec {
    std::string name;
    std::string url;
    int         fps   = 5;
    int         width = 720;
};

static std::vector<CamSpec> parse_cameras() {
    std::vector<CamSpec> out;
    std::string raw = read_file("/data/qpext/cameras.json");
    if (raw.empty()) return out;

    jsmn_parser p; jsmn_init(&p);
    int cap = 256;
    std::vector<jsmntok_t> toks(cap);
    int nt;
    for (;;) {
        nt = jsmn_parse(&p, raw.data(), raw.size(), toks.data(), cap);
        if (nt == JSMN_ERROR_NOMEM) { cap *= 2; toks.resize(cap); jsmn_init(&p); continue; }
        break;
    }
    if (nt < 1 || toks[0].type != JSMN_OBJECT) return out;

    int arr = obj_find(raw.c_str(), toks.data(), nt, 0, "cameras");
    if (arr < 0 || toks[arr].type != JSMN_ARRAY) return out;
    int n = toks[arr].size;
    int i = arr + 1;
    for (int k = 0; k < n && i < nt; ++k) {
        if (toks[i].type == JSMN_OBJECT) {
            CamSpec c;
            int ni = obj_find(raw.c_str(), toks.data(), nt, i, "name");
            int ui = obj_find(raw.c_str(), toks.data(), nt, i, "url");
            int fi = obj_find(raw.c_str(), toks.data(), nt, i, "fps");
            int wi = obj_find(raw.c_str(), toks.data(), nt, i, "width");
            if (ni > 0) c.name = tok_str(raw.c_str(), &toks[ni]);
            if (ui > 0) c.url  = tok_str(raw.c_str(), &toks[ui]);
            if (fi > 0) c.fps   = atoi(raw.c_str() + toks[fi].start);
            if (wi > 0) c.width = atoi(raw.c_str() + toks[wi].start);
            if (c.fps <= 0)   c.fps = 5;
            if (c.width <= 0) c.width = 720;
            // Reject placeholder.
            if (!c.name.empty() && !c.url.empty() && c.url.find("USER:PASS") == std::string::npos)
                out.push_back(c);
        }
        i = skip_tok(toks.data(), nt, i);
    }
    return out;
}

struct CamProc {
    CamSpec spec;
    pid_t   pid = -1;
    time_t  started_at = 0;
    bool    paused = false;
};

// Mirrored by cam_thread_fn each iteration; read by MQTT telemetry thread.
static std::vector<std::pair<std::string, bool>> g_cam_status;
static pthread_mutex_t cam_status_mu = PTHREAD_MUTEX_INITIALIZER;

// Forward decls — used by spawn_camera below.
extern "C" int prctl(int, unsigned long, unsigned long, unsigned long, unsigned long);
#ifndef PR_SET_PDEATHSIG
#define PR_SET_PDEATHSIG 1
#endif

static pid_t spawn_camera(const CamSpec& c) {
    // Build argv with owned storage so pointers stay valid through execvp.
    std::vector<std::string> args;
    args.push_back("gst-launch-1.0");
    args.push_back("-q");
    args.push_back("rtspsrc");
    args.push_back("location=" + c.url);
    args.push_back("latency=200");
    args.push_back("protocols=tcp");   // many IP cams reject RTP/UDP transport
    args.push_back("!"); args.push_back("rtph264depay");
    args.push_back("!"); args.push_back("h264parse");
    args.push_back("!"); args.push_back("mppvideodec");     // hw H.264 decode
    args.push_back("!"); args.push_back("videorate");
    args.push_back("!"); {
        char fr[64]; snprintf(fr, sizeof(fr), "video/x-raw,framerate=%d/1", c.fps);
        args.push_back(fr);
    }
    if (c.width > 0) {
        args.push_back("!"); args.push_back("videoscale");
        args.push_back("add-borders=false");
        args.push_back("!"); {
            char w[64]; snprintf(w, sizeof(w),
                "video/x-raw,width=(int)%d,pixel-aspect-ratio=(fraction)1/1", c.width);
            args.push_back(w);
        }
    }
    args.push_back("!"); args.push_back("videoconvert");    // NV12 may need I420 etc.
    args.push_back("!"); args.push_back("mppjpegenc");      // hw JPEG encode
    args.push_back("!"); args.push_back("multifilesink");
    args.push_back("location=/tmp/qpext/cam/" + c.name + "-%d.jpg");
    args.push_back("max-files=4");
    args.push_back("next-file=buffer");

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        // Die when the host process exits, so we don't leak gst-launch.
        prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);
        // Per-camera log.
        std::string log = "/tmp/qpext/cam/" + c.name + ".log";
        int fd = open(log.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        int nfd = open("/dev/null", O_RDONLY); if (nfd >= 0) { dup2(nfd, 0); close(nfd); }

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);

        execvp("gst-launch-1.0", argv.data());
        _exit(127);
    }
    return pid;
}

static void* cam_thread_fn(void*) {
    mkdir("/tmp/qpext", 0755);
    mkdir("/tmp/qpext/cam", 0755);

    std::vector<CamProc> running;
    std::string last_sig;

    for (;;) {
        // Re-read config and compute "signature" so we restart pipelines only when
        // the cameras section actually changed.
        std::string raw = read_file("/data/qpext/cameras.json");
        std::string sig = std::to_string(raw.size()) + ":" + raw.substr(0, 64);

        if (sig != last_sig) {
            last_sig = sig;
            std::vector<CamSpec> want = parse_cameras();

            // Kill all current pipelines and respawn — simplest, no diffing.
            for (auto& cp : running) {
                if (cp.pid > 0) { kill(cp.pid, SIGTERM); waitpid(cp.pid, nullptr, 0); }
            }
            running.clear();

            for (auto& c : want) {
                CamProc cp; cp.spec = c;
                cp.pid = spawn_camera(c);
                cp.started_at = time(nullptr);
                if (cp.pid > 0) {
                    qplog_c("[qpext-cam] spawn '%s' pid=%d url=%s",
                            c.name.c_str(), (int)cp.pid, c.url.c_str());
                    running.push_back(cp);
                } else {
                    qplog_c("[qpext-cam] fork failed for '%s'", c.name.c_str());
                }
            }
        }

        // Mirror current running set into g_cam_status for telemetry consumers.
        {
            pthread_mutex_lock(&cam_status_mu);
            g_cam_status.clear();
            for (auto& cp : running)
                g_cam_status.emplace_back(cp.spec.name, !cp.paused && cp.pid > 0);
            pthread_mutex_unlock(&cam_status_mu);
        }

        // Apply pause/resume based on heartbeat freshness.
        // If we've heard from the QML side within ~3s — keep pipelines running.
        // Otherwise SIGSTOP them; they wake up instantly on the next heartbeat.
        time_t now = time(nullptr);
        bool should_run = (now - g_last_heartbeat) < 3;
        for (auto& cp : running) {
            if (cp.pid <= 0) continue;
            if (should_run && cp.paused) {
                kill(cp.pid, SIGCONT);
                cp.paused = false;
                qplog_c("[qpext-cam] resume '%s' pid=%d", cp.spec.name.c_str(), (int)cp.pid);
            } else if (!should_run && !cp.paused) {
                kill(cp.pid, SIGSTOP);
                cp.paused = true;
                qplog_c("[qpext-cam] pause '%s' pid=%d (no heartbeat)",
                        cp.spec.name.c_str(), (int)cp.pid);
            }
        }

        // Reap and respawn dead children. Backoff = 3s minimum lifespan.
        for (auto& cp : running) {
            if (cp.pid <= 0) continue;
            if (cp.paused) continue;  // SIGSTOP-ed; don't waitpid block
            int status = 0;
            pid_t r = waitpid(cp.pid, &status, WNOHANG);
            if (r == cp.pid) {
                time_t lived = now - cp.started_at;
                qplog_c("[qpext-cam] pid=%d ('%s') exited (lived %lds), respawn in %ds",
                        (int)cp.pid, cp.spec.name.c_str(),
                        (long)lived, (lived < 3 ? 3 : 1));
                if (lived < 3) sleep(3);
                cp.pid = spawn_camera(cp.spec);
                cp.started_at = time(nullptr);
                cp.paused = false;
            }
        }
        sleep(1);
    }
    return nullptr;
}

// Snapshots the freshest "<name>-NNN.jpg" under /tmp/qpext/cam/ into
// "<name>.jpg" via atomic rename, so QML can read a stable filename.
// Polled 100 ms — cheap (stat of a few files per camera).
static bool copy_file(const std::string& src, const std::string& dst) {
    int in  = open(src.c_str(), O_RDONLY);
    if (in < 0) return false;
    int out = open(dst.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (out < 0) { close(in); return false; }
    char buf[8192];
    ssize_t r;
    while ((r = read(in, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(out, buf + off, r - off);
            if (w <= 0) { close(in); close(out); unlink(dst.c_str()); return false; }
            off += w;
        }
    }
    close(in); close(out);
    return true;
}

static void* cam_snapshot_thread_fn(void*) {
    std::map<std::string, time_t> last_mtime;   // per-camera
    for (;;) {
        DIR* d = opendir("/tmp/qpext/cam");
        if (!d) { sleep(1); continue; }

        // Find newest "<name>-NNN.jpg" per camera-name.
        std::map<std::string, std::pair<std::string, time_t>> latest;
        struct dirent* e;
        while ((e = readdir(d))) {
            const char* n = e->d_name;
            const char* dash = strrchr(n, '-');
            if (!dash) continue;
            const char* dot = strstr(dash, ".jpg");
            if (!dot || dot[4] != 0) continue;
            // digits between dash and .jpg
            bool digits = (dash + 1 < dot);
            for (const char* p = dash + 1; p < dot && digits; ++p)
                if (*p < '0' || *p > '9') { digits = false; break; }
            if (!digits) continue;
            std::string name(n, dash - n);

            std::string path = "/tmp/qpext/cam/" + std::string(n);
            struct stat st;
            if (stat(path.c_str(), &st) != 0) continue;
            if (st.st_size < 1000) continue;   // skip half-written
            auto it = latest.find(name);
            if (it == latest.end() || st.st_mtime > it->second.second)
                latest[name] = { path, st.st_mtime };
        }
        closedir(d);

        for (auto& kv : latest) {
            const std::string& name = kv.first;
            const std::string& path = kv.second.first;
            time_t            mtime = kv.second.second;
            if (last_mtime[name] == mtime) continue;
            last_mtime[name] = mtime;
            std::string dst = "/tmp/qpext/cam/" + name + ".jpg";
            std::string tmp = dst + ".tmp";
            if (copy_file(path, tmp)) rename(tmp.c_str(), dst.c_str());
        }
        usleep(100 * 1000);
    }
    return nullptr;
}

// Tiny HTTP listener on 127.0.0.1:8765. QML pings GET /heartbeat while the
// Cameras tab is visible; the cam_thread uses g_last_heartbeat to decide
// whether pipelines should run or sleep.
static void* http_thread_fn(void*) {
    int sfd = -1;
    for (int attempt = 0; attempt < 30; ++attempt) {
        sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd < 0) { sleep(1); continue; }
        int one = 1;
        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
        setsockopt(sfd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(8765);
        if (bind(sfd, (struct sockaddr*)&addr, sizeof(addr)) == 0) break;
        qplog_c("[qpext-http] bind attempt %d failed: %s",
                attempt + 1, strerror(errno));
        close(sfd); sfd = -1;
        sleep(2);
    }
    if (sfd < 0) { qplog_c("[qpext-http] gave up binding"); return nullptr; }
    listen(sfd, 8);
    qplog_c("[qpext-http] listening on 127.0.0.1:8765");

    for (;;) {
        int c = accept(sfd, nullptr, nullptr);
        if (c < 0) { if (errno == EINTR) continue; break; }
        char buf[512]; ssize_t n = read(c, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            if (strncmp(buf, "GET /heartbeat", 14) == 0) {
                g_last_heartbeat = time(nullptr);
            }
        }
        static const char* resp =
            "HTTP/1.0 200 OK\r\nContent-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        ssize_t w = write(c, resp, strlen(resp)); (void)w;
        close(c);
    }
    return nullptr;
}

// Watch widgets.json for "events" config. Re-read every poll cycle inside the
// snapshot thread is overkill — instead snapshot it here once per second.
static void* events_config_thread_fn(void*) {
    std::string last_sig;
    for (;;) {
        std::string raw = read_file("/data/qpext/widgets.json");
        std::string sig = std::to_string(raw.size()) + ":" + raw.substr(0, 64);
        if (sig != last_sig && !raw.empty()) {
            last_sig = sig;
            jsmn_parser p; jsmn_init(&p);
            int cap = 1024;
            std::vector<jsmntok_t> toks(cap);
            int nt;
            for (;;) {
                nt = jsmn_parse(&p, raw.data(), raw.size(), toks.data(), cap);
                if (nt == JSMN_ERROR_NOMEM) { cap *= 2; toks.resize(cap); jsmn_init(&p); continue; }
                break;
            }
            std::vector<EventTrigger> nw;
            if (nt > 0 && toks[0].type == JSMN_OBJECT) {
                int evs = obj_find(raw.c_str(), toks.data(), nt, 0, "events");
                if (evs >= 0 && toks[evs].type == JSMN_ARRAY) {
                    int n = toks[evs].size, i = evs + 1;
                    for (int k = 0; k < n && i < nt; ++k) {
                        if (toks[i].type == JSMN_OBJECT) {
                            EventTrigger t;
                            int ei = obj_find(raw.c_str(), toks.data(), nt, i, "entity");
                            int si = obj_find(raw.c_str(), toks.data(), nt, i, "switch_to");
                            if (ei > 0) t.entity    = tok_str(raw.c_str(), &toks[ei]);
                            if (si > 0) t.switch_to = tok_str(raw.c_str(), &toks[si]);
                            if (!t.entity.empty() && !t.switch_to.empty())
                                nw.push_back(t);
                        }
                        i = skip_tok(toks.data(), nt, i);
                    }
                }
            }
            pthread_mutex_lock(&cam_mu);
            // Preserve last_marker by entity name.
            for (auto& t : nw)
                for (auto& old : g_triggers)
                    if (old.entity == t.entity) { t.last_marker = old.last_marker; break; }
            g_triggers = nw;
            pthread_mutex_unlock(&cam_mu);
            qplog_c("[qpext-evt] %zu triggers loaded", g_triggers.size());
        }
        sleep(1);
    }
    return nullptr;
}

// Called from apply_message after a "state_changed" event is cached.
// If the entity matches a configured trigger and its "marker" (we use
// last_triggered, last_updated, or state as fallback) changed — write
// /tmp/qpext/tab_event for the QML side to pick up.
static void check_event_trigger(const std::string& eid, const std::string& new_state_json) {
    pthread_mutex_lock(&cam_mu);
    std::string switch_to;
    std::string* marker_slot = nullptr;
    for (auto& t : g_triggers) {
        if (t.entity == eid) { switch_to = t.switch_to; marker_slot = &t.last_marker; break; }
    }
    pthread_mutex_unlock(&cam_mu);
    if (switch_to.empty()) return;

    // Pull marker out of the cached JSON: try attributes.last_triggered, fall
    // back to last_updated, then state.
    jsmn_parser p; jsmn_init(&p);
    jsmntok_t toks[256];
    int nt = jsmn_parse(&p, new_state_json.data(), new_state_json.size(), toks, 256);
    if (nt < 1 || toks[0].type != JSMN_OBJECT) return;
    auto pick = [&](int parent, const char* key) -> std::string {
        int x = obj_find(new_state_json.c_str(), toks, nt, parent, key);
        return (x > 0) ? tok_str(new_state_json.c_str(), &toks[x]) : "";
    };
    std::string marker;
    int attrs = obj_find(new_state_json.c_str(), toks, nt, 0, "attributes");
    if (attrs > 0) marker = pick(attrs, "last_triggered");
    if (marker.empty()) marker = pick(0, "last_updated");
    if (marker.empty()) marker = pick(0, "state");
    if (marker.empty()) return;

    pthread_mutex_lock(&cam_mu);
    bool changed = (marker_slot && *marker_slot != marker);
    if (marker_slot) *marker_slot = marker;
    pthread_mutex_unlock(&cam_mu);
    if (!changed) return;

    qplog_c("[qpext-evt] %s triggered (%.40s), switching to '%s'",
            eid.c_str(), marker.c_str(), switch_to.c_str());

    // Write /tmp/qpext/tab_event atomically. QML polls this.
    char body[256];
    snprintf(body, sizeof(body),
        "{\"switch_to\":\"%s\",\"ts\":%ld}\n",
        switch_to.c_str(), (long)time(nullptr));
    std::string tmp = "/tmp/qpext/tab_event.tmp";
    std::string fin = "/tmp/qpext/tab_event";
    int fd = open(tmp.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd >= 0) {
        ssize_t w = write(fd, body, strlen(body)); (void)w;
        close(fd);
        rename(tmp.c_str(), fin.c_str());
    }
}

// Snapshot of camera process state, called from the MQTT telemetry path.
extern "C" void qpext_get_cam_status_into(std::string* out) {
    pthread_mutex_lock(&cam_status_mu);
    if (g_cam_status.empty()) { *out = "idle"; pthread_mutex_unlock(&cam_status_mu); return; }
    std::string s;
    for (auto& p : g_cam_status) {
        if (!s.empty()) s += ",";
        s += p.first + ":" + (p.second ? "running" : "paused");
    }
    pthread_mutex_unlock(&cam_status_mu);
    *out = s;
}

extern "C" void qpext_start_cam_thread() {
    static bool started = false;
    if (started) return;
    started = true;
    mkdir("/tmp/qpext", 0755);
    pthread_t t1, t2, t3, t4;
    pthread_create(&t1, nullptr, cam_thread_fn,           nullptr);
    pthread_create(&t2, nullptr, cam_snapshot_thread_fn,  nullptr);
    pthread_create(&t3, nullptr, http_thread_fn,          nullptr);
    pthread_create(&t4, nullptr, events_config_thread_fn, nullptr);
    pthread_detach(t1); pthread_detach(t2); pthread_detach(t3); pthread_detach(t4);
    qplog_c("[qpext-cam] threads spawned (launcher + snapshot + http + events)");
}
