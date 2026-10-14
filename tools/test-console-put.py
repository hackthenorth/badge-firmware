#!/usr/bin/env python3
"""Exercise the actual console put handler with a host model of CR translation."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "main/hal/hal_console.cpp").read_text()
handler = source.split("static int cmd_put(", 1)[1].split("// Legacy Web Serial", 1)[0]
handler = "static int cmd_put(" + handler
prefix = r'''
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unistd.h>
static constexpr int ESP_LINE_ENDINGS_CR=1, ESP_LINE_ENDINGS_LF=2;
static int rx_mode=ESP_LINE_ENDINGS_CR;
static bool fail_write=false;
static std::vector<unsigned char> input;
static size_t cursor=0;
static void usb_serial_jtag_vfs_set_rx_line_endings(int mode) { rx_mode=mode; }
static void hal_sleep_note_activity() {}
static bool hal_fs_mkdir_p(const char *) { return true; }
static bool resolve_path(const char *src, char *dst, size_t size) {
    return snprintf(dst,size,"%s",src) < (int)size;
}
static int payload_read(int, void *out, size_t count) {
    size_t n=0;
    while(n<count && cursor<input.size()) {
        unsigned char c=input[cursor++];
        if(rx_mode==ESP_LINE_ENDINGS_CR && c==13) c=10;
        static_cast<unsigned char *>(out)[n++]=c;
    }
    return (int)n;
}
static size_t payload_write(const void *data, size_t size, size_t count, FILE *f) {
    return fail_write ? 0 : fwrite(data,size,count,f);
}
#define read payload_read
#define fwrite payload_write
'''
suffix = r'''
#undef read
#undef fwrite
int main() {
    char command[]="put", path[]="upload.bin", length[]="256", probe[]="--binary";
    char *args[]={command,path,length};
    for(int i=0;i<256;i++) input.push_back((unsigned char)i);
    assert(cmd_put(3,args)==0);
    assert(rx_mode==ESP_LINE_ENDINGS_CR);
    FILE *f=fopen(path,"rb");unsigned char result[256];
    assert(fread(result,1,256,f)==256);fclose(f);
    assert(memcmp(result,input.data(),256)==0);
    cursor=0;fail_write=true;
    assert(cmd_put(3,args)==1);assert(rx_mode==ESP_LINE_ENDINGS_CR);
    cursor=256;fail_write=false;
    assert(cmd_put(3,args)==1);assert(rx_mode==ESP_LINE_ENDINGS_CR);
    char *capability[]={command,probe};
    assert(cmd_put(2,capability)==0);assert(rx_mode==ESP_LINE_ENDINGS_CR);
    puts("PASS: all 256 byte values survive; console CR mode restored after success, write failure and short input");
}
'''
with tempfile.TemporaryDirectory(prefix="badge-put-test-") as tmp:
    tmp = Path(tmp)
    cpp = tmp / "put.cpp"
    cpp.write_text(prefix + handler + suffix)
    exe = tmp / "put-test"
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], cwd=tmp, check=True)
