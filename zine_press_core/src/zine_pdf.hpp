#pragma once
// zine_pdf.hpp - Zine Press on real PDFs: inspect a PDF, make the printable booklet files.
//
// Imposition is vector (qpdf, Apache-2.0): each source page becomes a form XObject whose bounding box
// is the A5 page (the bleed and crop marks around it are clipped away), placed two up on A4 landscape
// in the order of zine_plan.hpp. "Flatten" then renders every sheet side once at 300 dpi (PDFium,
// BSD-3) and rebuilds the files from those JPEG images: a 40 MB vector zine that took minutes per
// sheet through a home printer's driver comes out a few MB and prints at the printer's own speed.
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace zine {

using json = nlohmann::json;

struct Options {
    double scale = 0.96;          // 0.96 for a printer with a margin, 1.0 for borderless
    std::string cut = "auto";     // auto | a5 | none: cut each page to a centred A5 (the bleed off)
    bool pad = true;              // blank pages before the back cover up to a multiple of 4
    bool reverseFronts = true;    // one-sided printers that stack face up: fronts last-to-first
    int copies = 1;
    int batch = 2;                // copies per printing pass (a double-feed ruins the rest of a pass)
    bool flatten = true;          // render to images (fast and small for home printers)
    int dpi = 300;
    int quality = 85;             // JPEG quality when flattening
    // Reprint only these sheet sides (0-based sheet, back?) after a double-feed: make() then writes
    // just reprint-fronts.pdf / reprint-backs.pdf, one copy, in the same order as the full passes
    std::vector<std::pair<int, bool>> reprint;
};
Options parseOptions(const std::string& optionsJson);
json optionsToJson(const Options& o);

using Progress = std::function<void(double fraction, const std::string& step)>;

// {ok, path, pages, pageMm:[w,h], fit: a5|a5-bleed|other, sizes (distinct page sizes), sheets, plan}
json inspect(const std::string& path, const Options& o = Options());
// Writes the files into outDir (created): booklet.pdf, fronts.pdf + backs.pdf (or batch-N-*),
// test-sheet.pdf, HOW-TO-PRINT.txt. With o.reprint set: only reprint-fronts.pdf / reprint-backs.pdf.
// Returns {ok, outDir, files:[{name, path, sheets, what}], ...}.
json make(const std::string& path, const std::string& outDir, const Options& o, Progress progress = nullptr);
// The printing guide for this job (also written as HOW-TO-PRINT.txt)
std::string guideText(int pages, int sheets, const Options& o, const json& files);

} // namespace zine
