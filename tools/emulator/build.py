#!/usr/bin/env python3
"""Build tools/emulator/index.html: the badge firmware compiled to WebAssembly.

The firmware sources under main/ are compiled unmodified with Emscripten,
together with LVGL, Lua, cJSON and the slice of libsodium the firmware uses.
Hardware is replaced by src/emu_*.cpp and the ESP-IDF headers by src/idf/.
The result, the page (src/shell.html) and the badge photo are packed into one
self-contained index.html that runs from file:// with no server.

    tools/emulator/build.py                  # incremental build
    tools/emulator/build.py --install-emsdk  # fetch Emscripten into .emsdk first
    tools/emulator/build.py --clean          # drop cached objects and rebuild
"""

import argparse
import base64
from concurrent.futures import ThreadPoolExecutor
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SRC = HERE / "src"
BUILD = HERE / ".build"
EMSDK = HERE / ".emsdk"
OUTPUT = HERE / "index.html"

# Pinned so the generated page is reproducible. Bump deliberately.
EMSDK_VERSION = "6.0.12"

MAIN = ROOT / "main"
COMPONENTS = ROOT / "managed_components"
LVGL = COMPONENTS / "lvgl__lvgl"
LUA = COMPONENTS / "georgik__lua"
SODIUM = COMPONENTS / "espressif__libsodium"
SODIUM_SRC = SODIUM / "libsodium/src/libsodium"

# The badge photo: a flatbed scan of the front of the board. The build
# straightens it, cuts the board out of the scanner background and shrinks it.
# Positions in src/shell.html are pixels of the straightened scan.
BADGE_PHOTO = HERE / "high-res-crop.jpg"
PHOTO_ROTATION = -0.48   # degrees (PIL: counter-clockwise positive) to square it
PHOTO_WIDTH = 1500       # width of the embedded image
# The two slide-switch levers overhang the board edge, where the outline would
# trim them. Keep these (left, top, right, bottom) boxes, in straightened pixels.
PHOTO_LEVERS = [(1043, 0, 1078, 42), (62, 1843, 136, 1881)]

# HAL sources the emulator replaces (src/emu_hal.cpp); everything else in
# main/ is the real firmware.
REPLACED_HAL = {
    "hal_display.cpp", "hal_buttons.cpp", "hal_accel.cpp", "hal_nfc.cpp",
    "hal_radio.cpp", "hal_fs.cpp", "hal_i2c.cpp", "rc522_new_api.cpp",
}

# Ed25519 keypairs (Solana wallet) and SHA-256 (admin password) are all the
# firmware asks of libsodium.
SODIUM_SOURCES = [
    "crypto_sign/crypto_sign.c",
    "crypto_sign/ed25519/sign_ed25519.c",
    "crypto_sign/ed25519/ref10/keypair.c",
    "crypto_sign/ed25519/ref10/open.c",
    "crypto_sign/ed25519/ref10/sign.c",
    "crypto_core/ed25519/ref10/ed25519_ref10.c",
    "crypto_hash/sha256/hash_sha256.c",
    "crypto_hash/sha256/cp/hash_sha256_cp.c",
    "crypto_hash/sha512/hash_sha512.c",
    "crypto_hash/sha512/cp/hash_sha512_cp.c",
    "crypto_verify/verify.c",
    "randombytes/randombytes.c",
    "sodium/utils.c",
]

# The standalone interpreter, the amalgamation and the test harness are not
# part of the managed component's build either.
LUA_EXCLUDED = {"lua.c", "onelua.c", "ltests.c"}

EXPORTED_RUNTIME = [
    "FS", "HEAPU8", "HEAPU32", "UTF8ToString", "stringToUTF8", "lengthBytesUTF8",
]


def log(message):
    print(f"[emulator] {message}", flush=True)


def fail(message):
    sys.exit(f"[emulator] error: {message}")


def find_emcc():
    """emcc on PATH, then $EMSDK, then the project-local install."""
    candidates = []
    on_path = shutil.which("emcc")
    if on_path:
        candidates.append(Path(on_path))
    if os.environ.get("EMSDK"):
        candidates.append(Path(os.environ["EMSDK"]) / "upstream/emscripten/emcc")
    candidates.append(EMSDK / "upstream/emscripten/emcc")
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return None


