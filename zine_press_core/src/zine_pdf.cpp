// zine_pdf.cpp - see zine_pdf.hpp.
#include "zine_pdf.hpp"
#include "zine_plan.hpp"

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>
#if __has_include(<fpdfview.h>)
#include <fpdfview.h>
#else
#include <public/fpdfview.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <jpeglib.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <map>
#include <mutex>
#include <sstream>

namespace zine {

namespace {

constexpr double kMmPerPt = 25.4 / 72.0;

std::string fmtMm(double v) { std::ostringstream s; s.precision(1); s << std::fixed << v; return s.str(); }

QPDFObjectHandle rectArray(const QPDFObjectHandle::Rectangle& r) {
    return QPDFObjectHandle::newArray(r);
}

// The visible page (CropBox, else MediaBox) and its rotation
struct PageGeom { QPDFObjectHandle::Rectangle vis; int rot = 0; double wMm = 0, hMm = 0; };
PageGeom geomOf(QPDFPageObjectHelper& ph) {
    PageGeom g;
    QPDFObjectHandle cb = ph.getCropBox(false, false);
    g.vis = (cb.isRectangle() ? cb : ph.getMediaBox(false)).getArrayAsRectangle();
    QPDFObjectHandle r = ph.getAttribute("/Rotate", false);
    if (r.isInteger()) g.rot = ((int)(r.getIntValue() % 360) + 360) % 360;
    double w = (g.vis.urx - g.vis.llx) * kMmPerPt, h = (g.vis.ury - g.vis.lly) * kMmPerPt;
    if (g.rot == 90 || g.rot == 270) std::swap(w, h);
    g.wMm = w; g.hMm = h;
    return g;
}

// The part of the page that becomes the A5 page, in the page's own (unrotated) coordinates
QPDFObjectHandle::Rectangle contentBox(QPDFPageObjectHelper& ph, const Options& o, std::string* how = nullptr) {
    PageGeom g = geomOf(ph);
    auto centredA5 = [&]() {
        double w = kA5W, h = kA5H;
        if (g.rot == 90 || g.rot == 270) std::swap(w, h);
        const double cx = (g.vis.llx + g.vis.urx) / 2, cy = (g.vis.lly + g.vis.ury) / 2;
        return QPDFObjectHandle::Rectangle(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2);
    };
    if (o.cut == "none") { if (how) *how = "whole page"; return g.vis; }
    if (o.cut == "a5") { if (how) *how = "centred A5"; return centredA5(); }
    // auto: a TrimBox that differs from the visible page is the author's own cut
    QPDFObjectHandle tb = ph.getObjectHandle().getKey("/TrimBox");
    if (tb.isRectangle()) {
        auto t = tb.getArrayAsRectangle();
        if (std::fabs(t.llx - g.vis.llx) > 1 || std::fabs(t.lly - g.vis.lly) > 1 || std::fabs(t.urx - g.vis.urx) > 1 || std::fabs(t.ury - g.vis.ury) > 1) {
            if (how) *how = "the PDF's trim box";
            return t;
        }
    }
    Fit f = guessFit(g.wMm, g.hMm);
    if (f.kind == "a5-bleed") { if (how) *how = o.scale >= 0.999 ? "centred A5 (bleed kept past the paper edge, crop marks off the page)" : "centred A5 (bleed and crop marks cut off)"; return centredA5(); }
    if (how) *how = f.kind == "a5" ? "whole page (A5)" : "whole page, scaled to fit A5";
    return g.vis;
}

// One source PDF, its pages as form XObjects clipped to their A5 box
struct Source {
    QPDF pdf;
    std::vector<QPDFPageObjectHelper> pages;
    std::vector<QPDFObjectHandle> forms;   // lazily made
    std::vector<QPDFObjectHandle::Rectangle> trims, viss;
    std::vector<bool> rotated;
    Options o;
    explicit Source(const std::string& path, const Options& opt) : o(opt) {
        pdf.processFile(path.c_str());
        QPDFPageDocumentHelper dh(pdf);
        dh.pushInheritedAttributesToPage();
        pages = dh.getAllPages();
        forms.resize(pages.size());
        trims.resize(pages.size());
        viss.resize(pages.size());
        rotated.resize(pages.size());
    }
    QPDFObjectHandle form(int i) {
        if (!forms[i].isInitialized() || forms[i].isNull()) {
            auto box = contentBox(pages[i], o);
            PageGeom g = geomOf(pages[i]);
            trims[i] = box; viss[i] = g.vis; rotated[i] = g.rot != 0;
            // getFormXObjectForPage uses the TrimBox for the rotation matrix; the BBox clips the rest
            pages[i].getObjectHandle().replaceKey("/TrimBox", rectArray(box));
            QPDFObjectHandle fo = pages[i].getFormXObjectForPage(true);
            fo.getDict().replaceKey("/BBox", rectArray(box));
            forms[i] = fo;
        }
        return forms[i];
    }
    // What a page shows in its slot: the A5 trim, plus the page's bleed on the OUTER edges (top,
    // bottom, and the sheet's outside edge) - never at the spine, where it would cover the facing
    // page. Borderless printing then ends in the bleed instead of a white line, and the paper's edge
    // cuts the bleed and crop marks off. Files for a printer with margins (scale < 1) and rotated
    // pages keep the plain trim.
    QPDFObjectHandle::Rectangle shown(int i, int slot) {
        const auto& t = trims[i]; const auto& v = viss[i];
        // only for borderless files: at 96% the bleed and its crop marks would land on the paper
        if (rotated[i] || o.scale < 0.999) return t;
        if (std::fabs(t.llx - v.llx) < 0.5 && std::fabs(t.lly - v.lly) < 0.5 && std::fabs(t.urx - v.urx) < 0.5 && std::fabs(t.ury - v.ury) < 0.5) return t;
        return QPDFObjectHandle::Rectangle(slot == 0 ? std::min(v.llx, t.llx) : t.llx, std::min(v.lly, t.lly),
                                           slot == 1 ? std::max(v.urx, t.urx) : t.urx, std::max(v.ury, t.ury));
    }
};

QPDFObjectHandle newSheetPage(QPDF& out) {
    QPDFObjectHandle page = QPDFObjectHandle::newDictionary();
    page.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
    page.replaceKey("/MediaBox", rectArray(QPDFObjectHandle::Rectangle(0, 0, kA4W, kA4H)));
    QPDFObjectHandle res = QPDFObjectHandle::newDictionary();
    res.replaceKey("/XObject", QPDFObjectHandle::newDictionary());
    page.replaceKey("/Resources", res);
    return out.makeIndirectObject(page);
}

QPDFObjectHandle::Rectangle slotRect(int slot, double scale) {
    const double half = kA4W / 2, w = kA5W * scale, h = kA5H * scale;
    const double x = slot * half + (half - w) / 2, y = (kA4H - h) / 2;
    return QPDFObjectHandle::Rectangle(x, y, x + w, y + h);
}

// One sheet side, vector: the two pages placed from the source
void addVectorSide(QPDF& out, Source& src, const Side& side) {
    QPDFObjectHandle page = newSheetPage(out);
    QPDFPageObjectHelper ph(page);
    std::string content;
    const int slots[2] = {side.left, side.right};
    for (int s = 0; s < 2; s++) {
        if (slots[s] < 0) continue;   // a blank page
        QPDFObjectHandle fo = out.copyForeignObject(src.form(slots[s]));
        const std::string name = "/P" + std::to_string(s);
        page.getKey("/Resources").getKey("/XObject").replaceKey(name, fo);
        // place by the trim (the A5 page fills its slot), then let it show the outer bleed
        fo.getDict().replaceKey("/BBox", rectArray(src.trims[slots[s]]));
        const std::string placed = ph.placeFormXObject(fo, name, slotRect(s, src.o.scale), true, true, true);
        fo.getDict().replaceKey("/BBox", rectArray(src.shown(slots[s], s)));
        content += "q\n" + placed + "Q\n";
    }
    page.replaceKey("/Contents", QPDFObjectHandle::newStream(&out, content));
    QPDFPageDocumentHelper(out).addPage(ph, false);
}

// One sheet side as a JPEG image covering the A4 page. cache: one image object per side in a file,
// so whole copies in a batch don't store each image again.
void addImageSide(QPDF& out, const std::string& jpeg, int w, int h, std::map<const std::string*, QPDFObjectHandle>& cache) {
    QPDFObjectHandle page = newSheetPage(out);
    auto hit = cache.find(&jpeg);
    if (hit != cache.end()) {
        page.getKey("/Resources").getKey("/XObject").replaceKey("/Im0", hit->second);
        std::ostringstream c; c.precision(4); c << std::fixed << "q " << kA4W << " 0 0 " << kA4H << " 0 0 cm /Im0 Do Q\n";
        page.replaceKey("/Contents", QPDFObjectHandle::newStream(&out, c.str()));
        QPDFPageDocumentHelper(out).addPage(QPDFPageObjectHelper(page), false);
        return;
    }
    QPDFObjectHandle img = QPDFObjectHandle::newStream(&out);
    QPDFObjectHandle d = img.getDict();
    d.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
    d.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
    d.replaceKey("/Width", QPDFObjectHandle::newInteger(w));
    d.replaceKey("/Height", QPDFObjectHandle::newInteger(h));
    d.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceRGB"));
    d.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
    img.replaceStreamData(jpeg, QPDFObjectHandle::newName("/DCTDecode"), QPDFObjectHandle::newNull());
    cache[&jpeg] = img;
    page.getKey("/Resources").getKey("/XObject").replaceKey("/Im0", img);
    std::ostringstream c; c.precision(4); c << std::fixed << "q " << kA4W << " 0 0 " << kA4H << " 0 0 cm /Im0 Do Q\n";
    page.replaceKey("/Contents", QPDFObjectHandle::newStream(&out, c.str()));
    QPDFPageDocumentHelper(out).addPage(QPDFPageObjectHelper(page), false);
}

void writePdf(QPDF& out, const std::string& path) {
    QPDFWriter w(out, path.c_str());
    w.setObjectStreamMode(qpdf_o_generate);
    w.setCompressStreams(true);
    w.write();
}

// PDFium is process-wide and not thread-safe: one lock around every use
std::mutex& pdfiumLock() { static std::mutex m; return m; }
void pdfiumInit() {
    static bool done = false;
    if (done) return;
    FPDF_LIBRARY_CONFIG cfg{};
    cfg.version = 2;
    FPDF_InitLibraryWithConfig(&cfg);
    done = true;
}

// libjpeg directly (qpdf's Pl_DCT compress config isn't in qpdf 11, the version the Basecamp builder pins)
std::string jpegOf(const std::vector<unsigned char>& rgb, int w, int h, int quality) {
    jpeg_compress_struct c;
    jpeg_error_mgr err;
    c.err = jpeg_std_error(&err);
    jpeg_create_compress(&c);
    unsigned char* mem = nullptr;
    unsigned long memLen = 0;
    jpeg_mem_dest(&c, &mem, &memLen);
    c.image_width = (JDIMENSION)w; c.image_height = (JDIMENSION)h;
    c.input_components = 3; c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, quality, TRUE);
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < c.image_height) {
        JSAMPROW row = const_cast<JSAMPROW>(rgb.data() + (size_t)c.next_scanline * w * 3);
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    std::string out(reinterpret_cast<char*>(mem), memLen);
    jpeg_destroy_compress(&c);
    free(mem);
    return out;
}

} // namespace

