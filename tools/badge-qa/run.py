#!/usr/bin/env python3
"""Run badge UI regressions on the host with real LVGL and file-backed config."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def run(work: Path):
    work.mkdir(parents=True, exist_ok=True)
    (work / "esp_err.h").write_text(
        "#pragma once\nusing esp_err_t = int;\n"
        "constexpr int ESP_OK = 0;\nconstexpr int ESP_FAIL = -1;\n"
        "constexpr int ESP_ERR_INVALID_STATE = -2;\n"
        "constexpr int ESP_ERR_NO_MEM = -3;\nconstexpr int ESP_ERR_INVALID_ARG = -4;\n"
        "inline const char *esp_err_to_name(esp_err_t) { return \"host error\"; }\n")
    (work / "esp_log.h").write_text(
        "#pragma once\n"
        "static inline void host_esp_log(const char *tag, const char *format, ...) "
        "{ (void)tag; (void)format; }\n"
        "#define ESP_LOGE(tag, ...) host_esp_log((tag), __VA_ARGS__)\n"
        "#define ESP_LOGI(tag, ...) host_esp_log((tag), __VA_ARGS__)\n"
        "#define ESP_LOGW(tag, ...) host_esp_log((tag), __VA_ARGS__)\n")
    (work / "esp_random.h").write_text(
        "#pragma once\n#include <cstdint>\n#include <cstddef>\nuint32_t esp_random();\nvoid esp_fill_random(void *, size_t);\n")
    (work / "esp_rom_crc.h").write_text(
        "#pragma once\n#include <stdint.h>\n"
        "uint32_t esp_rom_crc32_le(uint32_t, const uint8_t *, uint32_t);\n")
    (work / "esp_heap_caps.h").write_text(
        "#pragma once\n#include <stddef.h>\n"
        "#define MALLOC_CAP_8BIT 1\n#define MALLOC_CAP_INTERNAL 2\n"
        "static inline size_t heap_caps_get_free_size(unsigned) { return 0; }\n"
        "static inline size_t heap_caps_get_largest_free_block(unsigned) { return 0; }\n")
    json_dir = Path(os.environ.get("IDF_PATH", str(Path.home() / ".espressif/v5.5.3/esp-idf"))) / "components/json/cJSON"
    sodium = Path("/opt/homebrew/opt/libsodium")
    sodium_flags = [f"-I{sodium / 'include'}"] if sodium.exists() else []
    sodium_link = [f"-L{sodium / 'lib'}", "-lsodium"] if sodium.exists() else ["-lsodium"]
    lv = ROOT / "managed_components/lvgl__lvgl"
    build = work / "lvgl-build"
    flags = [f'-DLV_CONF_PATH="{HERE / "lv_conf.h"}"',
             f'-DBADGE_QA_DIR="{work}"',
             '-DLV_LVGL_H_INCLUDE_SIMPLE', f'-I{ROOT / "managed_components/georgik__lua/lua"}', f'-I{ROOT / "managed_components/georgik__lua/include"}', f'-I{json_dir}', *sodium_flags,
             *[f'-I{path}' for path in (lv, ROOT / 'main/apps',
                                       ROOT / 'main/hal', ROOT / 'main',
                                       HERE / 'stubs', work)]]
    with (work / "build.log").open("w") as log:
        def build_step(command):
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)

        build_step(['cmake', '-S', str(lv), '-B', str(build),
                    f'-DLV_BUILD_CONF_PATH={HERE / "lv_conf.h"}',
                    '-DCONFIG_LV_BUILD_DEMOS=OFF', '-DCONFIG_LV_BUILD_EXAMPLES=OFF',
                    '-DCONFIG_LV_USE_THORVG_INTERNAL=OFF'])
        build_step(['cmake', '--build', str(build), '-j8'])
        objects = []
        for symbol in ['icon_mood', 'icon_rules', 'icon_socials', 'icon_scanner', 'icon_sponsors', 'icon_workshops', 'icon_ktnh',
                       'icon_bubblewrap', 'icon_eightball', 'icon_flashlight',
                       'icon_lights', 'icon_breakout', 'icon_maze',
                       'icon_tictactoe', 'icon_flappy',
                       'scanner_sticker', 'icon_card', 'icon_blindbox', 'icon_progress',
                       'icon_dice', 'icon_snake', 'icon_badgebump',
                       'icon_sokoban', 'spr_soko_player', 'spr_soko_box', 'spr_soko_wall', 'spr_soko_dock',
                       'icon_pong', 'icon_dance', 'icon_etch', 'icon_outrun', 'icon_hopper', 'spr_frog', 'spr_car', 'spr_log_long', 'spr_log_short',
                       'icon_2048', 'icon_mines', 'icon_fortune', 'icon_reaction', 'icon_hardwarephysics',
                       *[p.stem for p in (ROOT / 'main/apps/icons_generated').glob('pal_*.c')]]:
            obj = work / f'{symbol}.o'
            build_step(['clang', '-c', *flags,
                        str(ROOT / 'main/apps/icons_generated' / f'{symbol}.c'),
                        '-o', str(obj)])
            objects.append(str(obj))
        cjson = work / 'cJSON.o'
        build_step(['clang', '-c', str(json_dir / 'cJSON.c'), '-o', str(cjson)])
        objects.append(str(cjson))
        wallet = (ROOT / 'main/apps/solana_wallet.cpp').read_text().replace(
            '"/littlefs/solana.json"', '"wallet.json"')
        (work / 'solana_wallet.cpp').write_text(wallet)
        # Exercise the production config reader and atomic file writer against
        # an isolated host directory; all other firmware source is unmodified.
        config = (ROOT / 'main/hal/hal_config.cpp').read_text().replace(
            '"/littlefs/config"', json.dumps(str(work / 'config')))
        (work / 'hal_config.cpp').write_text(config)
        sponsor_store = (ROOT / 'main/apps/sponsor_store.cpp').read_text().replace(
            '"/littlefs/config/sponsors"', json.dumps(str(work / 'config/sponsors')))
        (work / 'sponsor_store.cpp').write_text(sponsor_store)
        for name in ('contacts_store', 'app_connect'):
            source = (ROOT / 'main/apps' / f'{name}.cpp').read_text().replace(
                '"/littlefs/config/contacts"', json.dumps(str(work / 'config/contacts')))
            if name == 'contacts_store':
                # Mirror the badge's LittleFS quirk: access() may claim an
                # existing file is absent. Production deduplication must use
                # the VFS-supported stat() path instead.
                source = source.replace(
                    '#include <unistd.h>',
                    '#include <unistd.h>\n#define access(...) (-1)')
            (work / f'{name}.cpp').write_text(source)
        apps = ['app_onboarding', 'app_mood', 'app_bubble_wrap', 'app_scanner', 'app_workshops',
                'workshop_catalog', 'workshop_store',
                'app_launcher', 'app_rules', 'launcher_slugs',
                'app_sponsors', 'sponsor_layout', 'sponsor_catalog',
                'app_badge_id', 'app_blindbox', 'pal_picker', 'pal_catalog',
                'ripple_state', 'json_util', 'app_progress', 'app_breakout',
                'app_dice', 'app_tictactoe', 'app_snake', 'bump_proto',
                'app_blackjack', 'app_flappy', 'app_sokoban', 'app_pong', 'app_dance', 'app_etch', 'app_outrun',
                'app_hopper', 'app_2048', 'app_mines', 'app_fortune', 'app_reaction', 'app_ktnh',
                'app_flashlight', 'app_hardware_physics', 'mp_session', 'share_xfer', 'share_bundle']
        sources = [HERE / 'main.cpp', work / 'hal_config.cpp',
                   work / 'sponsor_store.cpp', work / 'solana_wallet.cpp',
                   work / 'app_connect.cpp', work / 'contacts_store.cpp',
                   ROOT / 'main/hal/hal_lights.cpp',
                   *[ROOT / 'main/apps' / f'{app}.cpp' for app in apps]]
        def compile_source(source):
            obj = work / f'{source.stem}.o'
            build_step(['clang++', '-std=c++17', '-g', '-fsanitize=address',
                        '-Wall', '-Wextra', '-Werror', *flags, '-c', str(source), '-o', str(obj)])
            return str(obj)
        with ThreadPoolExecutor(max_workers=8) as pool:
            app_objects = list(pool.map(compile_source, sources))
        build_step(['clang++', '-fsanitize=address', *app_objects, *objects,
                    str(build / 'lib/liblvgl.a'), *sodium_link, '-o', str(work / 'qa')])
        build_step(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    *flags, str(HERE / 'registry.cpp'),
                    str(ROOT / 'main/apps/app_registry.cpp'),
                    str(build / 'lib/liblvgl.a'), '-o', str(work / 'registry-qa')])
        # Compile the other changed game apps against real LVGL as well.
        build_step(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsyntax-only', *flags,
                    *[str(ROOT / 'main/apps' / f'app_{name}.cpp')
                      for name in ('tetris', 'maze', 'level', 'chess', 'share', 'sync',
                                   '67', 'invaders', 'jetpack', 'outrun', 'simon', 'whack', 'slots')]])
    subprocess.run([str(work / 'registry-qa')], check=True)
    for args in ([], ['reboot']):
        subprocess.run([str(work / 'qa'), *args], cwd=work, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out-dir', type=Path,
                        help='Keep the build, isolated test config, and 320x240 PPM screenshots here.')
    args = parser.parse_args()
    if args.out_dir:
        # A dedicated child prevents the test config reset from touching any
        # existing config directory in the caller's output directory.
        work = args.out_dir.resolve() / 'badge-ui-qa'
        run(work)
        print(f'Host QA artifacts: {work}')
    else:
        with tempfile.TemporaryDirectory(prefix='badge-ui-qa-') as tmp:
            run(Path(tmp))


if __name__ == '__main__':
    main()
