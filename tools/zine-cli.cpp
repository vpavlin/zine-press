// zine-cli - Zine Press from the command line (tests and scripts):
//   zine-cli inspect FILE.pdf ['{options}']
//   zine-cli make FILE.pdf OUTDIR ['{options}']
//   zine-cli printers | info URI | print URI FILE.pdf ['{jobopts}'] | status URI JOBID | cancel URI JOBID
#include "zine_ipp.hpp"
#include "zine_pdf.hpp"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "printers") { std::printf("%s\n", zine::ipp::localPrinters(5000).dump(2).c_str()); return 0; }
    if (argc < 3) { std::fprintf(stderr, "usage: zine-cli inspect FILE [opts] | make FILE OUTDIR [opts]\n"); return 2; }
    const std::string cmd = argv[1];
    if (cmd == "inspect") {
        auto r = zine::inspect(argv[2], zine::parseOptions(argc > 3 ? argv[3] : "{}"));
        std::printf("%s\n", r.dump(2).c_str());
        return r.value("ok", false) ? 0 : 1;
    }
    if (cmd == "make" && argc >= 4) {
        auto r = zine::make(argv[2], argv[3], zine::parseOptions(argc > 4 ? argv[4] : "{}"),
                            [](double f, const std::string& s) { std::fprintf(stderr, "[%3d%%] %s\n", (int)(f * 100), s.c_str()); });
        r.erase("guide");
        std::printf("%s\n", r.dump(2).c_str());
        return r.value("ok", false) ? 0 : 1;
    }
    namespace ipp = zine::ipp;
    if (cmd == "info") { std::printf("%s\n", ipp::printerInfo(argv[2], argc > 3, 8000).dump(2).c_str()); return 0; }
    if (cmd == "print" && argc >= 4) { std::printf("%s\n", ipp::printJob(argv[2], argv[3], zine::json::parse(argc > 4 ? argv[4] : "{}"), 60000).dump(2).c_str()); return 0; }
    if (cmd == "status" && argc >= 4) { std::printf("%s\n", ipp::jobStatus(argv[2], std::atoi(argv[3]), 8000).dump(2).c_str()); return 0; }
    if (cmd == "cancel" && argc >= 4) { std::printf("%s\n", ipp::cancelJob(argv[2], std::atoi(argv[3]), 8000).dump(2).c_str()); return 0; }
    return 2;
}
