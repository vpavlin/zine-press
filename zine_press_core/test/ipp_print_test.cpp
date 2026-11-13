// Zine Press print defaults vs a fake DCP-T230 queue (fakeipp.py on :8632)
// Build: g++ -std=c++17 -I<nlohmann_json>/include -I../src ipp_print_test.cpp ../src/zine_ipp.cpp -pthread
// Run against fakeipp.py (same folder): ./a.out FILE.pdf <folder of fakeipp.py>
#include <cstdio>
#include <fstream>
#include <thread>
#include "zine_ipp.hpp"
#include "zine_print.hpp"
using namespace zine;
static int fails = 0;
#define CHECK(c, l) do { if (c) std::printf("  ok   %s\n", l); else { std::printf("  FAIL %s\n", l); fails++; } } while (0)
static json waitDone(PrintTasks& p, int id) {
    json s;
    for (int i = 0; i < 100; i++) { s = p.status(id); std::string ph = s.value("phase", ""); if (ph != "sending" && ph != "queued" && ph != "printing") break; std::this_thread::sleep_for(std::chrono::milliseconds(200)); }
    return s;
}
int main(int argc, char** argv) {
    const std::string uri = "ipp://127.0.0.1:8632/printers/DCPT230", pdf = argv[1], dir = argv[2];
    json info = ipp::printerInfo(uri, false, 5000);
    CHECK(info.value("ok", false), "printerInfo answers");
    CHECK(info.value("defaultSize", "") == "A4", ("default paper is plain A4 (" + info.value("defaultSize", "") + ")").c_str());
    std::string bl;
    for (auto& s : info["sizes"]) if (s["key"] == "BrA4_B") bl = s["label"];
    CHECK(bl.find("slow, heavy on ink") != std::string::npos, ("borderless A4 is labelled honestly (" + bl + ")").c_str());
    CHECK(info.value("defaultQuality", "") == "Normal", "quality Normal");
    CHECK(info.contains("mediaType") && info["mediaType"].value("attr", "") == "BRMediaType" && info["mediaType"].value("key", "") == "Plain", "media type BRMediaType=Plain");

    // a print with the view's options: plain A4, Normal, Plain, actual size
    { std::ofstream q(dir + "/QUEUE"); q << 0; }
    std::remove((dir + "/requests.jsonl").c_str());
    PrintTasks p;
    json o = {{"title", "t"}, {"cups", true}, {"fitToPage", false}, {"sheets", 6},
              {"size", {{"attr", "PageSize"}, {"key", info["defaultSize"]}}}, {"quality", {{"attr", "BRResolution"}, {"key", "Normal"}}},
              {"mediaType", info["mediaType"]}, {"pageRanges", json::array({json::array({6, 6})})}};
    json st = waitDone(p, p.start(uri, pdf, o)["printId"]);
    CHECK(st.value("phase", "") == "completed", ("an idle printer prints (" + st.value("phase", "") + " " + st.value("error", "") + ")").c_str());
    std::ifstream lg(dir + "/requests.jsonl"); std::string line; json job;
    while (std::getline(lg, line)) { json r = json::parse(line); if (r["op"] == "0x2") job = r; }
    json ja = json::object();
    for (auto& g : job["groups"]) if (g["group"] == 2) ja = g["attrs"];
    auto v = [&](const char* k) { return ja.contains(k) ? ja[k][0].dump() : std::string("-"); };
    std::printf("  sent: PageSize=%s BRResolution=%s BRMediaType=%s print-scaling=%s fit-to-page=%s page-ranges=%s\n",
                v("PageSize").c_str(), v("BRResolution").c_str(), v("BRMediaType").c_str(), v("print-scaling").c_str(), v("fit-to-page").c_str(), v("page-ranges").c_str());
    CHECK(v("PageSize") == "\"A4\"" && v("BRResolution") == "\"Normal\"" && v("BRMediaType") == "\"Plain\"", "job: PageSize=A4 BRResolution=Normal BRMediaType=Plain");
    CHECK(v("print-scaling") == "\"none\"" && v("fit-to-page") == "-", "job: actual size (print-scaling=none, no fit-to-page)");
    CHECK(v("page-ranges") == "[6,6]", "job: one page (a single sheet)");

    // the queue already has a job: refuse, send nothing
    { std::ofstream q(dir + "/QUEUE"); q << 1; }
    std::remove((dir + "/requests.jsonl").c_str());
    st = waitDone(p, p.start(uri, pdf, o)["printId"]);
    CHECK(st.value("phase", "") == "error" && st.value("busy", false), ("a busy queue is refused (" + st.value("error", "") + ")").c_str());
    std::ifstream lg2(dir + "/requests.jsonl"); bool sent = false;
    while (std::getline(lg2, line)) if (json::parse(line)["op"] == "0x2") sent = true;
    CHECK(!sent, "...and no Print-Job was sent");
    { std::ofstream q(dir + "/QUEUE"); q << 0; }
    std::printf(fails ? "IPP PRINT TEST FAILED\n" : "IPP PRINT TEST GREEN\n");
    return fails ? 1 : 0;
}
