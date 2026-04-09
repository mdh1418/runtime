// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Async-signal-safe module enumerator.
// Parses /proc/self/maps manually using only open/read/close.
// No fopen, no fgets, no sscanf, no malloc.

#include "moduleenumerator.h"
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <elf.h>

#if defined(__LP64__)
typedef Elf64_Ehdr ElfNative_Ehdr;
typedef Elf64_Phdr ElfNative_Phdr;
#else
typedef Elf32_Ehdr ElfNative_Ehdr;
typedef Elf32_Phdr ElfNative_Phdr;
#endif

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

// Compute the ELF image base (load bias) for a mapping. For ELF images this is
// more accurate than just using the mapping start, since the first executable
// mapping may begin at a non-zero file offset / virtual address.
static uint64_t ComputeImageBase(uint64_t startAddr, uint64_t endAddr, uint64_t fileOffset)
{
    uint64_t mappedFileBase = startAddr - fileOffset;
    if (mappedFileBase < startAddr || mappedFileBase + sizeof(ElfNative_Ehdr) > endAddr)
    {
        return mappedFileBase;
    }

    const ElfNative_Ehdr* ehdr = (const ElfNative_Ehdr*)(uintptr_t)mappedFileBase;

    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3)
    {
        return mappedFileBase;
    }

    if (ehdr->e_phentsize != sizeof(ElfNative_Phdr))
    {
        return mappedFileBase;
    }

    uint64_t phdrBytes = (uint64_t)ehdr->e_phnum * sizeof(ElfNative_Phdr);
    if (ehdr->e_phoff > endAddr - mappedFileBase || phdrBytes > endAddr - mappedFileBase - ehdr->e_phoff)
    {
        return mappedFileBase;
    }

    const ElfNative_Phdr* phdrs = (const ElfNative_Phdr*)(uintptr_t)(mappedFileBase + ehdr->e_phoff);
    for (int i = 0; i < ehdr->e_phnum; i++)
    {
        if (phdrs[i].p_type == PT_LOAD && phdrs[i].p_offset == 0)
        {
            return mappedFileBase - phdrs[i].p_vaddr;
        }
    }

    return mappedFileBase;
}

// Process one line from /proc/self/maps
// Format: startAddr-endAddr perms offset dev inode pathname
typedef void (*ModuleCallback)(uint64_t startAddr, uint64_t endAddr, uint64_t fileOffset, const char* filename, void* ctx);

static void ParseMapsLine(const char* line, ModuleCallback callback, void* ctx,
                          char* lastModule, int lastModuleSize,
                          int sharedLibrariesOnly, int deduplicate)
{
    uint64_t startAddr;
    const char* p = ParseHex(line, &startAddr);
    if (*p != '-') return;
    p++;

    uint64_t endAddr;
    p = ParseHex(p, &endAddr);
    if (*p != ' ') return;
    p++;

    // Parse the permissions field.
    const char* permissions = p;
    while (*p && *p != ' ') p++;
    if (*p != ' ') return;

    int executable = (p - permissions > 2) && permissions[2] == 'x';
    p++;

    // Parse the file offset so we can recover the image base (load bias).
    while (*p == ' ') p++;
    uint64_t fileOffset = 0;
    p = ParseHex(p, &fileOffset); // offset
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

    // Only show .so and .dll files when requested.
    if (sharedLibrariesOnly && !EndsWith(pathname, ".so") && !EndsWith(pathname, ".dll")) return;

    // Skip duplicate (same module as previous line)
    if (deduplicate && strcmp(pathname, lastModule) == 0) return;

    if (deduplicate)
    {
        int copyLen = plen < lastModuleSize - 1 ? plen : lastModuleSize - 1;
        memcpy(lastModule, pathname, copyLen);
        lastModule[copyLen] = '\0';
    }

    callback(startAddr, endAddr, fileOffset, GetFilename(pathname), ctx);
}