def install_emsdk():
    if not (EMSDK / "emsdk").exists():
        log(f"cloning emsdk into {EMSDK.relative_to(ROOT)}")
        subprocess.run(["git", "clone", "--depth", "1",
                        "https://github.com/emscripten-core/emsdk.git", str(EMSDK)],
                       check=True)
    log(f"installing Emscripten {EMSDK_VERSION} (about 1.8 GB)")
    subprocess.run([str(EMSDK / "emsdk"), "install", EMSDK_VERSION], check=True)
    subprocess.run([str(EMSDK / "emsdk"), "activate", EMSDK_VERSION], check=True)


def idf_cjson_dir():
    idf = os.environ.get("IDF_PATH") or str(Path.home() / ".espressif/v5.5.3/esp-idf")
    path = Path(idf) / "components/json/cJSON"
    if not (path / "cJSON.c").exists():
        fail(f"cJSON not found at {path}. Activate ESP-IDF or set IDF_PATH.")
    return path


def firmware_version():
    try:
        described = subprocess.run(
            ["git", "describe", "--always", "--tags", "--dirty"], cwd=ROOT,
            capture_output=True, text=True, check=True).stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        described = "unknown"
    # esp_app_desc_t::version is 32 bytes including the NUL.
    return (described + "-emu")[:31]


def check_lvgl_config():
    """Warn when the checked-in LVGL config has drifted from the firmware's."""
    sdkconfig = ROOT / "build/config/sdkconfig.h"
    if not sdkconfig.exists():
        return

    def lv_lines(path):
        return sorted(
            line.strip() for line in path.read_text().splitlines()
            if line.startswith("#define CONFIG_LV_")
            and "CONFIG_LV_BUILD_EXAMPLES" not in line
            and "CONFIG_LV_BUILD_DEMOS" not in line)

    if lv_lines(sdkconfig) != lv_lines(SRC / "lv_kconfig.h"):
        log("warning: src/lv_kconfig.h differs from build/config/sdkconfig.h; "
            "the emulator may not render exactly like the firmware. "
            "See the header comment in src/lv_kconfig.h.")


def collect_sources(cjson):
    """Returns [(source, kind)] where kind selects the flag set."""
    sources = []
    sources += [(p, "lvgl") for p in sorted((LVGL / "src").rglob("*.c"))]
    sources += [(p, "lua") for p in sorted((LUA / "lua").glob("*.c"))
                if p.name not in LUA_EXCLUDED]
    sources += [(cjson / "cJSON.c", "c")]
    sources += [(SODIUM_SRC / rel, "sodium") for rel in SODIUM_SOURCES]

    firmware = [MAIN / "main.cpp", MAIN / "boot_splash.cpp", MAIN / "test.cpp"]
    firmware += sorted(p for p in (MAIN / "hal").glob("*.cpp")
                       if p.name not in REPLACED_HAL)
    firmware += sorted((MAIN / "apps").glob("*.cpp"))
    firmware += sorted((MAIN / "apps").glob("*.c"))
    firmware += sorted((MAIN / "apps/icons_generated").glob("*.c"))
    sources += [(p, "firmware") for p in firmware]
    sources += [(p, "emu") for p in sorted(SRC.glob("emu_*.cpp"))]

    missing = [str(p) for p, _ in sources if not p.exists()]
    if missing:
        fail("missing sources (run `idf.py reconfigure` to fetch managed "
             "components?):\n  " + "\n  ".join(missing[:10]))
    return sources


def flag_sets(cjson, version):
    lvgl_defines = [
        "-DLV_CONF_KCONFIG_EXTERNAL_INCLUDE=<lv_kconfig.h>",
        "-DLV_LVGL_H_INCLUDE_SIMPLE",
    ]
    # wasm longjmp is for Lua's error handling. Warnings are off: the firmware
    # is warning-clean under ESP-IDF's GCC, and clang's opinions about the
    # same code (mostly %lu against a 32-bit uint32_t) are noise here.
    base = ["-O2", "-sSUPPORT_LONGJMP=wasm", "-w"]
    cxx = ["-std=gnu++20", "-fno-exceptions", "-fno-rtti"]
    # ESP-IDF force-includes sdkconfig.h into every translation unit.
    force_config = ["-include", str(SRC / "idf/sdkconfig.h")]
    includes = [f"-I{p}" for p in (
        SRC / "idf", SRC, MAIN, MAIN / "hal", MAIN / "apps",
        LVGL, LVGL / "src", LUA / "lua", LUA / "include", cjson,
        SODIUM_SRC / "include", SODIUM / "port_include",
    )]
    sodium_private = [
        f"-I{SODIUM_SRC / 'include/sodium'}", f"-I{SODIUM / 'port_include/sodium'}",
        "-DCONFIGURED=1", "-DNATIVE_LITTLE_ENDIAN=1", "-DHAVE_WEAK_SYMBOLS=1",
        "-D__STDC_LIMIT_MACROS", "-D__STDC_CONSTANT_MACROS",
    ]
    return {
        "lvgl": base + lvgl_defines + [f"-I{SRC}", f"-I{LVGL}"],
        # Same recursion cap the firmware's root CMakeLists applies.
        "lua": base + force_config + ["-DLUAI_MAXCCALLS=32", f"-I{SRC / 'idf'}",
                                      f"-I{LUA / 'include'}", f"-I{LUA / 'lua'}"],
        "c": base,
        "sodium": base + sodium_private,
        "firmware": base + force_config + lvgl_defines + includes,
        "emu": base + force_config + lvgl_defines + includes
               + [f'-DEMU_FW_VERSION="{version}"'],
    }, cxx


