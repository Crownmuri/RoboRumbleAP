#include "common.h"

void *rr_hook_iat(HMODULE module, const char *dll, const char *func, void *replacement)
{
    BYTE *base = (BYTE *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
         imp->Name; imp++) {
        if (lstrcmpiA((const char *)(base + imp->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk
                                                                ? imp->OriginalFirstThunk
                                                                : imp->FirstThunk));
        IMAGE_THUNK_DATA *addrs = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, func) != 0) continue;
            DWORD old;
            void *prev = (void *)addrs->u1.Function;
            VirtualProtect(&addrs->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            addrs->u1.Function = (DWORD_PTR)replacement;
            VirtualProtect(&addrs->u1.Function, sizeof(void *), old, &old);
            return prev;
        }
    }
    return NULL;
}

BYTE *rr_find_unique(const BYTE *pattern, size_t len)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    BYTE *found = NULL;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE *p = base + sec->VirtualAddress;
        size_t size = sec->Misc.VirtualSize;
        for (size_t i = 0; i + len <= size; i++) {
            if (p[i] == pattern[0] && memcmp(p + i, pattern, len) == 0) {
                if (found) return NULL;
                found = p + i;
            }
        }
    }
    return found;
}

BOOL rr_write_jmp(BYTE *at, void *to, size_t len)
{
    DWORD old;
    if (len < 5 || !VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old)) return FALSE;
    at[0] = 0xE9;
    *(DWORD *)(at + 1) = (DWORD)((BYTE *)to - (at + 5));
    for (size_t i = 5; i < len; i++) at[i] = 0x90;
    VirtualProtect(at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, len);
    return TRUE;
}
