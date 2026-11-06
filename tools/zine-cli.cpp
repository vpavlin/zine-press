// zine-cli - Zine Press from the command line (tests and scripts):
//   zine-cli inspect FILE.pdf ['{options}']
//   zine-cli make FILE.pdf OUTDIR ['{options}']
#include "zine_pdf.hpp"
#include <cstdio>

int main(int argc, char** argv) {
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
    return 2;
}