def needs_rebuild(obj, dep, signature):
    stamp = obj.with_suffix(".sig")
    if not obj.exists() or not dep.exists() or not stamp.exists():
        return True
    if stamp.read_text() != signature:
        return True
    built = obj.stat().st_mtime
    # Make-style depfile: "target: dep dep \\\n dep".
    text = dep.read_text().replace("\\\n", " ")
    deps = text.split(":", 1)[1].split() if ":" in text else []
    for path in deps:
        try:
            if os.stat(path).st_mtime > built:
                return True
        except OSError:
            return True
    return False


def compile_all(emcc, sources, flags, cxx_flags, jobs):
    objdir = BUILD / "obj"
    objdir.mkdir(parents=True, exist_ok=True)
    tasks = []
    objects = []
    for source, kind in sources:
        digest = hashlib.sha1(str(source).encode()).hexdigest()[:10]
        obj = objdir / f"{source.stem}-{digest}.o"
        dep = obj.with_suffix(".d")
        command = [str(emcc), "-c", str(source), "-o", str(obj), "-MMD", "-MF", str(dep),
                   *flags[kind]]
        if source.suffix == ".cpp":
            command += cxx_flags
        signature = hashlib.sha1(" ".join(command).encode()).hexdigest()
        objects.append(obj)
        if needs_rebuild(obj, dep, signature):
            tasks.append((source, obj, command, signature))

    if not tasks:
        log(f"{len(objects)} objects up to date")
        return objects, False

    log(f"compiling {len(tasks)} of {len(objects)} sources with {jobs} jobs")
    started = time.time()

    def run(task):
        source, obj, command, signature = task
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            return source, result.stderr or result.stdout
        obj.with_suffix(".sig").write_text(signature)
        return source, None

    # Compile one file first: a cold Emscripten cache builds its system
    # libraries on first use, which is not worth racing in parallel.
    failures = []
    first, rest = tasks[0], tasks[1:]
    source, error = run(first)
    if error:
        failures.append((source, error))
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        for source, error in pool.map(run, rest):
            if error:
                failures.append((source, error))
    if failures:
        for source, error in failures[:12]:
            lines = error.strip().splitlines()
            shown = "\n".join(lines[:30]) + ("\n  ..." if len(lines) > 30 else "")
            print(f"\n--- {source} ---\n{shown}", file=sys.stderr)
        fail(f"{len(failures)} source(s) failed to compile")
    log(f"compiled in {time.time() - started:.1f}s")
    return objects, True


def link(emcc, objects, relink):
    js = BUILD / "badge.js"
    wasm = BUILD / "badge.wasm"
    response = BUILD / "objects.rsp"
    listing = "\n".join(f'"{o}"' for o in objects)
    if (not relink and js.exists() and wasm.exists() and response.exists()
            and response.read_text() == listing
            and js.stat().st_mtime >= Path(__file__).stat().st_mtime):
        return js, wasm
    response.write_text(listing)
    log("linking")
    command = [
        # em++ so the C++ standard library is linked.
        str(emcc.with_name("em++")), f"@{response}", "-o", str(js),
        "-O2", "-sSUPPORT_LONGJMP=wasm",
        "-sMODULARIZE=1", "-sEXPORT_NAME=createBadgeModule",
        "-sENVIRONMENT=web,node",
        "-sALLOW_MEMORY_GROWTH=1", "-sINITIAL_MEMORY=33554432",
        "-sSTACK_SIZE=1048576",
        "-sFORCE_FILESYSTEM=1", "-sEXIT_RUNTIME=0",
        "-sASSERTIONS=1",
        "-sEXPORTED_RUNTIME_METHODS=" + ",".join(EXPORTED_RUNTIME),
        "-sEXPORTED_FUNCTIONS=_main,_malloc,_free",
    ]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stderr, file=sys.stderr)
        fail("link failed")
    if result.stderr.strip():
        print(result.stderr.strip(), file=sys.stderr)
    return js, wasm


