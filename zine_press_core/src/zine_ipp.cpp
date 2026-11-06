#include "zine_ipp.hpp"

#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

// macOS has no MSG_NOSIGNAL: SO_NOSIGPIPE on the socket instead
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace zine::ipp {

namespace {

// IPP tags
constexpr int OP = 0x01, JOB = 0x02, END = 0x03, PRINTER = 0x04;
constexpr int T_INT = 0x21, T_BOOL = 0x22, T_ENUM = 0x23, T_RANGE = 0x33, T_TEXT = 0x41, T_NAME = 0x42,
              T_KEYWORD = 0x44, T_URI = 0x45, T_CHARSET = 0x47, T_LANG = 0x48, T_MIME = 0x49;

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
bool has(const std::string& s, const std::string& sub) { return lower(s).find(lower(sub)) != std::string::npos; }
bool contains(const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }

void put16(std::string& b, int v) { b.push_back((char)((v >> 8) & 0xff)); b.push_back((char)(v & 0xff)); }
void put32(std::string& b, int v) { for (int s = 24; s >= 0; s -= 8) b.push_back((char)((v >> s) & 0xff)); }
int get16(const std::string& b, size_t i) { return ((unsigned char)b[i] << 8) | (unsigned char)b[i + 1]; }
int get32(const std::string& b, size_t i) { return (int)(((uint32_t)(unsigned char)b[i] << 24) | ((uint32_t)(unsigned char)b[i + 1] << 16) | ((uint32_t)(unsigned char)b[i + 2] << 8) | (uint32_t)(unsigned char)b[i + 3]); }

std::string statusText(int s) {
    switch (s) {
    case 0x0400: return "the printer refused the request (bad request)";
    case 0x0401: return "the printer refused (forbidden)";
    case 0x0402: return "the printer asks for a user name and password - Zine Press can't log in yet";
    case 0x0403: return "this user may not do that on the printer (not authorized)";
    case 0x0404: return "the printer can't do that right now (not possible)";
    case 0x0406: return "no such printer or job on that server";
    case 0x0407: return "the job is gone from the printer's list";
    case 0x040A: return "the printer doesn't take PDF files";
    case 0x040B: return "the printer doesn't support one of the options";
    case 0x0500: return "the printer server had an internal error";
    case 0x0501: return "the printer doesn't support this request";
    case 0x0506: return "the printer isn't accepting jobs (paused or disabled on the server)";
    case 0x0507: return "the printer is busy - try again in a moment";
    default: { std::ostringstream o; o << "the printer answered 0x" << std::hex << s; return o.str(); }
    }
}

// ── sockets ──
struct Fd { int fd = -1; ~Fd() { if (fd >= 0) ::close(fd); } };

bool waitFor(int fd, short ev, int ms) {
    pollfd p{fd, ev, 0};
    int r;
    do { r = ::poll(&p, 1, ms); } while (r < 0 && errno == EINTR);
    return r > 0;
}

bool connectWithTimeout(int fd, const sockaddr* a, socklen_t len, int ms) {
#ifdef SO_NOSIGPIPE
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int r = ::connect(fd, a, len);
    if (r < 0 && errno != EINPROGRESS) return false;
    if (r < 0) {
        if (!waitFor(fd, POLLOUT, ms)) return false;
        int err = 0; socklen_t el = sizeof err;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) return false;
    }
    fcntl(fd, F_SETFL, fl);
    return true;
}

const char* cupsSocket() {
    static const char* paths[] = {"/run/cups/cups.sock", "/var/run/cups/cups.sock", "/private/var/run/cupsd", "/var/run/cupsd"};
    struct stat st;
    for (const char* p : paths) if (::stat(p, &st) == 0 && S_ISSOCK(st.st_mode)) return p;
    return nullptr;
}

bool isLocal(const std::string& h) { return h == "localhost" || h == "127.0.0.1" || h == "::1"; }

int openTo(const Target& t, int ms, std::string* err) {
    if (isLocal(t.host) && t.port == 631) {
        if (const char* s = cupsSocket()) {
            int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
            sockaddr_un a{}; a.sun_family = AF_UNIX;
            std::strncpy(a.sun_path, s, sizeof a.sun_path - 1);
            if (fd >= 0 && connectWithTimeout(fd, (sockaddr*)&a, sizeof a, ms)) return fd;
            if (fd >= 0) ::close(fd);
        }
    }
    addrinfo hints{}; hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
    addrinfo* res = nullptr;
    const std::string port = std::to_string(t.port);
    int g = getaddrinfo(t.host.c_str(), port.c_str(), &hints, &res);
    if (g != 0) { *err = "can't find the printer's host \"" + t.host + "\" (" + gai_strerror(g) + ")"; return -1; }
    int fd = -1;
    for (addrinfo* a = res; a; a = a->ai_next) {
        fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (connectWithTimeout(fd, a->ai_addr, a->ai_addrlen, ms)) break;
        ::close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) *err = "can't connect to " + t.host + ":" + port + (isLocal(t.host) ? " - is CUPS running on this computer?" : " - is the printer (or its server) on and reachable?");
    return fd;
}

bool sendAll(int fd, const char* p, size_t n, int ms) {
    while (n) {
        if (!waitFor(fd, POLLOUT, ms)) return false;
        ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR || errno == EAGAIN) continue; return false; }
        p += w; n -= (size_t)w;
    }
    return true;
}

