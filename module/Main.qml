import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Logos.Theme
import Logos.Controls

// Zine Press - print a zine at home as an A5 saddle-stitched booklet on A4.
// A pure view over the zine_press_core module (inspect / make / job); every call is async.
Item {
    id: root
    width: 1200
    height: 800

    // ── state ──
    property string pdfPath: ""          // what the core gets (file:// URL or path)
    property var info: null              // inspect() result
    property string outDir: ""
    property string jobId: ""
    property var job: null               // job() result while running / when done
    property var result: null            // the finished make() result
    property var reprintResult: null     // the finished reprint (make() with options.reprint)
    property var marks: ({})             // sheet sides to reprint: {"3-back": true, ...} (1-based sheets)
    property string error: ""
    property string tab: "print"         // guide tab: print | bind | trouble

    // options
    property bool duplex: false          // the printer prints both sides
    property bool borderless: false
    property bool reverseFronts: true
    property bool flatten: true
    property string cut: "auto"
    property int copies: 1
    property int batch: 2

    function options() {
        // one side at a time: the files hold ONE copy and the run prints it copy by copy, checking each
        // (a wrong reload then spoils one copy, not several - Václav, 2026-11-13)
        return { scale: root.borderless ? 1.0 : 0.96, cut: root.cut, pad: true, reverseFronts: root.reverseFronts,
                 copies: root.duplex ? root.copies : 1, batch: root.duplex ? root.batch : 1,
                 flatten: root.flatten, dpi: 300, outDir: root.outDir };
    }

    // ── core plumbing (async only: a blocking call would freeze the view) ──
    function callVia(m, a, cb) {
        var done = function (r) { try { cb(r === undefined || r === null ? "" : String(r)); } catch (e) { console.warn("zine_press", m, e); } };
        if (typeof logos === "undefined" || !logos) { Qt.callLater(function () { done(""); }); return; }
        if (typeof logos.callModuleAsync === "function") {
            try { logos.callModuleAsync("zine_press_core", m, a || [], done, 20000); } catch (e) { Qt.callLater(function () { done(""); }); }
            return;
        }
        Qt.callLater(function () { var r = ""; try { r = logos.callModule("zine_press_core", m, a || []); } catch (e) {} done(r); });
    }
    function parse(raw) {
        var s = String(raw || "").trim();
        for (var i = 0; i < 2 && s.charAt(0) === "\""; i++) { try { s = String(JSON.parse(s)).trim(); } catch (e) { return null; } }
        if (s.charAt(0) !== "{") return null;
        try { return JSON.parse(s); } catch (e) { return null; }
    }

    function pick(url) {
        root.pdfPath = String(url);
        root.result = null; root.job = null; root.error = ""; root.outDir = "";
        root.reprintResult = null; root.marks = {};
        inspectNow();
    }
    function inspectNow() {
        if (!root.pdfPath) return;
        callVia("inspect", [root.pdfPath, JSON.stringify(options())], function (raw) {
            var r = parse(raw);
            if (!r) { root.error = "Zine Press core did not answer - is zine_press_core installed?"; return; }
            if (!r.ok) { root.error = r.error || "can't read this PDF"; root.info = null; return; }
            root.error = ""; root.info = r;
            if (!root.outDir) root.outDir = r.outDir || "";
        });
    }
    function makeNow() {
        if (!root.info || root.busy) return;
        root.result = null; root.error = "";
        startMake(options());
    }
    // ── reprint: after a double-feed, only the marked sheet sides ──
    function markKey(sheet, side) { return sheet + "-" + side; }
    function toggleMark(sheet, side) {
        var m = Object.assign({}, root.marks), k = markKey(sheet, side);
        if (m[k]) delete m[k]; else m[k] = true;
        root.marks = m;
    }
    readonly property int markCount: Object.keys(root.marks).length
    function reprintNow() {
        if (!root.info || root.busy || root.markCount === 0) return;
        var o = options(), list = [];
        for (var k in root.marks) { var p = k.split("-"); list.push({ sheet: parseInt(p[0]), side: p[1] }); }
        o.reprint = list;
        root.reprintResult = null; root.error = "";
        startMake(o);
    }
    function startMake(o) {
        callVia("make", [root.pdfPath, JSON.stringify(o)], function (raw) {
            var r = parse(raw);
            if (!r || !r.ok) { root.error = r && r.error ? r.error : "couldn't start"; return; }
            root.jobId = r.jobId; root.outDir = r.outDir || root.outDir;
            root.job = { state: "running", fraction: 0, step: "starting" };
            poll.start();
        });
    }
    readonly property bool busy: !!(root.job && root.job.state === "running")
    Timer {
        id: poll
        interval: 500; repeat: true
        onTriggered: root.callVia("job", [root.jobId], function (raw) {
            var r = root.parse(raw);
            if (!r || !r.ok) return;
            root.job = r;
            if (r.state !== "running") {
                poll.stop();
                if (r.state === "done") { if (r.result && r.result.reprint) root.reprintResult = r.result; else root.result = r.result; }
                else {
                    root.error = (r.result && r.result.error) || "making the booklet failed";
                    if (root.reprintPending) { root.reprintPending = false; root.runNote = "couldn't make the reprint files: " + root.error; }
                }
            }
        })
    }
    // options that change the sheet plan: ask again
    onCutChanged: { root.marks = {}; root.reprintResult = null; inspectNow() }
    // the cut description depends on borderless (the bleed is kept past the paper edge)
    onBorderlessChanged: inspectNow()

    function fileNamed(n) {
        if (!root.result || !root.result.files) return null;
        for (var i = 0; i < root.result.files.length; i++) if (root.result.files[i].name === n) return root.result.files[i];
        return null;
    }
    // ══ printing from here (core: printers / printerInfo / printFile / printStatus / printCancel) ══
    // "open": each file opens in the system's PDF viewer and is printed from its own dialog (any
    // printer the OS has); "ipp": sent straight to a printer Zine Press can reach (progress, Stop)
    property string printMode: "open"
    property string openNote: ""         // open mode: the viewer didn't start
    property var printerList: []         // [{uri, label, saved}]
    property string printerUri: ""
    property var printer: null           // printerInfo() of the chosen printer
    property string printerError: ""
    property bool printerBusy: false
    property int sizeIndex: -1
    property int qualityIndex: -1
    // actual size: fit-to-page squashes the page (and some queues fit by default)
    property bool fitToPage: false
    // the run: steps [{kind: fronts|backs|duplex, copy, of, copies, file, sheets, pageRanges, single}]
    property var steps: []
    property int stepIndex: -1
    // idle | ready | printing | opened | reload (fronts out: put the stack back) | check (backs out:
    // look at the cover sheet) | stopped | failed | done
    property string phase: "idle"
    property var lastStep: null          // the step that just finished (for the reload / check texts)
    property int oneSheet: 1             // "reprint one sheet": which sheet
    property string printId: ""
    property var pstat: null             // printStatus() of the step being printed
    property var sort: ({})              // after Stop: "copy-sheet" -> ok | back | both (one side) / ok | again (both sides)
    property bool reprintPending: false  // make(reprint) running for the run
    property string runNote: ""

    function loadPrinters() {
        callVia("printers", [], function (raw) {
            var r = parse(raw); if (!r) return;
            var l = [];
            (r.local || []).forEach(function (p) { l.push({ uri: p.uri, label: (p.info || p.name) + (p.makeModel ? "  (" + p.makeModel + ")" : "") + "  - this computer", saved: false }); });
            (r.saved || []).forEach(function (p) { l.push({ uri: p.uri, label: (p.name || p.uri) + "  - " + p.uri, saved: true }); });
            root.printerList = l;
            if (!root.printerUri && l.length) {
                var pick = l[0].uri;
                for (var i = 0; i < l.length; i++) if (l[i].uri === r.last) pick = r.last;
                choosePrinter(pick);
            }
        });
    }
    function choosePrinter(uri) {
        root.printerUri = uri; root.printer = null; root.printerError = ""; root.printerBusy = true;
        callVia("printerInfo", [uri], function (raw) {
            root.printerBusy = false;
            var r = parse(raw);
            if (!r || !r.ok) { root.printerError = r && r.error ? r.error : "the printer didn't answer"; return; }
            root.printer = r;
            root.sizeIndex = pickSize(r);
            root.qualityIndex = indexOfKey(r.qualities, r.defaultQuality);
        });
    }
    function addPrinter(uri) {
        if (!uri) return;
        root.printerBusy = true; root.printerError = "";
        callVia("addPrinter", [uri], function (raw) {
            root.printerBusy = false;
            var r = parse(raw);
            if (!r || !r.ok) { root.printerError = r && r.error ? r.error : "the printer didn't answer"; return; }
            root.printerUri = r.uri; root.printer = r; uriField.text = "";
            root.sizeIndex = pickSize(r);
            root.qualityIndex = indexOfKey(r.qualities, r.defaultQuality);
            loadPrinters();
        });
    }
    function forgetPrinter() {
        callVia("forgetPrinter", [root.printerUri], function () { root.printerUri = ""; root.printer = null; loadPrinters(); });
    }
    // Paper: plain A4 (the core's pick). Borderless only when chosen by hand: on a Brother DCP-T230 it
    // is the printer's own borderless mode, slow and heavy on ink even at Normal (2026-11-13).
    readonly property bool filesBorderless: root.result ? root.result.options.scale >= 0.999 : root.borderless
    function pickSize(r) {
        if (!r || !r.sizes) return -1;
        return indexOfKey(r.sizes, r.defaultSize);
    }
    readonly property var chosenSize: root.printer && root.sizeIndex >= 0 ? root.printer.sizes[root.sizeIndex] : null
    function indexOfKey(list, key) {
        for (var i = 0; list && i < list.length; i++) if (list[i].key === key) return i;
        return list && list.length ? 0 : -1;
    }
    Component.onCompleted: Qt.callLater(loadPrinters)

    function stem() { var b = root.baseName(root.pdfPath); return b.replace(/\.pdf$/i, ""); }
    function fileFrom(res, name) {
        if (!res || !res.files) return null;
        for (var i = 0; i < res.files.length; i++) if (res.files[i].name === name) return res.files[i];
        return null;
    }
    // The run from the made files. Both sides: one step per batch. One side: ONE COPY AT A TIME -
    // its fronts, a stop to put the stack back, its backs, a stop to check the cover sheet, then the
    // next copy. Never several copies' fronts or backs in one go.
    function buildSteps() {
        var r = root.result, o = r.options, S = r.sheets, l = [];
        if (root.duplex) {
            var nb = Math.ceil(o.copies / o.batch);
            for (var b = 0; b < nb; b++) {
                var c = Math.min(o.batch, o.copies - b * o.batch);
                l.push({ kind: "duplex", batch: b + 1, batches: nb, copies: c, file: "booklet.pdf", sheets: S * c });
            }
            return l;
        }
        for (var k = 1; k <= root.copies; k++) {
            l.push({ kind: "fronts", copy: k, of: root.copies, copies: 1, file: "fronts.pdf", sheets: S });
            l.push({ kind: "backs", copy: k, of: root.copies, copies: 1, file: "backs.pdf", sheets: S });
        }
        return l;
    }
    // The sheets of one copy, fronts and backs: they must agree, or the backs land on the wrong fronts
    function filesAgree() {
        if (root.duplex) return "";
        var f = fileNamed("fronts.pdf"), b = fileNamed("backs.pdf");
        if (!f || !b) return "fronts.pdf or backs.pdf is missing - make the files again.";
        if (f.pages !== b.pages) return "fronts.pdf has " + f.pages + " sheets but backs.pdf has " + b.pages + " - make the files again.";
        if (root.result.sheets && f.pages !== root.result.sheets) return "fronts.pdf holds " + f.pages + " sheets, not one copy (" + root.result.sheets + ") - make the files again.";
        return "";
    }
    // "Reprint one sheet" (a jam or a double-feed): its front, a stop, its back. Sheet 1 is the cover sheet.
    function frontPageOf(n) { var S = root.result.sheets; return root.result.options.reverseFronts ? S + 1 - n : n; }
    function reprintOneSheet(n) {
        if (!root.result) return;
        var why = filesAgree(); if (why) { root.phase = "failed"; root.runNote = why; return; }
        var l;
        if (root.duplex) l = [{ kind: "duplex", single: n, src: "main", batch: -1, copies: 1, file: "booklet.pdf", sheets: 1, pageRanges: [[2 * n - 1, 2 * n]] }];
        else l = [{ kind: "fronts", single: n, src: "main", copies: 1, file: "fronts.pdf", sheets: 1, pageRanges: [[frontPageOf(n), frontPageOf(n)]] },
                  { kind: "backs", single: n, src: "main", copies: 1, file: "backs.pdf", sheets: 1, pageRanges: [[n, n]] }];
        insertSteps(l);
    }
    readonly property var step: root.stepIndex >= 0 && root.stepIndex < root.steps.length ? root.steps[root.stepIndex] : null
    function stepTitle(st) {
        if (!st) return "";
        var side = st.kind === "fronts" ? "fronts" : st.kind === "backs" ? "backs" : "both sides";
        if (st.single) return "Sheet " + st.single + (st.single === 1 ? " (the cover sheet)" : "") + ": " + (st.kind === "fronts" ? "its front" : st.kind === "backs" ? "its back" : "both sides");
        if (st.reprint) return "Reprint: " + side + " (" + st.sheets + " sheet" + (st.sheets > 1 ? "s" : "") + ")";
        if (st.kind === "duplex") return (st.batches > 1 ? "Batch " + st.batch + " of " + st.batches : (st.copies > 1 ? st.copies + " copies" : "The zine")) + ": both sides (" + st.sheets + " sheets)";
        return (st.of > 1 ? "Copy " + st.copy + " of " + st.of : "The zine") + ": " + side + " (" + st.sheets + " sheet" + (st.sheets > 1 ? "s" : "") + ")";
    }
    readonly property string reloadText: "Put the printed stack back exactly as it came out: printed side up, not turned, not flipped, nothing else in the tray."
    function stepHow(st) {
        if (!st) return "";
        if (st.kind === "fronts") return st.single
            ? "Put ONE sheet of plain paper in the tray, nothing else."
            : "Put " + st.sheets + " sheet" + (st.sheets > 1 ? "s" : "") + " of plain paper in the tray - one copy's worth, fanned and square.";
        if (st.kind === "backs") return st.reprint
            ? "Put the stack back in printed side up, as it came out: the reprinted fronts and the sheets with a blank back together, in sheet order, as the backs pass had them. Nothing else in the tray."
            : root.reloadText;
        return "Put " + st.sheets + " sheet" + (st.sheets > 1 ? "s" : "") + " of plain paper in the tray.";
    }
    // After a backs pass: what to look at before going on
    function checkText(st) {
        if (!st) return "";
        if (st.single) {
            var pl = root.info && root.info.plan ? root.info.plan[st.single - 1] : null;
            return "Check sheet " + st.single + ": " + (pl ? "pages " + root.pageLabel(pl.back[0]) + "|" + root.pageLabel(pl.back[1]) + " on the back of " + root.pageLabel(pl.front[0]) + "|" + root.pageLabel(pl.front[1]) : "its back on the right side")
                   + ", the right way up. Then put it in its place in the copy.";
        }
        return "Check the first sheet: page 2 on the back of the cover, the right way up. If it isn't, don't go on - see If it goes wrong.";
    }

    function startRun() {
        if (!root.result || (root.printMode === "ipp" && !root.printer)) return;
        var why = filesAgree();
        if (why) { root.runNote = why; root.phase = "failed"; root.steps = []; root.stepIndex = -1; return; }
        root.steps = buildSteps(); root.stepIndex = 0; root.phase = "ready"; root.runNote = ""; root.sort = {};
    }
    function endRun() { printPoll.stop(); root.phase = "idle"; root.steps = []; root.stepIndex = -1; root.pstat = null; root.printId = ""; }
    function sendStep() {
        var st = root.step; if (!st) return;
        var res = st.src === "reprint" ? root.reprintResult : root.result;
        var f = fileFrom(res, st.file);
        if (!f) { root.phase = "failed"; root.runNote = "the file " + st.file + " isn't there - make the files again"; return; }
        if (root.printMode === "open") { openStep(f); return; }
        if (!root.printer) return;
        var o = {
            title: "Zine Press: " + stem() + " - " + stepTitle(st),
            copies: st.kind === "duplex" ? st.copies : 1, collate: true,
            sides: st.kind === "duplex" ? "two-sided-short-edge" : "",
            fitToPage: root.fitToPage, cups: !!root.printer.ppd,
            sheets: st.sheets, duplex: st.kind === "duplex"
        };
        if (root.sizeIndex >= 0) o.size = { attr: root.printer.sizes[root.sizeIndex].attr, key: root.printer.sizes[root.sizeIndex].key };
        if (root.qualityIndex >= 0) o.quality = { attr: root.printer.qualities[root.qualityIndex].attr, key: root.printer.qualities[root.qualityIndex].key };
        if (root.printer.mediaType) o.mediaType = { attr: root.printer.mediaType.attr, key: root.printer.mediaType.key };
        if (st.pageRanges) o.pageRanges = st.pageRanges;
        root.phase = "printing"; root.pstat = { phase: "sending", done: 0, total: st.sheets }; root.runNote = "";
        callVia("printFile", [root.printerUri, f.path, JSON.stringify(o)], function (raw) {
            var r = parse(raw);
            if (!r || !r.ok) { root.phase = "failed"; root.runNote = r && r.error ? r.error : "couldn't send to the printer"; return; }
            root.printId = String(r.printId);
            printPoll.start();
        });
    }
    // open mode: the system viewer prints it; the user says when it's done
    function openStep(f) {
        root.openNote = "";
        root.openedPath = f.path;
        root.phase = "opened";
        openInSystem(f.path, function (err) { root.openNote = "Couldn't open it (" + err + ") - open " + f.path + " yourself."; });
    }
    // The core opens it (xdg-open / open): the view itself can't start other programs reliably
    function openInSystem(path, onFail) {
        callVia("openPath", [path], function (raw) {
            var r = parse(raw);
            if (!r || !r.ok) onFail(r && r.error ? r.error : "no answer from zine_press_core");
        });
    }
    property string openedPath: ""
    // What to set in the viewer's print dialog: its defaults are wrong for a booklet
    function dialogText(st) {
        if (!st || !root.result) return "";
        var o = root.result.options, l = ["In the print dialog:"];
        if (st.kind === "duplex") {
            l.push("- Two-sided: on, flip on the SHORT edge.");
            l.push("- Copies: " + st.copies + (st.copies > 1 ? ", collated." : "."));
        } else {
            l.push("- One-sided. Copies: 1 - " + (st.copies > 1 ? "the " + st.copies + " copies are already in the file." : "the file is one copy."));
        }
        if (st.pageRanges) l.push("- Pages: " + st.pageRanges.map(function (r) { return r[0] + "-" + r[1]; }).join(", ") + " (only these).");
        l.push("- Scale: Actual size / 100% - never \"Fit\" or \"Shrink to page\".");
        l.push("- Paper: plain A4, NOT borderless (on many printers borderless is a slow, ink-heavy mode). Paper type: Plain."
               + (o.scale >= 0.999 ? " These files go edge to edge, so the printer's own margin (about 3 mm) trims the outer edge." : ""));
        l.push("- Landscape (the viewer usually picks it from the file).");
        if (st.kind !== "duplex") l.push("- No \"reverse order\" / \"print in reverse\": the order is already in the file.");
        l.push("- Nothing else waiting in the printer's queue: two jobs mixed together spoil the copy.");
        l.push("- Quality: Normal.");
        return l.join("\n");
    }
    Timer {
        id: printPoll
        interval: 1500; repeat: true
        onTriggered: root.callVia("printStatus", [root.printId], function (raw) {
            var r = root.parse(raw); if (!r || !r.ok) return;
            root.pstat = r;
            if (root.phase !== "printing") return;
            if (r.phase === "completed") { printPoll.stop(); root.stepDone(); }
            else if (r.phase === "canceled") { printPoll.stop(); root.toStopped(); }
            else if (r.phase === "aborted" || r.phase === "error" || r.phase === "lost") {
                printPoll.stop(); root.phase = "failed";
                root.runNote = r.error || (r.phase === "aborted" ? "the printer gave up on the job" + (r.jobMessage ? ": " + r.jobMessage : "") : "printing failed");
            }
        })
    }
    function stepDone() {
        var st = root.step;
        root.lastStep = st;
        var last = root.stepIndex + 1 >= root.steps.length;
        if (!last) root.stepIndex = root.stepIndex + 1;
        if (st.kind === "fronts") { root.phase = last ? "done" : "reload"; return; }
        if (st.kind === "backs") { root.phase = "check"; root.checkLast = last; return; }
        root.phase = last ? "done" : "ready";
    }
    property bool checkLast: false
    function afterCheck() { root.phase = root.checkLast ? "done" : "ready"; }
    function stopNow() {
        if (root.phase !== "printing") return;
        root.runNote = "stopping...";
        callVia("printCancel", [root.printId], function (raw) {
            var r = parse(raw);
            if (r && !r.ok) root.runNote = "couldn't cancel: " + (r.error || "") + " - cancel it on the printer";
        });
        // printStatus reports "canceled" and moves on to the sorting
    }
    function toStopped() {
        // every sheet of this batch starts as "needs its back"; the user corrects the rest
        var st = root.step, srt = {};
        var S = root.result.sheets;
        for (var c = 1; c <= (st.reprint ? 1 : st.copies); c++)
            for (var k = 1; k <= S; k++) srt[c + "-" + k] = st.kind === "duplex" ? "again" : "back";
        root.sort = srt; root.phase = "stopped"; root.runNote = "";
    }
    function setSort(key, v) { var m = Object.assign({}, root.sort); m[key] = v; root.sort = m; }
    readonly property var sortRows: {
        if (root.phase !== "stopped" || !root.result) return [];
        var l = [], keys = Object.keys(root.sort);
        keys.sort(function (a, b) { var x = a.split("-"), y = b.split("-"); return (x[0] - y[0]) || (x[1] - y[1]); });
        keys.forEach(function (k) { var p = k.split("-"); l.push({ key: k, copy: parseInt(p[0]), sheet: parseInt(p[1]) }); });
        return l;
    }
    // After the sorting: print exactly what's missing, then go on with the next batch
    function reprintSorted() {
        var st = root.step, list = [], dsheets = [];
        for (var k in root.sort) {
            var sh = parseInt(k.split("-")[1]), v = root.sort[k];
            if (st.kind === "duplex") { if (v === "again") dsheets.push(sh); }
            else {
                if (v === "both") list.push({ sheet: sh, side: "front" });
                if (v === "both" || v === "back") list.push({ sheet: sh, side: "back" });
            }
        }
        // drop the rest of this copy (one side) or batch (both sides): the reprint completes it
        var same = function (x) { return st.copy ? x.copy === st.copy : (st.batch !== undefined && x.batch === st.batch); };
        var next = root.stepIndex + 1;
        while (next < root.steps.length && same(root.steps[next]) && !root.steps[next].reprint && !root.steps[next].single) next++;
        root.steps = root.steps.slice(0, root.stepIndex + 1).concat(root.steps.slice(next));
        queueReprint(list, dsheets);
    }
    function queueReprint(list, dsheets) {
        if (dsheets.length) {
            // both sides: straight from booklet.pdf with page ranges, one round per repeat
            var rounds = [], count = {};
            dsheets.forEach(function (s) { count[s] = (count[s] || 0) + 1; });
            for (var round = 0; ; round++) {
                var rs = [];
                for (var s in count) if (count[s] > round) rs.push(parseInt(s));
                if (!rs.length) break;
                rs.sort(function (a, b) { return a - b; });
                rounds.push({ kind: "duplex", reprint: true, src: "main", batch: -1, copies: 1, file: "booklet.pdf", sheets: rs.length,
                              pageRanges: rs.map(function (x) { return [2 * x - 1, 2 * x]; }) });
            }
            insertSteps(rounds);
            return;
        }
        if (!list.length) { advanceAfterStop(); return; }
        root.reprintPending = true; root.runNote = "making the reprint files...";
        var o = options(); o.reprint = list;
        root.reprintResult = null;
        startMake(o);
    }
    onReprintResultChanged: {
        if (!root.reprintPending || !root.reprintResult) return;
        root.reprintPending = false; root.runNote = "";
        var l = [];
        var f = fileFrom(root.reprintResult, "reprint-fronts.pdf"), b = fileFrom(root.reprintResult, "reprint-backs.pdf");
        if (f) l.push({ kind: "fronts", reprint: true, src: "reprint", batch: -1, copies: 1, file: f.name, sheets: f.pages });
        if (b) l.push({ kind: "backs", reprint: true, src: "reprint", batch: -1, copies: 1, file: b.name, sheets: b.pages });
        insertSteps(l);
    }
    function insertSteps(l) {
        var at = root.phase === "idle" || root.stepIndex < 0 ? 0 : (root.phase === "stopped" ? root.stepIndex + 1 : root.stepIndex);
        if (root.phase === "idle" || root.phase === "done" || root.stepIndex < 0) { root.steps = l; root.stepIndex = 0; root.phase = "ready"; return; }
        root.steps = root.steps.slice(0, at).concat(l, root.steps.slice(at));
        root.stepIndex = at; root.phase = "ready";
    }
    function advanceAfterStop() {
        if (root.stepIndex + 1 >= root.steps.length) { root.phase = "done"; return; }
        root.stepIndex = root.stepIndex + 1; root.phase = "ready";
    }
    // Reprint the sides marked on the sheet plan (any time a printer is chosen and nothing is printing)
    function printMarked() {
        var list = [], ds = {};
        for (var k in root.marks) { var p = k.split("-"); list.push({ sheet: parseInt(p[0]), side: p[1] }); ds[p[0]] = true; }
        root.marks = {};
        if (root.duplex) queueReprint([], Object.keys(ds).map(function (x) { return parseInt(x); }));
        else queueReprint(list, []);
    }
    // the printer's own problems, in words
    function reasonText(rs) {
        var out = [];
        (rs || []).forEach(function (r) {
            var b = r.replace(/-(report|warning|error)$/, "");
            if (b === "media-empty" || b === "media-needed") out.push("Out of paper: add paper, push the tray fully in and press the printer's continue button (on a Brother: Mono Copy). The job carries on.");
            else if (b === "media-jam") out.push("Paper jam: clear it, then press the printer's continue button.");
            else if (b === "door-open" || b === "cover-open") out.push("A cover is open.");
            else if (b === "marker-supply-empty" || b === "toner-empty") out.push("Out of ink.");
            else if (b === "offline" || b === "shutdown") out.push("The printer is offline - is it on and plugged in?");
            else if (b === "paused") out.push("The print queue is paused on its server.");
        });
        return out.join("\n");
    }

    function mb(b) { return (b / 1048576).toFixed(1) + " MB"; }
    function folderUrl(p) { return "file://" + encodeURI(p); }
    function baseName(p) { var s = decodeURIComponent(String(p)); return s.substring(s.lastIndexOf("/") + 1); }
    function pageLabel(n) { return n === null || n === undefined ? "–" : String(n); }

    FileDialog {
        id: pdfDialog
        title: "Choose the zine (a PDF)"
        nameFilters: ["PDF files (*.pdf)"]
        onAccepted: root.pick(selectedFile)
    }

    Rectangle { anchors.fill: parent; color: Theme.palette.background }

    RowLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacing.xlarge
        spacing: Theme.spacing.xlarge

        // ══ left: the steps ══
        ScrollView {
            Layout.preferredWidth: 520
            Layout.fillHeight: true
            clip: true
            ColumnLayout {
                width: 500
                spacing: Theme.spacing.large

                LogosText { textFormat: Text.PlainText; text: "Zine Press"; font.pixelSize: Theme.typography.pageTitleText; font.weight: Font.Bold; color: Theme.palette.text }
                LogosText { textFormat: Text.PlainText;
                    Layout.fillWidth: true; Layout.preferredWidth: 100; wrapMode: Text.WordWrap
                    text: "Print a zine at home: an A5 booklet on A4 paper, folded and stitched. Pick a PDF, tell Zine Press about your printer, and it makes the files and the steps."
                    color: Theme.palette.textSecondary; font.pixelSize: Theme.typography.primaryText
                }

                // 1. the PDF
                Card {
                    title: "1. The zine"
                    LogosText { textFormat: Text.PlainText;
                        Layout.fillWidth: true; Layout.preferredWidth: 100; wrapMode: Text.WordWrap
                        text: root.pdfPath ? root.baseName(root.pdfPath) : "A PDF of the zine's pages, in reading order (cover first)."
                        color: root.pdfPath ? Theme.palette.text : Theme.palette.textTertiary; font.pixelSize: Theme.typography.primaryText
                    }
                    LogosText { textFormat: Text.PlainText;
                        visible: !!root.info; Layout.fillWidth: true; Layout.preferredWidth: 100; wrapMode: Text.WordWrap
                        text: root.info ? (root.info.pages + " pages, " + root.info.pageMm[0] + " x " + root.info.pageMm[1] + " mm - " + root.info.cut + ".  "
                                           + root.info.sheets + " sheet" + (root.info.sheets > 1 ? "s" : "") + " of A4 per copy"
                                           + (root.info.blanks ? ", with " + root.info.blanks + " blank page" + (root.info.blanks > 1 ? "s" : "") + " before the back cover" : "") + ".") : ""
                        color: Theme.palette.textSecondary; font.pixelSize: Theme.typography.secondaryText
                    }
                    LogosButton { text: root.pdfPath ? "Choose another PDF" : "Choose a PDF"; onClicked: pdfDialog.open() }
                }

                // 2. the printer
                Card {
                    title: "2. Your printer"
                    Chips {
                        label: "It prints"
                        model: [{ k: false, t: "one side at a time" }, { k: true, t: "both sides" }]
                        current: root.duplex
                        onPicked: function (k) { root.duplex = k }
                    }
                    Toggle { text: "Borderless A4 (prints to the edge)"; checked: root.borderless; onToggled: function (v) { root.borderless = v } }
                    Hint { text: root.borderless ? "Pages fill their half of the sheet; on plain A4 the printer's own margin (about 3 mm) trims the outer edge." : "Pages are shrunk to 96% so the printer's margin doesn't cut them." }
                    Toggle { text: "Flatten to 300 dpi images"; checked: root.flatten; onToggled: function (v) { root.flatten = v } }
                    Hint { text: root.flatten ? "Small files that print at the printer's own speed. Recommended." : "Vector files: sharpest, but a busy zine can take minutes per sheet in the printer driver." }
                    Chips {
                        label: "Pages"
                        model: [{ k: "auto", t: "auto" }, { k: "a5", t: "cut to A5" }, { k: "none", t: "whole page" }]
                        current: root.cut
                        onPicked: function (k) { root.cut = k }
                    }
                    Hint { text: "Auto cuts the bleed and crop marks off a page that is A5 plus bleed, and fits any other size to A5." }
                    RowLayout {
                        spacing: Theme.spacing.medium
                        LogosText { textFormat: Text.PlainText; text: "Copies"; color: Theme.palette.textSecondary; font.pixelSize: Theme.typography.primaryText }
                        Stepper { value: root.copies; from: 1; to: 50; onChanged: function (v) { root.copies = v } }
                        LogosText { textFormat: Text.PlainText; visible: root.duplex; text: "per batch"; color: Theme.palette.textSecondary; font.pixelSize: Theme.typography.primaryText }
                        Stepper { visible: root.duplex; value: root.batch; from: 1; to: 5; onChanged: function (v) { root.batch = v } }
                    }
                    Hint { visible: !root.duplex; text: "One copy at a time: its fronts, then you put the stack back, then its backs, then you check the cover sheet - and only then the next copy. A wrong reload spoils one copy, not all of them." }
                    Toggle { visible: !root.duplex; text: "Fronts last sheet first"; checked: root.reverseFronts; onToggled: function (v) { root.reverseFronts = v } }
                    Hint { visible: !root.duplex; text: "Most one-sided printers stack the output face up, last sheet on top: printing the fronts backwards makes the stack come out in order. The one-sheet test tells you." }
                }

                // 3. make
                Card {
                    title: "3. The files"
                    LogosText { textFormat: Text.PlainText; text: "Saved to"; color: Theme.palette.textTertiary; font.pixelSize: Theme.typography.secondaryText }
                    Rectangle {
                        Layout.fillWidth: true; implicitHeight: 34; radius: Theme.spacing.radiusSmall
                        color: Theme.palette.backgroundInset; border.color: Theme.palette.borderHairline
                        TextInput {
                            id: outField
                            anchors.fill: parent; anchors.leftMargin: Theme.spacing.small; anchors.rightMargin: Theme.spacing.small
                            verticalAlignment: TextInput.AlignVCenter; clip: true
                            text: root.outDir; color: Theme.palette.text; font.pixelSize: Theme.typography.secondaryText
                            onEditingFinished: root.outDir = text
                        }
                    }
                    RowLayout {
                        spacing: Theme.spacing.medium
                        LogosButton { text: root.busy ? "Making..." : "Make the files"; enabled: !!root.info && !root.busy; onClicked: root.makeNow() }
                        LogosButton { visible: !!root.result; text: "Open the folder"; onClicked: root.openInSystem(root.result.outDir, function (e) { root.error = e; }) }
                    }
                    ProgressBar {
                        visible: root.busy; Layout.fillWidth: true; from: 0; to: 1; value: root.job ? root.job.fraction : 0
                    }
                    LogosText { textFormat: Text.PlainText; visible: root.busy; text: root.job ? root.job.step : ""; color: Theme.palette.textSecondary; font.pixelSize: Theme.typography.secondaryText }
                    LogosText { textFormat: Text.PlainText; visible: root.error !== ""; Layout.fillWidth: true; Layout.preferredWidth: 100; wrapMode: Text.WordWrap; text: root.error; color: Theme.palette.error; font.pixelSize: Theme.typography.primaryText }
                    Repeater {
                        model: root.result ? root.result.files : []
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacing.small
                            readonly property bool forMe: root.duplex ? modelData.name === "booklet.pdf" : modelData.name !== "booklet.pdf"
                            LogosText { textFormat: Text.PlainText; text: modelData.name; font.weight: Font.Medium; color: forMe ? Theme.palette.primary : Theme.palette.text; font.pixelSize: Theme.typography.primaryText; Layout.preferredWidth: 150 }
                            LogosText { textFormat: Text.PlainText; Layout.fillWidth: true; Layout.preferredWidth: 100; wrapMode: Text.WordWrap; text: modelData.what + " - " + root.mb(modelData.bytes); color: Theme.palette.textSecondary; font.pixelSize: Theme.typography.secondaryText }
                        }
                    }
                }

                // 4. print from here
                Card {
                    title: "4. Print, step by step"
                    Chips {
                        enabled: root.phase === "idle" || root.phase === "done"
                        model: [{ k: "open", t: "open for printing" }, { k: "ipp", t: "send to a printer" }]
                        current: root.printMode
                        onPicked: function (k) { root.printMode = k }
                    }
                    Hint { text: root.printMode === "open"
                                 ? "Each file opens in your PDF viewer; print it from there with any printer your computer has. Zine Press tells you what to set and walks you through the batches."
                                 : "Zine Press sends the files straight to a printer it can reach (one set up on this computer, or a network printer by its address) and follows the job: progress, Stop, reprints." }
                    RowLayout {
                        visible: root.printMode === "ipp"
                        Layout.fillWidth: true
                        spacing: Theme.spacing.small
                        ComboBox {
                            id: printerBox
                            Layout.fillWidth: true
                            enabled: root.phase === "idle" || root.phase === "done"
                            model: root.printerList
                            textRole: "label"
                            displayText: root.printerList.length ? (currentIndex >= 0 ? currentText : "Choose a printer") : "No printers yet - add one by its address"
                            currentIndex: { for (var i = 0; i < root.printerList.length; i++) if (root.printerList[i].uri === root.printerUri) return i; return -1; }
                            onActivated: function (i) { root.choosePrinter(root.printerList[i].uri) }
                        }
                        SmallButton { text: "\u21bb"; onClicked: root.loadPrinters() }
                    }
                    RowLayout {
                        visible: root.printMode === "ipp"
                        Layout.fillWidth: true
                        spacing: Theme.spacing.small
                        Rectangle {
                            Layout.fillWidth: true; implicitHeight: 34; radius: Theme.spacing.radiusSmall
                            color: Theme.palette.backgroundInset; border.color: Theme.palette.borderHairline
                            TextInput {
                                id: uriField
                                anchors.fill: parent; anchors.leftMargin: Theme.spacing.small; anchors.rightMargin: Theme.spacing.small
                                verticalAlignment: TextInput.AlignVCenter; clip: true
                                color: Theme.palette.text; font.pixelSize: Theme.typography.secondaryText
                                onAccepted: root.addPrinter(text)
                                LogosText { textFormat: Text.PlainText; anchors.verticalCenter: parent.verticalCenter; visible: !uriField.text; text: "ipp://host:631/printers/NAME"; color: Theme.palette.textTertiary; font.pixelSize: Theme.typography.secondaryText }
                            }
                        }
                        LogosButton { text: "Add"; enabled: !root.printerBusy && uriField.text.length > 0; onClicked: root.addPrinter(uriField.text) }
                    }
                    LogosText { textFormat: Text.PlainText; visible: root.printMode === "ipp" && root.printerBusy; text: "Asking the printer..."; color: Theme.palette.textSecondary; font.pixelSize: Theme.typography.secondaryText }
                    LogosText { textFormat: Text.PlainText; visible: root.printMode === "ipp" && root.printerError !== ""; Layout.fillWidth: true; Layout.preferredWidth: 100; wrapMode: Text.WordWrap; text: root.printerError; color: Theme.palette.error; font.pixelSize: Theme.typography.secondaryText }

                    // the chosen printer's own choices
                    ColumnLayout {
                        visible: root.printMode === "ipp" && !!root.printer
                        Layout.fillWidth: true
                        spacing: Theme.spacing.small
                        RowLayout {
                            Layout.fillWidth: true
                            LogosText { textFormat: Text.PlainText; Layout.fillWidth: true; Layout.preferredWidth: 100; wrapMode: Text.WordWrap
                                text: root.printer ? (root.printer.makeModel || root.printer.name) + (root.printer.state === 5 ? " - stopped" : root.printer.state === 4 ? " - printing" : " - ready") : ""
                                color: Theme.palette.text; font.pixelSize: Theme.typography.secondaryText }
                            SmallButton { visible: root.printerList.some(function (p) { return p.uri === root.printerUri && p.saved; }); text: "\u00d7"; onClicked: root.forgetPrinter() }
                        }
                        RowLayout {
                            spacing: Theme.spacing.small
                            LogosText { textFormat: Text.PlainText; text: "Paper"; color: Theme.palette.textSecondary; font.pixelSize: Theme.typography.primaryText; Layout.preferredWidth: 60 }
                            ComboBox {
                                Layout.fillWidth: true
                                model: root.printer ? root.printer.sizes : []
                                textRole: "label"
                                currentIndex: root.sizeIndex
                                onActivated: function (i) { root.sizeIndex = i }
                            }
                        }
                        RowLayout {
                            spacing: Theme.spacing.small
                            visible: !!root.printer && root.printer.qualities.length > 0
                            LogosText { textFormat: Text.PlainText; text: "Quality"; color: Theme.palette.textSecondary; font.pixelSize: Theme.typography.primaryText; Layout.preferredWidth: 60 }
                            ComboBox {
                                Layout.fillWidth: true
                                model: root.printer ? root.printer.qualities : []
                                textRole: "label"
                                currentIndex: root.qualityIndex
                                onActivated: function (i) { root.qualityIndex = i }
                            }
                        }
                        Hint { visible: !!root.printer && !!root.printer.mediaType; text: root.printer && root.printer.mediaType ? "Paper type: " + root.printer.mediaType.label + "." : "" }
                        Toggle { text: "Fit to page (shrinks the page - leave off)"; checked: root.fitToPage; onToggled: function (v) { root.fitToPage = v } }
                        Hint { visible: !!root.printer && !root.printer.pdf; text: "This printer doesn't say it takes PDF files - printing may fail." }
                        Hint { visible: !!root.chosenSize && root.chosenSize.borderless
                               text: "Borderless is the printer's own borderless mode: it prints slowly and uses a lot of ink, even at Normal quality. Plain A4 at actual size looks right for a zine." }
                        Hint { visible: !!root.chosenSize && !root.chosenSize.borderless && root.filesBorderless && !root.fitToPage
                               text: "The files go edge to edge: the printer's own margin (about 3 mm) trims the outer edge. That's expected." }
                        Hint { visible: !!root.printer && root.duplex && !root.printer.duplex; text: "This printer can't print both sides: choose \"one side at a time\" above and make the files again." }
                    }

                    // the run
                    ColumnLayout {
                        visible: root.printMode === "open" || !!root.printer
                        Layout.fillWidth: true
                        spacing: Theme.spacing.small
                        LogosText { textFormat: Text.PlainText; visible: !root.result && root.phase === "idle"; Layout.fillWidth: true; Layout.preferredWidth: 100; wrapMode: Text.WordWrap
                            text: "Make the files first (3)."; color: Theme.palette.textTertiary; font.pixelSize: Theme.typography.secondaryText }
                        LogosButton { visible: root.phase === "idle" && !!root.result; text: "Print " + (root.copies > 1 ? root.copies + " copies" : "the zine"); onClicked: root.startRun() }

                        LogosText { textFormat: Text.PlainText; visible: !!root.step && root.phase !== "idle" && root.phase !== "done"; text: root.stepTitle(root.step); font.weight: Font.Bold; color: Theme.palette.text; font.pixelSize: Theme.typography.primaryText }

                        // ready: what goes in the tray
                        LogosText { textFormat: Text.PlainText; visible: root.phase === "ready"; Layout.fillWidth: true; Layout.preferredWidth: 100; wrapMode: Text.WordWrap; text: root.stepHow(root.step); color: Theme.palette.text; font.pixelSize: Theme.typography.primaryText }
                        RowLayout {
                            visible: root.phase === "ready"
                            spacing: Theme.spacing.medium
                            LogosButton { text: root.printMode === "open" ? "Open " + (root.step ? root.step.file : "") : (root.step && root.step.kind === "backs" ? "Print the backs" : root.step && root.step.kind === "fronts" ? "Print the fronts" : "Print"); onClicked: root.sendStep() }
                            LogosButton { text: "End"; onClicked: root.endRun() }
                        }

                        // open mode: the file is in the viewer
                        LogosText { textFormat: Text.PlainText; visible: root.phase === "opened"; Layout.fillWidth: true; Layout.preferredWidth: 100; wrapMode: Text.WordWrap
                            text: root.stepHow(root.step) + "\n\n" + root.dialogText(root.step)
                            color: Theme.palette.text; font.pixelSize: Theme.typography.primaryText }
                        LogosText { textFormat: Text.PlainText; visible: root.phase === "opened" && root.openNote !== ""; Layout.fillWidth: true; Layout.preferredWidth: 100; wrapMode: Text.WordWrap
                            text: root.openNote; color: Theme.palette.error; font.pixelSize: Theme.typography.secondaryText }
                        RowLayout {
                            visible: root.phase === "opened"
                            spacing: Theme.spacing.medium
                            LogosButton { text: "Printed - next"; onClicked: root.stepDone() }
                            LogosButton { text: "Open again"; onClicked: root.sendStep() }
                        }
                        RowLayout {
                            visible: root.phase === "opened"
                            spacing: Theme.spacing.medium
                            LogosButton { text: "Something went wrong"; onClicked: root.toStopped() }
                            LogosButton { text: "End"; onClicked: root.endRun() }
                        }

                        // printing
                        ProgressBar {
                            visible: root.phase === "printing"; Layout.fillWidth: true
                            from: 0; to: 1
                            indeterminate: !root.pstat || !root.pstat.done
                            value: root.pstat && root.pstat.total ? root.pstat.done / root.pstat.total : 0
                        }
                        LogosText { textFormat: Text.PlainText; visible: root.phase === "printing"; Layout.fillWidth: true; Layout.preferredWidth: 100; wrapMode: Text.WordWrap
                            text: !root.pstat ? "" : root.pstat.phase === "sending" ? "Sending to the printer..."
                                  : root.pstat.phase === "queued" ? "Waiting in the printer's queue..."
                                  : root.pstat.done > 0 ? "Sheet " + Math.min(root.pstat.done + 1, root.pstat.total) + " of " + root.pstat.total
                                  : "Printing" + (root.pstat.jobMessage ? " - " + root.pstat.jobMessage : "...")
                            color: Theme.palette.textSecondary; font.pixelSize: Theme.typography.secondaryText }
                        LogosText { textFormat: Text.PlainText; visible: root.phase === "printing" && text !== ""; Layout.fillWidth: true; Layout.preferredWidth: 100; wrapMode: Text.WordWrap
                            text: root.pstat ? root.reasonText(root.pstat.printerReasons) : ""; color: Theme.palette.error; font.pixelSize: Theme.typography.primaryText }
                        LogosButton { visible: root.phase === "printing"; text: "Stop"; onClicked: root.stopNow() }

                        // fronts are out: the stack goes back, exactly as it came out
                        LogosText { textFormat: Text.PlainText; visible: root.phase === "reload"; Layout.fillWidth: true; Layout.preferredWidth: 100; wrapMode: Text.WordWrap
                            text: (root.lastStep && root.lastStep.single ? "The front of sheet " + root.lastStep.single + " is printed." : "The fronts are printed. Check the top sheet is the cover.")
                                  + "\n\n" + root.reloadText
                                  + "\n\nIf the ink is still wet, let it dry a few minutes and fan the sheets first - damp sheets cling and pull through two at a time."
                            color: Theme.palette.text; font.pixelSize: Theme.typography.primaryText }
                        RowLayout {
                            visible: root.phase === "reload"
                            spacing: Theme.spacing.medium
                            LogosButton { text: root.printMode === "open" ? "Open " + (root.step ? root.step.file : "") : (root.step && root.step.single ? "Print its back" : "Print the backs"); onClicked: root.sendStep() }
                            LogosButton { text: "End"; onClicked: root.endRun() }
                        }

                        // backs are out: look before the next copy
                        LogosText { textFormat: Text.PlainText; visible: root.phase === "check"; Layout.fillWidth: true; Layout.preferredWidth: 100; wrapMode: Text.WordWrap
                            text: root.checkText(root.lastStep); color: Theme.palette.text; font.pixelSize: Theme.typography.primaryText }
                        RowLayout {
                            visible: root.phase === "check"
                            spacing: Theme.spacing.medium
                            LogosButton { text: root.checkLast ? "Done" : (root.step && root.step.kind === "fronts" && root.step.copy ? "Next copy (" + root.step.copy + " of " + root.step.of + ")" : "Go on"); onClicked: root.afterCheck() }
                            LogosButton { text: "Something's wrong"; onClicked: { root.stepIndex = Math.max(0, root.steps.indexOf(root.lastStep)); root.toStopped(); } }
                        }

                        // one sheet again (a jam, a double-feed, a smudge): its front, a stop, its back
                        RowLayout {
                            visible: !!root.result && (root.phase === "idle" || root.phase === "done" || root.phase === "check" || root.phase === "ready")
                            spacing: Theme.spacing.small
                            LogosText { textFormat: Text.PlainText; text: "Reprint one sheet"; color: Theme.palette.textSecondary; font.pixelSize: Theme.typography.secondaryText }
                            Stepper { value: root.oneSheet; from: 1; to: root.result ? root.result.sheets : 1; onChanged: function (v) { root.oneSheet = v } }
                            LogosText { textFormat: Text.PlainText
                                text: { var pl = root.info && root.info.plan ? root.info.plan[root.oneSheet - 1] : null;
                                        return (root.oneSheet === 1 ? "cover sheet " : "") + (pl ? "(" + root.pageLabel(pl.front[0]) + "|" + root.pageLabel(pl.front[1]) + ", back " + root.pageLabel(pl.back[0]) + "|" + root.pageLabel(pl.back[1]) + ")" : ""); }
                                color: Theme.palette.textTertiary; font.pixelSize: Theme.typography.secondaryText }
                            SmallButton { text: "\u2192"; enabled: root.printMode === "open" || !!root.printer; onClicked: root.reprintOneSheet(root.oneSheet) }
                        }

                        // stopped: sort the sheets
                        LogosText { textFormat: Text.PlainText; visible: root.phase === "stopped"; Layout.fillWidth: true; Layout.preferredWidth: 100; wrapMode: Text.WordWrap
                            text: root.step && root.step.kind === "duplex"
                                  ? (root.printMode === "open" ? "Cancel the job in the print queue (or on the printer) first. Then take" : "Stopped. Take") + " the sheets out and mark the ones to print again; the rest are done."
                                  : (root.printMode === "open" ? "Cancel the job in the print queue (or on the printer) first. Then take" : "Stopped. Take") + " the sheets out (and the ones still in the tray) and sort them: done (both sides right), needs its back (front right, back blank), or both sides again (spoiled or never printed). The page numbers tell you which sheet is which."
                            color: Theme.palette.text; font.pixelSize: Theme.typography.primaryText }
                        Repeater {
                            model: root.sortRows
                            RowLayout {
                                spacing: Theme.spacing.small
                                readonly property var plan: root.info ? root.info.plan[modelData.sheet - 1] : null
                                LogosText { textFormat: Text.PlainText; Layout.preferredWidth: 150
                                    text: (root.step && root.step.copies > 1 ? "Copy " + modelData.copy + ", " : "") + "sheet " + modelData.sheet
                                          + (plan ? "  (" + root.pageLabel(plan.front[0]) + "|" + root.pageLabel(plan.front[1]) + ")" : "")
                                    color: Theme.palette.text; font.pixelSize: Theme.typography.secondaryText }
                                Chips {
                                    model: root.step && root.step.kind === "duplex" ? [{ k: "ok", t: "done" }, { k: "again", t: "again" }]
                                                                                    : [{ k: "ok", t: "done" }, { k: "back", t: "needs back" }, { k: "both", t: "both again" }]
                                    current: root.sort[modelData.key]
                                    onPicked: function (k) { root.setSort(modelData.key, k) }
                                }
                            }
                        }
                        RowLayout {
                            visible: root.phase === "stopped"
                            spacing: Theme.spacing.medium
                            LogosButton { text: "Reprint and go on"; enabled: !root.reprintPending; onClicked: root.reprintSorted() }
                            LogosButton { text: "End"; onClicked: root.endRun() }
                        }

                        // failed
                        RowLayout {
                            visible: root.phase === "failed"
                            spacing: Theme.spacing.medium
                            LogosButton { text: "Try again"; onClicked: root.sendStep() }
                            LogosButton { text: "Skip"; onClicked: root.advanceAfterStop() }
                            LogosButton { text: "End"; onClicked: root.endRun() }
                        }

                        LogosText { textFormat: Text.PlainText; visible: root.phase === "done"; Layout.fillWidth: true; Layout.preferredWidth: 100; wrapMode: Text.WordWrap
                            text: "All printed. The printer may still be finishing the last sheets. Next: fold and bind."
                            color: Theme.palette.primary; font.pixelSize: Theme.typography.primaryText }
                        LogosButton { visible: root.phase === "done"; text: "Close"; onClicked: root.endRun() }
                        LogosText { textFormat: Text.PlainText; visible: root.runNote !== ""; Layout.fillWidth: true; Layout.preferredWidth: 100; wrapMode: Text.WordWrap
                            text: root.runNote; color: root.phase === "failed" ? Theme.palette.error : Theme.palette.textSecondary; font.pixelSize: Theme.typography.secondaryText }
                    }
                }
            }
        }

        // ══ right: the sheet plan + the guide ══
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.spacing.large

            LogosText { textFormat: Text.PlainText; text: "The sheets"; font.pixelSize: Theme.typography.panelTitleText; font.weight: Font.Bold; color: Theme.palette.text }
            LogosText { textFormat: Text.PlainText;
                Layout.fillWidth: true; Layout.preferredWidth: 100; wrapMode: Text.WordWrap
                text: root.info ? "Each A4 sheet carries four pages, two per side. Sheet 1 is the outside of the booklet." : "Choose a PDF to see which pages go on which sheet."
                color: Theme.palette.textSecondary; font.pixelSize: Theme.typography.secondaryText
            }
            Flow {
                Layout.fillWidth: true
                spacing: Theme.spacing.medium
                Repeater {
                    model: root.info ? root.info.plan : []
                    Rectangle {
                        width: 168; height: 100; radius: Theme.spacing.radiusMedium
                        color: Theme.palette.backgroundElevated; border.color: Theme.palette.borderHairline
                        Column {
                            anchors.centerIn: parent
                            spacing: 4
                            LogosText { textFormat: Text.PlainText; text: "Sheet " + (index + 1); color: Theme.palette.textTertiary; font.pixelSize: Theme.typography.secondaryText }
                            Row {
                                spacing: 6
                                Spread { label: "front"; sheet: index + 1; pageL: modelData.front[0]; pageR: modelData.front[1] }
                                Spread { label: "back"; sheet: index + 1; pageL: modelData.back[0]; pageR: modelData.back[1] }
                            }
                        }
                    }
                }
            }

            // a double-feed spoilt some sheets: mark their sides above, print just those again
            ColumnLayout {
                visible: !!root.info
                Layout.fillWidth: true
                spacing: Theme.spacing.small
                LogosText { textFormat: Text.PlainText;
                    Layout.fillWidth: true; Layout.preferredWidth: 100; wrapMode: Text.WordWrap
                    text: root.markCount === 0
                          ? "Two sheets went through at once? Tap the sides that need printing again (a blank back, or both sides of a spoilt sheet)" + ", then Print these."
                          : root.markCount + " side" + (root.markCount > 1 ? "s" : "") + " marked for reprinting."
                    color: Theme.palette.textSecondary; font.pixelSize: Theme.typography.secondaryText
                }
                RowLayout {
                    visible: root.markCount > 0
                    spacing: Theme.spacing.medium
                    LogosButton { text: root.busy ? "Making..." : "Make the reprint files"; enabled: !root.busy; onClicked: root.reprintNow() }
                    LogosButton { visible: (root.printMode === "open" || !!root.printer) && (root.phase === "idle" || root.phase === "ready" || root.phase === "done"); text: "Print these"; enabled: !root.busy; onClicked: root.printMarked() }
                    LogosButton { text: "Clear"; enabled: !root.busy; onClicked: { root.marks = {}; root.reprintResult = null } }
                }
                Repeater {
                    model: root.reprintResult ? root.reprintResult.files : []
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spacing.small
                        LogosText { textFormat: Text.PlainText; text: modelData.name; font.weight: Font.Medium; color: Theme.palette.primary; font.pixelSize: Theme.typography.primaryText; Layout.preferredWidth: 150 }
                        LogosText { textFormat: Text.PlainText; Layout.fillWidth: true; Layout.preferredWidth: 100; wrapMode: Text.WordWrap; text: modelData.what + " - " + root.mb(modelData.bytes); color: Theme.palette.textSecondary; font.pixelSize: Theme.typography.secondaryText }
                    }
                }
                LogosButton { visible: !!root.reprintResult; text: "Open the folder"; onClicked: root.openInSystem(root.reprintResult.outDir, function (e) { root.error = e; }) }
            }

            RowLayout {
                spacing: Theme.spacing.small
                Repeater {
                    model: [{ k: "print", t: "Print" }, { k: "bind", t: "Fold and bind" }, { k: "trouble", t: "If it goes wrong" }]
                    LogosButton { text: modelData.t; onClicked: root.tab = modelData.k; opacity: root.tab === modelData.k ? 1 : 0.6 }
                }
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: Theme.spacing.radiusLarge
                color: Theme.palette.backgroundElevated
                border.color: Theme.palette.borderHairline
                ScrollView {
                    id: guideScroll
                    anchors.fill: parent
                    anchors.margins: Theme.spacing.large
                    clip: true
                    contentWidth: availableWidth
                    LogosText {
                        width: guideScroll.availableWidth
                        wrapMode: Text.WordWrap
                        textFormat: Text.PlainText
                        color: Theme.palette.text
                        font.pixelSize: Theme.typography.primaryText
                        lineHeight: 1.25
                        text: root.tab === "print" ? (root.duplex ? root.guideDuplex : root.guideSimplex)
                              : root.tab === "bind" ? root.guideBind : root.guideTrouble
                    }
                }
            }
        }
    }

    // ── the guide (also written to HOW-TO-PRINT.txt with each job) ──
    readonly property string guideCommon:
        "Printer settings for every file: plain A4 paper (not borderless - on many printers that is a slow, ink-heavy mode), paper type Plain, landscape, actual size / 100% - never \"fit to page\". "
        + "Quality Normal: Fine is much slower and you won't see the difference on plain paper. "
        + "Nothing else in the printer's queue while you print.\n\n"
    readonly property string guideDuplex: guideCommon
        + "Print booklet.pdf on both sides, \"flip on the short edge\".\n\n"
        + "Then fold the stack in the middle - see Fold and bind."
    readonly property string guideSimplex: guideCommon
        + "1. The one-sheet test. Print test-sheet.pdf, page 1 only (the cover sheet's front). Draw an arrow at the top edge. "
        + "Put the sheet back in the tray the way your printer needs to print the other side - for most: printed side up, the same edge first. "
        + "Print page 2. Fold it: the inside of the cover must be page 2. If it isn't, turn the sheet the other way and note what works: "
        + "that is how the whole stack goes back in.\n\n"
        + "One copy at a time. For each copy:\n"
        + "2. Print the fronts file" + (root.reverseFronts ? " (it runs last sheet first, so the stack comes out with the cover on top)" : "") + ". Check the top sheet is the cover.\n\n"
        + "3. Put the printed stack back exactly as it came out: printed side up, not turned, not flipped, nothing else in the tray. If the ink is still wet, let it dry a few minutes and fan the sheets first: damp sheets cling and double-feed on the backs pass.\n\n"
        + "4. Print the backs file. Check the first sheet: page 2 on the back of the cover, the right way up.\n\n"
        + "5. Only then the next copy. A wrong reload then spoils one copy, not all of them.\n\n"
        + "A sheet jammed or two went through at once: Reprint one sheet (sheet 1 is the cover sheet) prints its front, stops for you to put it back, then prints its back.\n\n"
        + "Before a big run: pull the paper stack out, fan it and put it back square - most double-feeds come from sheets stuck together.\n\n"
        + "Out of paper in the middle of a job: add paper, push the tray fully in and press the button the printer asks for (on a Brother: Mono Copy). The job carries on where it stopped."
    readonly property string guideBind:
        "Fold each sheet in half on its own and crease it with a fingernail or a spoon. Nest them in order, sheet 1 on the outside.\n\n"
        + "Three-hole pamphlet stitch (needle, about 60 cm of thread):\n"
        + "1. Open the booklet in the middle. Pierce three holes along the fold: one in the middle and one 3.5 cm from each end.\n"
        + "2. From the outside, go in through the middle hole, leaving a 10 cm tail.\n"
        + "3. Come out through one end hole.\n"
        + "4. Go back in through the other end hole, passing the middle hole on the outside.\n"
        + "5. Come out through the middle hole again, on the other side of the long stitch from the tail.\n"
        + "6. Pull it snug along the fold, tie the tail and the end in a knot over the long stitch, trim.\n\n"
        + "No needle? Two staples on the fold with a long-arm stapler. Or the cardboard trick: open a normal stapler, lay the booklet open on a piece of cardboard, staple through the fold into the cardboard, then lift it off and press the staple legs flat by hand.\n\n"
        + "Trim the front edge with a sharp knife and a ruler if the inner sheets stick out (they always creep a little on thicker zines)."
    readonly property string guideTrouble:
        "White bands across photos: the print head is clogged. Run a head cleaning from the printer's menu and print again.\n\n"
        + "Printing is very slow: turn on \"Flatten to 300 dpi images\" - a zine full of detailed drawings is millions of shapes the printer driver has to draw one by one.\n\n"
        + "The backs are upside down: the stack went back in the wrong way round. Do the one-sheet test again and turn the stack the way it showed.\n\n"
        + "The page order is wrong after printing the backs: your printer stacks face down. Turn off \"Fronts last sheet first\" and make the files again.\n\n"
        + "Two sheets went through at once (a double-feed):\n"
        + "1. Cancel the job on the printer straight away.\n"
        + "2. Sort the sheets into three piles: back printed correctly (done), back still blank (needs only its back), back printed wrong (spoilt: needs both sides again). The page numbers tell you which sheet each one is - match them against the sheet plan.\n"
        + "3. On the sheet plan, tap the sides to print again and press Reprint. You get reprint-fronts.pdf and reprint-backs.pdf with just those sides.\n"
        + "4. Print the fronts and let them dry. Stack them with the blank-back sheets in sheet order, as the backs pass had them, put the stack in the same way round as before and print the backs.\n\n"
        + "Out of paper in the middle of a job: add paper, push the tray fully in and press the button the printer asks for (on a Brother: Mono Copy). The job carries on.\n\n"
        + "Pages are cut off at the edge: turn off Borderless in (2) and make the files again - they are then shrunk to 96% to clear the printer's margin.\n\n"
        + "Printing is slow and soaks the paper: check the paper size isn't a borderless one, and the quality is Normal."

    // ── small parts ──
    component Card: Rectangle {
        default property alias content: col.data
        property string title: ""
        Layout.fillWidth: true
        implicitHeight: col.implicitHeight + 2 * Theme.spacing.large
        radius: Theme.spacing.radiusLarge
        color: Theme.palette.backgroundElevated
        border.color: Theme.palette.borderHairline
        ColumnLayout {
            id: col
            anchors.fill: parent
            anchors.margins: Theme.spacing.large
            spacing: Theme.spacing.small
            LogosText { textFormat: Text.PlainText; text: parent.parent.title; font.pixelSize: Theme.typography.subtitleText; font.weight: Font.Bold; color: Theme.palette.text }
        }
    }
    component Hint: LogosText { textFormat: Text.PlainText;
        Layout.fillWidth: true; Layout.preferredWidth: 100; wrapMode: Text.WordWrap
        color: Theme.palette.textTertiary; font.pixelSize: Theme.typography.secondaryText
    }
    component Toggle: RowLayout {
        id: tg
        property string text: ""
        property bool checked: false
        signal toggled(bool v)
        spacing: Theme.spacing.small
        Rectangle {
            width: 34; height: 18; radius: 9
            color: tg.checked ? Theme.palette.primary : Theme.palette.surface
            Rectangle { width: 14; height: 14; radius: 7; y: 2; x: tg.checked ? 18 : 2; color: Theme.palette.text }
            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: tg.toggled(!tg.checked) }
        }
        LogosText { textFormat: Text.PlainText; text: tg.text; color: Theme.palette.text; font.pixelSize: Theme.typography.primaryText
            MouseArea { anchors.fill: parent; onClicked: tg.toggled(!tg.checked) } }
    }
    component Chips: RowLayout {
        id: ch
        property string label: ""
        property var model: []
        property var current
        signal picked(var k)
        spacing: Theme.spacing.small
        LogosText { textFormat: Text.PlainText; text: ch.label; color: Theme.palette.textSecondary; font.pixelSize: Theme.typography.primaryText }
        Repeater {
            model: ch.model
            Rectangle {
                readonly property bool on: modelData.k === ch.current
                implicitWidth: cl.implicitWidth + 2 * Theme.spacing.medium; implicitHeight: 28; radius: 14
                color: on ? Theme.palette.primary : "transparent"
                border.color: on ? Theme.palette.primary : Theme.palette.border
                LogosText { textFormat: Text.PlainText; id: cl; anchors.centerIn: parent; text: modelData.t; color: Theme.palette.text; font.pixelSize: Theme.typography.secondaryText }
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: ch.picked(modelData.k) }
            }
        }
    }
    component Stepper: RowLayout {
        id: st
        property int value: 1
        property int from: 1
        property int to: 10
        signal changed(int v)
        spacing: 2
        SmallButton { text: "-"; enabled: st.value > st.from; onClicked: st.changed(st.value - 1) }
        LogosText { textFormat: Text.PlainText; text: String(st.value); color: Theme.palette.text; font.pixelSize: Theme.typography.primaryText; horizontalAlignment: Text.AlignHCenter; Layout.preferredWidth: 26 }
        SmallButton { text: "+"; enabled: st.value < st.to; onClicked: st.changed(st.value + 1) }
    }
    component SmallButton: Rectangle {
        id: sb
        property string text: ""
        signal clicked()
        implicitWidth: 28; implicitHeight: 28; radius: 14
        color: "transparent"; border.color: Theme.palette.border
        opacity: enabled ? 1 : 0.4
        LogosText { textFormat: Text.PlainText; anchors.centerIn: parent; text: sb.text; color: Theme.palette.text; font.pixelSize: Theme.typography.subtitleText }
        MouseArea { anchors.fill: parent; enabled: sb.enabled; cursorShape: Qt.PointingHandCursor; onClicked: sb.clicked() }
    }
    component Spread: Item {
        id: sp
        property string label: ""
        property int sheet: 0
        property var pageL
        property var pageR
        readonly property bool marked: !!root.marks[root.markKey(sheet, label)]
        readonly property bool markable: !root.busy
        width: spc.implicitWidth; height: spc.implicitHeight
        Column {
            id: spc
            spacing: 2
            Row {
                spacing: 1
                Repeater {
                    model: [sp.pageL, sp.pageR]
                    Rectangle {
                        width: 34; height: 44; radius: 2
                        border.width: sp.marked ? 2 : 0; border.color: Theme.palette.error
                        color: modelData === null || modelData === undefined ? Theme.palette.backgroundInset : Theme.palette.surface
                        LogosText { textFormat: Text.PlainText; anchors.centerIn: parent; text: root.pageLabel(modelData); color: Theme.palette.text; font.pixelSize: Theme.typography.primaryText; font.weight: Font.Medium }
                    }
                }
            }
            LogosText { textFormat: Text.PlainText; text: sp.marked ? "reprint" : sp.label; color: sp.marked ? Theme.palette.error : Theme.palette.textTertiary; font.pixelSize: 10; anchors.horizontalCenter: parent.horizontalCenter }
        }
        MouseArea {
            anchors.fill: parent
            enabled: sp.markable; cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: root.toggleMark(sp.sheet, sp.label)
        }
    }
}
