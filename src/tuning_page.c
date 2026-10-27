// Enhanced Tuning page: gives the new tab its own panel in the Tuning screen.
//
// The game loads each UI page's XAML through one call (game+0x16ebb37), which
// Extra Options redirects to a small jump of its own (game+0x13fe354) so it can
// edit some screens. We chain onto that jump: the game and Extra Options load
// the file as usual, then, only for the Tuning screen, we insert three things
// at fixed anchors in whatever text came back:
//   - a storyboard that shows our panel,
//   - the VisualState_EnhancedTuning state that plays it,
//   - the StackPanel_0_EnhancedTuning panel itself, one row per slider in tunes.c.
// Anything unexpected (Extra Options' hook missing, an anchor not found exactly
// once, the panel already present) leaves the file exactly as it was loaded.

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tuning_page.h"
#include "ui_badge.h"
#include "tunes.h"

enum {
    RESOURCE_CALL = 0x16ebb37,  // call into Extra Options' jump (originally the game's loader)
    RESOURCE_THUNK = 0x13fe354, // mov rax, imm64; jmp rax
    NATIVE_ALLOCATE = 0x3229c0,
    NATIVE_FREE = 0x31c7b0,
    OBJECT_INIT = 0x143eb80,
    OBJECT_CLEANUP = 0x1442050,
    DELETE_OBJECT = 0x257730,
    MAX_XAML = 1024 * 1024,
};

typedef struct {
    void **vtable;
    unsigned char header[0x20];
    char *bytes;
    int length;
    int capacity;
} NativeResource;

typedef void **(*GetResourceFn)(void *, void **);
typedef void *(*AllocateFn)(size_t);
typedef void (*ObjectFn)(void *);

static unsigned char *game;
static TabLog log_message;
static GetResourceFn chained;  // Extra Options' handler, which calls the game's loader
static void *resource_vtable[7];
static volatile LONG logged_mismatch;

static const char screen_class[] = "x:Class=\"ForzaUI.Scenes.Anthem.A_TuningOptions\"";
static const char our_panel_name[] = "x:Name=\"StackPanel_0_EnhancedTuning\"";
static char panel[65536];  // our CommonListBox, built once by build_panel

typedef struct {
    const char *anchor;
    int after;          // insert after the anchor instead of before it
    const char *text;
} Insert;

static const Insert inserts[] = {
    {"<Storyboard x:Key=\"A_Storyboard_VisualState_FWD\">", 0,
     "<local:NumUnitConverter x:Key=\"LiveTune_Multiplier\" FractionDigits=\"2\" ShowUnit=\"False\"/>\r\n\r\n"
     "            <Storyboard x:Key=\"LiveTune_Storyboard_VisualState_EnhancedTuning\">\r\n"
     "                <ObjectAnimationUsingKeyFrames Storyboard.TargetProperty=\"(UIElement.Visibility)\" Storyboard.TargetName=\"StackPanel_0_EnhancedTuning\">\r\n"
     "                    <DiscreteObjectKeyFrame KeyTime=\"0\" Value=\"{x:Static Visibility.Visible}\"/>\r\n"
     "                </ObjectAnimationUsingKeyFrames>\r\n"
     "            </Storyboard>\r\n\r\n            "},
    {"<VisualState x:Name=\"VisualState_Differential\" Storyboard=\"{StaticResource A_Storyboard_VisualState_Differential}\"/>", 1,
     "\r\n                <VisualState x:Name=\"VisualState_EnhancedTuning\" Storyboard=\"{StaticResource LiveTune_Storyboard_VisualState_EnhancedTuning}\"/>"},
    {"<local:CommonListBox x:Name=\"StackPanel_9_Differential\"", 0, panel},  // built from tune_sliders
};

