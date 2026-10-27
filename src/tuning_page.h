#ifndef LIVETUNE_TUNING_PAGE_H
#define LIVETUNE_TUNING_PAGE_H
#include <stddef.h>

#include "tuning_tab.h"

// Chains onto Extra Options' UI resource hook so the Tuning screen gets the
// Enhanced Tuning panel and page state. Returns 1 when chained.
int tuning_page_install(unsigned char *image, size_t image_size, TabLog log);

// The text edit on its own (for testing): returns the new length, or 0 when
// the input isn't the Tuning screen or doesn't match.
size_t tuning_page_edit(const char *text, size_t length, char *out, size_t capacity);

#endif
