#!/usr/bin/env python3
"""Compile two independent instances of the real Share engine and simulate BLE."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
with tempfile.TemporaryDirectory(prefix="share-stream-") as directory:
    work = Path(directory)
    headers = {
        "esp_err.h": "#pragma once\nusing esp_err_t=int;\nconstexpr int ESP_OK=0;\n",
        "esp_log.h": '#pragma once\n#include <cstdio>\n#define ESP_LOGE(tag, ...) do { fprintf(stderr, "%s: ", tag); fprintf(stderr, __VA_ARGS__); fputc(\'\\n\', stderr); } while (0)\n',
        "esp_timer.h": "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time();\n",
        "esp_random.h": "#pragma once\n#include <stdint.h>\ninline uint32_t esp_random(){return 1234;}\n",
        "freertos/FreeRTOS.h": "#pragma once\n#define pdTRUE 1\n#define portMAX_DELAY 0\n",
        "freertos/semphr.h": "#pragma once\nusing SemaphoreHandle_t=int*;\ninline int* xSemaphoreCreateMutex(){static int n;return &n;}\ninline int xSemaphoreTake(int*,unsigned){return 1;}\ninline void xSemaphoreGive(int*){}\n",
    }
    headers["esp_log.h"] += "#define ESP_LOGI ESP_LOGE\n#define ESP_LOGW ESP_LOGE\n"
    for name, content in headers.items():
        p = work / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", *shlex.split(os.getenv("SHARE_TEST_FLAGS", "-fsanitize=address,undefined -g"))]
    flags += [f"-I{p}" for p in (work, here, root / "main/apps", root / "main/hal")]
    compiler = os.getenv("CXX", "c++")
    functions = sorted(set(re.findall(r"\bshare_xfer_\w+(?=\()", (root / "main/apps/share_xfer.h").read_text())))
    objects = []
    for side in ("sender", "receiver"):
        source = '#include "engine.h"\n#include <unistd.h>\nssize_t test_write(int,const void*,size_t);\nssize_t test_read(int,void*,size_t);\n#define write test_write\n#define read test_read\n'
        source += "\n".join(f"#define {f} {side}_{f}" for f in functions)
        source += f'\nvoid {side}_share_xfer_on_frame(const uint8_t*,int8_t,const uint8_t*,size_t);\n'
        source += f'#include "{root / "main/apps/share_xfer.cpp"}"\n'
        fields = ("start_send", "start_listen", "accept_offer", "tick", "abort", "on_frame", "state", "fail", "rx_file")
        source += f'Engine {side}_engine() {{ return {{' + ','.join(f'{side}_share_xfer_{f}' for f in fields) + '}; }\n'
        cpp, obj = work / f"{side}.cpp", work / f"{side}.o"
        cpp.write_text(source)
        subprocess.run([compiler, *flags, "-c", str(cpp), "-o", str(obj)], check=True)
        objects.append(str(obj))
    binary = work / "test"
    subprocess.run([compiler, *flags, str(here / "engine.cpp"), str(root / "main/apps/share_bundle.cpp"), *objects, "-o", str(binary)], check=True)
    subprocess.run([str(binary), str(work)], check=True)