// The panel's rows come from the same table the native properties are made
// from, so the XAML ranges always match what the setters accept. Same two-row
// shape as the game's own sliders: a header per group, then one row per slider
// whose Tag is the help text, with the slider's stock setting as the history
// marker (1.0 for a multiplier, 0 for MW Handling). The list box scrolls
// (Shared_TextMenus' FZScrollViewer), so rows past the panel's 600 px are reached
// by moving down.
static const char panel_open[] =
    "<local:CommonListBox x:Name=\"StackPanel_0_EnhancedTuning\"\r\n"
    "                                             KeyboardNavigation.DirectionalNavigation=\"Contained\"\r\n"
    "                                             Style=\"{StaticResource A_Style_ListBox_List_Tuning}\"\r\n"
    "                                             ItemContainerStyle=\"{StaticResource A_Style_ListBoxItem_List_Tuning}\">\r\n";
static const char panel_header[] =
    "                                <ItemsControl Style=\"{StaticResource A_Style_ItemsControl_1x3_TuningSpaced_Header}\">\r\n"
    "                                    <local:FZTextBlock Style=\"{StaticResource A_Style_FZTextBlock_Tune_ItemTitle}\" Text=\"%s\" HorizontalAlignment=\"Left\" Grid.Column=\"0\"/>\r\n"
    "                                    <local:FZTextBlock Style=\"{StaticResource A_Style_FZTextBlock_Tune_ItemTitle}\" Text=\"{local:Loc Tuning.IDS_Low}\" HorizontalAlignment=\"Left\" Grid.Column=\"1\"/>\r\n"
    "                                    <local:FZTextBlock Style=\"{StaticResource A_Style_FZTextBlock_Tune_ItemTitle}\" Text=\"{local:Loc Tuning.IDS_High}\" HorizontalAlignment=\"Right\" Grid.Column=\"1\"/>\r\n"
    "                                </ItemsControl>\r\n";
static const char panel_row[] =
    "                                <ItemsControl Style=\"{StaticResource A_Style_ItemsControl_1x3_TuningSpaced}\" Tag=\"%s\">\r\n"
    "                                    <local:FZTextBlock Style=\"{StaticResource A_Style_FZTextBlock_Tune_ItemLabel}\" Text=\"%s\" HorizontalAlignment=\"Left\" Grid.Column=\"0\"/>\r\n"
    "                                    <Grid Grid.Column=\"1\">\r\n"
    "                                        <Slider Value=\"{Binding %s, Mode=TwoWay}\" Minimum=\"%g\" Maximum=\"%g\" SmallChange=\"%g\" LargeChange=\"%g\" Style=\"{StaticResource A_Style_Slider_Tuning_Adjust}\"/>\r\n"
    "                                        <Slider Value=\"%g\" Minimum=\"%g\" Maximum=\"%g\" Style=\"{StaticResource A_Style_Slider_Tuning_Adjust_History}\"/>\r\n"
    "                                    </Grid>\r\n"
    "                                    <local:FZTextBlock Style=\"{StaticResource A_Style_FZTextBlock_Tune_ItemLabel}\" Text=\"{Binding %s, Converter={StaticResource LiveTune_Multiplier}}\" HorizontalAlignment=\"Right\" Grid.Column=\"2\"/>\r\n"
    "                                </ItemsControl>\r\n";
static const char panel_close[] = "                            </local:CommonListBox>\r\n\r\n                            ";

static void escape_xml(char *out, size_t size, const char *text) {
    size_t n = 0;
    for (; *text && n + 7 < size; ++text) {
        const char *entity = *text == '&' ? "&amp;" : *text == '<' ? "&lt;" : *text == '>' ? "&gt;"
                           : *text == '"' ? "&quot;" : NULL;
        if (entity) { memcpy(out + n, entity, strlen(entity)); n += strlen(entity); }
        else out[n++] = *text;
    }
    out[n] = 0;
}

