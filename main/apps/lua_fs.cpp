#include "lua_internal.h"

extern "C" {
#include "lauxlib.h"
}

#include "hal_fs.h"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr std::size_t kMaxRelBytes = 128;
constexpr std::size_t kMaxFileBytes = 16 * 1024;
constexpr std::size_t kQuotaBytes = 64 * 1024;
constexpr const char *kAppdataPrefix = "appdata/";

LuaCtx *check_ctx(lua_State *L)
{
    LuaCtx *ctx = badge_lua_get_ctx(L);
    if (!ctx) luaL_error(L, "Lua app context is unavailable");
    return ctx;
}

void check_arg_count(lua_State *L, int min, int max, const char *name)
{
    const int n = lua_gettop(L);
    if (n < min || n > max) {
        if (min == max) {
            luaL_error(L, "%s expects %d argument%s", name, min,
                       min == 1 ? "" : "s");
        } else {
            luaL_error(L, "%s expects %d-%d arguments", name, min, max);
        }
    }
}

// Lexical sandbox: no normalization to get wrong because nothing that needs
// normalizing is admitted. Rejects absolute paths, backslashes, NUL bytes,
// and any ".." segment outright.
bool resolve(lua_State *L, LuaCtx *ctx, int index, std::string *absolute)
{
    std::size_t len = 0;
    const char *rel = luaL_checklstring(L, index, &len);
    if (len == 0 || len > kMaxRelBytes) return false;
    if (memchr(rel, '\0', len) != nullptr) return false;
    if (rel[0] == '/') return false;

    std::string path(rel, len);
    if (path.find('\\') != std::string::npos) return false;

    std::string base;
    if (path.compare(0, strlen(kAppdataPrefix), kAppdataPrefix) == 0) {
        path.erase(0, strlen(kAppdataPrefix));
        if (path.empty()) return false;
        base = "/littlefs/appdata/";
        base += ctx->slug;
    } else {
        base = ctx->dir;
    }

    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        std::string seg = path.substr(start, end - start);
        if (seg.empty() || seg == "." || seg == "..") return false;
        if (end == path.size()) break;
        start = end + 1;
    }

    *absolute = base + "/" + path;
    return true;
}

const char *check_path(lua_State *L, LuaCtx *ctx, int index, std::string *absolute)
{
    if (!resolve(L, ctx, index, absolute)) {
        luaL_error(L, "path escapes the app sandbox");
    }
    return absolute->c_str();
}

std::size_t file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    return static_cast<std::size_t>(st.st_size);
}

// Depth-capped: each level costs ~350 B of LVGL-task stack, and nesting is
// console-reachable, so unbounded recursion could outrun the C-stack probe.
constexpr unsigned kMaxScanDepth = 8;

std::size_t scan_usage_depth(const char *base, unsigned depth)
{
    std::size_t total = 0;
    DIR *d = opendir(base);
    if (!d) return 0;
    struct dirent *de;
    char child[256];
    while ((de = readdir(d)) != nullptr) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        // Join manually: base comes from ctx strings the compiler can't
        // bound, and snprintf here trips -Wformat-truncation (an error).
        const std::size_t bl = std::strlen(base);
        const std::size_t dl = std::strlen(de->d_name);
        if (bl + 1 + dl + 1 > sizeof(child)) continue;
        std::memcpy(child, base, bl);
        child[bl] = '/';
        std::memcpy(child + bl + 1, de->d_name, dl + 1);
        struct stat st;
        if (stat(child, &st) != 0) continue;
        if (S_ISREG(st.st_mode)) {
            total += static_cast<std::size_t>(st.st_size);
        } else if (S_ISDIR(st.st_mode) && depth < kMaxScanDepth) {
            total += scan_usage_depth(child, depth + 1);
        }
    }
    closedir(d);
    return total;
}

std::size_t scan_usage(const char *base)
{
    return scan_usage_depth(base, 0);
}

void quota_reserve(lua_State *L, LuaCtx *ctx, std::size_t bytes)
{
    if (bytes > kQuotaBytes - (ctx->fs_quota_used > kQuotaBytes
                                   ? kQuotaBytes
                                   : ctx->fs_quota_used)) {
        luaL_error(L, "filesystem quota exceeded (%d KiB total)",
                   (int)(kQuotaBytes / 1024));
    }
}

int fs_read(lua_State *L)
{
    check_arg_count(L, 1, 1, "badge.fs.read");
    LuaCtx *ctx = check_ctx(L);
    std::string absolute;
    check_path(L, ctx, 1, &absolute);
    FILE *f = std::fopen(absolute.c_str(), "rb");
    if (!f) {
        lua_pushnil(L);
        lua_pushstring(L, "no such file");
        return 2;
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0 || (std::size_t)size > kMaxFileBytes) {
        std::fclose(f);
        lua_pushnil(L);
        lua_pushstring(L, "file too large");
        return 2;
    }
    std::string data;
    data.resize(static_cast<std::size_t>(size));
    std::size_t got = data.empty() ? 0 : std::fread(&data[0], 1, data.size(), f);
    std::fclose(f);
    if (got != data.size()) {
        lua_pushnil(L);
        lua_pushstring(L, "read failed");
        return 2;
    }
    lua_pushlstring(L, data.data(), data.size());
    return 1;
}