// Read /proc/self/maps and call callback for each unique executable module.
// Async-signal-safe: uses only open/read/close.
static void EnumerateModules(ModuleCallback callback, void* ctx, int sharedLibrariesOnly, int deduplicate)
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
                ParseMapsLine(lineBuf, callback, ctx, lastModule, sizeof(lastModule), sharedLibrariesOnly, deduplicate);
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
static void ModuleToJson(uint64_t startAddr, uint64_t endAddr, uint64_t fileOffset, const char* filename, void* ctx)
{
    CrashJsonWriter* w = (CrashJsonWriter*)ctx;
    uint64_t imageBase = ComputeImageBase(startAddr, endAddr, fileOffset);
    CrashJson_OpenObject(w, NULL);
    CrashJson_WriteString(w, "module_name", filename);
    CrashJson_WriteHex(w, "base_address", imageBase);
    CrashJson_CloseObject(w);
}

void CrashModules_WriteToJson(CrashJsonWriter* w)
{
    CrashJson_OpenArray(w, "modules");
    EnumerateModules(ModuleToJson, w, /*sharedLibrariesOnly*/ 1, /*deduplicate*/ 1);
    CrashJson_CloseArray(w);
}

struct LookupAddressCtx
{
    uint64_t address;
    uint64_t baseAddress;
    char* filename;
    int filenameLen;
    int found;
};

static void ModuleLookupByAddress(uint64_t startAddr, uint64_t endAddr, uint64_t fileOffset, const char* filename, void* ctx)
{
    LookupAddressCtx* lookup = (LookupAddressCtx*)ctx;
    if (lookup->found || lookup->address < startAddr || lookup->address >= endAddr)
    {
        return;
    }

    lookup->baseAddress = ComputeImageBase(startAddr, endAddr, fileOffset);
    lookup->found = 1;

    if (lookup->filename != NULL && lookup->filenameLen > 0)
    {
        int len = 0;
        while (filename[len] && len < lookup->filenameLen - 1)
        {
            lookup->filename[len] = filename[len];
            len++;
        }
        lookup->filename[len] = '\0';
    }
}

int CrashModules_TryLookupModuleForAddress(uint64_t address, uint64_t* baseAddress, char* filename, int filenameLen)
{
    LookupAddressCtx ctx = { address, 0, filename, filenameLen, 0 };
    if (filename != NULL && filenameLen > 0)
    {
        filename[0] = '\0';
    }

    EnumerateModules(ModuleLookupByAddress, &ctx, /*sharedLibrariesOnly*/ 0, /*deduplicate*/ 0);

    if (ctx.found && baseAddress != NULL)
    {
        *baseAddress = ctx.baseAddress;
    }

    return ctx.found;
}

struct ProcessNameCtx
{
    char* filename;
    int filenameLen;
    int found;
};

static void ProcessNameCallback(uint64_t startAddr, uint64_t endAddr, uint64_t fileOffset, const char* filename, void* ctx)
{
    ProcessNameCtx* processName = (ProcessNameCtx*)ctx;
    if (processName->found || processName->filename == NULL || processName->filenameLen <= 0)
    {
        return;
    }

    int len = 0;
    while (filename[len] && len < processName->filenameLen - 1)
    {
        processName->filename[len] = filename[len];
        len++;
    }
    processName->filename[len] = '\0';
    processName->found = 1;
}

int CrashModules_TryGetProcessName(char* filename, int filenameLen)
{
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd != -1)
    {
        char cmdline[256];
        ssize_t bytesRead = read(fd, cmdline, sizeof(cmdline) - 1);
        close(fd);

        if (bytesRead > 0)
        {
            cmdline[bytesRead] = '\0';
            const char* baseName = GetFilename(cmdline);
            int len = 0;
            while (baseName[len] && len < filenameLen - 1)
            {
                filename[len] = baseName[len];
                len++;
            }
            filename[len] = '\0';
            return len > 0;
        }
    }

    ProcessNameCtx ctx = { filename, filenameLen, 0 };
    if (filename != NULL && filenameLen > 0)
    {
        filename[0] = '\0';
    }

    EnumerateModules(ProcessNameCallback, &ctx, /*sharedLibrariesOnly*/ 0, /*deduplicate*/ 1);
    return ctx.found;
}