// Returns 0 when the panel doesn't fit (it then stays empty and the edit is refused).
static int build_panel(void) {
    size_t n = 0;
    char group[64], label[96], help[1024];
    const char *last_group = NULL;
    n += (size_t)snprintf(panel + n, sizeof panel - n, "%s", panel_open);
    for (unsigned i = 0; i < tune_slider_count && n < sizeof panel; ++i) {
        const SliderSpec *s = &tune_sliders[i];
        if (!last_group || strcmp(last_group, s->group)) {
            escape_xml(group, sizeof group, s->group);
            n += (size_t)snprintf(panel + n, sizeof panel - n, panel_header, group);
            last_group = s->group;
        }
        if (n >= sizeof panel) break;
        escape_xml(label, sizeof label, s->label);
        escape_xml(help, sizeof help, s->help);
        n += (size_t)snprintf(panel + n, sizeof panel - n, panel_row, help, label, s->property, s->min, s->max,
                              s->small_change, s->large_change, s->stock, s->min, s->max, s->property);
    }
    if (n < sizeof panel) n += (size_t)snprintf(panel + n, sizeof panel - n, "%s", panel_close);
    if (n >= sizeof panel) {
        panel[0] = 0;
        return 0;
    }
    return 1;
}

static const char *find(const char *text, size_t length, const char *needle, size_t needle_length) {
    if (needle_length > length) return NULL;
    for (size_t i = 0; i + needle_length <= length; ++i)
        if (text[i] == needle[0] && !memcmp(text + i, needle, needle_length)) return text + i;
    return NULL;
}

static const char *find_unique(const char *text, size_t length, const char *needle) {
    size_t n = strlen(needle);
    const char *first = find(text, length, needle, n);
    if (!first) return NULL;
    size_t rest = length - (size_t)(first - text) - 1;
    return find(first + 1, rest, needle, n) ? NULL : first;
}

static void report_once(const char *message) {
    if (!InterlockedExchange(&logged_mismatch, 1)) log_message(message);
}

// Returns the new length, or 0 to keep the original. `out` holds `capacity` bytes.
size_t tuning_page_edit(const char *text, size_t length, char *out, size_t capacity) {
    if (!find(text, length, screen_class, strlen(screen_class))) return 0;
    if (find(text, length, our_panel_name, strlen(our_panel_name))) return 0;  // already there
    if (!panel[0] && !build_panel()) return 0;  // normally built at install
    const char *at[ARRAYSIZE(inserts)];
    size_t total = length;
    for (unsigned i = 0; i < ARRAYSIZE(inserts); ++i) {
        at[i] = find_unique(text, length, inserts[i].anchor);
        if (!at[i]) {
            report_once("LIVETUNE PAGE: Tuning screen layout did not match; Enhanced Tuning page not added.\r\n");
            return 0;
        }
        if (inserts[i].after) at[i] += strlen(inserts[i].anchor);
        total += strlen(inserts[i].text);
    }
    if (total + 1 > capacity) return 0;
    // Copy in text order, splicing each insert at its position.
    size_t copied = 0, written = 0;
    int done[ARRAYSIZE(inserts)] = {0};
    for (;;) {
        int next = -1;
        for (unsigned i = 0; i < ARRAYSIZE(inserts); ++i)
            if (!done[i] && (next < 0 || at[i] < at[next])) next = (int)i;
        if (next < 0) break;
        done[next] = 1;
        size_t upto = (size_t)(at[next] - text);
        memcpy(out + written, text + copied, upto - copied);
        written += upto - copied;
        copied = upto;
        size_t n = strlen(inserts[next].text);
        memcpy(out + written, inserts[next].text, n);
        written += n;
    }
    memcpy(out + written, text + copied, length - copied);
    written += length - copied;
    out[written] = 0;
    return written;
}

static void release_object(void *object) {
    if (object && InterlockedDecrement((volatile LONG *)((unsigned char *)object + 0x10)) == 0)
        ((ObjectFn)(game + DELETE_OBJECT))(object);
}

