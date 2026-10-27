// Enhanced Tuning tab: adds a 10th category to the game's Tuning menu.
//
// The game keeps its 9 tuning categories (Tires .. Differential) in a static
// table of 0x50-byte entries and indexes it by the selected tab with no bounds
// check. The table is packed against another one, so it can't grow in place.
// Instead this builds a 10-entry copy next to the game image (the 9 originals
// keep pointing at their own callback storage), redirects the three
// RIP-relative references to it and bumps the page's loop count from 9 to 10.
//
// Non-destructive: nothing is written unless every patch site holds exactly
// the expected bytes and the original table is populated. All changes live in
// memory for this session only.

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tuning_tab.h"

enum {
    TAB_TABLE = 0x3540a10,         // 9 entries, 0x50 bytes each
    TAB_COUNT = 9,
    TAB_STRIDE = 0x50,
    TIRES_STATE_NAME = 0x1f7b9f8,  // "VisualState_Tires", entry 0's state name
    TIRES_LABEL_VTABLE = 0x2e5aad8,
    GAME_ALLOC = 0x3229c0,
    LOC_LOOKUP = 0x123a570,        // (out, table, id, unused, 0) -> holder
    MAKE_LABEL = 0x140af10,        // (out, &object) wraps a string for the tab bar
    LABEL_ID = 64000,              // "ENHANCED TUNING", added to Tuning via strings.json
};

// The three readers of the table: page set-up (label callbacks at +0x30),
// panel focus (+0x08) and tab change (+0x00).
static const struct {
    uint32_t rva;
    uint8_t opcode[3];
    uint32_t field;
} references[] = {
    {0xe90d60, {0x48, 0x8d, 0x1d}, 0x30},  // lea rbx, [rip+X]
    {0xe93f73, {0x48, 0x8d, 0x05}, 0x08},  // lea rax, [rip+X]
    {0xe94dfa, {0x48, 0x8d, 0x35}, 0x00},  // lea rsi, [rip+X]
};
// One-byte limits that assume 9 tabs: the page's loop count (lea r14d,[rdi+0xa]
// with rdi = -1) and the next-tab handler's "stop at the last tab" (cmp rbx, 8).
static const struct {
    uint32_t rva;
    uint8_t bytes[4];  // last byte is the limit
} limits[] = {
    {0xe90d67, {0x44, 0x8d, 0x77, 0x0a}},
    {0xe8a8db, {0x48, 0x83, 0xfb, 0x08}},
};

typedef struct {
    unsigned char entries[(TAB_COUNT + 1) * TAB_STRIDE];
    void *label_vtable[6];
    char state_name[32];
    char panel_name[32];
} TabBlock;

static unsigned char *game;

typedef void *(*AllocFn)(size_t);
typedef void **(*LocLookupFn)(void **, const char *, unsigned, void *, int);
typedef void *(*MakeLabelFn)(void *, void **);
typedef void (*VirtualFn)(void *);

static void virtual_call(void *object, unsigned slot) {
    ((VirtualFn)(*(void ***)object)[slot])(object);
}

// Slot 2 of the label callback: same as the game's own label functions, but
// with our string ID.
static void *label_call(void *self, void *out) {
    (void)self;
    void *found[4] = {0};
    void **result = ((LocLookupFn)(game + LOC_LOOKUP))(found, "Tuning", LABEL_ID, NULL, 0);
    void *text = result ? *result : NULL;
    if (text) virtual_call(text, 2);   // add a reference for the holder
    void *holder = text;
    ((MakeLabelFn)(game + MAKE_LABEL))(out, &holder);
    if (holder) virtual_call(holder, 3);
    if (found[0]) virtual_call(found[0], 3);
    return out;
}

static TabBlock *block;

// Slots 0/1 (copy/move): the callable has no state, so a copy is just our vtable.
static void *label_copy(void *self, void *destination) {
    (void)self;
    if (!destination) destination = ((AllocFn)(game + GAME_ALLOC))(0x10);
    if (destination) *(void **)destination = block->label_vtable;
    return destination;
}

