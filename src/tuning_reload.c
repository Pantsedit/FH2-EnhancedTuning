// Reload the car when leaving Tuning, so Enhanced Tuning changes apply at once.
//
// Extra Options edits pause.xml in memory to add its Get in Car reload sequence
// (starting at node "_fh2_car_store"), then calls the game's XML parser
// (game+0x13de7e0). It refuses to patch a file that already mentions
// "_fh2_car_", so our edit has to come after its edit, just before parsing.
//
// So the parser's entry gets a jump to us: for pause.xml, if Extra Options'
// sequence is present, the pause menu's Tuning node gets its "back" exit
// pointed at it instead of back to the pause menu. Leaving Tuning then reloads
// the car from the (edited) database and resumes driving. Every other file, and
// pause.xml without that sequence, is parsed exactly as before.
//
// The entry hook (entry_hook.c) only applies when the parser's prologue matches.

#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "entry_hook.h"
#include "tuning_reload.h"

enum {
    XML_PARSE = 0x13de7e0,  // unsigned char parse(CXML4Resource *)
    PROLOGUE = 17,          // push rbp; push rsi; push rdi; sub rsp,30h; mov [rsp+20h],-2
    GAME_ALLOC = 0x3229c0,
    GAME_FREE = 0x31c7b0,
    MAX_XML = 8 * 1024 * 1024,
};

static const unsigned char prologue[PROLOGUE] = {0x40, 0x55, 0x56, 0x57, 0x48, 0x83, 0xec, 0x30,
                                                 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff};

typedef unsigned char (*XmlParseFn)(void *resource);
typedef void *(*AllocFn)(size_t);
typedef void (*FreeFn)(void *);

static unsigned char *game;
static TabLog log_message;
static XmlParseFn original_parse;  // trampoline

static const char tune_node[] = "<node id=\"tune_car_select\" state=\"ui_screens.tune_menu_without_upgrades\">";
static const char back_exit[] = "<exit id=\"back\" target=\"pause_menu.enterback\" />";
static const char reload_exit[] = "<exit id=\"back\" target=\"_fh2_car_store\" />";
static const char reload_node[] = "<node id=\"_fh2_car_store\"";
static const char flow_state[] = "<state id=\"pause_menu_flow\">";

static const char *find(const char *text, size_t length, const char *needle) {
    size_t n = strlen(needle);
    for (size_t i = 0; n <= length && i + n <= length; ++i)
        if (text[i] == needle[0] && !memcmp(text + i, needle, n)) return text + i;
    return NULL;
}

static const char *find_unique(const char *text, size_t length, const char *needle) {
    const char *first = find(text, length, needle);
    if (!first) return NULL;
    return find(first + 1, length - (size_t)(first - text) - 1, needle) ? NULL : first;
}

// The resource's file name, as Extra Options reads it: a short-string-optimised
// std::string at +0xb8 (size +0xc8, capacity +0xd0).
static int is_pause_flow(const unsigned char *resource) {
    size_t length = *(const size_t *)(resource + 0xc8), capacity = *(const size_t *)(resource + 0xd0);
    if (!length || length > 4096 || capacity < length) return 0;
    const char *name = capacity >= 16 ? *(const char *const *)(resource + 0xb8) : (const char *)(resource + 0xb8);
    if (!name) return 0;
    size_t start = 0;
    for (size_t i = 0; i < length; ++i)
        if (name[i] == '/' || name[i] == '\\') start = i + 1;
    return length - start == 9 && !_strnicmp(name + start, "pause.xml", 9);
}

// Returns the new length, or 0 to leave the text alone.
size_t tuning_reload_edit(const char *text, size_t length, char *out, size_t capacity) {
    const char *state = find_unique(text, length, flow_state);
    const char *node = find_unique(text, length, tune_node);
    const char *target = find_unique(text, length, reload_node);
    if (!state || !node || !target || node < state || target < state) return 0;
    const char *state_end = find(state, length - (size_t)(state - text), "</state>");
    const char *node_end = find(node, length - (size_t)(node - text), "</node>");
    // The reload sequence must be a sibling of the Tuning node for the target to resolve.
    if (!state_end || !node_end || node_end > state_end || target > state_end) return 0;
    const char *exit = find_unique(node, (size_t)(node_end - node), back_exit);
    if (!exit) return 0;
    size_t before = (size_t)(exit - text), after = before + strlen(back_exit);
    size_t total = length - strlen(back_exit) + strlen(reload_exit);
    if (total + 1 > capacity) return 0;
    memcpy(out, text, before);
    memcpy(out + before, reload_exit, strlen(reload_exit));
    memcpy(out + before + strlen(reload_exit), text + after, length - after);
    out[total] = 0;
    return total;
}

static unsigned char parse_hook(void *game_resource) {
    unsigned char *resource = game_resource;
    if (resource && is_pause_flow(resource)) {
        char **buffer = (char **)(resource + 0xe0);
        uint32_t *length = (uint32_t *)(resource + 0xe8);
        if (*buffer && *length && *length < MAX_XML) {
            size_t capacity = (size_t)*length + 64;
            char *replacement = ((AllocFn)(game + GAME_ALLOC))(capacity);
            size_t edited = replacement ? tuning_reload_edit(*buffer, *length, replacement, capacity) : 0;
            if (edited) {
                ((FreeFn)(game + GAME_FREE))(*buffer);
                *buffer = replacement;
                *length = (uint32_t)edited;
                log_message("LIVETUNE RELOAD: leaving Tuning now reloads the car.\r\n");
            } else {
                if (replacement) ((FreeFn)(game + GAME_FREE))(replacement);
                log_message("LIVETUNE RELOAD: pause menu flow didn't match (Get in Car sequence missing?); Tuning exit unchanged.\r\n");
            }
        }
    }
    return original_parse(game_resource);
}

int tuning_reload_install(unsigned char *image, size_t image_size, TabLog log, int get_in_car_enabled) {
    game = image;
    log_message = log;
    if (!get_in_car_enabled) {
        log("LIVETUNE RELOAD: Extra Options' Get in Car is off; leave Tuning and use Get in Car manually.\r\n");
        return 0;
    }
    original_parse = (XmlParseFn)entry_hook_install(image, image_size, XML_PARSE, prologue, PROLOGUE, (void *)parse_hook);
    if (!original_parse) {
        log("LIVETUNE RELOAD: menu-flow parser didn't match or stayed busy; Tuning exit unchanged.\r\n");
        return 0;
    }
    log("LIVETUNE RELOAD: hooked the menu-flow parser after Extra Options' edits.\r\n");
    return 1;
}
