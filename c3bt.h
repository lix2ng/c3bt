/*
 * C3BT: Compact Clustered Crit-Bit Tree
 *
 * Copyright (c) 2012, 2013 Ling LI <lix2ng@gmail.com>
 *
 * TERMS OF USE:
 *   1. Do not remove the copyright notice above and this terms of use.
 *   2. You do not need to mention your use of this code, but when you do,
 *      call it "C3BT".
 *   3. This code is provided "as is" and the author disclaims liability for
 *      any consequence caused by this code itself or any larger work that
 *      incorporates it.
 */

#ifndef _C3BT_H_
#define _C3BT_H_

#include <stdint.h>
typedef unsigned int uint;

/* Or you may use <stdbool.h> */
typedef unsigned char bool;
#define true    1
#define false   0

#ifdef __cplusplus
extern "C" {
#endif

#define NODES_PER_CELL  8
/*
 * Enable this to get statistics data of C3BT internals.
 * Note: these are global stats, not per-tree.
 */
#define C3BT_STATS

#ifdef C3BT_STATS
extern uint c3bt_stat_cells; /* numbers of cells in use. */
extern uint c3bt_stat_pushdowns; /* node push-down operations. */
extern uint c3bt_stat_splits; /* cell split operations. */
extern uint c3bt_stat_pushups; /* up-merge of incomplete cells. */
extern uint c3bt_stat_merges; /* cell merges. */
/*
 * Count of cells grouped by occupancy. [n] holds number of cells with n+1
 * nodes.  Note: for ease of implementation, this data is collected during
 * tree_destroy(); so it's an autopsy.
 */
extern uint c3bt_stat_popdist[NODES_PER_CELL];
#endif

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

/* 
 * The opaque version of the tree structure.
 *
 * For details please check c3bt_tree_impl in the source.
 */
typedef struct c3bt_tree {
    void *opaque1[2];
    int opaque2[3];
} c3bt_tree;

/*
 * The opaque version of cursor.
 *
 * Cursor is the internal coordination to indicate an user object's position.
 * It's used in iteration.  Don't modify it directly.
 */
typedef struct c3bt_cursor {
    void *opaque1;
    int16_t opaque2[2];
} c3bt_cursor;

enum c3bt_key_datatypes {
    /* BITS: fixed-length bit string. */
    C3BT_KDT_BITS = 0,
#ifdef C3BT_WITH_STRING
    /*
     * PSTR: the key is a pointer to a zero-terminated string.
     * STR: the key is a zero-terminated string.
     */
    C3BT_KDT_PSTR, C3BT_KDT_STR,
#endif
#ifdef C3BT_WITH_INTS
    C3BT_KDT_U32, C3BT_KDT_S32, C3BT_KDT_U64, C3BT_KDT_S64,
#endif
    C3BT_KDT_CUSTOM,
};

/*
 * Tree initialization with a common data type.
 *
 * kdt - key data type, see c3bt_key_datatypes.
 * koffset - byte offset of the key inside user object.
 * kbits - length (in bits) of the key.
 * Return true if successful.
 *
 * If kdt is a predefined fixed length type (like integers) kbits is ignored.
 * For string type, kbit=0 means "best effort" and effectively kbits is set to
 * maximum supported length; other values will be respected faithfully.  Bit
 * string must have an exact, non-zero length.
 */
extern bool c3bt_init(c3bt_tree *tree, uint kdt, uint koffset, uint kbits);

/*
 * Tree initialization with a custom bitops function.
 *
 * Return true if successful.
 *
 * Internal key data type is set to C3BT_KDT_CUSTOM and koffset is set to 0,
 * which means custom bitops function takes the whole user object as input.
 */
extern bool c3bt_init_bitops(c3bt_tree *tree,
    int (*bitops)(int, void *, void *));

/*
 * Free all cells and uproot the tree.  If C3BT_STATS is defined, it also
 * census population distribution of the cells.
 *
 * Return true if successful.
 */
extern bool c3bt_destroy(c3bt_tree *tree);

/*
 * Get the number of user objects being indexed by the tree.
 *
 * Return 0 if tree is null.
 */
extern uint c3bt_nobjects(c3bt_tree *tree);

/*
 * Add an user object to the C3BT index.
 *
 * Return true if successful; false if user object exists (or no memory).
 */
extern bool c3bt_add(c3bt_tree *tree, void *uobj);

/*
 * Remove an user object from the C3BT index.
 *
 * Return true if successful; false if user object doesn't exist.
 */
extern bool c3bt_remove(c3bt_tree *tree, void *uobj);

/*
 * Find bit string key by value.
 *
 * Return the user object pointer when found, otherwise NULL.  Note that the
 * length of bit-string is fixed during c3bt_init.
 */
extern void *c3bt_find_bits(c3bt_tree *tree, uint8_t *key);

#ifdef C3BT_WITH_STRING
/*
 * Find a zero-terminated string by value.
 *
 * Return the user object pointer when found, otherwise NULL.   This function
 * supports both STR and PSTR.
 */
extern void *c3bt_find_str(c3bt_tree *tree, char *key);
#endif

#ifdef C3BT_WITH_INTS
/*
 * Find an integer key by value.
 *
 * Return the user object pointer when found, otherwise NULL.  Be sure to use a
 * function that matches the key data type, e.g., if a tree is initialized with
 * C3BT_KDT_U32, c3bt_find_s32() won't work (NULL is returned all the time).
 */
extern void *c3bt_find_u32(c3bt_tree *tree, uint32_t key);
extern void *c3bt_find_s32(c3bt_tree *tree, int32_t key);
extern void *c3bt_find_u64(c3bt_tree *tree, uint64_t key);
extern void *c3bt_find_s64(c3bt_tree *tree, int64_t key);
#endif

/*
 * Locate an user object in the tree.
 *
 * Check if an user object exists in the tree, and if true, return a pointer to
 * it and a cursor (if cur is not NULL), which is useful for further iteration.
 * If the user object is not in the tree, NULL is returned and cursor becomes
 * invalid.
 *
 * Note: the search is "by value".  The returned object would have the same
 * valued key as the input object, not necessarily itself.
 */
extern void *c3bt_locate(c3bt_tree *tree, void *uobj, c3bt_cursor *cur);

/*
 * Return the user object with lowest order in a tree.
 *
 * Return a pointer and set the cursor (if cur is not NULL) to the object with
 * lowest order in the tree.  NULL is returned if tree is empty and cursor
 * becomes undefined.
 */
extern void *c3bt_first(c3bt_tree *tree, c3bt_cursor *cur);

/*
 * Return the user object with highest order in a tree.
 *
 * Return a pointer and set the cursor (if cur is not NULL) to the object with
 * highest order in the tree.  NULL is returned if tree is empty and cursor
 * becomes undefined.
 */
extern void *c3bt_last(c3bt_tree *tree, c3bt_cursor *cur);

/*
 * Return next valued user object.
 *
 * Return the next higher ordered user object relative to the cursor, and the
 * cursor is updated accordingly.  cur must be a valid cursor.
 *
 * NULL is returned if current location has no successor, tree is null, empty,
 * or singleton.
 */
extern void *c3bt_next(c3bt_tree *tree, c3bt_cursor *cur);

/*
 * Return previous valued user object.
 *
 * Return the next lower ordered user object relative to the cursor, and the
 * cursor is updated accordingly.  cur must be a valid cursor.
 *
 * NULL is returned if current location has no predecessor, tree is null,
 * empty, or singleton.
 */
extern void *c3bt_prev(c3bt_tree *tree, c3bt_cursor *cur);

#ifdef __cplusplus
}
#endif
#endif /*_C3BT_H_*/

/* vim: set syn=c.doxygen cin et sw=4 ts=4 tw=80 fo=croqmMj: */
