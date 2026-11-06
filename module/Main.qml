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
        return { scale: root.borderless ? 1.0 : 0.96, cut: root.cut, pad: true, reverseFronts: root.reverseFronts,
                 copies: root.copies, batch: root.batch, flatten: root.flatten, dpi: 300, outDir: root.outDir };
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
                else root.error = (r.result && r.result.error) || "making the booklet failed";
            }
        })
    }
    // options that change the sheet plan: ask again
    onCutChanged: { root.marks = {}; root.reprintResult = null; inspectNow() }

    function fileNamed(n) {
        if (!root.result || !root.result.files) return null;
        for (var i = 0; i < root.result.files.length; i++) if (root.result.files[i].name === n) return root.result.files[i];
        return null;
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
                    Hint { text: root.borderless ? "Pages fill their half of the sheet." : "Pages are shrunk to 96% so the printer's margin doesn't cut them." }
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
                        LogosText { textFormat: Text.PlainText; visible: !root.duplex; text: "per batch"; color: Theme.palette.textSecondary; font.pixelSize: Theme.typography.primaryText }
                        Stepper { visible: !root.duplex; value: root.batch; from: 1; to: 5; onChanged: function (v) { root.batch = v } }
                    }
                    Hint { visible: !root.duplex; text: "A double-feed (two sheets pulled at once) ruins every sheet after it in the batch: keep batches at 2-3 copies." }
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
                        LogosButton { visible: !!root.result; text: "Open the folder"; onClicked: Qt.openUrlExternally(root.folderUrl(root.result.outDir)) }
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
                visible: !!root.info && !root.duplex
                Layout.fillWidth: true
                spacing: Theme.spacing.small
                LogosText { textFormat: Text.PlainText;
                    Layout.fillWidth: true; Layout.preferredWidth: 100; wrapMode: Text.WordWrap
                    text: root.markCount === 0
                          ? "Two sheets went through at once? Tap the sides that need printing again (a blank back, or both sides of a spoilt sheet), then Reprint."
                          : root.markCount + " side" + (root.markCount > 1 ? "s" : "") + " marked for reprinting."
                    color: Theme.palette.textSecondary; font.pixelSize: Theme.typography.secondaryText
                }
                RowLayout {
                    visible: root.markCount > 0
                    spacing: Theme.spacing.medium
                    LogosButton { text: root.busy ? "Making..." : "Reprint"; enabled: !root.busy; onClicked: root.reprintNow() }
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
                LogosButton { visible: !!root.reprintResult; text: "Open the folder"; onClicked: Qt.openUrlExternally(root.folderUrl(root.reprintResult.outDir)) }
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
        "Printer settings for every file: A4 paper, landscape, actual size / 100% - never \"fit to page\". "
        + (root.borderless ? "Borderless printing on. " : "")
        + "Quality Normal: Fine is much slower and you won't see the difference on plain paper.\n\n"
    readonly property string guideDuplex: guideCommon
        + "Print booklet.pdf on both sides, \"flip on the short edge\".\n\n"
        + "Then fold the stack in the middle - see Fold and bind."
    readonly property string guideSimplex: guideCommon
        + "1. The one-sheet test. Print test-sheet.pdf, page 1 only (the cover sheet's front). Draw an arrow at the top edge. "
        + "Put the sheet back in the tray the way your printer needs to print the other side - for most: printed side up, the same edge first. "
        + "Print page 2. Fold it: the inside of the cover must be page 2. If it isn't, turn the sheet the other way and note what works: "
        + "that is how the whole stack goes back in.\n\n"
        + "2. Print the fronts file" + (root.reverseFronts ? " (it runs last sheet first, so the stack comes out ready for the backs)" : "") + ".\n\n"
        + "3. Let the fronts dry 10-15 minutes, then fan them well before they go back in. Fresh ink makes sheets cling together: double-feeds happen on the backs pass.\n\n"
        + "4. Put the stack back in the tray the way the test showed, and print the backs file.\n\n"
        + "5. The finished stack is in order: the cover sheet at the bottom, cover face down.\n\n"
        + (root.copies > 1 ? "With " + root.copies + " copies the files come in batches of " + root.batch + " (each batch holds whole copies). Print one batch's fronts and backs before the next: a double-feed then only ruins one batch.\n\n" : "")
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
        + "Pages are cut off at the edge: turn off Borderless (or your printer has a wider margin than most - try the whole page option)."

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
        readonly property bool markable: !root.duplex && !root.busy
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
