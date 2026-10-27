#ifndef LIVETUNE_UI_BADGE_H
#define LIVETUNE_UI_BADGE_H
#include <stddef.h>

// Retitles the pause menu's Tune Car tile (Cars tab) "ENHANCED TUNING".
// Returns the new length, or 0 to leave the text untouched: not that screen,
// already badged, an anchor missing or not unique, or `capacity` too small.
// `out` receives a NUL-terminated copy with the badge spliced in.
size_t ui_badge_edit(const char *text, size_t length, char *out, size_t capacity);

// The screen check on its own, so a resource hook can skip other files cheaply.
int ui_badge_is_xaml(const char *text, size_t length);

#endif
