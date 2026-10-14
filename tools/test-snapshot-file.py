#!/usr/bin/env python3
"""FileOut must leave the physical file identical to the logical document.

Regression test: the snapshot walker writes entries incrementally and rewinds
to an entry mark when one overflows the budget. Past ~1 KB of staged output
the mark sits in the already-flushed file region, so the rewind truncates the
file with ftruncate(). Without flushing stdio's own buffer first, the fseek()
after the truncate pushes stale buffered bytes back past the new end: the file
stays longer than the logical length with shifted content. The badge then
checksums to EOF while the transfer only covers the logical prefix, which the
station reports as a CRC error on every large sync.

Drives the real main/apps/json_util.cpp through that shape (small incremental
writes, mid-entry overflow, rewind, stub) and asserts the physical file equals
the logical content in both size and bytes.
"""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="badge-fileout-test-") as directory:
    out = Path(directory)
    source = r'''#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>
#include "main/apps/json_util.h"

static uint32_t s_rng = 0x12345678;
static uint32_t rnd()
{
    s_rng = s_rng * 1103515245u + 12345u;
    return s_rng >> 16;
}

// Writes entries the way the snapshot walker does: incrementally in small
// slices, rewinding to the entry mark and writing a stub when one overflows.
// Mirrors the logical content in `expect` and returns file-region rewinds.
static int run_case(const char *fpath, size_t base, size_t cap, size_t reserve,
                    int entry_count, uint32_t entry_max, std::string &expect)
{
    FILE *f = fopen(fpath, "wb");
    assert(f);
    uint8_t zeros[64] = {};
    for (size_t left = base; left > 0;) {
        size_t n = left < sizeof(zeros) ? left : sizeof(zeros);
        assert(fwrite(zeros, 1, n, f) == n);
        left -= n;
    }
    uint8_t stage[1024];
    FileOut o;
    fout_init(o, f, stage, sizeof(stage), cap, reserve, base);
    assert(!o.io_error);
    expect.clear();

    int rewinds_file = 0;
    for (int i = 0; i < entry_count; ++i) {
        size_t total = 100 + rnd() % entry_max;
        std::string entry = "{\"e" + std::to_string(i) + "\":\"";
        while (entry.size() < total - 3) entry += (char)('a' + rnd() % 26);
        entry += "\"},";

        size_t mark = fout_mark(o);
        size_t elen = expect.size();
        size_t pos = 0;
        bool ok_all = true;
        while (pos < entry.size()) {
            size_t n = 1 + rnd() % 53;
            if (n > entry.size() - pos) n = entry.size() - pos;
            if (!fout_putn(o, entry.data() + pos, n)) { ok_all = false; break; }
            expect.append(entry.data() + pos, n);
            pos += n;
        }
        if (ok_all) continue;
        size_t file_len_before = o.file_len;
        assert(fout_rewind(o, mark));
        if (mark < file_len_before) ++rewinds_file;
        expect.resize(elen);
        const std::string stub = "{\"stub\":1},";
        if (fout_putn(o, stub.data(), stub.size())) expect.append(stub);
        // else: no room even for the stub, like the real walker
    }
    assert(fout_flush(o));
    assert(!o.io_error);
    size_t logical = fout_len(o);
    fclose(f);

    FILE *r = fopen(fpath, "rb");
    assert(r);
    assert(fseek(r, 0, SEEK_END) == 0);
    long phys = ftell(r);
    assert(phys == (long)(base + logical));
    assert(fseek(r, (long)base, SEEK_SET) == 0);
    std::string got(logical, '\0');
    assert(logical == 0 || fread(got.data(), 1, logical, r) == logical);
    // No trailing garbage past the logical end.
    assert(fgetc(r) == EOF);
    fclose(r);
    assert(got == expect);
    return rewinds_file;
}

int main()
{
    std::string fpath = (std::string("/tmp/fileout-test-")
                         + std::to_string((long)getpid()) + ".bin");
    std::string expect;
    // Snapshot scale: multi-KB entries against a 20 KB cap. Incremental writes
    // push several entry starts into the flushed file region, so rewinds must
    // truncate the file itself.
    assert(run_case(fpath.c_str(), 80, 20 * 1024, 512, 60, 7000, expect) > 0);
    // Small scale: rewinds stay inside the staging buffer.
    assert(!expect.empty());
    run_case(fpath.c_str(), 16, 4 * 1024, 256, 20, 700, expect);
    remove(fpath.c_str());
    printf("fileout: physical file equals logical content at both scales\n");
    return 0;
}
'''
    (out / "test.cpp").write_text(source)
    include = ["-I", str(root)]
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", *include,
                    str(out / "test.cpp"), str(root / "main/apps/json_util.cpp"),
                    "-o", str(out / "test")], check=True)
    subprocess.run([str(out / "test")], check=True)
    print("fileout: physical/logical identity, stage and file rewinds passed")
