# Zine Press

Print a zine at home: pick a PDF, and Zine Press makes the files for an **A5 booklet on A4 paper**,
folded in the middle and stitched or stapled (a *saddle-stitched* booklet), plus the steps to print,
fold and bind it. A Logos Basecamp app: the `zine_press_core` module does the work, the `zine_press`
view is what you click.

It came out of printing and sewing Logos' *LAN Zine, Issue 02* on a Brother DCP-T230, a one-sided
inkjet, with orange thread. Everything below was learnt doing that.

## What you get

For a PDF of the zine's pages in reading order (cover first):

| File | What it's for |
|---|---|
| `booklet.pdf` | printers that print both sides: print two-sided, **flip on the short edge** |
| `fronts.pdf`, `backs.pdf` | printers that print one side at a time: fronts first, then the stack goes back in for the backs |
| `batch-N-fronts.pdf`, `batch-N-backs.pdf` | the same, for several copies, a few whole copies per batch |
| `test-sheet.pdf` | one sheet, to find out which way your printer takes the paper back in |
| `reprint-fronts.pdf`, `reprint-backs.pdf` | after a double-feed: only the sheet sides you marked for printing again |
| `HOW-TO-PRINT.txt` | the steps for this job |

## How a booklet is laid out

Every A4 sheet carries four A5 pages, two on each side. For a 24-page zine that is 6 sheets:

```
sheet 1   front: 24 | 1     back: 2 | 23      (the outside: back cover and cover)
sheet 2   front: 22 | 3     back: 4 | 21
...
sheet 6   front: 14 | 11    back: 12 | 13     (the middle: the centre spread)
```

A page count that isn't a multiple of 4 gets blank pages just before the back cover.

Pages designed as **A5 plus bleed** (the LAN zine: 154.5 x 216 mm, with crop marks) are cut to the
A5 in the middle, which takes the bleed and the crop marks off. Plain A5 pages are used as they are;
any other size is scaled to fit A5.

## Printing on a one-sided printer

1. **The one-sheet test.** Print `test-sheet.pdf`, page 1 only. Draw an arrow at the top edge. Put the
   sheet back in the tray the way your printer needs to print the other side (for most: printed side
   up, the same edge first) and print page 2. Fold it: the inside of the cover must be page 2. If it
   isn't, turn the sheet the other way. Now you know how the whole stack goes back in.
2. **Print the fronts.** Zine Press puts them last sheet first: most one-sided printers stack the
   output face up with the last sheet on top, so the stack comes out ready for the backs.
3. **Let the fronts dry 10-15 minutes**, then fan them well before they go back in. Fresh ink makes
   sheets cling together: on the first five copies of the LAN zine, both double-feeds happened on the
   backs pass, with fronts printed a few minutes earlier.
4. **Print the backs**, with the stack back in the tray the way the test showed. The finished stack is
   in order: the cover sheet at the bottom, cover face down.

Several copies: print them in **batches of 2 or 3**. A double-feed (two sheets pulled at once) shifts
every sheet after it, and the rest of the batch is printed wrong.

**When a double-feed happens:**

1. Cancel the job on the printer straight away.
2. Sort the sheets into three piles: back printed correctly (done), back still blank (needs only its
   back), back printed wrong (spoilt: needs both sides again). The page numbers on a sheet tell you
   which sheet it is; the app's sheet plan shows them.
3. In the app, tap those sides on the sheet plan and press **Reprint**. You get `reprint-fronts.pdf`
   and `reprint-backs.pdf` with just those sides, in the same order as the full passes.
4. Print the fronts and let them dry. Stack them with the blank-back sheets in sheet order, as the
   backs pass had them, put the stack in the same way round as before and print the backs.

**Out of paper in the middle of a job:** add paper, push the tray fully in and press the button the
printer asks for (on a Brother DCP: **Mono Copy**). The job carries on where it stopped.

**Settings:** A4, landscape, actual size / 100% (never "fit to page"). Quality **Normal**: Fine is much
slower and you won't see the difference on plain paper. A borderless printer can print the pages
full size (Borderless on); otherwise they are shrunk to 96% so the margin doesn't cut them.

**Flatten to 300 dpi** (on by default): the pages are rendered to images. The LAN zine's 40 MB of
detailed artwork took minutes per sheet in the printer driver; flattened, the files are a few MB and
print at the printer's own speed.

## Printing from the app

Instead of opening the files yourself, **4. Print from here** sends them to the printer step by step:

