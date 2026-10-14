#!/usr/bin/env python3
"""Exercise actual admin handlers and SHA-256 verification with a test credential."""
from pathlib import Path
import hashlib
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
console = (ROOT / 'main/hal/hal_console.cpp').read_text()
handlers = '\n'.join(re.search(rf'^static int cmd_{name}\(.*?^\}}', console, re.M | re.S)[0]
                     for name in ('debug', 'appmode', 'seedall'))
assert 'BADGE_DEV_SHORTCUTS' not in console
for command in ('appmode', 'debug', 'seedall'):
    assert re.search(rf'\{{ "{command}",.*&cmd_{command}', console)
password = 'test-admin-password-never-deployed'
verifier = (ROOT / 'main/hal/admin_auth.cpp').read_text()
verifier = re.sub(r'(kAdminPasswordSha256\[crypto_hash_sha256_BYTES\] = \{).*?(\n\};)',
                  lambda m: m[1] + '\n' + ','.join(str(b) for b in hashlib.sha256(password.encode()).digest()) + m[2],
                  verifier, flags=re.S)
with tempfile.TemporaryDirectory(prefix='badge-admin-test-') as folder:
    out = Path(folder)
    (out / 'auth.cpp').write_text(verifier)
    (out / 'test.cpp').write_text(r'''
#include "admin_auth.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>
#include <string>
enum class LauncherAppMode { DefaultsOnly, PublicApps, PublicAndInternal };
static int writes=0, locks=0;
static std::string current="default";
void lvgl_port_lock(int){++locks;}
void lvgl_port_unlock(){--locks;}
void hal_config_get_string(const char*,const char*,char *out,size_t n,const char*){snprintf(out,n,"%s",current.c_str());}
void launcher_slugs_set_mode(LauncherAppMode mode){assert(locks==1);++writes;current=mode==LauncherAppMode::PublicApps?"public":mode==LauncherAppMode::PublicAndInternal?"all":"default";}
size_t launcher_slugs_unlock_internal_apps(){assert(locks==1);++writes;return 4;}
size_t launcher_slugs_unlock_public_apps(){assert(locks==1);++writes;return 8;}
''' + handlers + r'''
int call(int(*fn)(int,char**),std::initializer_list<const char*> args){std::vector<char*> argv;for(auto *s:args)argv.push_back(const_cast<char*>(s));return fn(argv.size(),argv.data());}
int main(){
 const char *pw="test-admin-password-never-deployed";
 assert(!admin_password_valid(nullptr)&&!admin_password_valid("")&&!admin_password_valid("wrong"));
 assert(admin_password_valid(pw));
 assert(call(cmd_appmode,{"appmode"})==0&&writes==0);
 assert(call(cmd_appmode,{"appmode","check",pw})==0&&writes==0);
 for(auto *mode:{"default","public","all","check"}){
  assert(call(cmd_appmode,{"appmode",mode})!=0);
  assert(call(cmd_appmode,{"appmode",mode,"wrong"})!=0);
 }
 assert(writes==0);
 assert(call(cmd_appmode,{"appmode","all",pw})==0&&current=="all");
 assert(call(cmd_appmode,{"appmode","public",pw})==0&&current=="public");
 assert(call(cmd_appmode,{"appmode","default",pw})==0&&current=="default");
 assert(call(cmd_appmode,{"appmode","invalid",pw})!=0&&writes==3);
 for(auto fn:{cmd_debug,cmd_seedall}){
  assert(call(fn,{"unlock"})!=0);assert(call(fn,{"unlock","wrong"})!=0);
  assert(call(fn,{"unlock",pw})==0);
 }
 assert(writes==5&&locks==0);
 puts("Admin console: PASS");
}
''')
    sodium = Path('/opt/homebrew/opt/libsodium')
    flags = [f'-I{sodium / "include"}', f'-L{sodium / "lib"}'] if sodium.exists() else []
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    f'-I{ROOT / "main/hal"}', *flags, str(out / 'test.cpp'), str(out / 'auth.cpp'),
                    '-lsodium', '-o', str(out / 'test')], check=True)
    result = subprocess.run([str(out / 'test')], check=True, capture_output=True, text=True)
    for expected in ('APP MODE default', 'APP MODE OK all', 'APP MODE OK public',
                     'APP MODE OK default', 'ADMIN AUTH OK', 'ADMIN AUTH FAIL'):
        assert expected in result.stdout
    assert password not in result.stdout
    print('Admin console: unauthenticated reads, preflight, protected mutations, invalid input, SHA-256 and no credential output PASS')
