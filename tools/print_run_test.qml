import QtQuick
// Run: QT_QPA_PLATFORM=offscreen QML_IMPORT_PATH=<qtdeclarative>/lib/qt-6/qml:<logos-design-system>/src/qml qml tools/print_run_test.qml
// (Qt 6 with QT_FORCE_STDERR_LOGGING=1 to see the lines). No printer, no core: "open for printing" mode only.
// Drives Zine Press's print run (open mode, no printer) through two copies + a single-sheet reprint
Item {
    width: 1200; height: 800
    property int fails: 0
    function check(c, l) { console.log((c ? "  ok   " : "  FAIL ") + l); if (!c) fails++; }
    Loader { id: ld; source: "../module/Main.qml" }
    Timer { interval: 300; running: true; onTriggered: parent.run() }
    function plan6() { var p = []; var N = 24; for (var i = 0; i < 6; i++) p.push({ front: [N - 2 * i, 1 + 2 * i], back: [2 + 2 * i, N - 1 - 2 * i] }); return p; }
    function res(bp) { return { sheets: 6, outDir: "/x", files: [{ name: "fronts.pdf", pages: 6, path: "/x/fronts.pdf" }, { name: "backs.pdf", pages: bp, path: "/x/backs.pdf" }],
                                options: { reverseFronts: true, scale: 0.96, copies: 1, batch: 1 } }; }
    function run() {
        var m = ld.item;
        check(!!m, "Main.qml loads");
        if (!m) { Qt.exit(1); return; }
        check(m.fitToPage === false, "actual size by default (fit off)");
        m.copies = 2; m.duplex = false;
        check(m.options().copies === 1 && m.options().batch === 1, "one side: the files hold ONE copy");
        m.duplex = true; check(m.options().copies === 2, "both sides: copies in the file as before"); m.duplex = false;
        m.printMode = "open"; m.info = { plan: plan6(), sheets: 6 };
        m.result = res(5);
        m.startRun();
        check(m.phase === "failed" && m.runNote.indexOf("backs.pdf has 5") >= 0, "fronts/backs disagreeing on sheets: refused (" + m.runNote + ")");
        m.endRun(); m.result = res(6);
        m.startRun();
        check(m.steps.length === 4 && m.phase === "ready", "2 copies = fronts, backs, fronts, backs (" + m.steps.length + ")");
        check(m.stepTitle(m.step) === "Copy 1 of 2: fronts (6 sheets)", "title: " + m.stepTitle(m.step));
        m.sendStep(); check(m.phase === "opened", "fronts opened");
        check(m.dialogText(m.step).indexOf("plain A4, NOT borderless") >= 0, "dialog: plain A4, not borderless");
        m.stepDone();
        check(m.phase === "reload" && m.step.kind === "backs" && m.step.copy === 1, "fronts out -> stop: put the stack back (" + m.phase + ")");
        check(m.reloadText.indexOf("exactly as it came out: printed side up, not turned, not flipped, nothing else in the tray") >= 0, "reload text says it exactly");
        m.sendStep(); m.stepDone();
        check(m.phase === "check" && !m.checkLast, "backs out -> stop: check the cover sheet");
        check(m.checkText(m.lastStep).indexOf("page 2 on the back of the cover") >= 0, "check text: page 2 on the back of the cover");
        check(m.step.kind === "fronts" && m.step.copy === 2, "the next copy waits for the button");
        m.afterCheck();
        check(m.phase === "ready" && m.stepTitle(m.step) === "Copy 2 of 2: fronts (6 sheets)", "next copy: " + m.stepTitle(m.step));
        m.sendStep(); m.stepDone(); m.sendStep(); m.stepDone();
        check(m.phase === "check" && m.checkLast, "last copy's backs -> check, then done");
        m.afterCheck(); check(m.phase === "done", "done");
        // one sheet: sheet 6 of 6 (24 pages: 14|11, back 12|13)
        m.reprintOneSheet(6);
        check(m.steps.length === 2 && m.phase === "ready", "reprint one sheet: front then back");
        check(JSON.stringify(m.steps[0].pageRanges) === "[[1,1]]" && JSON.stringify(m.steps[1].pageRanges) === "[[6,6]]",
              "sheet 6: fronts page 1 (reversed), backs page 6 - " + JSON.stringify(m.steps[0].pageRanges) + " " + JSON.stringify(m.steps[1].pageRanges));
        check(m.stepTitle(m.step) === "Sheet 6: its front", "title: " + m.stepTitle(m.step));
        m.sendStep(); m.stepDone();
        check(m.phase === "reload" && m.step.single === 6, "its front out -> stop: put it back");
        m.sendStep(); m.stepDone();
        check(m.phase === "check" && m.checkText(m.lastStep).indexOf("pages 12|13 on the back of 14|11") >= 0, "check: " + m.checkText(m.lastStep));
        m.endRun(); m.reprintOneSheet(1);
        check(JSON.stringify(m.steps[0].pageRanges) === "[[6,6]]" && JSON.stringify(m.steps[1].pageRanges) === "[[1,1]]", "sheet 1 (cover): fronts page 6, backs page 1");
        // a wrong copy: Something's wrong at the check keeps the next copy
        m.endRun(); m.startRun(); m.sendStep(); m.stepDone(); m.sendStep(); m.stepDone();
        m.stepIndex = Math.max(0, m.steps.indexOf(m.lastStep)); m.toStopped();
        check(m.phase === "stopped", "Something's wrong -> sort the sheets");
        var before = m.steps.length; m.reprintSorted();
        check(m.steps.length === before && m.steps[2].copy === 2, "sorting copy 1 keeps copy 2's steps (" + m.steps.length + ")");
        console.log(fails ? "VIEW RUN TEST FAILED" : "VIEW RUN TEST GREEN");
        Qt.exit(fails ? 1 : 0);
    }
}
