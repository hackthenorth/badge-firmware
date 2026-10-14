#!/usr/bin/env python3
"""Check unique-contact save results using the real store and a temporary filesystem."""

import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CXX = shutil.which("c++")
if not CXX:
    raise SystemExit("A C++17 compiler is required to test the contacts store")

with tempfile.TemporaryDirectory(prefix="badge-contacts-test-") as temp:
    out = Path(temp)
    contacts = out / "contacts"
    source = (ROOT / "main/apps/contacts_store.cpp").read_text()
    source = source.replace('"/littlefs/config/contacts"', json.dumps(str(contacts)))
    (out / "contacts_store.cpp").write_text(source)
    (out / "esp_err.h").write_text("#pragma once\nusing esp_err_t = int;\n")
    (out / "esp_log.h").write_text(
        "#pragma once\n"
        "#define ESP_LOGI(tag, ...) ((void)(tag))\n"
        "#define ESP_LOGE(tag, ...) ((void)(tag))\n"
    )
    (out / "test.cpp").write_text(r'''
#include "contacts_store.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

static bool storage_available = true;
bool hal_fs_mkdir_p(const char *path)
{
    if (!storage_available) return false;
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return !error;
}

static BumpCard card(const char *id, const char *name)
{
    BumpCard result = {};
    snprintf(result.badge_id, sizeof(result.badge_id), "%s", id);
    snprintf(result.display_name, sizeof(result.display_name), "%s", name);
    return result;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const std::filesystem::path directory(argv[1]);
    assert(contacts_store_count() == 0);
    bool added = true;
    assert(!contacts_store_save(card("", "No identity"), &added));
    assert(!added && contacts_store_count() == 0);

    BumpCard peer = card("peer-one", "Alex");
    assert(contacts_store_save(peer, &added));
    assert(added && contacts_store_count() == 1);
    for (int repeat = 0; repeat < 30; ++repeat) {
        added = true;
        assert(contacts_store_save(peer, &added));
        assert(!added && contacts_store_count() == 1);
    }

    // An updated profile still belongs to the same unique badge.
    peer = card("peer-one", "Alex Updated");
    added = true;
    assert(contacts_store_save(peer, &added));
    assert(!added && contacts_store_count() == 1);
    ContactRecord stored = {};
    assert(contacts_store_get(0, &stored));
    assert(strcmp(stored.badge_id, "peer-one") == 0);
    assert(strcmp(stored.name, "Alex Updated") == 0);

    // Equal display names do not collapse different badge IDs.
    assert(contacts_store_save(card("peer-two", "Alex Updated"), &added));
    assert(added && contacts_store_count() == 2);
    assert(contacts_store_save(peer)); // callers may omit the result flag
    assert(contacts_store_count() == 2);

    storage_available = false;
    added = true;
    assert(!contacts_store_save(card("peer-three", "Unsaved"), &added));
    assert(!added && contacts_store_count() == 2);
    storage_available = true;

    // Failed replacement must not report success or a new contact.
    const auto blocked = directory / "blocked.cfg";
    std::filesystem::create_directory(blocked);
    added = true;
    assert(!contacts_store_save(card("blocked", "Unsaved"), &added));
    assert(!added);
    assert(!std::filesystem::exists(directory / "blocked.cfg.tmp"));
    std::filesystem::remove(blocked);
    std::ofstream(directory / "interrupted.cfg.tmp") << "partial upload";
    assert(contacts_store_count() == 2);
    puts("contacts: unique badge IDs, repeat saves, profile updates, and failed writes passed");
}
''')
    exe = out / "contacts-test"
    subprocess.run(
        [CXX, "-std=c++17", "-Wall", "-Wextra", "-Werror",
         "-I", str(out), "-I", str(ROOT / "main/apps"),
         "-I", str(ROOT / "main/hal"), str(out / "test.cpp"),
         str(out / "contacts_store.cpp"), "-o", str(exe)],
        check=True,
    )
    subprocess.run([str(exe), str(contacts)], check=True)