// Read one HTTP response: status code, body (Content-Length, chunked, or until close)
bool readResponse(int fd, int ms, int* code, std::string* body, std::string* err) {
    std::string buf;
    char tmp[16384];
    auto more = [&]() -> int {
        if (!waitFor(fd, POLLIN, ms)) { *err = "the printer stopped answering (timeout)"; return -1; }
        ssize_t r = ::recv(fd, tmp, sizeof tmp, 0);
        if (r < 0) { if (errno == EINTR) return 1; *err = std::string("connection error: ") + std::strerror(errno); return -1; }
        if (r == 0) return 0;
        buf.append(tmp, (size_t)r); return 1;
    };
    size_t he;
    while ((he = buf.find("\r\n\r\n")) == std::string::npos) {
        int m = more();
        if (m <= 0) { if (m == 0) *err = "the printer closed the connection"; return false; }
    }
    const std::string head = buf.substr(0, he);
    std::string rest = buf.substr(he + 4);
    buf.swap(rest);
    if (head.size() < 12) { *err = "not an HTTP answer"; return false; }
    *code = std::atoi(head.c_str() + 9);
    std::string lh = lower(head);
    long clen = -1;
    bool chunked = lh.find("transfer-encoding: chunked") != std::string::npos;
    size_t cl = lh.find("content-length:");
    if (cl != std::string::npos) clen = std::atol(lh.c_str() + cl + 15);
    if (chunked) {
        std::string out;
        size_t pos = 0;
        while (true) {
            size_t le;
            while ((le = buf.find("\r\n", pos)) == std::string::npos) { int m = more(); if (m <= 0) { if (m == 0) *err = "the answer was cut short"; return false; } }
            long n = std::strtol(buf.c_str() + pos, nullptr, 16);
            pos = le + 2;
            if (n == 0) break;
            while (buf.size() < pos + (size_t)n + 2) { int m = more(); if (m <= 0) { if (m == 0) *err = "the answer was cut short"; return false; } }
            out.append(buf, pos, (size_t)n);
            pos += (size_t)n + 2;
        }
        *body = out;
    } else if (clen >= 0) {
        while ((long)buf.size() < clen) { int m = more(); if (m <= 0) { if (m == 0) *err = "the answer was cut short"; return false; } }
        *body = buf.substr(0, (size_t)clen);
    } else {
        while (true) { int m = more(); if (m < 0) return false; if (m == 0) break; }
        *body = buf;
    }
    return true;
}

std::string hostHeader(const Target& t) {
    std::string h = t.host.find(':') != std::string::npos ? "[" + t.host + "]" : t.host;
    if (isLocal(t.host) && t.port == 631) return "localhost";   // what CUPS expects over its socket
    return h + ":" + std::to_string(t.port);
}