Options parseOptions(const std::string& text) {
    Options o;
    json j = json::parse(text.empty() ? "{}" : text, nullptr, false);
    if (!j.is_object()) return o;
    auto num = [&](const char* k, double lo, double hi, double def) { return j.contains(k) && j[k].is_number() ? std::clamp(j[k].get<double>(), lo, hi) : def; };
    auto flag = [&](const char* k, bool def) { return j.contains(k) && j[k].is_boolean() ? j[k].get<bool>() : def; };
    o.scale = num("scale", 0.5, 1.0, o.scale);
    if (j.contains("cut") && j["cut"].is_string()) { std::string c = j["cut"]; if (c == "auto" || c == "a5" || c == "none") o.cut = c; }
    o.pad = flag("pad", o.pad);
    o.reverseFronts = flag("reverseFronts", o.reverseFronts);
    o.copies = (int)num("copies", 1, 50, o.copies);
    o.batch = (int)num("batch", 1, 10, o.batch);
    o.flatten = flag("flatten", o.flatten);
    o.dpi = (int)num("dpi", 100, 600, o.dpi);
    o.quality = (int)num("quality", 50, 95, o.quality);
    // "reprint": [{"sheet": 3, "side": "back"}, ...] - sheets 1-based, as the view shows them
    if (j.contains("reprint") && j["reprint"].is_array())
        for (const auto& r : j["reprint"]) {
            if (!r.is_object() || !r.contains("sheet") || !r["sheet"].is_number_integer()) continue;
            const int sheet = r["sheet"].get<int>() - 1;
            const std::string side = r.value("side", "");
            if (sheet < 0 || (side != "front" && side != "back")) continue;
            o.reprint.push_back({sheet, side == "back"});   // repeats allowed: the same sheet of two copies
        }
    return o;
}
json optionsToJson(const Options& o) {
    return {{"scale", o.scale}, {"cut", o.cut}, {"pad", o.pad}, {"reverseFronts", o.reverseFronts}, {"copies", o.copies},
            {"batch", o.batch}, {"flatten", o.flatten}, {"dpi", o.dpi}, {"quality", o.quality}};
}

