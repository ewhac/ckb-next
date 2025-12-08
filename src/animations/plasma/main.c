/*
 * plasma - An animation plugin for ckb-next.  Strongly influenced by --
 *          okay, ripped off from -- the ReallySlick screensaver "plasma."
 *
 * Leo L. Schwab <ewhac@ewhac.org>                      2025.05.13
 ****
 * Copyright (C) 2025 Leo L. Schwab
 *
 * This plugin is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include <bits/types/struct_timeval.h>
#include <ckb-next/animation.h>
#include <sys/time.h>

#include "uthash.h"


/**************************************************************************
 * #defines
 */
#define TWO_PI          (2.0 * M_PI)
#define NUMCONSTS       (3 * 6)

#define COUNT_OF(ary)   (sizeof (ary) / sizeof (0[ary]))

#define fabstrunc(v)    (v <= 0  ?  (v >= -1.0 ? -v : 1.0)  :  (v <= 1.0 ? v : 1.0))


/**************************************************************************
 * Structs.
 */
/*  Relies on ckb-key structs not being reallocated/changing addresses.  */
struct plasmadot {
    struct UT_hash_handle   hh;
    ckb_key                 *key;
    float                   x, y;
    float                   r, g, b;
};


/**************************************************************************
 * Globals.
 */
double  g_zoom;
double  g_focus;
double  g_speed;

//  "Plasma" coordinate space.
float   g_wide, g_high;
float   g_long, g_short;
float   g_aspect_ratio;

float   s[NUMCONSTS];   // sines
float   a[NUMCONSTS];   // angles: 0 <= a <= TWO_PI
float   da[NUMCONSTS];  // delta angles


/*
 * We need extra data per key, but CKB doesn't offer a user pointer in the
 * ckb_key struct.  So we create a parallel hashmap, indexed by a ckb_key*.
 */
// Base of hashmap.
struct plasmadot    *plasmadots = NULL;

// Memory holding plasmadots.
struct plasmadot    *dotmem;


/**************************************************************************
 * Code.
 */
void
ckb_info (void)
{
    // Plugin info
    CKB_NAME ("Plasma");
    CKB_VERSION ("0.1");
    CKB_COPYRIGHT ("2025", "ewhac");
    CKB_LICENSE ("GPLv2");
    CKB_GUID ("{15DCB997-B6E2-40D2-9E92-2D3A65938E67}");
    CKB_DESCRIPTION ("A flowing plasma effect.");

    // Effect parameters
    CKB_PARAM_DOUBLE ("zoom", "Zoom factor into plasma field:", "", 20.0, 0.1, 50.0);
    CKB_PARAM_DOUBLE ("focus", "Focus -- \"sharpness\" of plasma edges:", "", 1, 0.5, 50);
    CKB_PARAM_DOUBLE ("speed", "Animation speed:", "", 10.0, 0.1, 50);

    // Timing/input parameters
    CKB_KPMODE (CKB_KP_NONE);
    CKB_TIMEMODE (CKB_TIME_DURATION);
    CKB_LIVEPARAMS (TRUE);	// FIXME:?
    CKB_REPEAT (FALSE);

    // Presets
    CKB_PRESET_START ("Gentle Swirls");
    CKB_PRESET_PARAM ("zoom", "20.0");
    CKB_PRESET_PARAM ("focus", "1");
    CKB_PRESET_PARAM ("speed", "10.0");
    CKB_PRESET_END;
}


/**
 * Initialize plugin.
 *
 * @param context - Pointer to device context.
 */
void
ckb_init (ckb_runctx *context)
{
    struct timeval  now;

    // Randomize.
    gettimeofday (&now, NULL);
    srand (now.tv_usec);
}


/**
 * Parse plugin parameters
 *
 * @param _context - Pointer to device context.
 * @param name - name of parameter to parse.
 * @param value - pointer to storage to deposit parsed value.
 */
void
ckb_parameter (ckb_runctx *_context, char const *name, char const *value)
{
    CKB_PARSE_DOUBLE ("zoom", &g_zoom){}
    CKB_PARSE_DOUBLE ("focus", &g_focus){}
    CKB_PARSE_DOUBLE ("speed", &g_speed){}
}


/**
 * Start/stop plugin activity.
 *
 * @param context - Pointer to device context.
 * @param state - non-zero == activate plugin; zero == deactivate plugin.
 */