bool http(const std::string& uri, const std::string& method, const std::string& path, const std::string& ctype,
          const std::string& body, const std::string& extra, int ms, int* code, std::string* out, std::string* err) {
    Target t = parseUri(uri);
    if (!t.ok) { *err = "not an ipp:// address: " + uri; return false; }
    Fd s; s.fd = openTo(t, ms, err);
    if (s.fd < 0) return false;
    std::ostringstream h;
    h << method << " " << (path.empty() ? t.path : path) << " HTTP/1.1\r\n"
      << "Host: " << hostHeader(t) << "\r\n"
      << "User-Agent: ZinePress/0.2\r\n"
      << "Connection: close\r\n";
    if (!ctype.empty()) h << "Content-Type: " << ctype << "\r\n";
    if (method == "POST") h << "Content-Length: " << (body.size() + extra.size()) << "\r\n";
    h << "\r\n";
    const std::string hs = h.str();
    if (!sendAll(s.fd, hs.data(), hs.size(), ms) || !sendAll(s.fd, body.data(), body.size(), ms) || !sendAll(s.fd, extra.data(), extra.size(), ms)) {
        *err = "couldn't send to the printer (connection dropped or timed out)"; return false;
    }
    return readResponse(s.fd, ms, code, out, err);
}

Response parse(const std::string& b) {
    Response r;
    if (b.size() < 8) { r.error = "the printer's answer is too short"; return r; }
    r.status = get16(b, 2);
    size_t i = 8;
    int groupTag = 0;
    Group* g = nullptr;
    Attr* last = nullptr;
    while (i < b.size()) {
        int tag = (unsigned char)b[i++];
        if (tag == END) { r.data = b.substr(i); break; }
        if (tag < 0x10) { r.groups.push_back({tag, {}}); g = &r.groups.back().second; groupTag = tag; last = nullptr; continue; }
        if (i + 2 > b.size()) break;
        int nl = get16(b, i); i += 2;
        if (i + nl + 2 > b.size()) break;
        std::string name = b.substr(i, nl); i += nl;
        int vl = get16(b, i); i += 2;
        if (i + vl > b.size()) break;
        std::string raw = b.substr(i, vl); i += vl;
        std::string v;
        if ((tag == T_INT || tag == T_ENUM) && vl == 4) v = std::to_string(get32(raw, 0));
        else if (tag == T_BOOL && vl == 1) v = raw[0] ? "true" : "false";
        else if (tag == T_RANGE && vl == 8) v = std::to_string(get32(raw, 0)) + "-" + std::to_string(get32(raw, 4));
        else v = raw;
        if (!g) { r.groups.push_back({0, {}}); g = &r.groups.back().second; }
        if (nl == 0 && last) last->values.push_back(v);
        else { g->push_back({name, tag, {v}}); last = &g->back(); }
        (void)groupTag;
    }
    r.ok = r.status < 0x0100;
    if (!r.ok) {
        std::string m = first(r, "status-message");
        r.error = statusText(r.status) + (m.empty() ? "" : " (" + m + ")");
    }
    return r;
}

std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s; s << f.rdbuf();
    return s.str();
}

const char* jobStateName(int s) {
    switch (s) {
    case 3: return "pending"; case 4: return "held"; case 5: return "processing"; case 6: return "stopped";
    case 7: return "canceled"; case 8: return "aborted"; case 9: return "completed"; default: return "unknown";
    }
}

// "iso_a4_210x297mm" -> "A4 (210x297mm)", "custom_209.2x296.33mm_..." -> "custom 209.2x296.33mm"
std::string mediaLabel(const std::string& k) {
    std::vector<std::string> p; std::stringstream ss(k); std::string x;
    while (std::getline(ss, x, '_')) p.push_back(x);
    if (p.size() < 3) return k;
    std::string name = p[1], size = p[2];
    if (p[0] == "custom") return "custom " + name;
    for (auto& c : name) c = (char)std::toupper((unsigned char)c);
    return name + " (" + size + ")";
}

} // namespace

