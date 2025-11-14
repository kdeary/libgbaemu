/******************************************************************************\
**
**  This file is part of the Hades GBA Emulator, and is made available under
**  the terms of the GNU General Public License version 2.
**
**  Copyright (C) 2021-2024 - The Hades Authors
**
\******************************************************************************/
/*
** Modifications by Korbin Deary (kdeary).
** Licensed under the same terms as the Hades emulator (GNU GPLv2).
*/


#include "hs.h"
#include "gba/gba.h"

void
core_thumb_swi(
    struct gba *gba,
    uint16_t op
) {
    core_interrupt(gba, VEC_SVC, MODE_SVC, false);
}