void
ckb_start (ckb_runctx *context, int state)
{
    struct plasmadot    *pd;
    ckb_key             *key;
    float               *pa;
    float               *pda;
    int                 i;

    if (!state) {
        /*  TODO: Erase the hashmap here?  */
        return;
    }

    if (plasmadots) {
        HASH_CLEAR (hh, plasmadots);
    }

    g_aspect_ratio = (float) context->width / (float) context->height;
    if (g_aspect_ratio >= 1.0) {
        g_wide = 30.0 / g_zoom;
        g_high = g_wide / g_aspect_ratio;
        g_long = context->width;
        g_short = g_long - (context->width - context->height) * 0.75f;
    } else {
        g_high = 30.0 / g_zoom;
        g_wide = g_high * g_aspect_ratio;
        g_long = context->height;
        g_short = g_long - (context->height - context->width) * 0.75f;
    }

    /*
     * Initialize angles and velocities.
     */
    pa = a;
    pda = da;
    i = COUNT_OF (a);
    while (--i >= 0) {
        *pa++ = (float) rand() / (float) RAND_MAX * TWO_PI;
        *pda++ = (float) rand() / (float) RAND_MAX * 0.005;
    }

    /*  Allocate memory to hold hashmap entries.  */
    if (!dotmem) {
        dotmem = malloc (sizeof (*dotmem) * context->keycount);
        if (!dotmem) {
            return;
        }
    }

    /*
     * Build hashmap of keys to plasma info.
     */
    for (pd = dotmem, key = context->keys, i = context->keycount;
         --i >= 0;
         ++pd, ++key)
    {
        pd->key = key;

        pd->r =
        pd->g =
        pd->b = 0.0;

        /*
         * Convert from key location space to quasi-plasma coordinate space.
         * Note x and y are transposed.
         */
        if (g_aspect_ratio >= 1.0) {
            pd->x = (float) (key->y * g_wide) / g_long - (g_wide / 2.0);
            pd->y = (float) (key->x * g_high) / g_short - (g_high / 2.0);
        } else {
            pd->x = (float) (key->y * g_wide) / g_short - (g_wide / 2.0);
            pd->y = (float) (key->x * g_high) / g_long - (g_high / 2.0);
        }

        HASH_ADD_PTR (plasmadots, key, pd);
    }
}


void
ckb_keypress (ckb_runctx *context, ckb_key *key, int x, int y, int state)
{
    // Unused
}


/**
 * Called to advance "time" in the plugin.
 *
 * @param context - Pointer to device context.
 * @param delta - "Duration" since last frame.  May be thought of as "progress" through Playback:Duration for looping animations.
 */
void
ckb_time (ckb_runctx *context, double delta)
{
    float   *ps = s;
    float   *pa = a;
    float   *pda = da;

    for (int i = COUNT_OF (s);  --i >= 0;  ++ps, ++pa, ++pda) {
        // FIXME: Adjust according to delta.
        float new_a = *pa + *pda * g_speed + 0.0001;
        if (new_a >= TWO_PI)
            new_a -= TWO_PI;
        *ps = sin (new_a) * g_focus;
        *pa = new_a;
    }
}

/**
 * Called when it's time to render a "frame" of the LED pattern.
 *
 * @param context - Pointer to device context.
 */
int
ckb_frame (ckb_runctx *context)
{
    const float         maxdiff = 0.004f * (float) g_speed;
    struct plasmadot    *pd;
    ckb_key             *key;
    float               temp;
    float               r, g, b;
    float               posx, posy;
    int                 i;

    // Update colors
    for (key = context->keys, i = context->keycount;  --i >= 0;  ++key) {
        HASH_FIND_PTR (plasmadots, &key, pd);
        if (!pd) {
            fprintf (stderr, "Key %s not in hashmap.", key->name);
            continue;
        }

        posx = (float) pd->x;
        posy = (float) pd->y;

        r = pd->r;
        g = pd->g;
        b = pd->b;
        pd->r = 0.7f * (  s[ 0] * posx
                        + s[ 1] * posy
                        + s[ 2] * (posx * posx + 1.0f)
                        + s[ 3] * posx * posy
                        + s[ 4] * g
                        + s[ 5] * b);
        pd->g = 0.7f * (  s[ 6] * posx
                        + s[ 7] * posy
                        + s[ 8] * posx * posx
                        + s[ 9] * (posy * posy - 1.0f)
                        + s[10] * r
                        + s[11] * b);
        pd->b = 0.7f * (  s[12] * posx
                        + s[13] * posy
                        + s[14] * (1.0f - posx * posy)
                        + s[15] * posy * posy
                        + s[16] * r
                        + s[17] * g);

        temp = pd->r - r;
        if (temp > maxdiff)
            pd->r = r + maxdiff;
        if (temp < -maxdiff)
            pd->r = r - maxdiff;
        temp = pd->g - g;
        if (temp > maxdiff)
            pd->g = g + maxdiff;
        if (temp < -maxdiff)
            pd->g = g - maxdiff;
        temp = pd->b - b;
        if (temp > maxdiff)
            pd->b = b + maxdiff;
        if (temp < -maxdiff)
            pd->b = b - maxdiff;

        ckb_alpha_blend (key, 255, fabstrunc (pd->r) * 255, fabstrunc (pd->g) * 255, fabstrunc (pd->b) * 255);
    }
    return 0;
}
