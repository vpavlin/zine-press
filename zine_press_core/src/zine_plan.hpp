#pragma once
// zine_plan.hpp - the booklet arithmetic, with no PDF library: page order, padding, sheets, print
// batches. Pure functions, tested on their own (test/plan_test.cpp).
//
// An A5 saddle-stitched booklet on A4: every A4 sheet carries four A5 pages, two on each side,
// folded in the middle. For N pages (a multiple of 4), sheet i (0-based) has
//     front: page N-2i  | page 1+2i        (left | right, 1-based)
//     back:  page 2+2i  | page N-1-2i
// so the outermost sheet carries the back cover and the cover. This is the order of impose.py, the
// script the LAN zine issue 02 was printed and sewn with (2026-11-06).
#include <algorithm>
#include <string>
#include <vector>

namespace zine {

inline constexpr double kPtPerMm = 72.0 / 25.4;
inline constexpr double kA5W = 148.0 * kPtPerMm, kA5H = 210.0 * kPtPerMm;   // 419.53 x 595.28 pt
inline constexpr double kA4W = 297.0 * kPtPerMm, kA4H = 210.0 * kPtPerMm;   // landscape

// The pages of the booklet in reading order: indices into the source (0-based), -1 = a blank page.
// A source whose page count isn't a multiple of 4 gets blanks just before the back cover (the last
// page), so the back cover stays on the outside.
inline std::vector<int> bookletPages(int n, bool padBeforeBack = true) {
    std::vector<int> out;
    if (n <= 0) return out;
    const int total = (n + 3) / 4 * 4;
    const int blanks = total - n;
    if (!padBeforeBack || n < 2) {
        for (int i = 0; i < n; i++) out.push_back(i);
        for (int i = 0; i < blanks; i++) out.push_back(-1);
        return out;
    }
    for (int i = 0; i < n - 1; i++) out.push_back(i);
    for (int i = 0; i < blanks; i++) out.push_back(-1);
    out.push_back(n - 1);
    return out;
}

struct Side { int left = -1, right = -1; };      // source page index or -1 (blank)
struct Sheet { Side front, back; };

inline std::vector<Sheet> sheets(const std::vector<int>& pages) {
    std::vector<Sheet> out;
    const int n = (int)pages.size();
    if (n == 0 || n % 4 != 0) return out;
    for (int i = 0; i < n / 4; i++) {
        Sheet s;
        s.front.left = pages[n - 1 - 2 * i]; s.front.right = pages[2 * i];
        s.back.left = pages[2 * i + 1];      s.back.right = pages[n - 2 - 2 * i];
        out.push_back(s);
    }
    return out;
}

// Printing on a one-sided printer: one file of fronts, one of backs. copies are collated (sheet 1..S
// of copy 1, then of copy 2, ...) and split into batches: a double-feed ruins every sheet after it in
// the batch, so batches stay small (2-3 copies). reverseFronts: most one-sided printers stack the
// output last sheet on top and take the top sheet first; printing the fronts last-to-first and the
// backs first-to-last makes the finished stack come out in order, ready to fold.
struct Pass { std::vector<int> fronts, backs; int firstCopy = 1, copies = 1; };   // sheet indices
inline std::vector<Pass> passes(int sheetCount, int copies, int batch, bool reverseFronts) {
    std::vector<Pass> out;
    if (sheetCount <= 0) return out;
    copies = std::max(1, copies); batch = std::max(1, batch);
    for (int c = 0; c < copies; c += batch) {
        Pass p; p.firstCopy = c + 1; p.copies = std::min(batch, copies - c);
        for (int k = 0; k < p.copies; k++)
            for (int i = 0; i < sheetCount; i++) { p.fronts.push_back(i); p.backs.push_back(i); }
        if (reverseFronts) std::reverse(p.fronts.begin(), p.fronts.end());
        out.push_back(p);
    }
    return out;
}

// What the source pages are, from their size in mm (w x h of the visible page).
struct Fit { std::string kind; double cutW = 0, cutH = 0; };   // kind: a5-bleed | a5 | other
inline Fit guessFit(double wMm, double hMm) {
    Fit f;
    const double a5w = 148, a5h = 210;
    if (std::abs(wMm - a5w) < 1.5 && std::abs(hMm - a5h) < 1.5) { f.kind = "a5"; return f; }
    if (wMm > a5w && hMm > a5h && wMm - a5w <= 30 && hMm - a5h <= 30) { f.kind = "a5-bleed"; f.cutW = a5w; f.cutH = a5h; return f; }
    f.kind = "other";
    return f;
}

} // namespace zine
