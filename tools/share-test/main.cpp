// Host test harness for the App Share wire format and bundle codec.
//
// app_share_proto.h and share_bundle.{h,cpp} are deliberately free of LVGL
// and ESP-IDF so they build here with nothing but a C++17 compiler — the
// same arrangement as tools/chess-perft. Run this before touching firmware:
// a codec bug found in 200 ms beats the same bug found through two badges
// behind a multi-minute idf.py build.
//
//   make && ./share-test

#include "app_share_proto.h"
#include "share_bundle.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail = 0;
static int g_pass = 0;

static void ok(bool cond, const char *what)
{
    if (cond) {
        g_pass++;
        return;
    }
    g_fail++;
    printf("  FAIL  %s\n", what);
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("  SETUP FAIL  cannot write %s\n", path);
        exit(1);
    }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static void test_crc()
{
    // Standard check value.
    ok(share_bundle_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u,
       "crc32 check value");
    ok(share_bundle_crc32(nullptr, 0) == 0, "crc32 empty");
}

static void test_offer()
{
    uint8_t body[SHARE_MAX_BODY];
    size_t n = share_offer_build(body, sizeof(body), 1234, 0xDEADBEEFu, 7,
                                 "my_app", "My App", "1.2");
    ok(n > 0, "offer builds");
    uint16_t len = 0;
    uint32_t crc = 0;
    uint8_t total = 0;
    char slug[40] = {}, name[60] = {}, ver[60] = {};
    ok(share_offer_parse(body, n, &len, &crc, &total,
                         slug, sizeof(slug), name, sizeof(name),
                         ver, sizeof(ver)),
       "offer parses");
    ok(len == 1234 && crc == 0xDEADBEEFu && total == 7, "offer fields round-trip");
    ok(strcmp(slug, "my_app") == 0 && strcmp(name, "My App") == 0
       && strcmp(ver, "1.2") == 0, "offer strings round-trip");

    // Truncation and mutation must fail, not half-parse.
    ok(!share_offer_parse(body, n - 1, nullptr, nullptr, nullptr,
                          nullptr, 0, nullptr, 0, nullptr, 0),
       "offer truncated fails");
    uint8_t bad[SHARE_MAX_BODY];
    memcpy(bad, body, n);
    bad[7] = 0;   // slug_len = 0
    ok(!share_offer_parse(bad, n, nullptr, nullptr, nullptr,
                          nullptr, 0, nullptr, 0, nullptr, 0),
       "offer empty slug fails");
    memcpy(bad, body, n);
    bad[6] = 0;   // total_frames = 0
    ok(!share_offer_parse(bad, n, nullptr, nullptr, nullptr,
                          nullptr, 0, nullptr, 0, nullptr, 0),
       "offer zero frames fails");
    ok(share_offer_build(body, sizeof(body), 1, 1, 1, "", "x", "y") == 0,
       "offer empty slug refuses to build");

    // Oversize strings refuse at build time (loader caps).
    char big[80];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    ok(share_offer_build(body, sizeof(body), 1, 1, 1, big, "x", "y") == 0,
       "offer long slug refuses to build");
}

static void test_accept()
{
    uint8_t body[SHARE_MAX_BODY];
    size_t n = share_accept_build(body, sizeof(body), "Ada");
    ok(n == 4, "accept builds");
    char name[50] = {};
    ok(share_accept_parse(body, n, name, sizeof(name)), "accept parses");
    ok(strcmp(name, "Ada") == 0, "accept name round-trips");
    n = share_accept_build(body, sizeof(body), nullptr);
    ok(n == 1 && share_accept_parse(body, n, name, sizeof(name))
       && name[0] == '\0', "accept empty name round-trips");
    body[0] = 5;   // claims 5 name bytes, holds 0
    ok(!share_accept_parse(body, 1, name, sizeof(name)), "accept lies fail");
}

static void test_bitmap()
{
    uint8_t bm[SHARE_BITMAP_BYTES] = {};
    ok(share_bitmap_popcount(bm, 255) == 0, "bitmap empty");
    for (int i = 0; i < 232; i++) share_bitmap_set(bm, (uint8_t)i);
    ok(share_bitmap_popcount(bm, 232) == 232, "bitmap 232 frames");
    ok(!share_bitmap_test(bm, 232), "bitmap next clear");
    ok(share_bitmap_popcount(bm, 255) == 232, "bitmap popcount bounded");
}

static void test_static_sizes()
{
    ok(SHARE_HDR_SIZE + SHARE_MAX_BODY == SHARE_MAX_FRAME, "frame math");
    ok(sizeof(share_hdr_t) == SHARE_HDR_SIZE, "header size");
}

static char g_tmp[64];

