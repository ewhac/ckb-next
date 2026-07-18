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
#include <ckb-next/animation.h>
#include <time.h>

#include <uthash.h>


/**************************************************************************
 * #defines
 */
#define TWO_PI          (2.0 * M_PI)
#define NUMCONSTS       (3 * 6)

#define COUNT_OF(ary)   (sizeof (ary) / sizeof (0[ary]))


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
double  zoom;
double  sharpness;
double  speed;

//  "Plasma" coordinate space.
float   wide, high;
float   longer, shorter;
float   aspect_ratio;

float   sine[NUMCONSTS];    // sines
float   ang[NUMCONSTS];     // angles: 0 <= a <= TWO_PI
float   d_ang[NUMCONSTS];   // delta angles


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
/**
 * Returns abs(val) clamped to range [0.0, max].
 */
static double
fabsclamp (double val, double max)
{
    val = fabs (val);
    return val > max ? max : val;
}

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
    CKB_PARAM_DOUBLE ("zoom", "Zoom factor into plasma field:", "", 10.0, 0.1, 50.0);
    CKB_PARAM_DOUBLE ("sharpness", "\"Sharpness\" of plasma edges:", "", 1, 0.5, 50);
    CKB_PARAM_DOUBLE ("speed", "Animation speed:", "", 10.0, 0.1, 50);

    // Timing/input parameters
    CKB_KPMODE (CKB_KP_NONE);
    CKB_TIMEMODE (CKB_TIME_DURATION);
    CKB_LIVEPARAMS (TRUE);
    CKB_REPEAT (FALSE);

    // Presets
    CKB_PRESET_START ("Gentle Swirls");
    CKB_PRESET_PARAM ("zoom", "20.0");
    CKB_PRESET_PARAM ("sharpness", "1");
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
    // Seed RNG.
    srand (time (NULL));
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
    CKB_PARSE_DOUBLE ("zoom", &zoom){}
    CKB_PARSE_DOUBLE ("sharpness", &sharpness){}
    CKB_PARSE_DOUBLE ("speed", &speed){}
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

    if (plasmadots) {
        /*  Always clear this; we rebuild it from scratch.  */
        HASH_CLEAR (hh, plasmadots);
    }

    if (!state) {
        /*  Deactivate plugin.  */
        if (dotmem) {
            free (dotmem);
            dotmem = NULL;
        }
        return;
    }

    aspect_ratio = (float) context->width / (float) context->height;
    if (aspect_ratio >= 1.0) {
        wide = 30.0 / zoom;
        high = wide / aspect_ratio;
        longer = context->width;
        shorter = longer - (context->width - context->height) * 0.75f;
    } else {
        high = 30.0 / zoom;
        wide = high * aspect_ratio;
        longer = context->height;
        shorter = longer - (context->height - context->width) * 0.75f;
    }

    /*
     * Initialize angles and velocities.
     */
    pa = ang;
    pda = d_ang;
    i = COUNT_OF (ang);
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

        pd->r = 0.0;
        pd->g = 0.0;
        pd->b = 0.0;

        /*
         * Convert from key location space to quasi-plasma coordinate space.
         * Note x and y are transposed.
         */
        if (aspect_ratio >= 1.0) {
            pd->x = (float) (key->y * wide) / longer - (wide / 2.0);
            pd->y = (float) (key->x * high) / shorter - (high / 2.0);
        } else {
            pd->x = (float) (key->y * wide) / shorter - (wide / 2.0);
            pd->y = (float) (key->x * high) / longer - (high / 2.0);
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
    float *ps = sine;
    float *pa = ang;
    float *pda = d_ang;

    for (int i = COUNT_OF (sine);  --i >= 0;  ++ps, ++pa, ++pda) {
        float new_a = *pa + *pda * speed + 0.0001;
        if (new_a >= TWO_PI)
            new_a -= TWO_PI;
        *ps = sin (new_a) * sharpness;
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
    const float         maxdiff = 0.004f * (float) speed;
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
            DBG ("plasma: Key %s not in hashmap.", key->name);
            continue;
        }

        posx = (float) pd->x;
        posy = (float) pd->y;

        r = pd->r;
        g = pd->g;
        b = pd->b;
        pd->r = 0.7f * (  sine[ 0] * posx
                        + sine[ 1] * posy
                        + sine[ 2] * (posx * posx + 1.0f)
                        + sine[ 3] * posx * posy
                        + sine[ 4] * g
                        + sine[ 5] * b);
        pd->g = 0.7f * (  sine[ 6] * posx
                        + sine[ 7] * posy
                        + sine[ 8] * posx * posx
                        + sine[ 9] * (posy * posy - 1.0f)
                        + sine[10] * r
                        + sine[11] * b);
        pd->b = 0.7f * (  sine[12] * posx
                        + sine[13] * posy
                        + sine[14] * (1.0f - posx * posy)
                        + sine[15] * posy * posy
                        + sine[16] * r
                        + sine[17] * g);

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

        ckb_alpha_blend (key, 255, fabsclamp (pd->r, 1.0) * 255, fabsclamp (pd->g, 1.0) * 255, fabsclamp (pd->b, 1.0) * 255);
    }
    return 0;
}
