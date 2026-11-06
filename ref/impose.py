"""A5 saddle-stitch booklet on A4 from a PDF whose pages are A5 plus bleed.

Each page is cut to A5, centred (where the crop marks are), scaled by SCALE
so the printer's unprintable edge does not eat into it, and placed two up on
A4 landscape in booklet order. Writes booklet.pdf (both sides, for a duplex
printer), and fronts.pdf / backs.pdf for printing by hand on a simplex one.
"""
import sys
from pypdf import PdfReader, PdfWriter, Transformation, PageObject
from pypdf.generic import RectangleObject

src, out, scale = sys.argv[1], sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 0.96
A5W, A5H = 419.53, 595.28
A4W, A4H = 841.89, 595.28
r = PdfReader(src)
n = len(r.pages)
assert n % 4 == 0, f"{n} pages: a booklet needs a multiple of 4"

def cut(i):
    p = r.pages[i]
    w, h = float(p.mediabox.width), float(p.mediabox.height)
    x0, y0 = (w - A5W) / 2, (h - A5H) / 2
    box = RectangleObject([x0, y0, x0 + A5W, y0 + A5H])
    p.mediabox = box; p.cropbox = box; p.trimbox = box
    return p, x0, y0

def sheet(left, right):
    s = PageObject.create_blank_page(width=A4W, height=A4H)
    for slot, idx in ((0, left), (1, right)):
        p, x0, y0 = cut(idx)
        half = A4W / 2
        tx = slot * half + (half - A5W * scale) / 2
        ty = (A4H - A5H * scale) / 2
        t = Transformation().translate(-x0, -y0).scale(scale, scale).translate(tx, ty)
        s.merge_transformed_page(p, t)
    return s

both, fronts, backs = PdfWriter(), PdfWriter(), PdfWriter()
for i in range(n // 4):
    f = sheet(n - 1 - 2 * i, 2 * i)          # 1-indexed: n-2i | 1+2i
    b = sheet(2 * i + 1, n - 2 - 2 * i)      # 1-indexed: 2+2i | n-1-2i
    both.add_page(f); both.add_page(b)
    fronts.add_page(f); backs.add_page(b)
for w, name in ((both, "booklet"), (fronts, "fronts"), (backs, "backs")):
    with open(f"{out}/{name}.pdf", "wb") as fh:
        w.write(fh)
print(f"{n} pages -> {n // 4} sheets")
