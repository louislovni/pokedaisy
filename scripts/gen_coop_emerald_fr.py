#!/usr/bin/env python3
"""Co-op joined battles on French Emerald (BPEF): pk_coop_emerald_fr.h, gen_coop_emerald.py's header for the French cart.

    scripts/gen_coop_emerald_fr.py <pokeemerald dir> <French Emerald .gba>

pret/pokeemerald builds only the English cart, so every ROM address the English header names is found in the French cart:
a function by its code (its bytes with the calls and literal pools masked, searched near where the cart's code has moved
to), a ROM table by the literal a matched function loads it with. RAM is English's (the European carts share it). Then
gen_coop_emerald.py runs as for English over the French bytes and those addresses: every code site, CRC and the bridge's
assembly come out of the French cart the same way, and each check it makes (a site's shape, a literal pointing at the
function it should) checks the mapping too. Never by hand.
"""
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_coop_emerald as g  # noqa: E402

FRENCH_SHA1 = "ca666651374d89ca439007bed54d839eb7bd14d0"
HERE = os.path.dirname(os.path.abspath(__file__))
OUT_FR = os.path.join(HERE, "..", "app", "src", "main", "cpp", "pk_coop_emerald_fr.h")
ROM_BASE = 0x08000000
CODE_END = 0x08300000   # the English cart's code ends below this (data after)