static void test_bundle_roundtrip()
{
    char dir[96], sub[128];
    snprintf(dir, sizeof(dir), "%s/app", g_tmp);
    snprintf(sub, sizeof(sub), "%s/app/pkg", g_tmp);
    mkdir(dir, 0755);
    mkdir(sub, 0755);
    char p[160];
    snprintf(p, sizeof(p), "%s/manifest.cfg", dir);
    write_file(p, "slug=demo_app\nname=Demo App\nicon=DM\napi=2\nversion=1.0\nauthor=me\n");
    snprintf(p, sizeof(p), "%s/main.lua", dir);
    write_file(p, "function on_enter(root) end\n");
    snprintf(p, sizeof(p), "%s/pkg/mod.lua", dir);
    write_file(p, "return {}\n");
    snprintf(p, sizeof(p), "%s/icon.bin", dir);
    write_file(p, "BINARYDATA");
    snprintf(p, sizeof(p), "%s/.hidden", dir);
    write_file(p, "secret\n");
    char ad[160];
    snprintf(ad, sizeof(ad), "%s/appdata", dir);
    mkdir(ad, 0755);
    snprintf(p, sizeof(p), "%s/appdata/save.dat", dir);
    write_file(p, "personal\n");

    char why[160] = {};
    size_t len = 0;
    uint8_t *bundle = share_bundle_pack(dir, &len, why, sizeof(why));
    ok(bundle != nullptr, "bundle packs");
    if (!bundle) {
        printf("  why: %s\n", why);
        return;
    }
    ok(len <= SHARE_BUNDLE_MAX_LEN, "bundle under cap");

    char stage[96];
    snprintf(stage, sizeof(stage), "%s/stage", g_tmp);
    ok(share_bundle_unpack(bundle, len, stage, why, sizeof(why)), "bundle unpacks");
    if (why[0]) printf("  why: %s\n", why);
    struct stat st;
    snprintf(p, sizeof(p), "%s/main.lua", stage);
    ok(stat(p, &st) == 0, "main.lua survived");
    snprintf(p, sizeof(p), "%s/pkg/mod.lua", stage);
    ok(stat(p, &st) == 0, "module survived");
    snprintf(p, sizeof(p), "%s/icon.bin", stage);
    ok(stat(p, &st) == 0, "icon survived");
    snprintf(p, sizeof(p), "%s/.hidden", stage);
    ok(stat(p, &st) != 0, "dotfile excluded");
    snprintf(p, sizeof(p), "%s/appdata/save.dat", stage);
    ok(stat(p, &st) != 0, "appdata excluded");

    snprintf(p, sizeof(p), "%s/manifest.cfg", stage);
    char slug[40] = {};
    ok(share_bundle_slug_from_manifest(p, slug, sizeof(slug))
       && strcmp(slug, "demo_app") == 0, "slug extracts");
    ok(share_bundle_patch_slug(p, "demo_app-2"), "slug patches");
    ok(share_bundle_slug_from_manifest(p, slug, sizeof(slug))
       && strcmp(slug, "demo_app-2") == 0, "patched slug extracts");

    // Corruption anywhere must fail unpack, never half-install.
    bundle[10] ^= 0xff;
    char stage2[96];
    snprintf(stage2, sizeof(stage2), "%s/stage2", g_tmp);
    ok(!share_bundle_unpack(bundle, len, stage2, why, sizeof(why)),
       "corrupt bundle fails");
    bundle[10] ^= 0xff;
    ok(!share_bundle_unpack(bundle, len - 1, stage2, why, sizeof(why)),
       "truncated bundle fails");
    ok(!share_bundle_unpack(bundle, 3, stage2, why, sizeof(why)),
       "tiny bundle fails");

    free(bundle);
    share_bundle_remove_dir(dir);
    share_bundle_remove_dir(stage);
}

static void test_bundle_rejects()
{
    char why[160] = {};
    size_t len = 0;
    // Missing main.lua.
    char dir[96], p[160];
    snprintf(dir, sizeof(dir), "%s/noapp", g_tmp);
    mkdir(dir, 0755);
    snprintf(p, sizeof(p), "%s/manifest.cfg", dir);
    write_file(p, "slug=x\nname=X\n");
    ok(share_bundle_pack(dir, &len, why, sizeof(why)) == nullptr,
       "pack rejects missing main.lua");
    share_bundle_remove_dir(dir);

    ok(!share_bundle_valid_slug("") && !share_bundle_valid_slug("UPPER")
       && !share_bundle_valid_slug("-lead") && !share_bundle_valid_slug("has space")
       && share_bundle_valid_slug("ok_slug-2") && share_bundle_valid_slug("9lives"),
       "slug charset matches loader");
}