static json planJson(const std::vector<Sheet>& sh) {
    json a = json::array();
    auto p = [](int x) { return x < 0 ? json(nullptr) : json(x + 1); };   // 1-based, null = blank
    for (const auto& s : sh) a.push_back({{"front", {p(s.front.left), p(s.front.right)}}, {"back", {p(s.back.left), p(s.back.right)}}});
    return a;
}

json inspect(const std::string& path, const Options& o) {
    json out;
    try {
        QPDF pdf;
        pdf.processFile(path.c_str());
        QPDFPageDocumentHelper dh(pdf);
        dh.pushInheritedAttributesToPage();
        auto pages = dh.getAllPages();
        const int n = (int)pages.size();
        if (n == 0) return {{"ok", false}, {"error", "the PDF has no pages"}};
        std::map<std::string, int> sizes;
        std::string how;
        PageGeom g0 = geomOf(pages[0]);
        contentBox(pages[0], o, &how);
        for (auto& ph : pages) { PageGeom g = geomOf(ph); sizes[fmtMm(g.wMm) + " x " + fmtMm(g.hMm) + " mm"]++; }
        json sz = json::array(); for (auto& kv : sizes) sz.push_back({{"size", kv.first}, {"pages", kv.second}});
        auto bp = bookletPages(n, o.pad);
        auto sh = sheets(bp);
        out = {{"ok", true}, {"path", path}, {"pages", n}, {"pageMm", {std::round(g0.wMm * 10) / 10, std::round(g0.hMm * 10) / 10}},
               {"fit", guessFit(g0.wMm, g0.hMm).kind}, {"cut", how}, {"sizes", sz}, {"blanks", (int)bp.size() - n},
               {"sheets", (int)sh.size()}, {"plan", planJson(sh)}, {"options", optionsToJson(o)}};
    } catch (const std::exception& e) {
        out = {{"ok", false}, {"error", std::string("can't read this PDF: ") + e.what()}};
    }
    return out;
}

