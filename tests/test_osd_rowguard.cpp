// The per-paint check that a menu page's cursor agrees with its menumask.
//
// The interesting part of this is not the `selected != 1` comparison, it is the
// three silences: a pass that painted nothing, a page with no selectable rows,
// and a cursor parked off the mask. Each of them fires on ordinary menu use, so
// getting one wrong does not produce a missed bug -- it produces a console line
// on every menu open, which is how a diagnostic gets deleted.
//
// FALSIFIED against these mutations -- each was applied to the DUT and the named
// check failed:
//   - `writes <= 0` -> `writes < 0`          -> "a pass that painted nothing" fails
//   - dropping the cursor-on-mask guard      -> "a cursor parked off the mask" fails
//   - `menusub >= 64` -> `menusub > 64`      -> "a menusub past the mask width" fails
//   - `1 << menusub` (int, not uint64_t)     -> "the mask is tested at 63 bits wide"
//   - `selected != 1` -> `selected > 1`      -> "a menumask bit no row draws" fails
//   - `selected != 1` -> `selected < 1`      -> "two rows sharing a menusub" fails
//   - `selected != 1` -> `0`                 -> both of the above fail
//
// The second half is a source scan of menu.cpp for the bug the runtime guard
// cannot see: two branches of one if/else-if chain opening on the same
// menusub. The first branch wins, so the row's own handler is unreachable and
// the row looks like it does nothing -- CD speed on the Settings page, after
// the 2026-09-21 develop-into-master merge. Every static_assert on that page
// was green, because they check the hint table's length against the mask and
// nothing checked that the handler agrees with itself.
//
// FALSIFIED: the scan runs against the in-memory shapes below before it runs
// against the real file, and each shape names the check that must fire.

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

#include "../osd_rowguard.h"

static int failures = 0;

static void check(const char *name, int got, int want)
{
	if (!!got == !!want) printf("PASS %s\n", name);
	else { printf("FAIL %s: got %d, want %d\n", name, got, want); failures++; }
}

// --- handler chains that claim a menusub twice -------------------------------
//
// One if/else-if chain is a run of `if (` / `else if (` lines at one
// indentation depth; a fresh `if (` at that depth starts a new chain. Only a
// branch whose condition OPENS on menusub is counted -- `if (menusub == N`,
// or `if (snac && (menusub == N` as the Advanced page writes it -- because the
// main page legitimately tests the same row under different keys further into
// a condition (`c == KEY_BACKSPACE && (menusub == 2 || ...)`), and those are
// not the fault. Named rows (`cd_row`, `back_row`) count the same as literals.
//
// Comments are stripped to end of line, and `case MENU_...:` resets everything,
// so a chain never spans two pages.

struct dup_hit { std::string page, key; int first, second; };

static bool is_ident(char c) { return isalnum((unsigned char)c) || c == '_'; }

static bool leading_menusub(const std::string &s, std::string *key)
{
	// (else )?if *( *(ident *&& *()? *menusub *== *ident
	size_t i = 0;
	auto skipws = [&]() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++; };
	auto word = [&](const char *w) {
		size_t n = strlen(w);
		if (s.compare(i, n, w) != 0) return false;
		i += n; return true;
	};
	if (word("else")) skipws();
	if (!word("if")) return false;
	skipws();
	if (i >= s.size() || s[i] != '(') return false;
	i++; skipws();
	if (s.compare(i, 7, "menusub") != 0)
	{
		// one guard in front, as `snac && (menusub == ...`
		size_t j = i;
		while (j < s.size() && is_ident(s[j])) j++;
		if (j == i) return false;
		i = j; skipws();
		if (s.compare(i, 2, "&&") != 0) return false;
		i += 2; skipws();
		if (i >= s.size() || s[i] != '(') return false;
		i++; skipws();
		if (s.compare(i, 7, "menusub") != 0) return false;
	}
	i += 7; skipws();
	if (s.compare(i, 2, "==") != 0) return false;
	i += 2; skipws();
	size_t j = i;
	while (j < s.size() && is_ident(s[j])) j++;
	if (j == i) return false;
	*key = s.substr(i, j - i);
	return true;
}

static std::vector<dup_hit> scan_duplicate_branches(const std::string &text)
{
	std::vector<dup_hit> hits;
	std::string page = "?";
	std::map<size_t, std::map<std::string, int>> chains;   // depth -> key -> line
	int line = 0;
	size_t pos = 0;
	while (pos < text.size())
	{
		size_t nl = text.find('\n', pos);
		std::string l = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
		pos = (nl == std::string::npos) ? text.size() : nl + 1;
		line++;
		size_t c = l.find("//");
		if (c != std::string::npos) l.erase(c);

		size_t depth = 0;
		while (depth < l.size() && (l[depth] == ' ' || l[depth] == '\t')) depth++;
		std::string s = l.substr(depth);

		if (s.compare(0, 10, "case MENU_") == 0)
		{
			size_t e = s.find(':');
			page = s.substr(5, e == std::string::npos ? std::string::npos : e - 5);
			chains.clear();
			continue;
		}
		if (s.compare(0, 2, "if") == 0 && (s.size() == 2 || !is_ident(s[2])))
			chains[depth].clear();                       // a fresh chain at this depth
		std::string key;
		if (!leading_menusub(s, &key)) continue;
		std::map<std::string, int> &seen = chains[depth];
		std::map<std::string, int>::iterator it = seen.find(key);
		if (it != seen.end()) { dup_hit h = { page, key, it->second, line }; hits.push_back(h); }
		else seen[key] = line;
	}
	return hits;
}

