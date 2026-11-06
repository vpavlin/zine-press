// The booklet order matches impose.py (the script the LAN zine issue 02 was printed with) and the
// rules in zine_plan.hpp.
#include "zine_plan.hpp"
#include <cstdio>

static int fails = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (!(c)) { fails++; std::printf("  FAIL %s\n", m); } else std::printf("  ok   %s\n", m); } while (0)

int main() {
    // impose.py, n = 24, 0-based: front (n-1-2i, 2i), back (2i+1, n-2-2i)
    auto p24 = zine::bookletPages(24);
    auto s24 = zine::sheets(p24);
    bool same = s24.size() == 6;
    for (int i = 0; i < 6 && same; i++)
        same = s24[i].front.left == 23 - 2 * i && s24[i].front.right == 2 * i && s24[i].back.left == 2 * i + 1 && s24[i].back.right == 22 - 2 * i;
    CHECK(same, "24 pages: 6 sheets in impose.py's order");
    CHECK(s24[0].front.left == 23 && s24[0].front.right == 0 && s24[0].back.left == 1 && s24[0].back.right == 22, "sheet 1: back cover | cover, inside cover | page 23");
    CHECK(s24[5].front.left == 13 && s24[5].front.right == 10 && s24[5].back.left == 11 && s24[5].back.right == 12, "sheet 6 (the middle): 14 | 11, 12 | 13");
    // every page exactly once
    std::vector<int> seen(24, 0);
    for (auto& s : s24) for (int x : {s.front.left, s.front.right, s.back.left, s.back.right}) seen[x]++;
    bool once = true; for (int c : seen) once = once && c == 1;
    CHECK(once, "every page appears exactly once");

    auto p22 = zine::bookletPages(22);
    CHECK(p22.size() == 24 && p22[20] == 20 && p22[21] == -1 && p22[22] == -1 && p22[23] == 21, "22 pages: 2 blanks before the back cover");
    auto s22 = zine::sheets(p22);
    CHECK(s22[0].front.left == 21 && s22[0].front.right == 0, "...the back cover stays on the outside");
    auto p1 = zine::bookletPages(1);
    CHECK(p1.size() == 4 && p1[0] == 0 && p1[1] == -1, "1 page: a single sheet, the rest blank");
    CHECK(zine::bookletPages(8, false).back() == 7, "8 pages: no padding");

    auto ps = zine::passes(6, 5, 2, true);
    CHECK(ps.size() == 3 && ps[0].copies == 2 && ps[2].copies == 1 && ps[2].firstCopy == 5, "5 copies in batches of 2: 2 + 2 + 1");
    CHECK(ps[0].fronts.size() == 12 && ps[0].fronts.front() == 5 && ps[0].fronts.back() == 0 && ps[0].backs.front() == 0 && ps[0].backs.back() == 5,
          "fronts last-to-first, backs first-to-last, copies collated");
    auto pn = zine::passes(6, 1, 3, false);
    CHECK(pn[0].fronts.front() == 0, "reverse off: fronts in order");

    CHECK(zine::guessFit(154.5, 216).kind == "a5-bleed", "154.5 x 216 mm: A5 + 3 mm bleed");
    CHECK(zine::guessFit(148, 210).kind == "a5", "148 x 210 mm: A5");
    CHECK(zine::guessFit(210, 297).kind == "other", "A4: scaled to fit");

    std::printf("%d/%d checks passed\n%s\n", checks - fails, checks, fails ? "PLAN FAILED" : "PLAN GREEN");
    return fails ? 1 : 0;
}