def opening(values, window, big):
    """1-D morphological opening: flattens bumps toward `big` narrower than
    `window`. The signal is extended with `big` first, so a shape that runs off
    either end (a rounded corner) counts as wide and survives."""
    half = window // 2
    extended = [big] * window + values + [big] * window
    inward, outward = (min, max) if big > 0 else (max, min)

    def slide(seq, pick):
        n = len(seq)
        return [pick(seq[max(0, i - half):min(n, i + half + 1)]) for i in range(n)]

    opened = slide(slide(extended, inward), outward)
    return opened[window:window + len(values)]


def board_outline(image):
    """Alpha mask of the PCB within a straightened scan.

    The solder mask is the only saturated purple in the picture and it runs
    along the whole board edge, so each row's and column's first and last
    purple pixel trace the outline, rounded corners and waist included. Parts
    mounted on the edge (USB socket, switches) interrupt it for a short
    stretch; the opening bridges those.
    """
    from PIL import Image, ImageChops, ImageDraw, ImageFilter

    width, height = image.size
    hue, saturation, value = image.convert("HSV").split()
    purple = ImageChops.multiply(
        ImageChops.multiply(hue.point(lambda v: 255 if 190 <= v <= 222 else 0),
                            saturation.point(lambda v: 255 if v >= 200 else 0)),
        value.point(lambda v: 255 if v >= 70 else 0))
    purple = purple.filter(ImageFilter.MinFilter(5))  # drop specks
    # Undo the speck filter's 2 px, then take in the board's dark side wall.
    grow = 2 + 5

    left, right, top, bottom = [], [], [], []
    for y in range(height):
        box = purple.crop((0, y, width, y + 1)).getbbox()
        left.append(width if box is None else box[0] - grow)
        right.append(-1 if box is None else box[2] + grow)
    for x in range(width):
        box = purple.crop((x, 0, x + 1, height)).getbbox()
        top.append(height if box is None else box[1] - grow)
        bottom.append(-1 if box is None else box[3] + grow)

    window = int(0.075 * height)
    far = 10 ** 6
    left, right = opening(left, window, far), opening(right, window, -far)
    top, bottom = opening(top, window, far), opening(bottom, window, -far)

    rows = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(rows)
    for y in range(height):
        if right[y] > left[y]:
            draw.line([(left[y], y), (right[y], y)], fill=255)
    columns = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(columns)
    for x in range(width):
        if bottom[x] > top[x]:
            draw.line([(x, top[x]), (x, bottom[x])], fill=255)
    return ImageChops.multiply(rows, columns)


def badge_photo():
    """The badge scan as a data URI: straightened, cut out and downscaled.

    Returns (data_uri, crop) where crop = [x, y, w, h] is the part of the
    straightened scan the image shows; shell.html places the screen, buttons
    and LEDs in those same straightened pixels.
    """
    if not BADGE_PHOTO.exists():
        fail(f"badge photo not found at {BADGE_PHOTO}")
    try:
        from PIL import Image, ImageFilter
    except ImportError:
        fail("Pillow is needed to prepare the badge photo: python3 -m pip install pillow")

    raw = BADGE_PHOTO.read_bytes()
    recipe = repr((PHOTO_ROTATION, PHOTO_WIDTH, PHOTO_LEVERS, "outline-v2"))
    key = hashlib.sha1(raw + recipe.encode()).hexdigest()[:16]
    cached = BUILD / f"photo-{key}.json"
    if cached.exists():
        saved = json.loads(cached.read_text())
        return saved["uri"], saved["crop"]

    log("preparing the badge photo")
    scan = Image.open(io.BytesIO(raw)).convert("RGB")
    straight = scan.rotate(PHOTO_ROTATION, resample=Image.BICUBIC, fillcolor=(0, 0, 0))
    scale = PHOTO_WIDTH / straight.width
    small = straight.resize((PHOTO_WIDTH, round(straight.height * scale)), Image.LANCZOS)

    mask = board_outline(small)
    for box in PHOTO_LEVERS:
        mask.paste(255, tuple(round(v * scale) for v in box))
    # Pull the edge in a pixel and soften it so no scanner background fringes.
    mask = mask.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.GaussianBlur(0.7))

    box = mask.getbbox()
    cutout = Image.new("RGBA", small.size, (0, 0, 0, 0))
    cutout.paste(small, (0, 0), mask)
    out = io.BytesIO()
    cutout.crop(box).save(out, "WEBP", quality=84, method=6)
    uri = "data:image/webp;base64," + base64.b64encode(out.getvalue()).decode()
    crop = [round(box[0] / scale, 1), round(box[1] / scale, 1),
            round((box[2] - box[0]) / scale, 1), round((box[3] - box[1]) / scale, 1)]
    for stale in BUILD.glob("photo-*.json"):
        stale.unlink()
    cached.write_text(json.dumps({"uri": uri, "crop": crop}))
    return uri, crop