// ── requests ──
Request::Request(int op, const std::string& printerUri) {
    m_b.push_back(2); m_b.push_back(0);            // IPP/2.0
    put16(m_b, op);
    static int seq = 0;
    put32(m_b, ++seq);
    group(OP);
    add(T_CHARSET, "attributes-charset", "utf-8");
    add(T_LANG, "attributes-natural-language", "en");
    if (!printerUri.empty()) add(T_URI, "printer-uri", printerUri);
}
Request& Request::group(int tag) { m_b.push_back((char)tag); return *this; }
Request& Request::add(int tag, const std::string& name, const std::string& value) { return add(tag, name, std::vector<std::string>{value}); }
Request& Request::add(int tag, const std::string& name, const std::vector<std::string>& values) {
    for (size_t k = 0; k < values.size(); k++) {
        m_b.push_back((char)tag);
        const std::string n = k == 0 ? name : "";
        put16(m_b, (int)n.size()); m_b += n;
        put16(m_b, (int)values[k].size()); m_b += values[k];
    }
    return *this;
}
Request& Request::addInt(int tag, const std::string& name, int value) {
    m_b.push_back((char)tag); put16(m_b, (int)name.size()); m_b += name; put16(m_b, 4); put32(m_b, value);
    return *this;
}
Request& Request::addBool(const std::string& name, bool value) {
    m_b.push_back((char)T_BOOL); put16(m_b, (int)name.size()); m_b += name; put16(m_b, 1); m_b.push_back(value ? 1 : 0);
    return *this;
}
Request& Request::addRanges(const std::string& name, const std::vector<std::pair<int, int>>& ranges) {
    for (size_t k = 0; k < ranges.size(); k++) {
        m_b.push_back((char)T_RANGE);
        const std::string n = k == 0 ? name : "";
        put16(m_b, (int)n.size()); m_b += n;
        put16(m_b, 8); put32(m_b, ranges[k].first); put32(m_b, ranges[k].second);
    }
    return *this;
}
std::string Request::bytes() const { return m_b + (char)END; }

Target parseUri(const std::string& uri) {
    Target t;
    std::string u = uri;
    size_t sch = u.find("://");
    if (sch == std::string::npos) return t;
    const std::string scheme = lower(u.substr(0, sch));
    if (scheme != "ipp" && scheme != "http") return t;
    t.port = scheme == "http" ? 80 : 631;
    u = u.substr(sch + 3);
    size_t slash = u.find('/');
    std::string hp = slash == std::string::npos ? u : u.substr(0, slash);
    t.path = slash == std::string::npos ? "/" : u.substr(slash);
    if (!hp.empty() && hp[0] == '[') {
        size_t rb = hp.find(']');
        if (rb == std::string::npos) return t;
        t.host = hp.substr(1, rb - 1);
        if (rb + 1 < hp.size() && hp[rb + 1] == ':') t.port = std::atoi(hp.c_str() + rb + 2);
    } else {
        size_t c = hp.rfind(':');
        if (c != std::string::npos) { t.host = hp.substr(0, c); t.port = std::atoi(hp.c_str() + c + 1); }
        else t.host = hp;
    }
    t.ok = !t.host.empty() && t.port > 0;
    return t;
}

Response call(const std::string& uri, const std::string& request, const std::string& document, int timeoutMs) {
    int code = 0; std::string body, err;
    if (!http(uri, "POST", "", "application/ipp", request, document, timeoutMs, &code, &body, &err)) { Response r; r.error = err; return r; }
    if (code == 401) { Response r; r.error = statusText(0x0402); return r; }
    if (code != 200) { Response r; r.error = "the printer server answered HTTP " + std::to_string(code); return r; }
    return parse(body);
}

std::string httpGet(const std::string& uri, const std::string& path, int timeoutMs, std::string* err) {
    int code = 0; std::string body;
    if (!http(uri, "GET", path, "", "", "", timeoutMs, &code, &body, err)) return "";
    if (code != 200) { *err = "HTTP " + std::to_string(code); return ""; }
    return body;
}

std::string first(const Response& r, const std::string& name, int groupTag) {
    for (const auto& [t, g] : r.groups) {
        if (groupTag && t != groupTag) continue;
        for (const auto& a : g) if (a.name == name && !a.values.empty()) return a.values[0];
    }
    return "";
}
std::vector<std::string> all(const Response& r, const std::string& name, int groupTag) {
    for (const auto& [t, g] : r.groups) {
        if (groupTag && t != groupTag) continue;
        for (const auto& a : g) if (a.name == name) return a.values;
    }
    return {};
}

std::string userName() {
    if (passwd* pw = getpwuid(getuid())) if (pw->pw_name && *pw->pw_name) return pw->pw_name;
    if (const char* u = std::getenv("USER")) if (*u) return u;
    return "zine-press";
}