static std::vector<uint8_t> read_bytes(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static void write_bytes(const std::string &path, const std::vector<uint8_t> &bytes)
{
    std::ofstream f(path, std::ios::binary);
    f.write((const char *)bytes.data(), bytes.size());
    if (!f.good()) exit(1);
}

static void test_streaming()
{
    const std::string dir = std::string(g_tmp) + "/stream";
    const std::string packed = std::string(g_tmp) + "/bundle.bin";
    const std::string stage = std::string(g_tmp) + "/stream-stage";
    mkdir(dir.c_str(), 0755);
    const std::string manifest = "slug=stream\nname=Stream\napi=2\n";
    write_file((dir + "/manifest.cfg").c_str(), manifest.c_str());
    const size_t overhead = 6 + 1 + 12 + 4 + manifest.size() + 1 + 8 + 4;
    size_t sizes[] = {0, 1023, 1024, 1025, SHARE_BUNDLE_MAX_LEN - overhead};
    for (size_t n : sizes) {
        std::vector<uint8_t> data(n);
        for (size_t i = 0; i < n; ++i) data[i] = (uint8_t)(i * 7);
        write_bytes(dir + "/main.lua", data);
        size_t len = 0, memory_len = 0;
        uint32_t crc = 0;
        char why[160];
        ok(share_bundle_pack_file(dir.c_str(), packed.c_str(), &len, &crc, why, sizeof(why)), "stream pack");
        auto bytes = read_bytes(packed);
        uint8_t *memory = share_bundle_pack(dir.c_str(), &memory_len, why, sizeof(why));
        ok(memory && memory_len == len && bytes.size() == len
           && memcmp(memory, bytes.data(), len) == 0, "file and memory wire formats match");
        free(memory);
        ok(crc == share_bundle_crc32(bytes.data(), bytes.size()), "stream CRC matches whole bundle");
        uint32_t incremental = 0;
        for (size_t i = 0; i < bytes.size(); i += 17)
            incremental = share_bundle_crc32_update(incremental, bytes.data() + i,
                std::min(size_t(17), bytes.size() - i));
        ok(incremental == crc, "CRC independent of chunk boundaries");
        ok(share_bundle_unpack_file(packed.c_str(), stage.c_str(), why, sizeof(why)), "stream unpack");
        ok(read_bytes(stage + "/main.lua") == data, "stream contents preserved");
        share_bundle_remove_dir(stage.c_str());
        bytes.pop_back(); write_bytes(packed, bytes);
        ok(!share_bundle_unpack_file(packed.c_str(), stage.c_str(), why, sizeof(why))
           && access(stage.c_str(), F_OK) != 0, "truncation removes partial installation");
    }
    write_bytes(dir + "/main.lua", std::vector<uint8_t>(SHARE_BUNDLE_MAX_LEN, 1));
    char why[160]; size_t len=99; uint32_t crc=99;
    ok(!share_bundle_pack_file(dir.c_str(), packed.c_str(), &len, &crc, why, sizeof(why))
       && len == 0 && crc == 0 && access(packed.c_str(), F_OK) != 0,
       "oversize pack removes partial bundle and clears outputs");
    unlink((dir + "/main.lua").c_str());
    ok(!share_bundle_pack_file(dir.c_str(), packed.c_str(), &len, &crc, why, sizeof(why))
       && access(packed.c_str(), F_OK) != 0, "missing required file cleans staged bundle");
    share_bundle_remove_dir(dir.c_str());

    // Crafted records exercise the same untrusted input checks through file IO.
    for (std::string path : {"../escape", "pkg/../escape", "pkg/.hidden", "/absolute",
                             "pkg//empty", "pkg/", "a/b/c/d/e/f", "appdata/save", "main.lua"}) {
        std::vector<uint8_t> bytes = {'A','P','P','B',1,3};
        auto record = [&](const std::string &name) {
            bytes.push_back((uint8_t)name.size());
            bytes.insert(bytes.end(), name.begin(), name.end());
            bytes.insert(bytes.end(), {1,0,0,0,'x'});
        };
        record("manifest.cfg"); record("main.lua"); record(path);
        write_bytes(packed, bytes);
        ok(!share_bundle_unpack_file(packed.c_str(), stage.c_str(), why, sizeof(why))
           && access(stage.c_str(), F_OK) != 0, "unsafe or duplicate path leaves no installation");
    }
    unlink(packed.c_str());
    uint8_t short_offer[9] = {};
    ok(!share_offer_parse(short_offer, sizeof(short_offer), nullptr, nullptr, nullptr,
                         nullptr, 0, nullptr, 0, nullptr, 0), "9-byte OFFER rejected before reading byte 10");
}

int main()
{
    char tmpl[] = "/tmp/share-test-XXXXXX";
    if (!mkdtemp(tmpl)) {
        printf("SETUP FAIL: mkdtemp\n");
        return 1;
    }
    snprintf(g_tmp, sizeof(g_tmp), "%s", tmpl);

    test_crc();
    test_offer();
    test_accept();
    test_bitmap();
    test_static_sizes();
    test_bundle_roundtrip();
    test_bundle_rejects();
    test_streaming();

    share_bundle_remove_dir(g_tmp);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