1. **Choose a printer**: the ones set up on this computer (CUPS), or any network printer by its
   address, e.g. `ipp://printserver:631/printers/NAME` for a printer shared by another computer's CUPS.
   The paper size and quality lists come from the printer itself (for a CUPS printer, from its
   driver: a Brother DCP-T230 offers "A4 (Borderless)" and Print Quality Draft / Normal / Fine).
   Zine Press picks borderless A4 if there is one, Normal quality, and fit to page.
2. **Start** sends the fronts of the first batch and shows how far the printer has got.
3. When they're done, the app counts down **15 minutes of drying**: fan the sheets, put the stack
   back, **Continue** sends the backs. **Next batch** starts the next one.
4. **Stop** cancels the job at once, also if it is still waiting in the queue. Then you sort the
   sheets of that batch, one row per sheet: done, needs its back, or both sides again. **Reprint and
   go on** prints exactly what's missing (fronts, drying, backs) and continues with the next batch.
5. Any time, sides marked on the sheet plan can be printed with **Print these**.

A printer that prints both sides gets one step per batch: the booklet, two-sided, flip on the short
edge, with the batch's copies collated. Its reprints use page ranges of the same booklet.

If the printer runs out of paper, the app says so (from the printer's own status): add paper, push
the tray in and press its continue button (Mono Copy on a Brother).

The printer talk is plain IPP over HTTP (`zine_ipp.cpp`), with no CUPS library: it lists the local
CUPS printers, reads a printer's attributes and its CUPS driver file (PPD), and sends
Print-Job / Get-Job-Attributes / Cancel-Job. Driver options go out the way `lp -o PageSize=A4_B
-o BRResolution=Normal -o fit-to-page` sends them. Encrypted `ipps://` and printers that ask for a
password aren't supported yet. Remembered printers are kept in `~/.config/zine-press/printers.json`.

## Fold and bind

Fold each sheet on its own, crease it, and nest them in order, sheet 1 on the outside.

**Three-hole pamphlet stitch** (a needle and about 60 cm of thread):

1. Open the booklet in the middle and pierce three holes along the fold: one in the middle, one 3.5 cm
   from each end.
2. From the outside, go in through the middle hole, leaving a 10 cm tail.
3. Come out through one end hole.
4. Go back in through the other end hole, passing the middle on the outside.
5. Come out through the middle hole again, on the other side of the long stitch from the tail.
6. Pull it snug, tie the tail and the end in a knot over the long stitch, trim.

No needle: two staples on the fold with a **long-arm stapler**, or the **cardboard trick** - open a
normal stapler, lay the open booklet on cardboard, staple through the fold into the cardboard, lift it
off and press the legs flat by hand.

## If it goes wrong

- **White bands across photos:** the print head is clogged. Run a head cleaning and print again.
- **Backs upside down:** the stack went back in the wrong way round. Redo the one-sheet test.
- **Pages in the wrong order after the backs:** your printer stacks face down. Turn off "Fronts last
  sheet first" and make the files again.
- **Edges cut off:** turn Borderless off.

## Building

Two Basecamp modules, built with logos-module-builder 0.3.1 (each from its own flake):

```sh
nix build ./zine_press_core#lgx-portable   # the core: qpdf + PDFium + libjpeg, bundled
nix build ./module#lgx-portable            # the view
```

The arm64 and macOS packages are built on GitHub runners (`git tag arm64-X` / `darwin-X`) and merged
into one package per module.

The imposition is vector (qpdf, Apache-2.0): each page becomes a form XObject clipped to its A5 box,
two up on A4. Flattening renders each sheet side once with PDFium (BSD-3) and stores it as a JPEG.
`tools/zine-cli.cpp` runs the same engine from the command line, and `zine_press_core/test/plan_test.cpp`
checks the page order against the script the LAN zine was printed with.

The core's methods: `inspect(path, options)`, `make(path, options)` (returns a job id),
`job(id)`, `defaultOutDir(path)`, `about()`, and for printing `printers()`, `addPrinter(uri)`,
`forgetPrinter(uri)`, `printerInfo(uri)`, `printFile(uri, path, jobOptions)`, `printStatus(id)`,
`printCancel(id)`. Options:
`{scale, cut: auto|a5|none, pad, reverseFronts, copies, batch, flatten, dpi, quality, outDir,
reprint: [{sheet, side: front|back}]}`. With `reprint` set, `make` renders and writes only those sides
(one copy) as `reprint-fronts.pdf` / `reprint-backs.pdf`.

Later: a Logos Storage CID as the input, and publishing the printable set back to Storage.

## Licence

MIT. Links qpdf (Apache-2.0), PDFium (BSD-3-Clause) and libjpeg-turbo (IJG/BSD).
