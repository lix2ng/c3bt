/*
 * C3BT: Compact Clustered Crit-Bit Tree
 *
 * Terms of Use
 *
 *  1.  You do not need to mention your use of this code, but when you do, call
 *      it "C3BT".
 *  2.  You may use this code for any purpose in any way.  However the author
 *      disclaims liability for any consequence caused by this code itself or
 *      any larger work that incorporates it.
 *
 *  Copyright 2012 Ling LI <lix2ng@gmail.com>
 */

#ifndef _C3BT_H_
#define _C3BT_H_

#include <stdint.h>
typedef unsigned int uint;

/* Or you may use <stdbool.h> */
typedef unsigned char bool;
#define true    1
#define false   0

/*
 * Enable this to get statistics data of C3BT internals.
 * Note: these are global stats, not per-tree.
 */
#define C3BT_STATS

#ifdef C3BT_STATS
extern uint c3bt_stat_cells; /* numbers of cells in use. */
extern uint c3bt_stat_pushdowns; /* node push-down operations. */
extern uint c3bt_stat_splits; /* cell split operations. */
extern uint c3bt_stat_pushups; /* up-merge incomplete cells. */
extern uint c3bt_stat_mergeups; /* explicit upwards cell merges. */
extern uint c3bt_stat_mergedowns; /* explicit downwards cell merges. */
extern uint c3bt_stat_failed_merges; /* unsuccessful explicit merges. */
/*
 * Count of cells grouped by occupancy. [0] to [7] represent number of cells
 * with 1~8 nodes respectively. Note: for ease of implementation, this data is
 * collected during tree_destroy(); so it's an autopsy.
 */
extern uint c3bt_stat_popdist[8];
#endif

/*
 * Try merging an underpopulated cell into one of its sub-cells. It's a bit
 * expensive yet has marginal effect, hence disabled by default.
 */
#undef C3BT_ENABLE_MERGE_DOWN

/* Feature configurations. */
#define C3BT_FEATURE_MAX

#if defined(C3BT_FEATURE_MIN)
/* Minimal: bit-string only. */
#undef  C3BT_WITH_STRING
#undef  C3BT_WITH_INTS
#undef  C3BT_WITH_FLOATS
#elif defined(C3BT_FEATURE_COMMON)
/* Minimal + string, 32 and 64 bit integers. */
#define C3BT_WITH_STRING
#define C3BT_WITH_INTS
#undef  C3BT_WITH_FLOATS
#elif defined(C3BT_FEATURE_MAX)
/* Common + single and double precision floating point. */
#define C3BT_WITH_STRING
#define C3BT_WITH_INTS
#define C3BT_WITH_FLOATS
#endif

typedef struct c3bt_tree {
    void *_opaque1[2];
    int _opaque2[3];
} c3bt_tree;

typedef uint64_t c3bt_cursor;

enum c3bt_key_datatypes {
    /* BITS: fixed-length bit string. */
    C3BT_KDT_BITS = 0,
    /*
     * PSTR: the key is a pointer to a zero-terminated string.
     * STR: the key is a zero-terminated string.
     */
#ifdef C3BT_WITH_STRING
    C3BT_KDT_PSTR, C3BT_KDT_STR,
#endif
#ifdef C3BT_WITH_INTS
    C3BT_KDT_U32, C3BT_KDT_S32, C3BT_KDT_U64, C3BT_KDT_S64,
#endif
    C3BT_KDT_CUSTOM,
};

/* Initialization etc. */
bool c3bt_init(c3bt_tree *tree, uint kdt, uint koffset, uint kbits);
bool c3bt_init_bitops(c3bt_tree *tree, int (*bitops)(int, void *, void *));
bool c3bt_destroy(c3bt_tree *tree);
uint c3bt_nobjects(c3bt_tree *tree);

/* Manipulations. */
bool c3bt_add(c3bt_tree *tree, void *uobj);
bool c3bt_remove(c3bt_tree *tree, void *uobj);

/* Find user object by key value. */
void *c3bt_find_bits(c3bt_tree *tree, uint8_t *key);

#ifdef C3BT_WITH_STRING
void *c3bt_find_str(c3bt_tree *tree, char *key);
#endif

#ifdef C3BT_WITH_INTS
void *c3bt_find_u32(c3bt_tree *tree, uint32_t key);
void *c3bt_find_s32(c3bt_tree *tree, int32_t key);
void *c3bt_find_u64(c3bt_tree *tree, uint64_t key);
void *c3bt_find_s64(c3bt_tree *tree, int64_t key);
#endif

/* Functions that can be used to start iteration. */
void *c3bt_locate(c3bt_tree *tree, void *uobj, c3bt_cursor *cur);
void *c3bt_first(c3bt_tree *tree, c3bt_cursor *cur);
void *c3bt_last(c3bt_tree *tree, c3bt_cursor *cur);

/* Iteration functions. */
void *c3bt_next(c3bt_tree *tree, c3bt_cursor *cur);
void *c3bt_prev(c3bt_tree *tree, c3bt_cursor *cur);

#endif /*_C3BT_H_*/

/* vim: set syn=c.doxygen cin et sw=4 ts=4 tw=80 fo=croqmM: */
