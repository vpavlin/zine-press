#pragma once
// ZinePressCoreImpl - Zine Press as a Logos core module (universal authoring: the public methods
// below ARE the module's API; the glue is generated from this header). Comments go ABOVE a method,
// never after it on the same line (the parser would skip the method).
//
// Turns a PDF into the files for printing it at home as an A5 saddle-stitched booklet on A4:
// booklet.pdf (two-sided printers), fronts.pdf + backs.pdf (one-sided ones), a one-sheet test and
// a printing + binding guide. The work is in zine_pdf.cpp (qpdf + PDFium); a booklet is made on a
// worker thread and polled with job(), so no call blocks the module for long.
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "logos_module_context.h"
#include "zine_pdf.hpp"

class ZinePressCoreImpl : public LogosModuleContext {
public:
    ~ZinePressCoreImpl() override;

    // {name, version, engine}
    std::string about();
    // A PDF's pages, size, how it will be cut and the sheet plan for these options
    std::string inspect(std::string path, std::string optionsJson);
    // Start making the booklet files; returns {ok, jobId, outDir}. optionsJson may set "outDir".
    std::string make(std::string path, std::string optionsJson);
    // A job's state: {state: running|done|error, fraction, step, result}
    std::string job(std::string jobId);
    // The folder a booklet of this PDF goes to by default
    std::string defaultOutDir(std::string path);

private:
    struct Job { std::string id, state = "running", step; double fraction = 0; zine::json result; };
    std::mutex m_mtx;
    std::shared_ptr<Job> m_job;
    std::thread m_worker;
    long m_seq = 0;
};