json localPrinters(int timeoutMs) {
    const std::string server = "ipp://localhost:631/";
    Request q(0x4002, "");
    q.add(T_NAME, "requesting-user-name", userName());
    q.add(T_KEYWORD, "requested-attributes", std::vector<std::string>{"printer-name", "printer-info", "printer-make-and-model", "printer-state", "printer-is-accepting-jobs"});
    Response r = call(server, q.bytes(), "", timeoutMs);
    json list = json::array();
    if (!r.ok && r.status != 0x0406) return {{"ok", false}, {"error", r.error}, {"printers", list}};   // 0x406 = no printers
    for (const auto& [t, g] : r.groups) {
        if (t != PRINTER) continue;
        json p = {{"local", true}};
        for (const auto& a : g) {
            if (a.values.empty()) continue;
            if (a.name == "printer-name") p["name"] = a.values[0];
            else if (a.name == "printer-info") p["info"] = a.values[0];
            else if (a.name == "printer-make-and-model") p["makeModel"] = a.values[0];
            else if (a.name == "printer-state") p["state"] = std::atoi(a.values[0].c_str());
        }
        if (!p.contains("name")) continue;
        p["uri"] = "ipp://localhost/printers/" + p["name"].get<std::string>();
        list.push_back(p);
    }
    return {{"ok", true}, {"printers", list}};
}

json parsePpd(const std::string& ppd) {
    // PickOne options: name -> {label, default, choices[{key,label}]}
    json opts = json::object();
    std::istringstream in(ppd);
    std::string line, cur;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("*OpenUI *", 0) == 0) {
            std::string rest = line.substr(9);
            size_t colon = rest.find(':');
            std::string nl = rest.substr(0, colon);
            size_t sl = nl.find('/');
            cur = nl.substr(0, sl);
            opts[cur] = {{"label", sl == std::string::npos ? cur : nl.substr(sl + 1)}, {"choices", json::array()}, {"default", ""}};
            continue;
        }
        if (line.rfind("*CloseUI", 0) == 0) { cur.clear(); continue; }
        if (line.rfind("*Default", 0) == 0) {
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string name = line.substr(8, colon - 8), val = line.substr(colon + 1);
            val.erase(0, val.find_first_not_of(" \t"));
            if (opts.contains(name)) opts[name]["default"] = val;
            continue;
        }
        if (!cur.empty() && line.rfind("*" + cur + " ", 0) == 0) {
            std::string kv = line.substr(cur.size() + 2);
            size_t colon = kv.find(':');
            std::string kl = kv.substr(0, colon);
            size_t sl = kl.find('/');
            std::string key = kl.substr(0, sl), label = sl == std::string::npos ? key : kl.substr(sl + 1);
            while (!key.empty() && key.back() == ' ') key.pop_back();
            opts[cur]["choices"].push_back({{"key", key}, {"label", label}});
        }
    }
    json out = {{"options", opts}};
    // the quality option: a print-quality preset if there is one, else a resolution-ish option
    std::string q;
    for (const char* pref : {"cupsPrintQuality", "PrintQuality", "BRResolution"}) if (opts.contains(pref)) { q = pref; break; }
    if (q.empty()) for (auto& [k, v] : opts.items()) if (has(k, "quality") || has(v.value("label", ""), "quality")) { q = k; break; }
    if (q.empty()) for (auto& [k, v] : opts.items()) if (has(k, "resolution") && k != "Resolution") { q = k; break; }
    if (q.empty() && opts.contains("Resolution")) q = "Resolution";
    out["quality"] = q;
    return out;
}

