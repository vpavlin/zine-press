#pragma once
// zine_print.hpp - print jobs in flight: each runs on its own thread (send the PDF, then follow the
// job until it ends), so the module never blocks on a printer. The view polls status(id).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "zine_ipp.hpp"

namespace zine {

using json = nlohmann::json;

class PrintTasks {
public:
    ~PrintTasks() {
        std::vector<std::thread> ts;
        { std::lock_guard<std::mutex> l(m_mtx); for (auto& [id, t] : m_tasks) t->stop = true; ts.swap(m_threads); }
        for (auto& t : ts) if (t.joinable()) t.join();
    }

    // opts: printJob's options plus "sheets" (sheet sides in the file, for "x of y") and "duplex"
    json start(const std::string& uri, const std::string& path, const json& opts) {
        auto t = std::make_shared<Task>();
        t->uri = uri; t->total = opts.value("sheets", 0); t->duplex = opts.value("duplex", false);
        std::lock_guard<std::mutex> l(m_mtx);
        t->id = ++m_seq;
        m_tasks[t->id] = t;
        m_threads.emplace_back([t, path, opts] { run(t, path, opts); });
        return {{"ok", true}, {"printId", t->id}};
    }

    json status(int id) {
        std::shared_ptr<Task> t = find(id);
        if (!t) return {{"ok", false}, {"error", "unknown print"}};
        std::lock_guard<std::mutex> l(t->mtx);
        return {{"ok", true}, {"printId", id}, {"phase", t->phase}, {"jobId", t->jobId}, {"done", t->done}, {"total", t->total},
                {"jobState", t->jobState}, {"jobMessage", t->jobMessage}, {"printerReasons", t->reasons}, {"printerMessage", t->printerMessage},
                {"error", t->error}, {"note", t->note}, {"busy", t->busy}};
    }

    // Stop now: Cancel-Job if the printer has the job, otherwise as soon as it does
    json cancel(int id) {
        std::shared_ptr<Task> t = find(id);
        if (!t) return {{"ok", false}, {"error", "unknown print"}};
        t->stop = true;
        int jobId;
        { std::lock_guard<std::mutex> l(t->mtx); jobId = t->jobId; }
        if (jobId <= 0) return {{"ok", true}, {"note", "stopping before the printer has the job"}};
        json r = ipp::cancelJob(t->uri, jobId, 8000);
        std::lock_guard<std::mutex> l(t->mtx);
        if (r.value("ok", false)) { t->phase = "canceled"; t->cancelSent = true; }
        else t->error = r.value("error", "couldn't cancel");
        return r;
    }

private:
    struct Task {
        std::mutex mtx;
        int id = 0, jobId = 0, done = 0, total = 0;
        bool duplex = false, cancelSent = false, busy = false;
        std::atomic<bool> stop{false};
        std::string uri, phase = "sending", jobState, jobMessage, printerMessage, error, note;
        std::vector<std::string> reasons;
    };

    std::shared_ptr<Task> find(int id) {
        std::lock_guard<std::mutex> l(m_mtx);
        auto it = m_tasks.find(id);
        return it == m_tasks.end() ? nullptr : it->second;
    }

    static void sleepUnlessStopped(Task& t, int ms) {
        for (int w = 0; w < ms && !t.stop; w += 100) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    static void run(std::shared_ptr<Task> t, std::string path, json opts) {
        // Never put a job behind another one: two jobs interleaving on a one-sided run spoil a copy.
        // (If the printer can't say, the print goes ahead; Print-Job reports a dead printer anyway.)
        json qj = ipp::queuedJobs(t->uri, 10000);
        if (qj.value("ok", false) && qj.value("count", 0) > 0) {
            const int n = qj["count"];
            std::lock_guard<std::mutex> l(t->mtx);
            t->phase = "error"; t->busy = true;
            t->error = "The printer already has " + std::string(n == 1 ? "a job" : std::to_string(n) + " jobs")
                     + " in its queue. Wait for it to finish, or cancel it, then try again.";
            return;
        }
        if (t->stop) { std::lock_guard<std::mutex> l(t->mtx); t->phase = "canceled"; return; }
        json r = ipp::printJob(t->uri, path, opts, 120000);
        {
            std::lock_guard<std::mutex> l(t->mtx);
            if (!r.value("ok", false)) { t->phase = "error"; t->error = r.value("error", "printing failed"); return; }
            t->jobId = r["jobId"]; t->phase = "queued"; t->note = r.value("note", "");
        }
        if (t->stop) {                                   // Stop was pressed while the file was going over
            json c = ipp::cancelJob(t->uri, t->jobId, 8000);
            std::lock_guard<std::mutex> l(t->mtx);
            t->phase = c.value("ok", false) ? "canceled" : "error";
            if (!c.value("ok", false)) t->error = c.value("error", "couldn't cancel");
            return;
        }
        int failures = 0;
        while (true) {
            json s = ipp::jobStatus(t->uri, t->jobId, 10000);
            json p = ipp::printerState(t->uri, 10000);
            {
                std::lock_guard<std::mutex> l(t->mtx);
                if (s.value("ok", false)) {
                    failures = 0;
                    const int st = s["state"];
                    t->jobState = s.value("stateName", "");
                    t->jobMessage = s.value("message", "");
                    int imp = s.value("impressions", 0), sh = s.value("sheets", 0);
                    t->done = sh > 0 ? sh : (t->duplex ? (imp + 1) / 2 : imp);
                    if (!t->cancelSent) t->phase = st == 9 ? "completed" : st == 7 ? "canceled" : st == 8 ? "aborted" : st == 6 ? "stopped" : st >= 5 ? "printing" : "queued";
                    if (st >= 7) { if (st == 9 && t->total) t->done = t->total; }
                    if (p.value("ok", false)) { t->reasons = p["reasons"].get<std::vector<std::string>>(); t->printerMessage = p.value("message", ""); }
                    if (st >= 7) return;
                } else if (++failures >= 5) {
                    t->phase = "lost"; t->error = s.value("error", "lost touch with the printer"); return;
                }
            }
            sleepUnlessStopped(*t, 1500);
            if (t->stop) {
                std::lock_guard<std::mutex> l(t->mtx);
                if (t->cancelSent) return;               // cancel() did it; one more status read would only race it
            }
        }
    }

    std::mutex m_mtx;
    std::map<int, std::shared_ptr<Task>> m_tasks;
    std::vector<std::thread> m_threads;
    int m_seq = 0;
};

} // namespace zine