def nm_sized(elf):
    out = subprocess.run(["arm-none-eabi-nm", "-S", "--defined-only", elf], check=True, capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4:
            syms.setdefault(p[3], []).append((int(p[0], 16), int(p[1], 16), p[2]))
        elif len(p) == 3:
            syms.setdefault(p[2], []).append((int(p[0], 16), 0, p[1]))
    return syms


class Mapper:
    def __init__(self, en, fr, elf):
        self.en, self.fr = en, fr
        self.syms = nm_sized(elf)
        self.code = sorted({a & ~1 for v in self.syms.values() for a, s, t in v if t in "Tt" and ROM_BASE <= a < ROM_BASE + len(en)})
        self.size = {}
        for v in self.syms.values():
            for a, s, t in v:
                if t in "Tt":
                    self.size[a & ~1] = max(self.size.get(a & ~1, 0), s)
        self.anchors = []
        self.cache = {}

    def range_len(self, a):
        """The function's bytes up to the next code symbol (its literal pool included)."""
        import bisect
        i = bisect.bisect_right(self.code, a)
        nxt = self.code[i] if i < len(self.code) else ROM_BASE + len(self.en)
        return nxt - a

    def masked(self, a, n):
        o = a - ROM_BASE
        code = self.en[o:o + n]
        m = bytearray(n)
        i = 0
        while i + 3 < n:
            h1 = code[i] | code[i + 1] << 8
            h2 = code[i + 2] | code[i + 3] << 8
            if h1 & 0xF800 == 0xF000 and h2 & 0xF800 == 0xF800:
                m[i:i + 4] = b"\1\1\1\1"
                i += 4
                continue
            i += 2
        for i in range(0, n - 3, 2):
            if (a + i) % 4 == 0 and code[i + 3] in (0x08, 0x09, 0x02, 0x03):
                m[i:i + 4] = b"\1\1\1\1"
        return code, m

    def search(self, a, lo, hi):
        n = self.range_len(a)
        code, m = self.masked(a, n)
        best, cur, run = (0, 0), 0, 0
        for i in range(n + 1):
            if i < n and not m[i]:
                cur += 1
            else:
                if cur > best[1]:
                    best = (i - cur, cur)
                cur = 0
        st, ln = best
        if ln < 2:
            return []
        pat = code[st:st + ln]
        hits, s = [], max(0, lo - ROM_BASE)
        hi_o = min(len(self.fr), hi - ROM_BASE)
        while True:
            j = self.fr.find(pat, s, hi_o)
            if j < 0:
                break
            c = j - st
            if c >= 0 and c % 2 == 0 and all(m[k] or self.fr[c + k] == code[k] for k in range(n)):
                hits.append(c + ROM_BASE)
            s = j + 1
        return hits

    def delta_near(self, a):
        if not self.anchors:
            return 0
        return min(self.anchors, key=lambda x: abs(x[0] - a))[1]

    def func(self, a):
        a &= ~1
        if a in self.cache:
            return self.cache[a]
        guess = a + self.delta_near(a)
        for win in (0x200, 0x2000, 0x20000):
            hits = self.search(a, guess - win, guess + win)
            if len(hits) == 1:
                break
            if len(hits) > 1:
                hits = [min(hits, key=lambda h: abs(h - guess))] if win == 0x200 else hits
                if len(hits) == 1:
                    break
        if len(hits) != 1:
            g.die("no single French match for the function at 0x%08X (%d)" % (a, len(hits)))
        self.cache[a] = hits[0]
        self.anchors.append((a, hits[0] - a))
        return hits[0]

    def seed(self):
        """Anchors over the whole code: every ~200th function of 48+ bytes matched in the whole cart, kept when unique."""
        for a in self.code[::200]:
            if a >= CODE_END or self.range_len(a) < 48:
                continue
            hits = self.search(a, ROM_BASE, ROM_BASE + len(self.fr))
            if len(hits) == 1:
                self.anchors.append((a, hits[0] - a))

    def data_by_bytes(self, d, n=48):
        """Data nothing in the code loads (a battle script another script jumps to): its own bytes, pointers masked
        (any 4 bytes ending in 0x08 / 0x09), found once in the French cart."""
        o = d - ROM_BASE
        b = self.en[o:o + n]
        ext = self.en[o:o + n + 3]
        m = bytearray(n + 3)
        for i in range(n):
            if ext[i + 3] in (0x08, 0x09):
                m[i:i + 4] = b"\1\1\1\1"
        m = m[:n]
        runs, cur = [], 0
        best = (0, 0)
        for i in range(n + 1):
            if i < n and not m[i]:
                cur += 1
            else:
                if cur > best[1]:
                    best = (i - cur, cur)
                cur = 0
        st, ln = best
        hits, s = [], 0
        while True:
            j = self.fr.find(b[st:st + ln], s)
            if j < 0:
                break
            c = j - st
            if c >= 0 and all(m[k] or self.fr[c + k] == b[k] for k in range(n)):
                hits.append(c + ROM_BASE)
            s = j + 1
        if len(hits) != 1:
            g.die("data 0x%08X: %d French matches of its bytes" % (d, len(hits)))
        return hits[0]

    def data(self, d):
        """A ROM table / script: the word at the same place of the French copy of a function that loads it from its pool."""
        found = {}
        for o in range(0, CODE_END - ROM_BASE, 4):
            if struct.unpack_from("<I", self.en, o)[0] != d:
                continue
            a = ROM_BASE + o
            import bisect
            i = bisect.bisect_right(self.code, a) - 1
            if i < 0:
                continue
            f = self.code[i]
            if a - f > 0x1000:
                continue
            try:
                ff = self.func(f)
            except SystemExit:
                continue
            v = struct.unpack_from("<I", self.fr, ff + (a - f) - ROM_BASE)[0]
            found[v] = found.get(v, 0) + 1
            if len(found) == 1 and found[v] >= 3:
                break
        if not found:
            for n in (48, 32, 16):
                try:
                    return self.data_by_bytes(d, n)
                except SystemExit:
                    pass
            return None
        if len(found) > 1:
            g.die("table 0x%08X: the French literals disagree: %s" % (d, {hex(k): n for k, n in found.items()}))
        return next(iter(found))


def main():
    if len(sys.argv) != 3:
        g.die("usage: gen_coop_emerald_fr.py <pokeemerald dir> <French Emerald .gba>")
    decomp = os.path.abspath(sys.argv[1])
    with open(os.path.join(decomp, "pokeemerald.gba"), "rb") as f:
        en = f.read()
    with open(sys.argv[2], "rb") as f:
        fr = f.read()
    if hashlib.sha1(fr).hexdigest() != FRENCH_SHA1:
        g.die("%s isn't French Emerald (SHA-1 %s)" % (sys.argv[2], FRENCH_SHA1))
    elf = os.path.join(decomp, "pokeemerald.elf")
    m = Mapper(en, fr, elf)
    m.seed()
    en_syms = g.read_symbols(elf)

    wanted_code = {elf_name for _, elf_name in g.FUNCTIONS} | set(g.LOWEST_SYMBOLS) | {"ChooseHalfPartyForBattle"}
    wanted_code |= {fn for _, fn in g.STORY_SCRIPT_CMDS}
    wanted_data = set(g.ROM_DATA) | set(g.BATTLE_SCRIPTS) | {"gSpecials"}
    remapped = {}
    for name in sorted(wanted_code):
        out = []
        for a, t in en_syms.get(name, []):
            if t in "Tt" and ROM_BASE <= a < ROM_BASE + len(en):
                fa = m.func(a)
                out.append((fa | (a & 1), t))
                out.append((fa + m.range_len(a & ~1), "t"))   # its end, for gen's ranges
        remapped[name] = [x for x in out if True]
    ends = {}
    for name in list(remapped):
        keep = [x for x in remapped[name] if x[1] in "Tt"]
        remapped[name] = [keep[i] for i in range(0, len(keep), 2)]
        for i in range(1, len(keep), 2):
            ends["__end_%s_%d" % (name, i)] = [(keep[i][0], "t")]
    for name in sorted(wanted_data):
        for a, t in en_syms.get(name, []):
            if ROM_BASE <= a < ROM_BASE + len(en):
                v = m.data(a)
                if v is None:
                    g.die("%s (0x%08X): no function loads it" % (name, a))
                remapped.setdefault(name, []).append((v, t))
                print("%-36s 0x%08X -> 0x%08X" % (name, a, v))
    for name, v in en_syms.items():
        if name not in remapped and all(not (ROM_BASE <= a < ROM_BASE + len(en)) for a, t in v):
            remapped[name] = v   # RAM: English's
    remapped.update(ends)
    for name in sorted(wanted_code):
        for a, t in en_syms.get(name, []):
            if t in "Tt" and ROM_BASE <= a < ROM_BASE + len(en):
                print("%-36s 0x%08X -> 0x%08X" % (name, a & ~1, m.func(a)))

    # gen_coop_emerald over the French cart: its ROM in a copy of the decomp dir, its symbols remapped.
    with tempfile.TemporaryDirectory() as tmp:
        for entry in os.listdir(decomp):
            if entry not in ("pokeemerald.gba",):
                os.symlink(os.path.join(decomp, entry), os.path.join(tmp, entry))
        with open(os.path.join(tmp, "pokeemerald.gba"), "wb") as f:
            f.write(fr)
        real_read = g.read_symbols
        g.read_symbols = lambda path: remapped if os.path.basename(path) == "pokeemerald.elf" else real_read(path)
        g.RETAIL_SHA1 = FRENCH_SHA1
        g.OUT_H = OUT_FR
        g.write_kotlin = lambda rom, elf: None
        sys.argv = [sys.argv[0], tmp]
        g.main()
    with open(OUT_FR) as f:
        text = f.read()
    text = text.replace("built byte for byte as retail English\n// Emerald (BPEE", "built byte for byte as retail English,\n// mapped onto French Emerald by scripts/gen_coop_emerald_fr.py (BPEF")
    with open(OUT_FR, "w", newline="\n") as f:
        f.write(text)


if __name__ == "__main__":
    main()