json printerInfo(const std::string& uri, bool wantBorderless, int timeoutMs) {
    Target t = parseUri(uri);
    if (!t.ok) return {{"ok", false}, {"error", "not an ipp:// printer address: " + uri}};
    Request q(0x000B, uri);
    q.add(T_NAME, "requesting-user-name", userName());
    q.add(T_KEYWORD, "requested-attributes", std::vector<std::string>{
        "printer-name", "printer-info", "printer-make-and-model", "printer-state", "printer-state-reasons", "printer-state-message",
        "printer-is-accepting-jobs", "document-format-supported", "media-supported", "media-default", "print-quality-supported",
        "print-quality-default", "sides-supported", "print-scaling-supported", "copies-supported", "multiple-document-handling-supported"});
    Response r = call(uri, q.bytes(), "", timeoutMs);
    if (!r.ok) return {{"ok", false}, {"error", r.error}};
    json o = {{"ok", true}, {"uri", uri},
              {"name", first(r, "printer-info").empty() ? first(r, "printer-name") : first(r, "printer-info")},
              {"makeModel", first(r, "printer-make-and-model")},
              {"state", std::atoi(first(r, "printer-state").c_str())},
              {"stateReasons", all(r, "printer-state-reasons")},
              {"stateMessage", first(r, "printer-state-message")},
              {"accepting", first(r, "printer-is-accepting-jobs") != "false"}};
    const auto formats = all(r, "document-format-supported");
    o["pdf"] = formats.empty() || contains(formats, "application/pdf");
    o["duplex"] = contains(all(r, "sides-supported"), "two-sided-short-edge");
    o["collate"] = contains(all(r, "multiple-document-handling-supported"), "separate-documents-collated-copies");

    json sizes = json::array(), quals = json::array();
    std::string defSize, defQual, ppdDefSize, ppdDefQual;
    // a CUPS queue: its PPD names things the way the driver does
    std::string perr, ppd;
    if (t.path.rfind("/printers/", 0) == 0 || t.path.rfind("/classes/", 0) == 0) ppd = httpGet(uri, t.path + ".ppd", timeoutMs, &perr);
    const bool havePpd = ppd.rfind("*PPD-Adobe", 0) == 0;
    o["ppd"] = havePpd;
    if (havePpd) {
        json p = parsePpd(ppd);
        const json& opts = p["options"];
        if (opts.contains("PageSize")) {
            ppdDefSize = opts["PageSize"].value("default", "");
            for (const auto& c : opts["PageSize"]["choices"]) {
                const std::string k = c["key"], l = c["label"];
                const bool bl = has(k, "borderless") || has(l, "borderless") || (k.size() > 2 && k.substr(k.size() - 2) == "_B") || has(k, ".FB");
                const bool a4 = lower(k).rfind("a4", 0) == 0 || lower(l).rfind("a4", 0) == 0;
                sizes.push_back({{"attr", "PageSize"}, {"key", k}, {"label", l}, {"borderless", bl}, {"a4", a4}});
            }
        }
        const std::string qo = p.value("quality", "");
        if (!qo.empty() && opts.contains(qo)) {
            ppdDefQual = opts[qo].value("default", "");
            for (const auto& c : opts[qo]["choices"]) quals.push_back({{"attr", qo}, {"key", c["key"]}, {"label", c["label"]}});
            o["qualityLabel"] = opts[qo].value("label", qo);
        }
    }
    if (sizes.empty()) {
        ppdDefSize = first(r, "media-default");
        for (const auto& m : all(r, "media-supported"))
            sizes.push_back({{"attr", "media"}, {"key", m}, {"label", mediaLabel(m)}, {"borderless", has(m, "borderless")}, {"a4", has(m, "_a4_")}});
    }
    if (quals.empty()) {
        ppdDefQual = first(r, "print-quality-default");
        for (const auto& v : all(r, "print-quality-supported")) {
            const int n = std::atoi(v.c_str());
            quals.push_back({{"attr", "print-quality"}, {"key", v}, {"label", n == 3 ? "Draft" : n == 5 ? "High" : n == 4 ? "Normal" : v}});
        }
        o["qualityLabel"] = "Print quality";
    }
    // Zine Press's picks: A4 (borderless when asked and the printer has one), Normal quality
    for (int pass = 0; pass < 2 && defSize.empty(); pass++)
        for (const auto& s : sizes)
            if (s["a4"] && (pass == 1 || s["borderless"].get<bool>() == wantBorderless)) { defSize = s["key"]; break; }
    if (defSize.empty()) defSize = ppdDefSize;
    for (const auto& qv : quals) if (lower(qv["label"]) == "normal" || lower(qv["key"]) == "normal") { defQual = qv["key"]; break; }
    if (defQual.empty()) defQual = ppdDefQual;
    o["sizes"] = sizes; o["qualities"] = quals;
    o["defaultSize"] = defSize; o["defaultQuality"] = defQual;
    return o;
}