static std::string read_file(const char *path)
{
	std::string out;
	FILE *f = fopen(path, "rb");
	if (!f) return out;
	char buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
	fclose(f);
	return out;
}

int main(void)
{
	// --- the two bugs this exists to catch -------------------------------
	// Both are the real shape of the MENU_MINIMIG_CHIPSET1 defect: sixteen
	// selectable rows, the cursor on row 11, and the page's row literals not
	// matching. Two rows at k == 11 invert together; a mask bit no row draws
	// inverts nothing.
	check("two rows sharing a menusub is reported",
	      osd_rowguard_warn(16, 2, 11, 0xFFFF), 1);
	check("a menumask bit no row draws is reported",
	      osd_rowguard_warn(16, 0, 11, 0xFFFF), 1);
	check("three rows sharing a menusub is reported",
	      osd_rowguard_warn(16, 3, 11, 0xFFFF), 1);

	// And the page that agrees with itself says nothing, on every row of it.
	for (unsigned k = 0; k < 16; k++)
	{
		if (osd_rowguard_warn(16, 1, k, 0xFFFF))
		{
			printf("FAIL a consistent page is silent at menusub %u\n", k);
			failures++;
		}
	}
	printf("PASS a consistent page is silent on every one of its rows\n");

	// --- the three silences ----------------------------------------------
	// A page's ...2 state handles keys and paints nothing. Every such pass would
	// otherwise report 0 selected out of 0 rows.
	check("a pass that painted nothing is silent",
	      osd_rowguard_warn(0, 0, 3, 0xFFFF), 0);
	check("and a negative count is treated the same way",
	      osd_rowguard_warn(-1, 0, 3, 0xFFFF), 0);

	// Info screens and the file browser paint rows and have no menumask; the
	// browser draws its own cursor outside this scheme entirely. These two pass
	// through the cursor-on-mask test rather than a guard of their own -- an
	// explicit `if (!menumask)` was written first and removed, because no check
	// here could tell the two versions apart. The checks stay: they pin the
	// behaviour whatever shape the implementation takes.
	check("a page with no selectable rows is silent",
	      osd_rowguard_warn(16, 0, 0, 0), 0);
	check("even when it inverted several rows",
	      osd_rowguard_warn(16, 4, 0, 0), 0);

	// A page can set menusub before the mask machinery moves it onto a
	// selectable row. Nothing inverted is correct during those passes.
	check("a cursor parked off the mask is silent",
	      osd_rowguard_warn(16, 0, 4, 0x000F), 0);
	check("and so is one past the top of the mask",
	      osd_rowguard_warn(16, 0, 31, 0x000F), 0);

	// A cursor off the mask with something inverted anyway is not this check's
	// business either -- it would mean a row inverting for a reason other than
	// the cursor, which the exemption in menu.cpp covers.
	check("a cursor off the mask with a row inverted is still silent",
	      osd_rowguard_warn(16, 1, 4, 0x000F), 0);

	// --- the mask is 64 bits wide, and the shift has to be too -------------
	// menumask is a uint64_t. `1 << menusub` with an int 1 is undefined from 31
	// up and zero from 32 up, which would silence the guard on any page using the
	// high bits -- the AmigaCD pages use up to bit 15 today, so this is a trap
	// waiting rather than a live bug.
	check("the mask is tested at 63 bits wide",
	      osd_rowguard_warn(16, 2, 63, (uint64_t)1 << 63), 1);
	check("and a set bit at 32 is a live cursor",
	      osd_rowguard_warn(16, 0, 32, (uint64_t)1 << 32), 1);
	check("while a clear bit at 32 is not",
	      osd_rowguard_warn(16, 0, 32, (uint64_t)1 << 33), 0);

	// 64 and up cannot be tested against a 64-bit mask at all. Silent, because
	// the honest reading of a cursor outside every page's row set is not "this
	// page's row literals are wrong".
	check("a menusub past the mask width is silent",
	      osd_rowguard_warn(16, 0, 64, ~(uint64_t)0), 0);
	check("and so is one far past it",
	      osd_rowguard_warn(16, 0, 4000000000u, ~(uint64_t)0), 0);

	// --- the source scan: shapes first, then the real file ---------------------
	{
		// The CD speed bug, verbatim in shape: develop's Restore state and
		// master's CD speed both on 7 in one chain.
		const char *cdspeed =
			"\tcase MENU_X2:\n"
			"\t\tif (select || minus || plus)\n"
			"\t\t{\n"
			"\t\t\tif (menusub == 0) // FastRAM\n"
			"\t\t\t{\n"
			"\t\t\t}\n"
			"\t\t\telse if (menusub == 7 && select && !minimig_ss_blocked()) // Restore state\n"
			"\t\t\t{\n"
			"\t\t\t}\n"
			"\t\t\telse if (menusub == 7) // CD speed\n"
			"\t\t\t{\n"
			"\t\t\t}\n"
			"\t\t}\n";
		check("two branches of one chain on the same menusub are reported",
		      (int)scan_duplicate_branches(cdspeed).size(), 1);

		// Named rows, as the Advanced page writes them, count the same.
		const char *named =
			"\tcase MENU_X2:\n"
			"\t\t\tif (menusub == 0)\n"
			"\t\t\t{\n"
			"\t\t\t}\n"
			"\t\t\telse if (snac && (menusub == 2 || menusub == 3))\n"
			"\t\t\t{\n"
			"\t\t\t}\n"
			"\t\t\telse if (menusub == eth_row)\n"
			"\t\t\t{\n"
			"\t\t\t}\n"
			"\t\t\telse if (menusub == eth_row && select)\n"
			"\t\t\t{\n"
			"\t\t\t}\n";
		check("a named row claimed twice is reported",
		      (int)scan_duplicate_branches(named).size(), 1);

		// The silences. Different keys on the same row, deeper in the
		// condition, are the main page's eject and external-drive bindings.
		const char *keys =
			"\tcase MENU_X2:\n"
			"\t\tif (c == KEY_BACKSPACE && menusub == 0)\n"
			"\t\t{\n"
			"\t\t}\n"
			"\t\telse if (select || ((left || right) && (menusub == 2 || menusub == 3)))\n"
			"\t\t{\n"
			"\t\t}\n"
			"\t\telse if (c == KEY_BACKSPACE && (menusub == 2 || menusub == 3))\n"
			"\t\t{\n"
			"\t\t}\n";
		check("the same row under different keys, not leading, is silent",
		      (int)scan_duplicate_branches(keys).size(), 0);

		// An outer chain and an inner one may both name row 0.
		const char *nested =
			"\tcase MENU_X2:\n"
			"\t\tif (c == KEY_BACKSPACE && menusub == 0)\n"
			"\t\t{\n"
			"\t\t}\n"
			"\t\telse if (select)\n"
			"\t\t{\n"
			"\t\t\tif (menusub == 0)\n"
			"\t\t\t{\n"
			"\t\t\t}\n"
			"\t\t}\n";
		check("the same row at two nesting depths is silent",
		      (int)scan_duplicate_branches(nested).size(), 0);

		// And two pages may of course both have a row 7.
		const char *pages =
			"\tcase MENU_X2:\n"
			"\t\t\tif (menusub == 7)\n"
			"\t\t\t{\n"
			"\t\t\t}\n"
			"\tcase MENU_Y2:\n"
			"\t\t\tif (menusub == 7)\n"
			"\t\t\t{\n"
			"\t\t\t}\n";
		check("the same row on two pages is silent",
		      (int)scan_duplicate_branches(pages).size(), 0);

		// A second chain at the same depth, after a fresh `if`, starts over.
		const char *twochains =
			"\tcase MENU_X2:\n"
			"\t\t\tif (menusub == 7)\n"
			"\t\t\t{\n"
			"\t\t\t}\n"
			"\t\t\tif (menusub == 7)\n"
			"\t\t\t{\n"
			"\t\t\t}\n";
		check("two separate chains naming one row are silent",
		      (int)scan_duplicate_branches(twochains).size(), 0);

		// A comment must not count as a branch.
		const char *commented =
			"\tcase MENU_X2:\n"
			"\t\t\tif (menusub == 7)\n"
			"\t\t\t{\n"
			"\t\t\t}\n"
			"\t\t\t// else if (menusub == 7) -- the old way\n";
		check("a commented-out branch is silent",
		      (int)scan_duplicate_branches(commented).size(), 0);

		// Now menu.cpp itself. Run from tests/, as the Makefile does.
		std::string menu = read_file("../menu.cpp");
		if (menu.empty())
		{
			printf("FAIL menu.cpp: could not read ../menu.cpp (run from tests/)\n");
			failures++;
		}
		else
		{
			std::vector<dup_hit> hits = scan_duplicate_branches(menu);
			for (size_t i = 0; i < hits.size(); i++)
				printf("FAIL menu.cpp: %s claims menusub == %s twice in one chain, lines %d and %d\n",
				       hits[i].page.c_str(), hits[i].key.c_str(), hits[i].first, hits[i].second);
			if (hits.empty()) printf("PASS menu.cpp: no handler chain claims a menusub twice\n");
			failures += (int)hits.size();
		}
	}

	if (failures) { printf("%d FAILURES\n", failures); return 1; }
	printf("ALL TESTS PASSED\n");
	return 0;
}