def lua_examples():
    """Example Lua apps the page can install onto the emulated badge."""
    examples = {}
    base = ROOT / "assets/lua_apps"
    if not base.is_dir():
        return examples
    # script_app.cpp refuses some bundled apps by folder name; offering those
    # would only produce an install that silently does nothing.
    disabled = set(re.findall(r'strcmp\(de->d_name, "([^"]+)"\) == 0\) continue',
                              (MAIN / "apps/script_app.cpp").read_text()))
    for app in sorted(p for p in base.iterdir() if p.is_dir() and p.name not in disabled):
        files = {}
        for path in sorted(app.rglob("*")):
            if path.is_file() and path.name != ".DS_Store":
                files[path.relative_to(app).as_posix()] = base64.b64encode(
                    path.read_bytes()).decode()
        if "main.lua" in files and "manifest.cfg" in files:
            examples[app.name] = files
    return examples


def assemble(js, wasm, version):
    shell = (SRC / "shell.html").read_text()
    photo, crop = badge_photo()
    glue = js.read_text().replace("</script", "<\\/script")
    packed = base64.b64encode(gzip.compress(wasm.read_bytes(), 9, mtime=0)).decode()
    build_info = {
        "version": version,
        # The link time, so an unchanged build reproduces the same page.
        "built": time.strftime("%Y-%m-%d %H:%M", time.localtime(wasm.stat().st_mtime)),
        "wasmBytes": wasm.stat().st_size,
    }
    replacements = {
        "/*@@BADGE_GLUE@@*/": glue,
        "@@WASM_GZ_B64@@": packed,
        "@@BADGE_PHOTO@@": photo,
        "/*@@PHOTO_CROP@@*/": json.dumps(crop),
        "/*@@BUILD_INFO@@*/": json.dumps(build_info),
        "/*@@LUA_EXAMPLES@@*/": json.dumps(lua_examples()),
    }
    for marker, value in replacements.items():
        if marker not in shell:
            fail(f"src/shell.html is missing the {marker} marker")
        shell = shell.replace(marker, value)
    OUTPUT.write_text(shell)
    log(f"wrote {OUTPUT.relative_to(ROOT)} ({OUTPUT.stat().st_size / 1e6:.1f} MB, "
        f"wasm {wasm.stat().st_size / 1e6:.1f} MB)")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--install-emsdk", action="store_true",
                        help=f"install Emscripten {EMSDK_VERSION} into tools/emulator/.emsdk")
    parser.add_argument("--clean", action="store_true", help="discard cached objects first")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--no-page", action="store_true",
                        help="stop after linking .build/badge.{js,wasm} (headless tests)")
    args = parser.parse_args()

    if args.install_emsdk:
        install_emsdk()
    emcc = find_emcc()
    if not emcc:
        fail("emcc not found. Run with --install-emsdk, put emcc on PATH, or set EMSDK.")
    if args.clean and BUILD.exists():
        shutil.rmtree(BUILD)
    BUILD.mkdir(exist_ok=True)

    check_lvgl_config()
    cjson = idf_cjson_dir()
    version = firmware_version()
    flags, cxx_flags = flag_sets(cjson, version)
    sources = collect_sources(cjson)
    objects, changed = compile_all(emcc, sources, flags, cxx_flags, args.jobs)
    js, wasm = link(emcc, objects, changed)
    if not args.no_page:
        assemble(js, wasm, version)


if __name__ == "__main__":
    main()
