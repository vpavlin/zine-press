#include "zine_press_core_impl.h"

#include <cstdlib>
#include <filesystem>
#include <pwd.h>
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

} // namespace

ZinePressCoreImpl::~ZinePressCoreImpl() {
    if (m_worker.joinable()) m_worker.join();
}

std::string ZinePressCoreImpl::about() {
    return json({{"name", "zine_press_core"}, {"version", "0.1.3"}, {"engine", "qpdf + PDFium"}}).dump();
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