int fs_write(lua_State *L)
{
    check_arg_count(L, 2, 2, "badge.fs.write");
    LuaCtx *ctx = check_ctx(L);
    std::string absolute;
    check_path(L, ctx, 1, &absolute);
    std::size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    if (len > kMaxFileBytes) luaL_error(L, "file must be at most %d KiB", (int)(kMaxFileBytes / 1024));
    const std::size_t old = file_size(absolute.c_str());
    if (len > old) quota_reserve(L, ctx, len - old);
    FILE *f = std::fopen(absolute.c_str(), "wb");
    if (!f) luaL_error(L, "cannot open file for writing");
    const std::size_t wrote = len == 0 ? 0 : std::fwrite(data, 1, len, f);
    std::fclose(f);
    if (wrote != len) luaL_error(L, "write failed");
    if (len >= old) {
        ctx->fs_quota_used += len - old;
    } else {
        ctx->fs_quota_used -= old - len;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int fs_append(lua_State *L)
{
    check_arg_count(L, 2, 2, "badge.fs.append");
    LuaCtx *ctx = check_ctx(L);
    std::string absolute;
    check_path(L, ctx, 1, &absolute);
    std::size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    const std::size_t old = file_size(absolute.c_str());
    if (old + len > kMaxFileBytes) {
        luaL_error(L, "file must be at most %d KiB", (int)(kMaxFileBytes / 1024));
    }
    quota_reserve(L, ctx, len);
    FILE *f = std::fopen(absolute.c_str(), "ab");
    if (!f) luaL_error(L, "cannot open file for appending");
    const std::size_t wrote = len == 0 ? 0 : std::fwrite(data, 1, len, f);
    std::fclose(f);
    if (wrote != len) luaL_error(L, "append failed");
    ctx->fs_quota_used += len;
    lua_pushboolean(L, 1);
    return 1;
}

int fs_exists(lua_State *L)
{
    check_arg_count(L, 1, 1, "badge.fs.exists");
    LuaCtx *ctx = check_ctx(L);
    std::string absolute;
    check_path(L, ctx, 1, &absolute);
    lua_pushboolean(L, access(absolute.c_str(), F_OK) == 0 ? 1 : 0);
    return 1;
}

int fs_remove(lua_State *L)
{
    check_arg_count(L, 1, 1, "badge.fs.remove");
    LuaCtx *ctx = check_ctx(L);
    std::string absolute;
    check_path(L, ctx, 1, &absolute);
    struct stat st;
    if (stat(absolute.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    if (unlink(absolute.c_str()) != 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const std::size_t freed = static_cast<std::size_t>(st.st_size);
    ctx->fs_quota_used = freed > ctx->fs_quota_used ? 0 : ctx->fs_quota_used - freed;
    lua_pushboolean(L, 1);
    return 1;
}

int fs_list(lua_State *L)
{
    check_arg_count(L, 0, 1, "badge.fs.list");
    LuaCtx *ctx = check_ctx(L);
    std::string absolute;
    if (lua_gettop(L) == 0) {
        absolute = ctx->dir;
    } else {
        check_path(L, ctx, 1, &absolute);
    }
    DIR *d = opendir(absolute.c_str());
    if (!d) luaL_error(L, "cannot list directory");
    lua_newtable(L);
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        lua_pushstring(L, de->d_name);
        lua_rawseti(L, -2, ++n);
    }
    closedir(d);
    return 1;
}

int fs_mkdir(lua_State *L)
{
    check_arg_count(L, 1, 1, "badge.fs.mkdir");
    LuaCtx *ctx = check_ctx(L);
    std::string absolute;
    check_path(L, ctx, 1, &absolute);
    lua_pushboolean(L, hal_fs_mkdir_p(absolute.c_str()) ? 1 : 0);
    return 1;
}

} // namespace

void badge_lua_fs_install(lua_State *L, LuaCtx *ctx, int badge_index)
{
    badge_index = lua_absindex(L, badge_index);
    // Private data dir. Created here so factory_reset covering it is the
    // only lifecycle it needs.
    std::string appdata("/littlefs/appdata/");
    appdata += ctx->slug;
    hal_fs_mkdir_p(appdata.c_str());

    ctx->fs_quota_used = scan_usage(ctx->dir) + scan_usage(appdata.c_str());

    static const luaL_Reg fs_functions[] = {
        { "read",   fs_read   },
        { "write",  fs_write  },
        { "append", fs_append },
        { "exists", fs_exists },
        { "remove", fs_remove },
        { "list",   fs_list   },
        { "mkdir",  fs_mkdir  },
        { nullptr, nullptr },
    };
    lua_newtable(L);
    luaL_setfuncs(L, fs_functions, 0);
    lua_setfield(L, badge_index, "fs");
}
