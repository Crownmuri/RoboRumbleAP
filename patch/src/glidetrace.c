/* [general] trace_glide=1 (rr_glide.exe only): logs every Glide call the game makes during
   two frames after Scroll Lock is pressed, with arguments. Triangles are only counted.
   Used to see what a screen does per frame (clears, colour masks, LFB reads/writes, ...).
   Every glide2x import of the exe is redirected to a small stub that logs and then jumps on
   to the real function with the stack untouched. */
#include "common.h"
#include <stdio.h>
#include <stdlib.h>

#define MAX_FN 64
static const char *g_names[MAX_FN];
static int g_nargs[MAX_FN];
void *g_glide_real[MAX_FN];
static int g_nfn;

static int g_frames_left;          /* > 0 while tracing */
static int g_swap_index = -1;
static int g_tri_count;
static SHORT g_prev_scroll;

/* called from the stubs: frame = { index, return address, args... } */
void __cdecl glide_trace_log(DWORD *frame)
{
    int i = (int)frame[0];
    if (i == g_swap_index) {
        SHORT s = GetAsyncKeyState(VK_SCROLL);
        if ((s & 1) && !g_frames_left) { g_frames_left = 3; rr_log("trace: ---- start ----"); }
        g_prev_scroll = s;
    }
    if (g_frames_left <= 0) return;
    const char *n = g_names[i];
    if (strstr(n, "Triangle") || strstr(n, "DrawLine")) { g_tri_count++; return; }
    if (g_tri_count) { rr_log("trace:   (%d triangles/lines)", g_tri_count); g_tri_count = 0; }
    char args[160] = "";
    int len = 0;
    for (int a = 0; a < g_nargs[i] && a < 7; a++)
        len += snprintf(args + len, sizeof args - len, "%s%lx", a ? ", " : "", frame[2 + a]);
    rr_log("trace: %s(%s)", n, args);
    if (i == g_swap_index && --g_frames_left == 0) rr_log("trace: ---- end ----");
}

/* stub_N: push N; jmp common.  common: log, then pop N and jump to the real function. */
#define STUB(n) "_glide_stub_" #n ":\n pushl $" #n "\n jmp glide_trace_common\n"
__asm__(
    ".text\n"
    "glide_trace_common:\n"
    "    pushal\n"
    "    leal 32(%esp), %eax\n"
    "    pushl %eax\n"
    "    call _glide_trace_log\n"
    "    addl $4, %esp\n"
    "    popal\n"
    "    popl %eax\n"                      /* index (eax is free at a stdcall call boundary) */
    "    jmp *_g_glide_real(,%eax,4)\n"
    STUB(0) STUB(1) STUB(2) STUB(3) STUB(4) STUB(5) STUB(6) STUB(7) STUB(8) STUB(9)
    STUB(10) STUB(11) STUB(12) STUB(13) STUB(14) STUB(15) STUB(16) STUB(17) STUB(18) STUB(19)
    STUB(20) STUB(21) STUB(22) STUB(23) STUB(24) STUB(25) STUB(26) STUB(27) STUB(28) STUB(29)
    STUB(30) STUB(31) STUB(32) STUB(33) STUB(34) STUB(35) STUB(36) STUB(37) STUB(38) STUB(39)
    STUB(40) STUB(41) STUB(42) STUB(43) STUB(44) STUB(45) STUB(46) STUB(47) STUB(48) STUB(49)
    STUB(50) STUB(51) STUB(52) STUB(53) STUB(54) STUB(55) STUB(56) STUB(57) STUB(58) STUB(59)
    STUB(60) STUB(61) STUB(62) STUB(63));
#define DECL(n) void glide_stub_##n(void);
DECL(0) DECL(1) DECL(2) DECL(3) DECL(4) DECL(5) DECL(6) DECL(7) DECL(8) DECL(9)
DECL(10) DECL(11) DECL(12) DECL(13) DECL(14) DECL(15) DECL(16) DECL(17) DECL(18) DECL(19)
DECL(20) DECL(21) DECL(22) DECL(23) DECL(24) DECL(25) DECL(26) DECL(27) DECL(28) DECL(29)
DECL(30) DECL(31) DECL(32) DECL(33) DECL(34) DECL(35) DECL(36) DECL(37) DECL(38) DECL(39)
DECL(40) DECL(41) DECL(42) DECL(43) DECL(44) DECL(45) DECL(46) DECL(47) DECL(48) DECL(49)
DECL(50) DECL(51) DECL(52) DECL(53) DECL(54) DECL(55) DECL(56) DECL(57) DECL(58) DECL(59)
DECL(60) DECL(61) DECL(62) DECL(63)
static void (*const g_stubs[MAX_FN])(void) = {
    glide_stub_0, glide_stub_1, glide_stub_2, glide_stub_3, glide_stub_4, glide_stub_5, glide_stub_6, glide_stub_7,
    glide_stub_8, glide_stub_9, glide_stub_10, glide_stub_11, glide_stub_12, glide_stub_13, glide_stub_14, glide_stub_15,
    glide_stub_16, glide_stub_17, glide_stub_18, glide_stub_19, glide_stub_20, glide_stub_21, glide_stub_22, glide_stub_23,
    glide_stub_24, glide_stub_25, glide_stub_26, glide_stub_27, glide_stub_28, glide_stub_29, glide_stub_30, glide_stub_31,
    glide_stub_32, glide_stub_33, glide_stub_34, glide_stub_35, glide_stub_36, glide_stub_37, glide_stub_38, glide_stub_39,
    glide_stub_40, glide_stub_41, glide_stub_42, glide_stub_43, glide_stub_44, glide_stub_45, glide_stub_46, glide_stub_47,
    glide_stub_48, glide_stub_49, glide_stub_50, glide_stub_51, glide_stub_52, glide_stub_53, glide_stub_54, glide_stub_55,
    glide_stub_56, glide_stub_57, glide_stub_58, glide_stub_59, glide_stub_60, glide_stub_61, glide_stub_62, glide_stub_63,
};

void glidetrace_install(void)
{
    if (strcmp(g_exe_name, "rr_glide.exe") != 0 || !GetPrivateProfileIntA("general", "trace_glide", 0, g_ini_path)) return;
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (lstrcmpiA((const char *)(base + imp->Name), "glide2x.dll") != 0) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA *addrs = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData && g_nfn < MAX_FN; names++, addrs++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const char *n = (const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name;
            const char *at = strrchr(n, '@');
            g_names[g_nfn] = n;
            g_nargs[g_nfn] = at ? atoi(at + 1) / 4 : 0;
            if (strstr(n, "grBufferSwap")) g_swap_index = g_nfn;
            g_glide_real[g_nfn] = (void *)addrs->u1.Function;
            DWORD old;
            VirtualProtect(&addrs->u1.Function, 4, PAGE_READWRITE, &old);
            addrs->u1.Function = (DWORD_PTR)g_stubs[g_nfn];
            VirtualProtect(&addrs->u1.Function, 4, old, &old);
            g_nfn++;
        }
    }
    rr_log("trace: %d Glide functions traced; press Scroll Lock in game to log 2 frames", g_nfn);
}
