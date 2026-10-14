#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// More about the aircraft being looked at: its registered owner from
// adsbdb.com, and a photo from planespotters.net. Looked up one aircraft at
// a time as they are asked for, newest first, and kept for a while.

#define DETAILS_PHOTO_H 150     // photos are scaled to this height
#define DETAILS_PHOTO_W_MAX 300 // and no wider

void details_start(void);

// hex's registered owner into out: "" while it is looked up, or when nobody
// knows. A miss queues a lookup. Safe from any task.
void details_owner(const char *hex, char *out, size_t n);

// hex's photo as RGB565 into px, which holds DETAILS_PHOTO_W_MAX *
// DETAILS_PHOTO_H pixels, with its size and the photographer's name. False
// while it is looked up or when there is none; a miss queues a lookup. reg,
// the registration ("" if none), finds more photos than the address does.
// Safe from any task.
bool details_photo(const char *hex, const char *reg, uint16_t *px, int *w, int *h, char *credit, size_t cn);
