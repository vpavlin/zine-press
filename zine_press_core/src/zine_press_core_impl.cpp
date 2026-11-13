#include "zine_press_core_impl.h"

#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <fstream>
#include <pwd.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using zine::json;

namespace {

// A FileDialog hands over a file:// URL (percent-encoded); a script passes a plain path
std::string toPath(std::string p) {
    if (p.rfind("file://", 0) == 0) {
        p = p.substr(7);
        std::string out;
        for (size_t i = 0; i < p.size(); i++) {
            if (p[i] == '%' && i + 2 < p.size() && std::isxdigit((unsigned char)p[i + 1]) && std::isxdigit((unsigned char)p[i + 2])) {
                out.push_back((char)std::stoi(p.substr(i + 1, 2), nullptr, 16)); i += 2;
            } else out.push_back(p[i]);
        }
        p = out;
    }
    return p;
}

std::string homeDir() {
    if (const char* h = std::getenv("HOME")) if (*h) return h;
    if (passwd* pw = getpwuid(getuid())) return pw->pw_dir;
    return "/tmp";
}

bool writableDir(const fs::path& d) {
    std::error_code ec;
    if (!fs::is_directory(d, ec)) return false;
    return access(d.c_str(), W_OK) == 0;
}

// Remembered printers: $XDG_CONFIG_HOME/zine-press/printers.json (~/.config/... by default)
fs::path configFile() {
    const char* x = std::getenv("XDG_CONFIG_HOME");
    fs::path base = x && *x ? fs::path(x) : fs::path(homeDir()) / ".config";
    return base / "zine-press" / "printers.json";
}
json loadConfig() {
    std::ifstream f(configFile());
    json j = json::parse(f, nullptr, false);
    if (!j.is_object()) j = json::object();
    if (!j.contains("saved") || !j["saved"].is_array()) j["saved"] = json::array();
    return j;
}
void saveConfig(const json& j) {
    std::error_code ec;
    fs::create_directories(configFile().parent_path(), ec);
    const fs::path tmp = configFile().string() + ".tmp";
    { std::ofstream f(tmp); f << j.dump(2); }
    fs::rename(tmp, configFile(), ec);
}
std::string trim(std::string s) {
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    return s;
}

// The first of these found on PATH (or the usual places)
std::string findTool(std::initializer_list<const char*> names) {
    std::string path = std::getenv("PATH") ? std::getenv("PATH") : "";
    path += ":/usr/bin:/bin:/usr/local/bin:/run/current-system/sw/bin";
    for (const char* n : names) {
        size_t a = 0;
        while (a <= path.size()) {
            size_t b = path.find(':', a);
            if (b == std::string::npos) b = path.size();
            const std::string dir = path.substr(a, b - a);
            if (!dir.empty()) { const std::string f = dir + "/" + n; if (access(f.c_str(), X_OK) == 0) return f; }
            a = b + 1;
        }
    }
    return "";
}

} // namespace

ZinePressCoreImpl::~ZinePressCoreImpl() {
    if (m_worker.joinable()) m_worker.join();
}

std::string ZinePressCoreImpl::about() {
    return json({{"name", "zine_press_core"}, {"version", "0.2.3"}, {"engine", "qpdf + PDFium + IPP"}}).dump();
}

std::string ZinePressCoreImpl::inspect(std::string path, std::string optionsJson) {
    try {
        const std::string p = toPath(path);
        if (!fs::is_regular_file(p)) return json({{"ok", false}, {"error", "no such file: " + p}}).dump();
        json r = zine::inspect(p, zine::parseOptions(optionsJson));
        r["outDir"] = json::parse(defaultOutDir(p)).value("outDir", "");
        return r.dump();
    } catch (const std::exception& e) {
        return json({{"ok", false}, {"error", e.what()}}).dump();
    }
}

// Next to the PDF ("<name> - print"), or ~/Zine Press/<name> when that folder isn't writable
std::string ZinePressCoreImpl::defaultOutDir(std::string path) {
    const fs::path p = toPath(path);
    const std::string stem = p.stem().string().empty() ? "zine" : p.stem().string();
    fs::path dir = p.parent_path();
    fs::path out = writableDir(dir) ? dir / (stem + " - print") : fs::path(homeDir()) / "Zine Press" / stem;
    return json({{"ok", true}, {"outDir", out.string()}}).dump();
}

std::string ZinePressCoreImpl::make(std::string path, std::string optionsJson) {
    try {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_job && m_job->state == "running") return json({{"ok", false}, {"error", "a booklet is being made - wait for it to finish"}}).dump();
        const std::string p = toPath(path);
        if (!fs::is_regular_file(p)) return json({{"ok", false}, {"error", "no such file: " + p}}).dump();
        json oj = json::parse(optionsJson.empty() ? "{}" : optionsJson, nullptr, false);
        std::string outDir = oj.is_object() && oj.contains("outDir") && oj["outDir"].is_string() && !oj["outDir"].get<std::string>().empty()
                             ? toPath(oj["outDir"].get<std::string>()) : json::parse(defaultOutDir(p)).value("outDir", "");
        const zine::Options o = zine::parseOptions(optionsJson);
        if (m_worker.joinable()) m_worker.join();   // the previous job has finished
        auto job = std::make_shared<Job>();
        job->id = "job-" + std::to_string(++m_seq);
        m_job = job;
        m_worker = std::thread([this, job, p, outDir, o] {
            json r = zine::make(p, outDir, o, [this, job](double f, const std::string& s) {
                std::lock_guard<std::mutex> l(m_mtx); job->fraction = f; job->step = s;
            });
            std::lock_guard<std::mutex> l(m_mtx);
            job->result = r;
            job->fraction = 1;
            job->state = r.value("ok", false) ? "done" : "error";
        });
        return json({{"ok", true}, {"jobId", job->id}, {"outDir", outDir}}).dump();
    } catch (const std::exception& e) {
        return json({{"ok", false}, {"error", e.what()}}).dump();
    }
}

