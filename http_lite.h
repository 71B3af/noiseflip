// http_lite.h
//
// The platform-free parts of the UI's tiny local web server: reading a request, picking a content type, refusing
// paths that leave the ui folder, JSON text, and turning the browser's "section|key|value" lines into ini text.
// No Windows code here, so it is unit tested.
#pragma once
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace skate {
namespace http {

struct Request {
    std::string method, path, query, body;
    std::map<std::string, std::string> headers;   // names in lower case
    std::string header(const std::string& name) const {
        auto it = headers.find(name);
        return it == headers.end() ? std::string() : it->second;
    }
};

inline std::string lower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
inline std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// "GET /a/b?x=1 HTTP/1.1\r\nHost: ...\r\n..." (the head only, up to the blank line). False when it is not a request.
inline bool parse_head(const std::string& head, Request& out) {
    out = Request();
    const auto eol = head.find("\r\n");
    const std::string line = head.substr(0, eol);
    const auto sp1 = line.find(' ');
    const auto sp2 = line.rfind(' ');
    if (sp1 == std::string::npos || sp2 == sp1) return false;
    out.method = line.substr(0, sp1);
    std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const auto q = target.find('?');
    if (q != std::string::npos) { out.query = target.substr(q + 1); target.erase(q); }
    out.path = target;
    if (out.method != "GET" && out.method != "POST") return false;
    if (out.path.empty() || out.path[0] != '/') return false;
    std::size_t pos = eol == std::string::npos ? head.size() : eol + 2;
    while (pos < head.size()) {
        auto next = head.find("\r\n", pos);
        if (next == std::string::npos) next = head.size();
        const std::string h = head.substr(pos, next - pos);
        const auto colon = h.find(':');
        if (colon != std::string::npos) out.headers[lower(trim(h.substr(0, colon)))] = trim(h.substr(colon + 1));
        pos = next + 2;
    }
    return true;
}

// The value of one query parameter ("since=12"), or "".
inline std::string query_param(const std::string& query, const std::string& name) {
    std::size_t pos = 0;
    while (pos <= query.size()) {
        auto amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        const std::string part = query.substr(pos, amp - pos);
        const auto eq = part.find('=');
        if (eq != std::string::npos && part.substr(0, eq) == name) return part.substr(eq + 1);
        pos = amp + 1;
    }
    return "";
}

// A path under the ui folder: letters, digits and . _ - / only, no "..", no empty part, nothing that starts with a dot.
inline bool safe_path(const std::string& p) {
    if (p.empty() || p[0] != '/' || p.size() > 200) return false;
    std::string part;
    for (std::size_t i = 1; i <= p.size(); ++i) {
        if (i == p.size() || p[i] == '/') {
            if (i != p.size() && part.empty()) return false;          // "//"
            if (!part.empty() && part[0] == '.') return false;          // ".", ".." and hidden files
            part.clear();
            continue;
        }
        const unsigned char c = static_cast<unsigned char>(p[i]);
        if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
        part += static_cast<char>(c);
    }
    return true;
}

inline const char* mime_for(const std::string& path) {
    const auto dot = path.find_last_of('.');
    const std::string ext = dot == std::string::npos ? "" : lower(path.substr(dot + 1));
    if (ext == "html") return "text/html; charset=utf-8";
    if (ext == "css") return "text/css; charset=utf-8";
    if (ext == "js") return "text/javascript; charset=utf-8";
    if (ext == "json") return "application/json; charset=utf-8";
    if (ext == "png") return "image/png";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "ico") return "image/x-icon";
    if (ext == "ttf") return "font/ttf";
    if (ext == "woff") return "font/woff";
    if (ext == "woff2") return "font/woff2";
    return "application/octet-stream";
}

// The Host header must be this machine on our port: a web page on another site cannot reach the UI by DNS tricks.
inline bool host_ok(const std::string& host, int port) {
    const std::string h = lower(host), p = ":" + std::to_string(port);
    return h == "127.0.0.1" + p || h == "localhost" + p;
}

inline std::string json_escape(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += static_cast<char>(c);
        }
    }
    return o + "\"";
}

struct Setting { std::string section, key, value; };

inline bool name_ok(const std::string& s) {
    if (s.empty() || s.size() > 40) return false;
    for (char c : s) if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_')) return false;
    return true;
}

// The body of /api/set: one "section|key|value" per line. False (with a reason) for anything else.
inline bool parse_settings(const std::string& body, std::vector<Setting>& out, std::string& why) {
    out.clear();
    std::size_t pos = 0;
    while (pos < body.size()) {
        auto nl = body.find('\n', pos);
        if (nl == std::string::npos) nl = body.size();
        std::string line = body.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trim(line).empty()) continue;
        const auto a = line.find('|');
        const auto b = a == std::string::npos ? a : line.find('|', a + 1);
        if (b == std::string::npos) { why = "a setting is section|key|value"; return false; }
        Setting s{line.substr(0, a), line.substr(a + 1, b - a - 1), trim(line.substr(b + 1))};
        if (!name_ok(s.section) || !name_ok(s.key)) { why = "bad section or key name"; return false; }
        if (s.value.size() > 200) { why = "a value is at most 200 characters"; return false; }
        for (unsigned char c : s.value) if (c < 0x20 || c == 0x7f) { why = "a value has a control character"; return false; }
        if (s.value.find(';') != std::string::npos || s.value.find('#') != std::string::npos) { why = "a value cannot hold ; or #"; return false; }
        out.push_back(s);
        if (out.size() > 200) { why = "too many settings at once"; return false; }
    }
    if (out.empty()) { why = "nothing to set"; return false; }
    return true;
}

// Ini text for parse_ini, grouped by section in the order given.
inline std::string settings_to_ini(const std::vector<Setting>& v) {
    std::string out, current;
    std::vector<std::string> seen;
    for (const auto& s : v) {
        bool known = false;
        for (const auto& x : seen) if (x == s.section) known = true;
        if (!known) seen.push_back(s.section);
    }
    for (const auto& sec : seen) {
        out += "[" + sec + "]\n";
        for (const auto& s : v) if (s.section == sec) out += s.key + " = " + s.value + "\n";
    }
    return out;
}

// A preset's name is also its file name (presets/<name>.ini): plain ASCII, nothing Windows treats specially.
inline bool preset_name_ok(const std::string& n) {
    if (n.empty() || n.size() > 48) return false;
    if (n.front() == ' ' || n.back() == ' ' || n.back() == '.' || n.front() == '.' || n.front() == '_') return false;
    for (unsigned char c : n)
        if (!(std::isalnum(c) || c == ' ' || c == '_' || c == '-' || c == '.' || c == '(' || c == ')')) return false;
    std::string stem = lower(n.substr(0, n.find('.')));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    static const char* reserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
                                     "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    for (const char* r : reserved) if (stem == r) return false;
    return true;
}

inline std::string status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        default: return "Error";
    }
}

inline std::string response(int code, const std::string& type, const std::string& body, bool cache = false) {
    std::string h = "HTTP/1.1 " + std::to_string(code) + " " + status_text(code) + "\r\n";
    h += "Content-Type: " + type + "\r\n";
    h += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    h += cache ? "Cache-Control: max-age=86400\r\n" : "Cache-Control: no-store\r\n";
    h += "X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n";
    return h + body;
}

}  // namespace http
}  // namespace skate