static void *delete_resource(NativeResource *resource, unsigned flags) {
    ((ObjectFn)(game + OBJECT_CLEANUP))(resource);
    if (flags & 1) ((ObjectFn)(game + NATIVE_FREE))(resource);
    return resource;
}

static void **get_resource(void *reader, void **output) {
    chained(reader, output);
    NativeResource *original = *output;
    if (!original || !original->bytes || original->length <= 0 || original->length > MAX_XAML / 2) return output;
    // Two screens are edited: the Tuning screen (our tab's panel) and the pause
    // menu (the Tune Car tile's title, ui_badge.c).
    int badge = ui_badge_is_xaml(original->bytes, (size_t)original->length);
    if (!badge && !find(original->bytes, (size_t)original->length, screen_class, strlen(screen_class))) return output;

    size_t capacity = (size_t)original->length + strlen(panel) + 4096;  // panel plus the two small inserts
    NativeResource *replacement = ((AllocateFn)(game + NATIVE_ALLOCATE))(sizeof(NativeResource) + capacity);
    if (!replacement) return output;
    char *bytes = (char *)(replacement + 1);
    size_t length = badge ? ui_badge_edit(original->bytes, (size_t)original->length, bytes, capacity)
                          : tuning_page_edit(original->bytes, (size_t)original->length, bytes, capacity);
    if (!length) {
        ((ObjectFn)(game + NATIVE_FREE))(replacement);
        return output;
    }
    // Same shape as the loader's own resource; the parser owns it by reference count.
    ((ObjectFn)(game + OBJECT_INIT))(replacement);
    memcpy(resource_vtable, original->vtable - 1, sizeof resource_vtable);
    resource_vtable[1] = (void *)delete_resource;
    replacement->vtable = resource_vtable + 1;
    replacement->bytes = bytes;
    replacement->length = (int)length;
    replacement->capacity = (int)capacity;
    *output = replacement;
    release_object(original);
    log_message(badge ? "LIVETUNE PAGE: Tune Car tile retitled \"ENHANCED TUNING\".\r\n"
                      : "LIVETUNE PAGE: Enhanced Tuning page added to the Tuning screen.\r\n");
    return output;
}

int tuning_page_install(unsigned char *image, size_t image_size, TabLog log) {
    game = image;
    log_message = log;
    if (image_size <= RESOURCE_CALL + 5) return 0;
    unsigned char *call = image + RESOURCE_CALL;
    int32_t rel;
    memcpy(&rel, call + 1, 4);
    unsigned char *thunk = image + RESOURCE_THUNK;
    if (call[0] != 0xe8 || call + 5 + rel != thunk || thunk[0] != 0x48 || thunk[1] != 0xb8 ||
        thunk[10] != 0xff || thunk[11] != 0xe0) {
        log("LIVETUNE PAGE: Extra Options' UI hook isn't active; the tab has no page of its own.\r\n");
        return 0;
    }
    uint64_t target;
    memcpy(&target, thunk + 2, 8);
    if (!target) return 0;
    if (!build_panel()) {
        log("LIVETUNE PAGE: Enhanced Tuning rows don't fit; the tab has no page of its own.\r\n");
        return 0;
    }
    chained = (GetResourceFn)(uintptr_t)target;

    // The 8-byte target sits inside one cache line, so one aligned-enough store
    // swaps it atomically even while other threads load UI.
    DWORD old;
    if (!VirtualProtect(thunk, 12, PAGE_EXECUTE_READWRITE, &old)) return 0;
    *(volatile uint64_t *)(thunk + 2) = (uint64_t)(uintptr_t)get_resource;
    VirtualProtect(thunk, 12, old, &old);
    FlushInstructionCache(GetCurrentProcess(), thunk, 12);
    log("LIVETUNE PAGE: chained onto Extra Options' UI hook.\r\n");
    return 1;
}