json make(const std::string& path, const std::string& outDir, const Options& o, Progress progress) {
    auto step = [&](double f, const std::string& s) { if (progress) progress(f, s); };
    try {
        namespace fs = std::filesystem;
        fs::create_directories(outDir);
        step(0.02, "reading the PDF");
        Source src(path, o);
        const int n = (int)src.pages.size();
        if (n == 0) return {{"ok", false}, {"error", "the PDF has no pages"}};
        const auto bp = bookletPages(n, o.pad);
        const auto sh = sheets(bp);
        const int S = (int)sh.size();
        // every sheet side in booklet order: side 2i = sheet i front, 2i+1 = its back
        std::vector<Side> sides;
        for (const auto& s : sh) { sides.push_back(s.front); sides.push_back(s.back); }
        // a reprint only needs its own sides rendered and written
        std::vector<int> rFronts, rBacks;
        for (const auto& r : o.reprint) {
            if (r.first >= S) return {{"ok", false}, {"error", "there is no sheet " + std::to_string(r.first + 1) + " (this booklet has " + std::to_string(S) + ")"}};
            (r.second ? rBacks : rFronts).push_back(r.first);
        }
        const bool reprinting = !o.reprint.empty();
        std::sort(rFronts.begin(), rFronts.end());
        std::sort(rBacks.begin(), rBacks.end());
        if (o.reverseFronts) std::reverse(rFronts.begin(), rFronts.end());
        std::vector<int> need;   // side indices to render
        if (reprinting) {
            for (int f : rFronts) need.push_back(2 * f);
            for (int b : rBacks) need.push_back(2 * b + 1);
            std::sort(need.begin(), need.end());
            need.erase(std::unique(need.begin(), need.end()), need.end());   // render each side once
        }
        else for (int i = 0; i < 2 * S; i++) need.push_back(i);

        // flatten: render each side once, reuse the JPEG in every file
        std::map<int, std::string> jpegs;   // side index -> JPEG
        int pxW = 0, pxH = 0;
        if (o.flatten) {
            step(0.05, "arranging the pages");
            const std::string vec = (fs::path(outDir) / ".vector-booklet.pdf").string();
            { QPDF out; out.emptyPDF(); for (int k : need) addVectorSide(out, src, sides[k]); writePdf(out, vec); }
            pxW = (int)std::lround(kA4W / 72.0 * o.dpi); pxH = (int)std::lround(kA4H / 72.0 * o.dpi);
            std::lock_guard<std::mutex> lk(pdfiumLock());
            pdfiumInit();
            FPDF_DOCUMENT doc = FPDF_LoadDocument(vec.c_str(), nullptr);
            if (!doc) { fs::remove(vec); return {{"ok", false}, {"error", "couldn't render the pages (PDFium)"}}; }
            std::vector<unsigned char> rgb((size_t)pxW * pxH * 3);
            for (int n2 = 0; n2 < (int)need.size(); n2++) {
                const int i = need[n2];
                step(0.08 + 0.8 * n2 / need.size(), "rendering sheet " + std::to_string(i / 2 + 1) + (i % 2 ? " back" : " front") + " at " + std::to_string(o.dpi) + " dpi");
                FPDF_PAGE pg = FPDF_LoadPage(doc, n2);
                FPDF_BITMAP bmp = FPDFBitmap_Create(pxW, pxH, 0);
                FPDFBitmap_FillRect(bmp, 0, 0, pxW, pxH, 0xFFFFFFFF);
                FPDF_RenderPageBitmap(bmp, pg, 0, 0, pxW, pxH, 0, FPDF_PRINTING | FPDF_ANNOT);
                const unsigned char* buf = (const unsigned char*)FPDFBitmap_GetBuffer(bmp);
                const int stride = FPDFBitmap_GetStride(bmp);
                for (int y = 0; y < pxH; y++) {
                    const unsigned char* row = buf + (size_t)y * stride;
                    unsigned char* dst = rgb.data() + (size_t)y * pxW * 3;
                    for (int x = 0; x < pxW; x++) { dst[3 * x] = row[4 * x + 2]; dst[3 * x + 1] = row[4 * x + 1]; dst[3 * x + 2] = row[4 * x]; }   // BGRx -> RGB
                }
                FPDFBitmap_Destroy(bmp);
                FPDF_ClosePage(pg);
                jpegs[i] = jpegOf(rgb, pxW, pxH, o.quality);
            }
            FPDF_CloseDocument(doc);
            fs::remove(vec);
        }

        json files = json::array();
        auto writeSides = [&](const std::string& name, const std::vector<int>& idx, const std::string& what) {
            QPDF out; out.emptyPDF();
            std::map<const std::string*, QPDFObjectHandle> imgs;
            for (int k : idx) {
                if (o.flatten) addImageSide(out, jpegs.at(k), pxW, pxH, imgs);
                else addVectorSide(out, src, sides[k]);
            }
            const std::string p = (fs::path(outDir) / name).string();
            writePdf(out, p);
            files.push_back({{"name", name}, {"path", p}, {"pages", (int)idx.size()}, {"bytes", (long long)fs::file_size(p)}, {"what", what}});
        };
        step(0.9, "writing the files");
        if (reprinting) {
            auto list = [](const std::vector<int>& v) { std::string t; for (int x : v) t += (t.empty() ? "" : ", ") + std::to_string(x + 1); return t; };
            // an older reprint's files would be mistaken for this one's
            fs::remove(fs::path(outDir) / "reprint-fronts.pdf");
            fs::remove(fs::path(outDir) / "reprint-backs.pdf");
            std::vector<int> f, k;
            for (int x : rFronts) f.push_back(2 * x);
            for (int x : rBacks) k.push_back(2 * x + 1);
            if (!f.empty()) writeSides("reprint-fronts.pdf", f, "reprint: the front of sheet" + std::string(f.size() > 1 ? "s " : " ") + list(rFronts) + (o.reverseFronts && f.size() > 1 ? ", last sheet first" : ""));
            if (!k.empty()) writeSides("reprint-backs.pdf", k, "reprint: the back of sheet" + std::string(k.size() > 1 ? "s " : " ") + list(rBacks) + (k.size() > 1 ? ", first sheet first" : ""));
            step(1.0, "done");
            return {{"ok", true}, {"outDir", outDir}, {"reprint", true}, {"sheets", S}, {"files", files}, {"options", optionsToJson(o)}};
        }
        std::vector<int> all; for (int i = 0; i < 2 * S; i++) all.push_back(i);
        writeSides("booklet.pdf", all, "both sides, for a printer that prints both sides: flip on the short edge");
        const auto ps = passes(S, o.copies, o.batch, o.reverseFronts);
        for (size_t b = 0; b < ps.size(); b++) {
            std::vector<int> f, k;
            for (int s : ps[b].fronts) f.push_back(2 * s);
            for (int s : ps[b].backs) k.push_back(2 * s + 1);
            const std::string pre = ps.size() == 1 ? "" : "batch-" + std::to_string(b + 1) + "-";
            const std::string copies = ps[b].copies == 1 ? "1 copy" : std::to_string(ps[b].copies) + " copies";
            writeSides(pre + "fronts.pdf", f, std::string("one-sided printing, pass 1: the fronts") + (o.reverseFronts ? ", last sheet first" : "") + " (" + copies + ")");
            writeSides(pre + "backs.pdf", k, "one-sided printing, pass 2: the backs, first sheet first (" + copies + ")");
        }
        writeSides("test-sheet.pdf", {0, 1}, "one sheet (the cover sheet) to check which way your printer takes paper back in");
        const std::string guide = guideText(n, S, o, files);
        { std::ofstream g(fs::path(outDir) / "HOW-TO-PRINT.txt"); g << guide; }
        step(1.0, "done");
        return {{"ok", true}, {"outDir", outDir}, {"pages", n}, {"blanks", (int)bp.size() - n}, {"sheets", S}, {"files", files},
                {"plan", planJson(sh)}, {"guide", guide}, {"options", optionsToJson(o)}};
    } catch (const std::exception& e) {
        return {{"ok", false}, {"error", std::string("couldn't make the booklet: ") + e.what()}};
    }
}