static void *near_allocation(unsigned char *image, size_t image_size, size_t size) {
    // RIP-relative displacements reach +-2 GB, so stay well inside that.
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    uintptr_t granularity = info.dwAllocationGranularity;
    uintptr_t start = ((uintptr_t)image + image_size + granularity - 1) & ~(granularity - 1);
    for (uintptr_t at = start; at < start + 0x40000000u; at += granularity) {
        void *memory = VirtualAlloc((void *)at, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (memory) return memory;
    }
    return NULL;
}

static int32_t displacement_to(uint32_t rva, int length, const void *target) {
    return (int32_t)((intptr_t)target - (intptr_t)(game + rva + length));
}

static int patch_bytes(void *at, const void *bytes, size_t size) {
    DWORD old;
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(at, bytes, size);
    VirtualProtect(at, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, size);
    return 1;
}

int tuning_tab_install(unsigned char *image, size_t image_size, int first, TabLog log) {
    char line[200];
    game = image;
    if (image_size < TAB_TABLE + TAB_COUNT * TAB_STRIDE) { log("LIVETUNE TAB: unexpected game image size; tab not added.\r\n"); return 0; }

    // Every site must hold exactly the instruction we expect, aimed at the original table.
    for (unsigned i = 0; i < ARRAYSIZE(references); ++i) {
        unsigned char *site = image + references[i].rva;
        int32_t disp;
        memcpy(&disp, site + 3, 4);
        if (memcmp(site, references[i].opcode, 3) ||
            site + 7 + disp != image + TAB_TABLE + references[i].field) {
            snprintf(line, sizeof line, "LIVETUNE TAB: unexpected code at game+0x%x; tab not added.\r\n", references[i].rva);
            log(line);
            return 0;
        }
    }
    for (unsigned i = 0; i < ARRAYSIZE(limits); ++i) {
        if (memcmp(image + limits[i].rva, limits[i].bytes, sizeof limits[i].bytes)) {
            snprintf(line, sizeof line, "LIVETUNE TAB: unexpected tab limit at game+0x%x; tab not added.\r\n", limits[i].rva);
            log(line);
            return 0;
        }
    }
    // The game's static initializer must already have filled the table.
    unsigned char *table = image + TAB_TABLE;
    if (*(unsigned char **)table != image + TIRES_STATE_NAME) {
        log("LIVETUNE TAB: tuning tab table not initialised as expected; tab not added.\r\n");
        return 0;
    }
    for (unsigned i = 0; i < TAB_COUNT; ++i) {
        unsigned char *entry = table + i * TAB_STRIDE;
        if (!*(void **)entry || !*(void **)(entry + 8) || !*(void **)(entry + 0x48)) {
            log("LIVETUNE TAB: a tuning tab entry is empty; tab not added.\r\n");
            return 0;
        }
    }

    block = near_allocation(image, image_size, sizeof *block);
    if (!block) { log("LIVETUNE TAB: no memory near the game image; tab not added.\r\n"); return 0; }
    memset(block, 0, sizeof *block);
    strcpy(block->state_name, "VisualState_EnhancedTuning");
    strcpy(block->panel_name, "StackPanel_0_EnhancedTuning");

    // Our label callback: the Tires one's vtable with our copy and call slots.
    memcpy(block->label_vtable, image + TIRES_LABEL_VTABLE, sizeof block->label_vtable);
    block->label_vtable[0] = (void *)label_copy;
    block->label_vtable[1] = (void *)label_copy;
    block->label_vtable[2] = (void *)label_call;

    unsigned ours = first ? 0 : TAB_COUNT;
    for (unsigned i = 0, at = 0; i <= TAB_COUNT; ++i) {
        unsigned char *entry = block->entries + i * TAB_STRIDE;
        if (i == ours) {
            *(char **)entry = block->state_name;
            *(char **)(entry + 0x08) = block->panel_name;
            // +0x10..0x2f: no secondary state (impl pointer at +0x28 stays 0)
            *(void **)(entry + 0x30) = block->label_vtable;
            *(void **)(entry + 0x48) = entry + 0x30;
            continue;
        }
        memcpy(entry, table + at++ * TAB_STRIDE, TAB_STRIDE);
    }
    FlushInstructionCache(GetCurrentProcess(), block, sizeof *block);

    // Redirect the readers first, the count last, so the loop never runs past a
    // table. Any failure puts back what was already changed.
    int32_t originals[ARRAYSIZE(references)];
    unsigned done = 0;
    for (; done < ARRAYSIZE(references); ++done) {
        unsigned char *site = image + references[done].rva + 3;
        int32_t disp = displacement_to(references[done].rva, 7, block->entries + references[done].field);
        memcpy(&originals[done], site, sizeof disp);
        if (!patch_bytes(site, &disp, sizeof disp)) break;
    }
    unsigned raised = 0;
    if (done == ARRAYSIZE(references)) {
        for (; raised < ARRAYSIZE(limits); ++raised) {
            uint8_t limit = limits[raised].bytes[3] + 1;
            if (!patch_bytes(image + limits[raised].rva + 3, &limit, 1)) break;
        }
    }
    if (done < ARRAYSIZE(references) || raised < ARRAYSIZE(limits)) {
        while (raised--) patch_bytes(image + limits[raised].rva + 3, &limits[raised].bytes[3], 1);
        while (done--) patch_bytes(image + references[done].rva + 3, &originals[done], sizeof originals[done]);
        log("LIVETUNE TAB: could not patch the tuning menu code; changes undone, tab not added.\r\n");
        return 0;
    }
    snprintf(line, sizeof line, "LIVETUNE TAB: Enhanced Tuning tab added as tab %u of 10.\r\n", ours + 1);
    log(line);
    return 1;
}
