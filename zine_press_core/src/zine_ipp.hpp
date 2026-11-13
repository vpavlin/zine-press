#pragma once
// zine_ipp.hpp - printing from Zine Press: a small IPP/1.1 client over plain HTTP (no libcups, so
// the same code runs on Linux and macOS and nothing extra is bundled).
//
// Printers: the local CUPS (CUPS-Get-Printers over its socket or localhost:631) and any ipp:// URI
// (a remote CUPS queue such as ipp://proteus.office.mesh:631/printers/DCPT230, or an IPP Everywhere
// printer). Capabilities come from the printer: Get-Printer-Attributes, plus the queue's PPD when it
// is a CUPS queue (GET /printers/NAME.ppd), whose page sizes and quality option carry the driver's
// own names ("A4_B" / "A4 (Borderless)", "BRResolution" Normal) - they are sent the way
// `lp -o PageSize=A4_B` sends them. A job: Print-Job with the PDF, then Get-Job-Attributes until it
// finishes (job-impressions-completed = sheets printed), Cancel-Job to stop it.
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace zine::ipp {

using json = nlohmann::json;

// One attribute value as read from a response (strings, numbers and enums all as text)
struct Attr { std::string name; int tag = 0; std::vector<std::string> values; };
using Group = std::vector<Attr>;
struct Response { bool ok = false; int status = 0; std::string error; std::vector<std::pair<int, Group>> groups; std::string data; };

// A request being built
class Request {
public:
    Request(int op, const std::string& printerUri);
    Request& group(int tag);
    Request& add(int tag, const std::string& name, const std::string& value);
    Request& add(int tag, const std::string& name, const std::vector<std::string>& values);
    Request& addInt(int tag, const std::string& name, int value);
    Request& addBool(const std::string& name, bool value);
    Request& addRanges(const std::string& name, const std::vector<std::pair<int, int>>& ranges);
    std::string bytes() const;
private:
    std::string m_b;
};

// ipp://host[:port]/path -> its parts; "localhost" / 127.0.0.1 on 631 go through the CUPS socket when there is one
struct Target { std::string host; int port = 631; std::string path; bool ok = false; };
Target parseUri(const std::string& uri);

// POST an IPP request (document appended) and parse the reply. timeoutMs bounds connect and each read/write.
Response call(const std::string& uri, const std::string& request, const std::string& document, int timeoutMs);
// A plain GET (the PPD); returns the body or "" with err set
std::string httpGet(const std::string& uri, const std::string& path, int timeoutMs, std::string* err);

// First value of an attribute in any group of the given tag (0 = any), "" if missing
std::string first(const Response& r, const std::string& name, int groupTag = 0);
std::vector<std::string> all(const Response& r, const std::string& name, int groupTag = 0);

// The local CUPS's queues: [{uri, name, info, makeModel, state, local:true}]
json localPrinters(int timeoutMs);
// One printer's capabilities: {ok, uri, name, makeModel, state, stateReasons, stateMessage, pdf,
//  sizes:[{attr, key, label, borderless, a4}], qualities:[{attr, key, label}], duplex, fit,
//  defaultSize, defaultQuality (the keys Zine Press would pick), ppd}
json printerInfo(const std::string& uri, bool wantBorderless, int timeoutMs);
// Parse a PPD's PickOne options into {PageSize:[{key,label}], quality:{option,[{key,label}]}, defaults}
json parsePpd(const std::string& ppd);

// Print-Job: returns {ok, jobId, jobUri} or {ok:false, error}. opts: {title, copies, collate, sides,
// size:{attr,key}, quality:{attr,key}, mediaType:{attr,key}, fitToPage (else print-scaling=none),
// cups (a CUPS queue), pageRanges:[[from,to],...]}
json printJob(const std::string& uri, const std::string& pdfPath, const json& opts, int timeoutMs);
// Get-Job-Attributes: {ok, state (ipp enum), stateName, reasons, message, impressions, sheets}
json jobStatus(const std::string& uri, int jobId, int timeoutMs);
// Get-Printer-Attributes for the live state while printing: {state, reasons, message}
json printerState(const std::string& uri, int timeoutMs);
// Jobs the printer still has (queued or printing): {ok, count}. queued-job-count, else Get-Jobs.
json queuedJobs(const std::string& uri, int timeoutMs);
json cancelJob(const std::string& uri, int jobId, int timeoutMs);

std::string userName();

} // namespace zine::ipp