std::string guideText(int pages, int S, const Options& o, const json& files) {
    std::ostringstream g;
    const int blanks = (pages + 3) / 4 * 4 - pages;
    g << "ZINE PRESS - how to print and bind this booklet\n"
      << "================================================\n\n"
      << pages << " pages";
    if (blanks) g << " + " << blanks << " blank page" << (blanks > 1 ? "s" : "") << " before the back cover";
    g << " = " << S << " A4 sheet" << (S > 1 ? "s" : "") << " per copy, folded into an A5 booklet.\n\n"
      << "Files:\n";
    for (const auto& f : files) g << "  " << f.value("name", "") << " - " << f.value("what", "") << "\n";
    g << "  HOW-TO-PRINT.txt - this guide\n\n"
      << "PRINTER SETTINGS (every file)\n"
      << "  - Plain A4 paper, paper type Plain, landscape, actual size / 100% (never \"fit to page\").\n"
      << "  - NOT borderless: on many printers (Brother DCP-T230: BrA4_B) that is the printer's own\n"
      << "    borderless mode - slow and heavy on ink even at Normal quality.\n"
      << "  - Nothing else waiting in the printer's queue: two jobs mixed together spoil the copy.\n";
    if (o.scale >= 0.999) g << "  - The pages are laid out edge to edge: the printer's own margin (about 3 mm) trims the outer edge.\n";
    else g << "  - Pages are shrunk to " << (int)std::lround(o.scale * 100) << "% so the printer's unprintable margin doesn't cut them.\n";
    g << "  - Quality Normal. Fine is much slower and you won't see a difference on plain paper.\n\n"
      << "A PRINTER THAT PRINTS BOTH SIDES\n"
      << "  Print booklet.pdf, two-sided, \"flip on the short edge\". Fold the stack in the middle.\n\n"
      << "A PRINTER THAT PRINTS ONE SIDE AT A TIME\n"
      << "  1. Do the one-sheet test first (test-sheet.pdf): print page 1 only (the cover sheet's front).\n"
      << "     Draw an arrow at the top edge. Put the sheet back in the tray the way your printer needs\n"
      << "     to print the other side (for most: printed side up, same edge first). Print page 2.\n"
      << "     Fold it: the inside of the cover must be page 2. If not, turn the sheet the other way and\n"
      << "     note what works - that is how the whole stack goes back in.\n"
      << "  2. Print the fronts file. ";
    if (o.reverseFronts) g << "It runs last sheet first: most one-sided printers stack the\n"
                           << "     output face up with the last sheet on top, so the stack comes out ready for the backs.\n";
    else g << "It runs first sheet first (you turned off the reverse order).\n";
    g << "     Check the top sheet is the cover.\n"
      << "  3. Put the printed stack back EXACTLY as it came out: printed side up, not turned, not\n"
      << "     flipped, nothing else in the tray. If the ink is still wet, let it dry a few minutes and\n"
      << "     fan the sheets first: damp sheets cling and double-feed on the backs pass.\n"
      << "  4. Print the backs file. Check the first sheet: page 2 on the back of the cover.\n"
      << "  5. ONE COPY AT A TIME: only then the fronts of the next copy. A wrong reload then spoils\n"
      << "     one copy, not all of them.\n"
      << "  One sheet jammed or spoilt: print just that sheet's front, put it back as it came out,\n"
      << "  then its back (Zine Press: Reprint one sheet; sheet 1 is the cover sheet).\n";
    if (o.copies > 1) g << "  " << o.copies << " copies in batches of " << o.batch << ": each batch file holds whole copies (collated).\n"
                        << "  A double-feed (two sheets pulled at once) ruins every sheet after it in that batch -\n"
                        << "  small batches keep the damage small.\n";
    g << "\n"
      << "IF TWO SHEETS GO THROUGH AT ONCE (a double-feed)\n"
      << "  1. Cancel the job on the printer straight away.\n"
      << "  2. Sort the sheets into three piles: back printed correctly (done), back still blank\n"
      << "     (needs only its back), back printed wrong (spoiled: needs both sides again).\n"
      << "     The page numbers on a sheet tell you which sheet it is (see the sheet plan in the app).\n"
      << "  3. In Zine Press, mark those sheet sides on the sheet plan and press Reprint: you get\n"
      << "     reprint-fronts.pdf and reprint-backs.pdf with just those sides. Print the fronts and let\n"
      << "     them dry. Stack them with the blank-back sheets in sheet order, as the backs pass had them,\n"
      << "     put the stack in the same way round as before and print the backs.\n\n"
      << "OUT OF PAPER IN THE MIDDLE OF A JOB\n"
      << "  Add paper, push the tray fully in and press the button the printer asks for (on a Brother:\n"
      << "  Mono Copy). The job carries on where it stopped.\n\n"
      << "IF SOMETHING LOOKS WRONG\n"
      << "  - White bands across photos: run a head cleaning from the printer's menu, then print again.\n"
      << "  - Very slow printing: use the flattened files (Zine Press does this by default, at 300 dpi).\n"
      << "  - Pages upside down on the back: you put the stack back the wrong way; redo the one-sheet test.\n\n"
      << "FOLD AND BIND\n"
      << "  Fold every sheet in half on its own, then nest them in order (sheet 1 on the outside).\n"
      << "  Three-hole pamphlet stitch: holes on the fold in the middle and 3.5 cm from each end;\n"
      << "  about 60 cm of thread. Go in through the middle hole from the outside, out of one end hole,\n"
      << "  back in through the other end hole along the inside, out through the middle again, tie the\n"
      << "  two ends over the long stitch.\n"
      << "  No needle? Two staples on the fold with a long-arm stapler, or open a normal stapler over a\n"
      << "  piece of cardboard and close the staple legs by hand.\n";
    return g.str();
}

} // namespace zine
