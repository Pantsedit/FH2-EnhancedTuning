// "ENHANCED TUNING" as the title of the pause menu's Tune Car tile.
//
// The Cars tab of the pause menu (Scenes/Anthem/A_PauseMenu.xaml) has a 2x1
// tile that opens the Tuning screen where our Enhanced Tuning tab lives. The
// tile's text comes from its data item, a TileMenuDataItem resource keyed
// A_MenuDataItem_TuneCar in the same file (the Button's own Content isn't
// shown). We change only that item's Heading, to "ENHANCED TUNING".
// Nothing is added to the tile panel: the game's tile navigation reads grid
// properties from every child of that Canvas and crashes on anything that
// isn't a tile (an overlay Grid there did exactly that).
//
// Extra Options reorders this file's tabs before we see it; the resource is
// unchanged by it. Anchors: the only x:Key="A_MenuDataItem_TuneCar", a
// self-closing <local:TileMenuDataItem .../> with no nested element, and its
// Heading="...". Anything else leaves the file exactly as loaded.

#include <string.h>

#include "ui_badge.h"

static const char screen_class[] = "x:Class=\"ForzaUI.A_PauseMenu\"";
static const char item_key[] = "x:Key=\"A_MenuDataItem_TuneCar\"";
static const char item_tag[] = "<local:TileMenuDataItem";
static const char heading_attribute[] = "Heading=\"";
static const char title[] = "ENHANCED TUNING";

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

int ui_badge_is_xaml(const char *text, size_t length) {
    return find(text, length, screen_class, sizeof screen_class - 1) != NULL;
}

size_t ui_badge_edit(const char *text, size_t length, char *out, size_t capacity) {
    if (!ui_badge_is_xaml(text, length)) return 0;
    const char *key = find_unique(text, length, item_key);
    if (!key) return 0;
    // The item must be a self-closing <local:TileMenuDataItem ...> with no nested element.
    const char *start = key;
    while (start > text && *start != '<') --start;
    if ((size_t)(key - start) < sizeof item_tag - 1 || memcmp(start, item_tag, sizeof item_tag - 1)) return 0;
    const char *close = find(key, length - (size_t)(key - text), "/>", 2);
    if (!close || find(key, (size_t)(close - key), "<", 1)) return 0;
    // Its Heading="..." value, which must sit inside the element.
    const char *value = find(start, (size_t)(close - start), heading_attribute, sizeof heading_attribute - 1);
    if (!value) return 0;
    value += sizeof heading_attribute - 1;
    const char *value_end = find(value, (size_t)(close - value), "\"", 1);
    if (!value_end) return 0;
    size_t old_length = (size_t)(value_end - value);
    if (old_length == sizeof title - 1 && !memcmp(value, title, old_length)) return 0;  // already there

    size_t at = (size_t)(value - text);
    size_t new_length = length - old_length + (sizeof title - 1);
    if (new_length + 1 > capacity) return 0;
    char *w = out;
    memcpy(w, text, at), w += at;
    memcpy(w, title, sizeof title - 1), w += sizeof title - 1;
    memcpy(w, value_end, length - at - old_length), w += length - at - old_length;
    *w = 0;
    return new_length;
}