json printJob(const std::string& uri, const std::string& pdfPath, const json& opts, int timeoutMs) {
    const std::string doc = readFile(pdfPath);
    if (doc.empty()) return {{"ok", false}, {"error", "can't read " + pdfPath}};
    Request q(0x0002, uri);
    q.add(T_NAME, "requesting-user-name", userName());
    q.add(T_NAME, "job-name", opts.value("title", std::string("Zine Press")));
    q.add(T_MIME, "document-format", "application/pdf");
    q.group(JOB);
    const int copies = opts.value("copies", 1);
    if (copies > 1) q.addInt(T_INT, "copies", copies);
    if (copies > 1 && opts.value("collate", true)) q.add(T_KEYWORD, "multiple-document-handling", "separate-documents-collated-copies");
    const std::string sides = opts.value("sides", std::string(""));
    if (!sides.empty()) q.add(T_KEYWORD, "sides", sides);
    auto choice = [&](const char* k) {
        if (!opts.contains(k) || !opts[k].is_object()) return;
        const std::string a = opts[k].value("attr", ""), v = opts[k].value("key", "");
        if (a.empty() || v.empty()) return;
        if (a == "media") q.add(T_KEYWORD, "media", v);
        else if (a == "print-quality") q.addInt(T_ENUM, "print-quality", std::atoi(v.c_str()));
        else q.add(T_NAME, a, v);                      // a PPD option, as `lp -o Name=Value` sends it
    };
    if (opts.contains("pageRanges") && opts["pageRanges"].is_array() && !opts["pageRanges"].empty()) {
        std::vector<std::pair<int, int>> rs;
        for (const auto& r : opts["pageRanges"]) if (r.is_array() && r.size() == 2) rs.push_back({r[0].get<int>(), r[1].get<int>()});
        if (!rs.empty()) q.addRanges("page-ranges", rs);
    }
    choice("size");
    choice("quality");
    if (opts.value("fitToPage", false)) {
        if (opts.value("cups", false)) q.addBool("fit-to-page", true);
        else q.add(T_KEYWORD, "print-scaling", "fit");
    }
    Response r = call(uri, q.bytes(), doc, timeoutMs);
    if (!r.ok) return {{"ok", false}, {"error", r.error.empty() ? "printing failed" : r.error}};
    const std::string id = first(r, "job-id");
    if (id.empty()) return {{"ok", false}, {"error", "the printer took the file but gave no job number"}};
    return {{"ok", true}, {"jobId", std::atoi(id.c_str())}, {"jobUri", first(r, "job-uri")}, {"note", r.status ? "some options were ignored by the printer" : ""}};
}

json jobStatus(const std::string& uri, int jobId, int timeoutMs) {
    Request q(0x0009, uri);
    q.addInt(T_INT, "job-id", jobId);
    q.add(T_NAME, "requesting-user-name", userName());
    q.add(T_KEYWORD, "requested-attributes", std::vector<std::string>{"job-state", "job-state-reasons", "job-state-message",
        "job-impressions-completed", "job-media-sheets-completed"});
    Response r = call(uri, q.bytes(), "", timeoutMs);
    if (!r.ok) return {{"ok", false}, {"error", r.error}};
    const int st = std::atoi(first(r, "job-state").c_str());
    return {{"ok", true}, {"state", st}, {"stateName", jobStateName(st)}, {"reasons", all(r, "job-state-reasons")},
            {"message", first(r, "job-state-message")}, {"impressions", std::atoi(first(r, "job-impressions-completed").c_str())},
            {"sheets", std::atoi(first(r, "job-media-sheets-completed").c_str())}};
}

json printerState(const std::string& uri, int timeoutMs) {
    Request q(0x000B, uri);
    q.add(T_NAME, "requesting-user-name", userName());
    q.add(T_KEYWORD, "requested-attributes", std::vector<std::string>{"printer-state", "printer-state-reasons", "printer-state-message"});
    Response r = call(uri, q.bytes(), "", timeoutMs);
    if (!r.ok) return {{"ok", false}, {"error", r.error}};
    return {{"ok", true}, {"state", std::atoi(first(r, "printer-state").c_str())}, {"reasons", all(r, "printer-state-reasons")}, {"message", first(r, "printer-state-message")}};
}

json cancelJob(const std::string& uri, int jobId, int timeoutMs) {
    Request q(0x0008, uri);
    q.addInt(T_INT, "job-id", jobId);
    q.add(T_NAME, "requesting-user-name", userName());
    Response r = call(uri, q.bytes(), "", timeoutMs);
    // already finished or gone: nothing left to stop
    if (!r.ok && (r.status == 0x0407 || r.status == 0x0404 || r.status == 0x0406)) return {{"ok", true}, {"note", "the job had already finished"}};
    if (!r.ok) return {{"ok", false}, {"error", r.error}};
    return {{"ok", true}};
}

} // namespace zine::ipp