std::string ZinePressCoreImpl::job(std::string jobId) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_job || m_job->id != jobId) return json({{"ok", false}, {"error", "unknown job"}}).dump();
    return json({{"ok", true}, {"jobId", m_job->id}, {"state", m_job->state}, {"fraction", m_job->fraction}, {"step", m_job->step}, {"result", m_job->result}}).dump();
}

std::string ZinePressCoreImpl::openPath(std::string path) {
    const std::string p = toPath(path);
    std::error_code ec;
    if (!fs::exists(p, ec)) return json({{"ok", false}, {"error", "no such file: " + p}}).dump();
#ifdef __APPLE__
    const std::string tool = findTool({"open"});
#else
    const std::string tool = findTool({"xdg-open", "gio", "kde-open5", "gnome-open"});
#endif
    if (tool.empty()) return json({{"ok", false}, {"error", "no program to open files with (xdg-open) - open " + p + " yourself"}}).dump();
    const bool gio = tool.size() >= 4 && tool.substr(tool.size() - 4) == "/gio";
    // double fork: the viewer is nobody's child and leaves no zombie behind
    pid_t c = fork();
    if (c < 0) return json({{"ok", false}, {"error", "couldn't start " + tool}}).dump();
    if (c == 0) {
        setsid();
        if (fork() == 0) {
            if (gio) execl(tool.c_str(), tool.c_str(), "open", p.c_str(), (char*)nullptr);
            else execl(tool.c_str(), tool.c_str(), p.c_str(), (char*)nullptr);
            _exit(127);
        }
        _exit(0);
    }
    int st = 0;
    waitpid(c, &st, 0);
    return json({{"ok", true}, {"with", tool}}).dump();
}

// ── printing ──

std::string ZinePressCoreImpl::printers() {
    json local = zine::ipp::localPrinters(4000);
    std::lock_guard<std::mutex> lk(m_cfgMtx);
    json cfg = loadConfig();
    return json({{"ok", true}, {"local", local.value("printers", json::array())}, {"localError", local.value("error", "")},
                 {"saved", cfg["saved"]}, {"last", cfg.value("last", "")}}).dump();
}

std::string ZinePressCoreImpl::addPrinter(std::string uri) {
    uri = trim(uri);
    if (uri.rfind("ipps://", 0) == 0) return json({{"ok", false}, {"error", "ipps:// (encrypted) isn't supported yet - use the ipp:// address"}}).dump();
    if (uri.find("://") == std::string::npos) uri = "ipp://" + uri;
    json info = zine::ipp::printerInfo(uri, false, 8000);
    if (!info.value("ok", false)) return info.dump();
    std::lock_guard<std::mutex> lk(m_cfgMtx);
    json cfg = loadConfig();
    json saved = json::array();
    for (const auto& p : cfg["saved"]) if (p.value("uri", "") != uri) saved.push_back(p);
    saved.push_back({{"uri", uri}, {"name", info.value("name", uri)}, {"makeModel", info.value("makeModel", "")}});
    cfg["saved"] = saved; cfg["last"] = uri;
    saveConfig(cfg);
    return info.dump();
}

std::string ZinePressCoreImpl::forgetPrinter(std::string uri) {
    std::lock_guard<std::mutex> lk(m_cfgMtx);
    json cfg = loadConfig();
    json saved = json::array();
    for (const auto& p : cfg["saved"]) if (p.value("uri", "") != uri) saved.push_back(p);
    cfg["saved"] = saved;
    if (cfg.value("last", "") == uri) cfg["last"] = "";
    saveConfig(cfg);
    return json({{"ok", true}}).dump();
}

std::string ZinePressCoreImpl::printerInfo(std::string uri) {
    json info = zine::ipp::printerInfo(trim(uri), false, 8000);
    if (info.value("ok", false)) {
        std::lock_guard<std::mutex> lk(m_cfgMtx);
        json cfg = loadConfig();
        if (cfg.value("last", "") != uri) { cfg["last"] = uri; saveConfig(cfg); }
    }
    return info.dump();
}

std::string ZinePressCoreImpl::printFile(std::string uri, std::string path, std::string jobOptionsJson) {
    const std::string p = toPath(path);
    if (!fs::is_regular_file(p)) return json({{"ok", false}, {"error", "no such file: " + p}}).dump();
    json o = json::parse(jobOptionsJson.empty() ? "{}" : jobOptionsJson, nullptr, false);
    if (!o.is_object()) return json({{"ok", false}, {"error", "bad print options"}}).dump();
    return m_prints.start(trim(uri), p, o).dump();
}

std::string ZinePressCoreImpl::printStatus(std::string printId) {
    return m_prints.status(std::atoi(printId.c_str())).dump();
}

std::string ZinePressCoreImpl::printCancel(std::string printId) {
    return m_prints.cancel(std::atoi(printId.c_str())).dump();
}
