// Enhanced Tuning sliders: native UI properties on the Tuning page's type.
//
// Every slider on the Tuning screen binds to a property that the game's UI
// layer looks up by name on the page's data type. Extra Options adds its video
// settings the same way: copy the game's float-property template, give it our
// name and getter/setter, and link it into the type's property list.
//
// The Tuning screen uses two data types: the slider rows bind through the one
// that owns "Tuning" and "Benchmark" (set up by game+0xe98cd0), the tab bar
// through the one that owns "Categories" (game+0xe97ae0). Each property goes on
// both, each with its own list link.
//
// Only adds new names; no existing property or type is changed.

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tuning_slider.h"

// Each type's set-up registers a known property with `lea rcx,[rip+registry]`
// then `call register_property`; both are checked before we add ours.
static const struct {
    uint32_t init, registry, lea, call;
    const char *what;
} page_types[] = {
    {0xe98cd0, 0x3541a50, 0xe98dbc, 0xe98dc3, "slider rows (Tuning/Benchmark)"},
    {0xe97ae0, 0x35417e0, 0xe97bda, 0xe97be1, "tab bar (Categories)"},
};

enum {
    FLOAT_PROPERTY = 0xed6e90,     // returns the game's float property template
    REGISTER_PROPERTY = 0x155ee30, // (registry, link)
    OBJECT_INIT = 0x143eb80,
    BOX_FLOAT = 0x3f5990,          // (out, &value) -> boxed float
    PROPERTY_CHANGED = 0xcf9c60,   // (page, property): refresh bindings
};

typedef struct {
    void **vtable;
    unsigned char header[0x20];
    void *getter, *setter, *unused;
    const char *name;
    void *type;
    uint64_t flags;
} NativeProperty;

typedef struct Link {
    NativeProperty *property;
    struct Link *next;
} Link;

_Static_assert(sizeof(NativeProperty) == 0x58, "native property layout");

typedef void **(*BoxFloatFn)(void **, const float *);
typedef void (*PropertyChangedFn)(void *, void *);

static unsigned char *game;
static const SliderSpec *specs;
static NativeProperty properties[MAX_SLIDERS];
static Link links[MAX_SLIDERS][ARRAYSIZE(page_types)];  // one list link per slider and type
static SliderGet read_value;
static SliderSet write_value;

static TabLog log_message;
static volatile LONG set_logs;  // the first slider changes are logged, then it goes quiet

static void **get_value(unsigned index, void **output, void *page) {
    (void)page;
    float value = read_value(index);
    *output = NULL;
    return ((BoxFloatFn)(game + BOX_FLOAT))(output, &value);
}

static void set_value(unsigned index, NativeProperty *property, void *page, void *boxed) {
    const SliderSpec *spec = &specs[index];
    float value = -1;
    char line[200];
    if (boxed) memcpy(&value, (unsigned char *)boxed + 0x28, sizeof value);  // boxed float payload
    int ok = boxed && value == value && value >= spec->min && value <= spec->max;
    if (ok) {
        value = (float)(int)(value / spec->step + 0.5f) * spec->step;  // snap to the slider's steps
        if (value < spec->min) value = spec->min;
        if (value > spec->max) value = spec->max;
        ok = write_value(index, value);
        if (ok) ((PropertyChangedFn)(game + PROPERTY_CHANGED))(page, property);
    }
    if (InterlockedIncrement(&set_logs) <= 20) {
        snprintf(line, sizeof line, "LIVETUNE SLIDER: set %s %.2f -> %s\r\n", spec->property, value,
                 ok ? "applied" : "not applied (unchanged, or no car or database yet)");
        log_message(line);
    }
}

// The game calls a property's getter/setter without saying which slider it is
// for, so each slot gets its own pair that knows its index.
#define SLOT(i)                                                                              \
    static void **get_##i(void **output, NativeProperty *property, void *page) {            \
        (void)property;                                                                      \
        return get_value(i, output, page);                                                   \
    }                                                                                        \
    static void set_##i(NativeProperty *property, void *page, void *boxed) { set_value(i, property, page, boxed); }
SLOT(0) SLOT(1) SLOT(2) SLOT(3) SLOT(4) SLOT(5) SLOT(6) SLOT(7) SLOT(8) SLOT(9) SLOT(10) SLOT(11) SLOT(12) SLOT(13) SLOT(14) SLOT(15)
#undef SLOT

typedef void **(*GetterFn)(void **, NativeProperty *, void *);
typedef void (*SetterFn)(NativeProperty *, void *, void *);
static const GetterFn getters[MAX_SLIDERS] = {get_0, get_1, get_2, get_3, get_4, get_5,
                                              get_6, get_7, get_8, get_9, get_10, get_11,
                                              get_12, get_13, get_14, get_15};
static const SetterFn setters[MAX_SLIDERS] = {set_0, set_1, set_2, set_3, set_4, set_5,
                                              set_6, set_7, set_8, set_9, set_10, set_11,
                                              set_12, set_13, set_14, set_15};

int tuning_slider_install(unsigned char *image, size_t image_size, TabLog log, const SliderSpec *sliders,
                          unsigned count, SliderGet get, SliderSet set) {
    game = image;
    log_message = log;
    specs = sliders;
    read_value = get;
    write_value = set;
    if (!count || count > MAX_SLIDERS) {
        log("LIVETUNE SLIDER: bad slider count; sliders not added.\r\n");
        return 0;
    }
    // Each type's own set-up must still register its properties exactly where we expect.
    for (unsigned i = 0; i < ARRAYSIZE(page_types); ++i) {
        int32_t disp, rel;
        if (image_size <= page_types[i].registry) return 0;
        memcpy(&disp, image + page_types[i].lea + 3, 4);
        memcpy(&rel, image + page_types[i].call + 1, 4);
        if (memcmp(image + page_types[i].lea, "\x48\x8d\x0d", 3) ||
            image + page_types[i].lea + 7 + disp != image + page_types[i].registry ||
            image[page_types[i].call] != 0xe8 || image + page_types[i].call + 5 + rel != image + REGISTER_PROPERTY) {
            log("LIVETUNE SLIDER: Tuning page type didn't match; sliders not added.\r\n");
            return 0;
        }
    }
    for (unsigned i = 0; i < ARRAYSIZE(page_types); ++i)
        ((void (*)(void))(image + page_types[i].init))();  // idempotent: guarded by the game's own flags

    NativeProperty *template_property = ((NativeProperty * (*)(void))(image + FLOAT_PROPERTY))();
    if (!template_property || !template_property->vtable) {
        log("LIVETUNE SLIDER: no float property template; sliders not added.\r\n");
        return 0;
    }
    for (unsigned s = 0; s < count; ++s) {
        NativeProperty *property = &properties[s];
        ((void (*)(void *))(image + OBJECT_INIT))(property);
        property->vtable = template_property->vtable;
        memcpy((unsigned char *)property + 0x28, (unsigned char *)template_property + 0x28, 0x30);
        property->name = sliders[s].property;
        property->getter = (void *)getters[s];
        property->setter = (void *)setters[s];
        for (unsigned i = 0; i < ARRAYSIZE(page_types); ++i) {
            links[s][i].property = property;
            ((void (*)(void *, Link *))(image + REGISTER_PROPERTY))(image + page_types[i].registry, &links[s][i]);
        }
    }
    for (unsigned i = 0; i < ARRAYSIZE(page_types); ++i) {
        char line[160];
        snprintf(line, sizeof line, "LIVETUNE SLIDER: %u sliders registered on the %s type.\r\n", count, page_types[i].what);
        log(line);
    }
    return 1;
}
