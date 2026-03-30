// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Async-signal-safe module enumerator.
// Parses /proc/self/maps manually using only open/read/close.
// No fopen, no fgets, no sscanf, no malloc.

#include "moduleenumerator.h"
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>  // snprintf only

// Parse a hex number from a string. Returns pointer past the parsed number.
static const char* ParseHex(const char* p, uint64_t* out)
{
    uint64_t val = 0;
    while (*p)
    {
        char c = *p;
        if (c >= '0' && c <= '9')
            val = (val << 4) | (c - '0');
        else if (c >= 'a' && c <= 'f')
            val = (val << 4) | (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            val = (val << 4) | (c - 'A' + 10);
        else
            break;
        p++;
    }
    *out = val;
    return p;
}

// Extract filename from a path (everything after last '/')
static const char* GetFilename(const char* path)
{
    const char* last = path;
    for (const char* p = path; *p; p++)
    {
        if (*p == '/')
            last = p + 1;
    }
    return last;
}

// Check if string ends with a suffix
static int EndsWith(const char* str, const char* suffix)
{
    int slen = 0, xlen = 0;
    while (str[slen]) slen++;
    while (suffix[xlen]) xlen++;
    if (xlen > slen) return 0;
    return memcmp(str + slen - xlen, suffix, xlen) == 0;
}

// Process one line from /proc/self/maps
// Format: startAddr-endAddr perms offset dev inode pathname
typedef void (*ModuleCallback)(uint64_t baseAddr, const char* filename, void* ctx);

static void ParseMapsLine(const char* line, ModuleCallback callback, void* ctx,
                          char* lastModule, int lastModuleSize)
{
    uint64_t startAddr;
    const char* p = ParseHex(line, &startAddr);
    if (*p != '-') return;
    p++;

    uint64_t endAddr;
    p = ParseHex(p, &endAddr);
    if (*p != ' ') return;
    p++;

    // Parse permissions (4 chars + space)
    if (p[0] == '\0' || p[1] == '\0' || p[2] == '\0' || p[3] == '\0') return;
    int executable = (p[2] == 'x');
    p += 5; // "rwxp " or similar

    // Skip offset, dev, inode
    while (*p == ' ') p++;
    while (*p && *p != ' ') p++;  // offset
    while (*p == ' ') p++;
    while (*p && *p != ' ') p++;  // dev
    while (*p == ' ') p++;
    while (*p && *p != ' ') p++;  // inode
    while (*p == ' ') p++;

    // What remains is the pathname (may be empty)
    if (*p == '\0' || *p == '\n' || *p == '[') return;

    // Trim trailing newline
    char pathname[256];
    int plen = 0;
    while (p[plen] && p[plen] != '\n' && plen < 255)
    {
        pathname[plen] = p[plen];
        plen++;
    }
    pathname[plen] = '\0';

    if (!executable) return;

    // Only show .so and .dll files
    if (!EndsWith(pathname, ".so") && !EndsWith(pathname, ".dll")) return;

    // Skip duplicate (same module as previous line)
    if (strcmp(pathname, lastModule) == 0) return;

    // Copy for dedup
    int copyLen = plen < lastModuleSize - 1 ? plen : lastModuleSize - 1;
    memcpy(lastModule, pathname, copyLen);
    lastModule[copyLen] = '\0';

    callback(startAddr, GetFilename(pathname), ctx);
}

// Read /proc/self/maps and call callback for each unique executable module.
// Async-signal-safe: uses only open/read/close.
static void EnumerateModules(ModuleCallback callback, void* ctx)
{
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd == -1) return;

    char readBuf[4096];
    char lineBuf[512];
    int linePos = 0;
    char lastModule[256] = {0};
    ssize_t bytesRead;

    while ((bytesRead = read(fd, readBuf, sizeof(readBuf))) > 0)
    {
        for (int i = 0; i < bytesRead; i++)
        {
            if (readBuf[i] == '\n')
            {
                lineBuf[linePos] = '\0';
                ParseMapsLine(lineBuf, callback, ctx, lastModule, sizeof(lastModule));
                linePos = 0;
            }
            else if (linePos < (int)sizeof(lineBuf) - 1)
            {
                lineBuf[linePos++] = readBuf[i];
            }
        }
    }

    close(fd);
}

// Callback for JSON output
static void ModuleToJson(uint64_t baseAddr, const char* filename, void* ctx)
{
    CrashJsonWriter* w = (CrashJsonWriter*)ctx;
    CrashJson_OpenObject(w, NULL);
    CrashJson_WriteString(w, "module_name", filename);
    CrashJson_WriteHex(w, "base_address", baseAddr);
    CrashJson_CloseObject(w);
}

void CrashModules_WriteToJson(CrashJsonWriter* w)
{
    CrashJson_OpenArray(w, "modules");
    EnumerateModules(ModuleToJson, w);
    CrashJson_CloseArray(w);
}

// Callback for fd output (logcat/console)
struct FdCtx { int fd; };

static void ModuleToFd(uint64_t baseAddr, const char* filename, void* ctx)
{
    struct FdCtx* fc = (struct FdCtx*)ctx;
    char line[384];
    int len = snprintf(line, sizeof(line), "   0x%012llx %s\n", (unsigned long long)baseAddr, filename);
    if (len > 0) write(fc->fd, line, len);
}

void CrashModules_WriteToFd(int fd)
{
    const char* header = "Loaded modules:\n";
    write(fd, header, 17);
    struct FdCtx ctx = { fd };
    EnumerateModules(ModuleToFd, &ctx);
}
