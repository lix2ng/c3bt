/*
 * C3BT: Compact Clustered Crit-Bit Tree
 *
 * Copyright (c) 2012 Ling LI <lix2ng@gmail.com>
 *
 * TERMS OF USE:
 *   1. Do not remove the copyright notice above and this terms of use.
 *   2. You do not need to mention your use of this code, but when you do,
 *      call it "C3BT".
 *   3. This code is provided "as is" and the author disclaims liability for
 *      any consequence caused by this code itself or any larger work that
 *      incorporates it.
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#ifndef __GNUC__
#error "GCC IS REQUIRED."
#endif

#ifdef _LP64
#error "64-BIT NOT IMPLEMENTED."
#endif

#include "c3bt.h"

#define _likely(x)      __builtin_expect((x), 1)
#define _unlikely(x)    __builtin_expect((x), 0)
#define _compact        __attribute__((packed))
#define _inline         __attribute__((always_inline, unused))
#define _noinline       __attribute__((noinline, noclone))
#define _osize          __attribute__((optimize("Os")))
#define CT_ASSERT(x)    switch(0) {case 0: case(x): ;}

/*
 * Compact representation of a Crit-bit Tree node.
 *
 * Child can reference 3 kinds of targets:
 *    - If MSB (0x80) is set, low bits is the index to a user object pointer.
 *    - If next bit (0x40) is set, low bits is the index to a cell pointer.
 *    - If both bits are clear, it's an index of a c3bt_node within the same
 *      cell.  A special value, 0x3F, is used to mark an unoccupied node slot.
 *
 * The differing bit number (crit-bit) is stored in a byte, so keys can be
 * indexed up to 256 bits in the standard LP32 layout.
 */
typedef struct c3bt_node {
    uint8_t cbit;
    uint8_t child[2];
} c3bt_node;

#define CBIT_MAX            255
#define INVALID_NODE        0x3F
#define CHILD_IS_NODE(x)    ((x) < 8)
#define CHILD_CELL_BIT      0x40
#define CHILD_UOBJ_BIT      0x80
#define CHILD_IS_CELL(x)    ((x) & CHILD_CELL_BIT)
#define CHILD_IS_UOBJ(x)    ((x) & CHILD_UOBJ_BIT)
#define INDEX_MASK          0x0F
#define FLAGS_MASK          (CHILD_CELL_BIT | CHILD_UOBJ_BIT)

/*
 * Each C3BT cell is 64B under standard LP32 layout.
 *
 * Cell must be 8B-aligned to spare 3 low bits as node count.  This is already
 * satisfied by major platforms and C libraries but users of custom allocators
 * shall double check.
 *
 * Cell layout:
 *    - PNC (parent & node count), 4B, [0, 3]
 *    - Array of 8 crit-bit nodes, 24B, [4, 27]
 *    - Array of 9 external pointers, 36B, [28, 63]
 *
 * Node[0] is always the root of the cell's subtree.
 */
typedef struct c3bt_cell c3bt_cell;

struct c3bt_cell {
    c3bt_cell *pnc;
    c3bt_node N[NODES_PER_CELL];
    c3bt_cell *P[NODES_PER_CELL + 1];
};

/* The C3BT tree structure for implementation. */
typedef struct c3bt_tree_impl {
    c3bt_cell *root; /* the root cell. */
    int (*bitops)(int, void *, void *); /* the bitops function. */
    uint n_objects; /* number of user objects == number of nodes + 1. */
    uint key_offset; /* offset to the key in the user object. */
    uint16_t key_type; /* type of the key. */
    uint16_t key_nbits; /* maximum number of bits of the key. */
} c3bt_tree_impl;

typedef struct c3bt_cursor_impl {
    c3bt_cell *cell;
    uint16_t nid; /* node index in cell. */
    uint16_t cid; /* child index (0 or 1). */
} c3bt_cursor_impl;

#ifdef C3BT_STATS
uint c3bt_stat_cells;
uint c3bt_stat_pushdowns;
uint c3bt_stat_splits;
uint c3bt_stat_pushups;
uint c3bt_stat_merges;
uint c3bt_stat_popdist[NODES_PER_CELL];
#endif

/* Standard bitops for common data types. */
static int bitops_bits(int, void *, void *);
#ifdef C3BT_WITH_INTS
static int bitops_u32(int, void *, void *);
static int bitops_s32(int, void *, void *);
static int bitops_u64(int, void *, void *);
static int bitops_s64(int, void *, void *);
#endif
#ifdef C3BT_WITH_STRING
static int bitops_str(int, void *, void *);
static int bitops_pstr(int, void *, void *);
#endif

/*
 * Tree initialization with a common data type.
 */
bool c3bt_init(c3bt_tree *c3bt, uint kdt, uint koffset, uint kbits)
{
    c3bt_tree_impl *tree;

    CT_ASSERT(sizeof(c3bt_node) == 3);
    CT_ASSERT(sizeof(c3bt_cell) == 64);
    CT_ASSERT(sizeof(c3bt_tree) == sizeof(c3bt_tree_impl));
    CT_ASSERT(sizeof(c3bt_cursor) == sizeof(c3bt_cursor_impl));

    if (!c3bt)
        return false;
    tree = (c3bt_tree_impl*)c3bt;
    memset(tree, 0, sizeof(c3bt_tree_impl));
    tree->key_offset = koffset;
    tree->key_type = kdt;
    switch (kdt) {
        case C3BT_KDT_BITS:
            tree->bitops = bitops_bits;
            tree->key_nbits = kbits;
            break;
#ifdef C3BT_WITH_STRING
        case C3BT_KDT_PSTR:
            tree->bitops = bitops_pstr;
            tree->key_nbits = kbits == 0 ? CBIT_MAX + 1 : kbits;
            break;
        case C3BT_KDT_STR:
            tree->bitops = bitops_str;
            tree->key_nbits = kbits == 0 ? CBIT_MAX + 1 : kbits;
            break;
#endif
#ifdef C3BT_WITH_INTS
        case C3BT_KDT_U32:
            tree->bitops = bitops_u32;
            tree->key_nbits = 32;
            break;
        case C3BT_KDT_S32:
            tree->bitops = bitops_s32;
            tree->key_nbits = 32;
            break;
        case C3BT_KDT_U64:
            tree->bitops = bitops_u64;
            tree->key_nbits = 64;
            break;
        case C3BT_KDT_S64:
            tree->bitops = bitops_s64;
            tree->key_nbits = 64;
            break;
#endif
        default:
            return false;
    }
    if (tree->key_nbits > CBIT_MAX + 1)
        tree->key_nbits = CBIT_MAX + 1;
    return true;
}

bool c3bt_init_bitops(c3bt_tree *c3bt, int (*bitops)(int, void *, void *))
{
    c3bt_tree_impl *tree;

    if (!c3bt || !bitops)
        return false;
    tree = (c3bt_tree_impl*)c3bt;
    memset(tree, 0, sizeof(c3bt_tree_impl));
    /* Redundant:
     * tree->key_offset = 0;
     */
    tree->key_type = C3BT_KDT_CUSTOM;
    tree->key_nbits = CBIT_MAX + 1;
    tree->bitops = bitops;
    return true;
}

static int cell_ncount(c3bt_cell *cell)
{
    return ((intptr_t)(cell->pnc) & 7) + 1;
}

static c3bt_cell *cell_parent(c3bt_cell *cell)
{
    return (c3bt_cell*)((intptr_t)(cell->pnc) & ~7);
}

/* Be careful not to overflow or underflow. */
static c3bt_cell *cell_make_pnc(c3bt_cell *parent, int count)
{
    return (c3bt_cell*)(((intptr_t)parent & ~7) | (count - 1));
}

static void cell_set_parent(c3bt_cell *cell, c3bt_cell *parent)
{
    int n;

    n = (intptr_t)(cell->pnc) & 7;
    cell->pnc = (c3bt_cell*)((intptr_t)parent | n);
}

static void cell_inc_ncount(c3bt_cell *cell, int delta)
{
    cell->pnc = (c3bt_cell*)((intptr_t)(cell->pnc) + delta);
}

static void cell_dec_ncount(c3bt_cell *cell, int delta)
{
    cell->pnc = (c3bt_cell*)((intptr_t)(cell->pnc) - delta);
}

static void cell_free_node(c3bt_cell *cell, int nid)
{
    cell->N[nid].child[0] = INVALID_NODE;
}

static void cell_free_ptr(c3bt_cell *cell, int pid)
{
    cell->P[pid] = NULL;
}

static bool cell_node_is_vacant(c3bt_cell *cell, int nid)
{
    return cell->N[nid].child[0] == INVALID_NODE;
}

static uint cell_alloc_node(c3bt_cell *cell)
{
    int i;

    for (i = 1; !cell_node_is_vacant(cell, i); i++)
        /* nothing */;
    cell->N[i].child[0] = 0;
    return i;
}

static uint cell_alloc_ptr(c3bt_cell *cell)
{
    int i;

    for (i = 0; cell->P[i] != NULL; i++)
        /* nothing */;
    cell->P[i] = (c3bt_cell*)1;
    return i;
}

/*
 * Allocate and initialize a new cell.
 *
 * All nodes are marked as vacant, and the rest are zeroed.
 */
static c3bt_cell *cell_malloc(void)
{
    c3bt_cell *cell;
    int i;

    cell = malloc(sizeof(c3bt_cell));
    if (!cell)
        return NULL;
    assert(((intptr_t)cell & 7) == 0);
    memset(cell, 0, sizeof(c3bt_cell));
    for (i = 0; i < NODES_PER_CELL; i++)
        cell_free_node(cell, i);
    return cell;
}

static void cell_free(c3bt_cell *cell)
{
    free(cell);
}

/*
 * Helper function for destruction: find a child cell pointer, delist it and
 * return it to the caller.  This function is stateful and destructive.
 */
static c3bt_cell *cell_delist_subcell(c3bt_cell *cell)
{
    int n, c, tmp;

    if (!cell)
        return NULL;

    for (n = 0; n < NODES_PER_CELL; n++) {
        if (cell_node_is_vacant(cell, n))
            continue;
        for (c = 0; c < 2; c++)
            if (CHILD_IS_CELL(cell->N[n].child[c])) {
                tmp = cell->N[n].child[c] & INDEX_MASK;
                cell->N[n].child[c] = 0;
                return cell->P[tmp];
            }
    }
    return NULL;
}

#ifdef C3BT_STATS
static void cell_update_popdist(c3bt_cell *cell)
{
    int n = cell_ncount(cell) - 1;
    c3bt_stat_popdist[n]++;
}
#endif

bool c3bt_destroy(c3bt_tree *c3bt)
{
    c3bt_cell *cell, *next, *del, *tmp;

    if (c3bt == NULL)
        return false;
    /* Iterative Post-order Traversal of N-way Tree With Delayed Node Access.
     */
    cell = ((c3bt_tree_impl*)c3bt)->root;
    del = NULL;
    while (cell) {
        next = cell_delist_subcell(cell);
        if (!next) {
            cell_free(del);
            del = cell;
#ifdef C3BT_STATS
            cell_update_popdist(cell);
#endif
            next = cell_delist_subcell(cell_parent(cell));
            if (!next) {
                while (cell_parent(cell)) {
                    next = cell_parent(cell);
                    cell_free(del);
                    del = next;
#ifdef C3BT_STATS
                    cell_update_popdist(next);
#endif
                    tmp = cell_delist_subcell(cell_parent(next));
                    if (tmp) {
                        next = tmp;
                        break;
                    } else {
                        cell = next;
                        next = NULL;
                    }
                }
            }
        }
        cell = next;
    }
    cell_free(del);
    memset(c3bt, 0, sizeof(c3bt_tree_impl));
    return true;
}

uint c3bt_nobjects(c3bt_tree *tree)
{
    if (!tree)
        return 0;
    return (((c3bt_tree_impl*)tree)->n_objects);
}

/*
 * Tree lookup by key.
 *
 * Lookup from top of the tree trying to find the key, but it won't verify the
 * result.  Cursor is updated if specified. Special cases:
 *    - Empty tree: return NULL, cur->cell is NULL, nid and cid undefined.
 *    - Singleton tree: always return the uobj and cur is set as (nid=0,
 *      cid=0).
 */
static void *tree_lookup(c3bt_tree_impl *tree, void *key, c3bt_cursor_impl *cur)
{
    c3bt_cell *cell;
    c3bt_cursor_impl loc;
    int nid, cbit_nr, bit;
    void *robj = NULL;

    loc.cell = cell = tree->root;
    if (tree->n_objects == 1) {
        loc.nid = 0;
        loc.cid = 0;
        robj = cell->P[0];
        goto done;
    }
    while (cell) {
        loc.cell = cell;
        nid = 0;
        while (CHILD_IS_NODE(nid)) {
            loc.nid = nid;
            cbit_nr = cell->N[nid].cbit;
            bit = tree->bitops(cbit_nr, key, NULL);
            nid = cell->N[nid].child[bit];
            loc.cid = bit;
        }
        if (CHILD_IS_UOBJ(nid)) {
            robj = cell->P[nid & INDEX_MASK];
            goto done;
        }
        if (CHILD_IS_CELL(nid))
            cell = cell->P[nid & INDEX_MASK];
    }

    done:

    if (cur)
        *cur = loc;
    return robj;
}

/*
 * Find-by-value functions.
 */

void *c3bt_find_bits(c3bt_tree *c3bt, uint8_t *key)
{
    void *robj;
    c3bt_tree_impl *tree = (c3bt_tree_impl*)c3bt;

    if (!key || !tree || tree->key_type != C3BT_KDT_BITS)
        return NULL;

    robj = tree_lookup(tree, key, NULL);
    if (!robj)
        return NULL;
    if (memcmp(key, (char*)robj + tree->key_offset, (tree->key_nbits + 7) / 8)
        == 0)
        return robj;
    return NULL;
}

#ifdef C3BT_WITH_INTS
/* Common for all integers "find-by-value" functions. */
static void *c3bt_find_integer(c3bt_tree *c3bt, uint64_t key, uint kdt)
{
    union {
        uint32_t u32;
        uint64_t u64;
    } bits;
    void *robj;
    c3bt_tree_impl *tree = (c3bt_tree_impl*)c3bt;

    if (!tree || tree->key_type != kdt)
        return NULL;

    switch (kdt) {
        case C3BT_KDT_U32:
        case C3BT_KDT_S32:
            bits.u32 = (uint32_t)key;
            break;
        case C3BT_KDT_U64:
        case C3BT_KDT_S64:
            bits.u64 = key;
            break;
    }

    robj = tree_lookup(tree, &bits, NULL);
    if (!robj)
        return NULL;
    /* Faster than bitops. */
    switch (kdt) {
        case C3BT_KDT_U32:
        case C3BT_KDT_S32:
            if (*(uint32_t*)((char*)robj + tree->key_offset) == bits.u32)
                return robj;
            break;
        case C3BT_KDT_U64:
        case C3BT_KDT_S64:
            if (*(uint64_t*)((char*)robj + tree->key_offset) == bits.u64)
                return robj;
            break;
    }
    return NULL;
}
#endif

#ifdef C3BT_WITH_INTS
void *c3bt_find_u32(c3bt_tree *c3bt, uint32_t key)
{
    return c3bt_find_integer(c3bt, (uint64_t)key, C3BT_KDT_U32);
}

void *c3bt_find_s32(c3bt_tree *c3bt, int32_t key)
{
    return c3bt_find_integer(c3bt, (uint64_t)key, C3BT_KDT_S32);
}

void *c3bt_find_u64(c3bt_tree *c3bt, uint64_t key)
{
    return c3bt_find_integer(c3bt, key, C3BT_KDT_U64);
}

void *c3bt_find_s64(c3bt_tree *c3bt, int64_t key)
{
    return c3bt_find_integer(c3bt, (uint64_t)key, C3BT_KDT_S64);
}
#endif

#ifdef C3BT_WITH_STRING
void *c3bt_find_str(c3bt_tree *c3bt, char *key)
{
    char *str;
    void *robj;
    c3bt_tree_impl *tree;

    if (!key || !c3bt)
        return NULL;

    tree = (c3bt_tree_impl*)c3bt;
    if (tree->key_type == C3BT_KDT_PSTR) {
        str = key;
        robj = tree_lookup(tree, &str, NULL);
    } else if (tree->key_type == C3BT_KDT_STR)
        robj = tree_lookup(tree, key, NULL);
    else
        return NULL;
    if (!robj)
        return NULL;
    if (tree->key_type == C3BT_KDT_PSTR) {
        if (strncmp(key, *(char**)((char*)robj + tree->key_offset),
            tree->key_nbits / 8) == 0)
            return robj;
    } else {
        if (strncmp(key, (char*)robj + tree->key_offset, tree->key_nbits / 8)
            == 0)
            return robj;
    }
    return NULL;
}
#endif

void *c3bt_locate(c3bt_tree *c3bt, void *uobj, c3bt_cursor *cur)
{
    void *robj;
    c3bt_tree_impl *tree;

    if (!c3bt || !uobj)
        return NULL;

    tree = (c3bt_tree_impl*)c3bt;
    robj = tree_lookup(tree, (char*)uobj + tree->key_offset,
        (c3bt_cursor_impl*)cur);
    if (!robj)
        return NULL;
    if (tree->bitops(-(tree->key_nbits + 1), (char*)uobj + tree->key_offset,
        (char*)robj + tree->key_offset) == -1)
        return robj;
    return NULL;
}

/*
 * Go to either extreme end of the tree from a start cursor; used by iteration
 * functions: first(), last(), prev(), next().
 *
 * Tree must have at least 2 uobjs.
 */
static void *tree_rush_down(c3bt_tree_impl *tree, c3bt_cursor_impl *start,
    int dir)
{
    c3bt_cell *cell;
    int nid;

    start->cid = dir;
    cell = start->cell;
    nid = start->nid;
    while (cell) {
        start->cell = cell;
        while (CHILD_IS_NODE(nid)) {
            start->nid = nid;
            nid = cell->N[nid].child[dir];
        }
        if (CHILD_IS_UOBJ(nid))
            return cell->P[nid & INDEX_MASK];
        if (CHILD_IS_CELL(nid)) {
            cell = cell->P[nid & INDEX_MASK];
            nid = 0;
        }
    }
    return NULL;
}

/*
 * Go to either extreme of the tree.  Common code for first and last.
 */
static void *tree_extreme(c3bt_tree_impl *tree, c3bt_cursor *cur, int dir)
{
    c3bt_cursor_impl start;
    void * robj;

    if (!tree || !tree->root)
        return NULL;
    start.cell = tree->root;
    start.nid = 0;
    start.cid = 0;
    if (tree->n_objects == 1) {
        /* Singleton tree. */
        robj = tree->root->P[0];
        goto done;
    }
    robj = tree_rush_down(tree, &start, dir);

    done:

    if (cur)
        *(c3bt_cursor_impl*)cur = start;
    return robj;
}

void *c3bt_first(c3bt_tree *c3bt, c3bt_cursor *cur)
{
    return tree_extreme((c3bt_tree_impl*)c3bt, cur, 0);
}

void *c3bt_last(c3bt_tree *c3bt, c3bt_cursor *cur)
{
    return tree_extreme((c3bt_tree_impl*)c3bt, cur, 1);
}

/*
 * Step a cursor backwards or forwards.  Common for next() and prev().
 */
static void *tree_step(c3bt_tree_impl *tree, c3bt_cursor_impl *cur, int dir)
{
    c3bt_cell *cell;
    void *uobj;
    int upper, lower, bit, cur_cbit;

    /* Nowhere to step if tree is null, empty or singleton. */
    if (!cur || !tree || tree->n_objects < 2)
        return NULL;

    /* The easy case: the other sibling is on the desired path. */
    if (cur->cid != dir)
        goto down;
    /* The hard case: find an ancestor from where we can rush down.
     * Climbing up is cell by cell using the parent pointer; within each
     * cell it's key-guided descent.
     */
    cur_cbit = cur->cell->N[cur->nid].cbit;
    cell = cur->cell;
    uobj = cell->P[cell->N[cur->nid].child[cur->cid] & INDEX_MASK];
    while (cell) {
        lower = 0;
        upper = INVALID_NODE;
        while (CHILD_IS_NODE(lower)) {
            if (cell->N[lower].cbit >= cur_cbit)
                break;
            bit = tree->bitops(cell->N[lower].cbit,
                (char*)uobj + tree->key_offset, NULL);
            if (bit != dir)
                upper = lower;
            lower = cell->N[lower].child[bit];
        }
        if (upper != INVALID_NODE) {
            cur->cell = cell;
            cur->nid = upper;
            goto down;
        }
        cell = cell_parent(cell);
    }
    return NULL;

    down:

    lower = cur->cell->N[cur->nid].child[dir];
    if (CHILD_IS_UOBJ(lower)) {
        cur->cid = dir;
        return cur->cell->P[lower & INDEX_MASK];
    } else {
        if (CHILD_IS_CELL(lower)) {
            cur->cell = cur->cell->P[lower & INDEX_MASK];
            cur->nid = 0;
        } else
            cur->nid = lower;
        return tree_rush_down(tree, cur, 1 - dir);
    }
}

void *c3bt_prev(c3bt_tree *c3bt, c3bt_cursor *cur)
{
    return tree_step((c3bt_tree_impl*)c3bt, (c3bt_cursor_impl*)cur, 0);
}

void *c3bt_next(c3bt_tree *c3bt, c3bt_cursor *cur)
{
    return tree_step((c3bt_tree_impl*)c3bt, (c3bt_cursor_impl*)cur, 1);
}

/*
 * Find node's parent in a cell.
 *
 * Return value is in the form of parent_node_id<<1|which_child_i_am.  Note:
 * input can't be 0.
 */
static int cell_node_parent(c3bt_cell *cell, int node)
{
    int n, c;

    for (n = 0; n < NODES_PER_CELL; n++) {
        if (cell_node_is_vacant(cell, n))
            continue;
        for (c = 0; c < 2; c++)
            if (cell->N[n].child[c] == node)
                goto found;
    }

    found:

    return n << 1 | c;
}

/*
 * Find a split point for a fully populated cell.  Return the would-be new cell
 * root and a bitmap representing the nodes to be moved.
 */
static uint cell_find_split(c3bt_cell *cell, int *bitmap)
{
    uint8_t stack[NODES_PER_CELL - 2];
    int i, top, n, c, count, ret_n, ret_bmp, offset;

    ret_n = ret_bmp = 0; /* shut compiler up. */
    offset = NODES_PER_CELL;
    for (i = NODES_PER_CELL - 1; i > 0; i--) {
        if (!CHILD_IS_NODE(cell->N[i].child[0])
            && !CHILD_IS_NODE(cell->N[i].child[1]))
            continue;
        /* Pre-order traversal to count nodes in a subtree.  Cell-root and edge
         * nodes are excluded.
         */
        count = 1;
        *bitmap = 0;
        stack[0] = i;
        top = 0;
        while (top >= 0) {
            n = stack[top--];
            *bitmap |= 0x8000u >> n;
            for (c = 1; c >= 0; c--)
                if (CHILD_IS_NODE(cell->N[n].child[c])) {
                    stack[++top] = cell->N[n].child[c];
                    count++;
                }
        }
        /* Calculate deviation from perfect split. */
        c = count * 2 - NODES_PER_CELL;
        c = __builtin_abs(c);
        if (c == NODES_PER_CELL % 2)
            return i;
        if (c < offset) {
            offset = c;
            ret_n = i;
            ret_bmp = *bitmap;
        }
    }
    *bitmap = ret_bmp;
    return ret_n;
}

/*
 * Split a full cell in two.  New cell will become original cell's sub-cell.
 */
static bool cell_split(c3bt_cell *cell)
{
    c3bt_cell *new_cell;
    int i, c, p, anchor, new_root, count, bitmap;

    new_cell = cell_malloc();
    if (!new_cell)
        return false;

    new_root = cell_find_split(cell, &bitmap);
    count = 0;
    for (i = 0; i < NODES_PER_CELL; i++) {
        if (!(bitmap & (0x8000u >> i)))
            continue;
        /* Move node[i] and its children to new cell (same location) */
        new_cell->N[i] = cell->N[i];
        for (c = 0; c < 2; c++) {
            p = cell->N[i].child[c];
            if (!CHILD_IS_NODE(p)) {
                if (CHILD_IS_CELL(p))
                    cell_set_parent(cell->P[p & INDEX_MASK], new_cell);
                p &= INDEX_MASK;
                new_cell->P[p] = cell->P[p];
                cell_free_ptr(cell, p);
            }
        }
        count++;
        cell_free_node(cell, i);
    }

    /* Fix old cell. */
    p = cell_alloc_ptr(cell);
    cell->P[p] = new_cell;
    anchor = cell_node_parent(cell, new_root);
    cell->N[anchor >> 1].child[anchor & 1] = CHILD_CELL_BIT | p;
    cell_dec_ncount(cell, count);

    /* Fix new cell. */
    new_cell->N[0] = new_cell->N[new_root];
    cell_free_node(new_cell, new_root);
    new_cell->pnc = cell_make_pnc(cell, count);
    return true;
}

/*
 * Try to push down a node from a full cell.
 */
static bool cell_push_down(c3bt_cell *cell)
{
    int n, np, c, sibling, old_root, new_ptr;
    c3bt_cell *sub;

    for (n = 1; n < NODES_PER_CELL; n++) {
        /* All nodes are taken, no need for vacancy test.*/
        for (c = 0; c < 2; c++) {
            /* Only edge nodes can be pushed down. */
            if (CHILD_IS_CELL(cell->N[n].child[c])
                && !CHILD_IS_NODE(cell->N[n].child[1 - c])) {
                sub = cell->P[cell->N[n].child[c] & INDEX_MASK];
                if (cell_ncount(sub) < NODES_PER_CELL) {
                    sibling = cell->N[n].child[1 - c];
                    old_root = cell_alloc_node(sub);
                    new_ptr = cell_alloc_ptr(sub);
                    cell_inc_ncount(sub, 1);
                    np = cell_node_parent(cell, n);
                    cell->N[np >> 1].child[np & 1] = cell->N[n].child[c];
                    sub->N[old_root] = sub->N[0];
                    sub->P[new_ptr] = cell->P[sibling & INDEX_MASK];
                    sub->N[0].cbit = cell->N[n].cbit;
                    sub->N[0].child[c] = old_root;
                    sub->N[0].child[1 - c] = (sibling & FLAGS_MASK) | new_ptr;
                    if (CHILD_IS_CELL(sibling))
                        cell_set_parent(sub->P[new_ptr], sub);
                    cell_free_node(cell, n);
                    cell_free_ptr(cell, sibling & INDEX_MASK);
                    cell_dec_ncount(cell, 1);
#ifdef C3BT_STATS
                    c3bt_stat_pushdowns++;
#endif
                    return true;
                }
            }
        }
    }
    return false;
}

bool c3bt_add(c3bt_tree *c3bt, void *uobj)
{
    c3bt_tree_impl *tree;
    c3bt_cursor_impl cur;
    void *robj;
    int cbit_nr, bit, new_node, new_ptr, lower;

    if (!c3bt || !uobj)
        return false;
    tree = (c3bt_tree_impl*)c3bt;
    /* Empty -> singleton. */
    if (!tree->root) {
        cur.cell = cell_malloc();
        if (!cur.cell)
            return false;
        tree->root = cur.cell;
        cur.cell->N[0].child[0] = CHILD_UOBJ_BIT | 0;
        cur.cell->N[0].child[1] = CHILD_CELL_BIT | 1;
        cur.cell->P[0] = uobj;
        /* Redundant for a new cell:
         * cur.cell->P[1] = NULL;
         * cur.cell->pnc = cell_make_pnc(NULL, 1);
         */
#ifdef C3BT_STATS
        c3bt_stat_cells = 1;
#endif
        goto done;
    }
    robj = tree_lookup(tree, (char*)uobj + tree->key_offset, &cur);
    cbit_nr = tree->bitops(-(tree->key_nbits + 1),
        (char*)uobj + tree->key_offset, (char*)robj + tree->key_offset);
    if (cbit_nr == -1)
        return false;
    bit = tree->bitops(cbit_nr, (char*)uobj + tree->key_offset, NULL);
    /* Add to singleton. */
    if (tree->n_objects == 1) {
        tree->root->P[1] = uobj;
        tree->root->N[0].cbit = cbit_nr;
        tree->root->N[0].child[bit] = CHILD_UOBJ_BIT | 1;
        tree->root->N[0].child[1 - bit] = CHILD_UOBJ_BIT | 0;
        goto done;
    }
    /* Find insertion point. */
    if (cbit_nr > cur.cell->N[cur.nid].cbit) {
        /* No need to search from root. */
        lower = cur.cell->N[cur.nid].child[cur.cid];
    } else {
        /* Find location for new node.  We need to start from tree root because
         * it must follow the correct path, and we might insert a node with
         * large cbit in a high cell (so upwards cell-by-cell won't work).
         */
        cur.cell = tree->root;

        next:

        cur.nid = INVALID_NODE;
        lower = 0;
        while (!CHILD_IS_UOBJ(lower)) {
            if (cur.cell->N[lower].cbit > cbit_nr)
                break;
            cur.nid = lower;
            cur.cid = tree->bitops(cur.cell->N[lower].cbit,
                (char*)uobj + tree->key_offset, NULL);
            lower = cur.cell->N[lower].child[cur.cid];
            if (CHILD_IS_CELL(lower)) {
                cur.cell = cur.cell->P[lower & INDEX_MASK];
                goto next;
            }
        }
    }
    /* Make room for a full cell.  Re-searching the cell is necessary because
     * we don't know if the insertion point has been moved out.
     */
    if (cell_ncount(cur.cell) == NODES_PER_CELL) {
        /* Try to push down a node first; it's cheaper. */
        if (cell_push_down(cur.cell))
            goto next;
        /* Then we have to split. */
        if (!cell_split(cur.cell))
            return false;
#ifdef C3BT_STATS
        c3bt_stat_cells++;
        c3bt_stat_splits++;
#endif
        goto next;
    }
    new_node = cell_alloc_node(cur.cell);
    new_ptr = cell_alloc_ptr(cur.cell);
    cell_inc_ncount(cur.cell, 1);
    cur.cell->P[new_ptr] = uobj;
    if (cur.nid == INVALID_NODE) {
        /* Insert as cell root. */
        cur.cell->N[new_node] = cur.cell->N[0];
        lower = new_node;
        new_node = 0;
    }
    /* Insert between cur.nid and lower. */
    cur.cell->N[new_node].cbit = cbit_nr;
    if (cur.nid != INVALID_NODE)
        cur.cell->N[cur.nid].child[cur.cid] = new_node;
    cur.cell->N[new_node].child[bit] = new_ptr | CHILD_UOBJ_BIT;
    cur.cell->N[new_node].child[1 - bit] = lower;

    done:

    tree->n_objects++;
    return true;
}

/*
 * Find the anchor point (return as nid<<1|cid) in parent cell.
 * Note: cell must not be the root cell.
 */
static int cell_find_anchor(c3bt_cell *cell, c3bt_cell *parent)
{
    int i;
    uint nid, cid;

    for (i = 0; parent->P[i] != cell; i++)
        /* nothing */;
    i |= CHILD_CELL_BIT;
    for (nid = 0; nid < NODES_PER_CELL; nid++) {
        if (cell_node_is_vacant(parent, nid))
            continue;
        for (cid = 0; cid < 2; cid++)
            if (parent->N[nid].child[cid] == i)
                goto found;
    }

    found:

    return nid << 1 | cid;
}

/*
 * Merge a cell into its parent cell.
 *
 * Iterative post-order traversal using two stacks.  Sizes of the stacks are
 * set for largest possible cell (with NODES_PER_CELL-1 nodes).  This function
 * uses about 92B stack on x86 and 64B on ARM, which is a >70% reduction from
 * the recursive version under worst condition.
 */
static void cell_merge(c3bt_cell *cell, c3bt_cell *parent, int anchor)
{
    int wtop, ftop, n, c, new_node, new_ptr;
    uint8_t wstack[NODES_PER_CELL];
    uint8_t fstack[NODES_PER_CELL * 2 - 1];

    cell_free_ptr(parent,
        parent->N[anchor >> 1].child[anchor & 1] & INDEX_MASK);
    /* Use wstack to make a full post-order stack in fstack. */
    ftop = -1;
    wtop = 0;
    wstack[0] = 0;
    while (wtop >= 0) {
        n = wstack[wtop--];
        fstack[++ftop] = n;
        if (CHILD_IS_NODE(n))
            for (c = 0; c < 2; c++)
                wstack[++wtop] = cell->N[n].child[c];
    }
    /* Copy everything in full stack. */
    while (ftop >= 0) {
        n = fstack[ftop];
        if (CHILD_IS_NODE(n)) {
            /* Copy a node with its children, and replace with its new index in
             * parent cell.
             */
            new_node = cell_alloc_node(parent);
            cell_inc_ncount(parent, 1);
            parent->N[new_node].cbit = cell->N[n].cbit;
            wtop = ftop + 1;
            for (c = 1; c >= 0; c--) {
                while (fstack[wtop] == INVALID_NODE)
                    wtop++;
                parent->N[new_node].child[c] = fstack[wtop];
                fstack[wtop] = INVALID_NODE;
            }
            fstack[ftop] = new_node;
        } else {
            /* Copy a pointer and replace with its new index in parent cell. */
            new_ptr = cell_alloc_ptr(parent);
            c = n & INDEX_MASK;
            parent->P[new_ptr] = cell->P[c];
            if (CHILD_IS_CELL(n))
                cell_set_parent(cell->P[c], parent);
            fstack[ftop] = (n & FLAGS_MASK) | new_ptr;
        }
        ftop--;
    }
    parent->N[anchor >> 1].child[anchor & 1] = fstack[0];
    cell_free(cell);
}

bool c3bt_remove(c3bt_tree *c3bt, void *uobj)
{
    c3bt_tree_impl *tree;
    c3bt_cursor_impl loc;
    c3bt_cell *parent, *sub;
    uint8_t *pap;
    int n, c, sibling, anchor;

    if (!c3bt_locate(c3bt, uobj, (c3bt_cursor*)&loc))
        return false;
    tree = (c3bt_tree_impl*)c3bt;
    parent = cell_parent(loc.cell);
    cell_free_ptr(loc.cell, loc.cell->N[loc.nid].child[loc.cid] & INDEX_MASK);
    if (!loc.nid) {
        /* Remove from cell root. */
        sibling = loc.cell->N[0].child[1 - loc.cid];
        if (CHILD_IS_NODE(sibling)) {
            loc.cell->N[0] = loc.cell->N[sibling];
            cell_free_node(loc.cell, sibling);
        } else if (CHILD_IS_UOBJ(sibling) && parent == NULL) {
            /* Turn to singleton tree. */
            loc.cell->N[0].child[0] = CHILD_UOBJ_BIT | 0;
            loc.cell->N[0].child[1] = CHILD_CELL_BIT | 1;
            loc.cell->P[0] = loc.cell->P[sibling & INDEX_MASK];
            loc.cell->P[1] = NULL;
            goto done;
        } else {
            if (!parent) {
                /* Rare but possible: root cell has a single node, one child is
                 * uobj pointer (being removed) and another is a cell pointer.
                 */
                tree->root = loc.cell->P[sibling & INDEX_MASK];
                if (tree->root)
                    cell_set_parent(tree->root, 0);
            } else {
                /* Non-root cell is becoming incomplete; push up then free. */
                anchor = cell_find_anchor(loc.cell, parent);
                pap = &parent->N[anchor >> 1].child[anchor & 1];
                *pap &= INDEX_MASK;
                parent->P[*pap] = loc.cell->P[sibling & INDEX_MASK];
                if (CHILD_IS_CELL(sibling))
                    cell_set_parent(parent->P[*pap], parent);
                *pap |= sibling & FLAGS_MASK;
            }
            cell_free(loc.cell);
#ifdef C3BT_STATS
            c3bt_stat_cells--;
            c3bt_stat_pushups++;
#endif
            goto done;
        }
    } else {
        /* Not removing from cell root. */
        n = cell_node_parent(loc.cell, loc.nid);
        loc.cell->N[n >> 1].child[n & 1] = loc.cell->N[loc.nid].child[1
            - loc.cid];
        cell_free_node(loc.cell, loc.nid);
    }
    cell_dec_ncount(loc.cell, 1);

    /* Try merging up to parent. */
    if (parent && cell_ncount(loc.cell) + cell_ncount(parent) <= NODES_PER_CELL) {
        anchor = cell_find_anchor(loc.cell, parent);
        cell_merge(loc.cell, parent, anchor);
        goto merge_done;
    }
    /* Try merging up a subcell. */
    for (n = 0; n < NODES_PER_CELL; n++) {
        if (cell_node_is_vacant(loc.cell, n))
            continue;
        for (c = 0; c < 2; c++) {
            if (CHILD_IS_CELL(loc.cell->N[n].child[c])) {
                sub = loc.cell->P[loc.cell->N[n].child[c] & INDEX_MASK];
                if (cell_ncount(loc.cell) + cell_ncount(sub) <= NODES_PER_CELL) {
                    cell_merge(sub, loc.cell, n << 1 | c);
                    goto merge_done;
                }
            }
        }
    }
    goto done;

    merge_done:

#ifdef C3BT_STATS
    c3bt_stat_merges++;
    c3bt_stat_cells--;
#endif

    done:

    tree->n_objects--;
    return true;
}

/*
 * Standard bitops for common data types.
 */

/*
 * BITS' length is fixed in tree structure. The tail byte is zero-padded.
 */
static int bitops_bits(int req, void *key1, void*key2)
{
    uint i, x;
    if (req >= 0) {
        return ((uint8_t*)key1)[req / 8] & (0x80u >> (req % 8)) ? 1 : 0;
    } else {
        /* For crit_bit request, req is passed with -(tree->key_nbits+1), hence
         * -req-1 is tree->key_nbits, so we know how far to compare.
         */
        for (i = 0; i < (-req - 1 + 7) / 8; i++) {
            if ((x = ((uint8_t*)key1)[i] ^ ((uint8_t*)key2)[i]) != 0)
                return i * 8 + __builtin_clz(x) - 24;
        }
        return -1;
    }
}

/*
 * STR has variable length, and the caller can't know how long in advance, so
 * its bitops should return 0 for overrun requests. This is also needed for
 * proper ordering. E.g., "abc" and "abc1" should differ on bit #26, not #24
 * ("1" has 2 leading 0 bits).
 */
#ifdef C3BT_WITH_STRING
static int bitops_str(int req, void *key1, void *key2)
{
    uint i, nbits;
    char *q, *p = (char*)key1;

    if (req >= 0) {
        nbits = strlen(p) * 8;
        if (req >= nbits)
            return 0;
        return p[req / 8] & (0x80u >> (req % 8)) ? 1 : 0;
    } else {
        q = (char*)key2;
        /* Compare up to specified number of bits (exactly), if can't find a
         * differing bit, report they are equal.
         */
        for (i = 0; i < (-req - 1 + 7) / 8; i++) {
            if ((p[i] | q[i]) == 0)
                return -1;
            if (p[i] == q[i])
                continue;
            if (p[i] == 0 || q[i] == 0) {
                nbits = i * 8 + __builtin_clz((uint32_t)(p[i] ^ q[i])) - 24;
                return nbits >= (-req - 1) ? -1 : nbits;
            }
        }
        return -1;
    }
}

static int bitops_pstr(int req, void *key1, void*key2)
{
    return bitops_str(req, *(void**)key1, *(void**)key2);
}
#endif

#ifdef C3BT_WITH_INTS
static int bitops_u32(int req, void *key1, void *key2)
{
    uint32_t bits;

    bits = *(uint32_t*)key1;
    if (req >= 0)
        /* Return the requested bit in key1. */
        return bits & (0x80000000u >> req) ? 1 : 0;
    else {
        /* Return the position of first differing bit, or -1 if the two keys
         * are equal.
         */
        bits ^= *(uint32_t*)key2;
        if (bits == 0)
            return -1;
        return __builtin_clz(bits);
    }
}
#endif

#ifdef C3BT_WITH_INTS
static int bitops_s32(int req, void *key1, void *key2)
{
    uint32_t bits;

    /* Shifting from [INT_MIN, INT_MAX] to [0, UINT_MAX] to maintain proper
     * ordering for signed integers.  Subtraction and flipping MSB both will do.
     */
    bits = *(int32_t*)key1;
    if (req >= 0)
        return (bits ^ 0x80000000u) & (0x80000000u >> req) ? 1 : 0;
    else {
        bits ^= *(int32_t*)key2;
        if (bits == 0)
            return -1;
        return __builtin_clz(bits);
    }
}
#endif

#ifdef C3BT_WITH_INTS
static int bitops_u64(int req, void *key1, void *key2)
{
    union {
        uint64_t u64;
        uint32_t u32[2];
    } bits;

    bits.u64 = *(uint64_t*)key1;
    if (req >= 0) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        if (req < 32)
            return bits.u32[1] & (0x80000000u >> req) ? 1 : 0;
        else
            return bits.u32[0] & (0x80000000u >> (req - 32)) ? 1 : 0;
#else
        if (req < 32)
        return bits.u32[0] & (0x80000000u >> req) ? 1 : 0;
        else
        return bits.u32[1] & (0x80000000u >> (req - 32)) ? 1 : 0;
#endif
    } else {
        bits.u64 ^= *(uint64_t*)key2;
        if (bits.u64 == 0)
            return -1;
        return __builtin_clzll(bits.u64);
    }
}
#endif

#ifdef C3BT_WITH_INTS
static int bitops_s64(int req, void *key1, void *key2)
{
    union {
        uint64_t u64;
        uint32_t u32[2];
    } bits;

    bits.u64 = *(int64_t*)key1;
    if (req >= 0) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        if (req < 32) {
            bits.u32[1] ^= 0x80000000u;
            return bits.u32[1] & (0x80000000u >> req) ? 1 : 0;
        } else
            return bits.u32[0] & (0x80000000u >> (req - 32)) ? 1 : 0;
#else
        if (req < 32) {
            bits.u32[0] ^= 0x80000000u;
            return bits.u32[0] & (0x80000000u >> req) ? 1 : 0;
        }
        else
        return bits.u32[1] & (0x80000000u >> (req - 32)) ? 1 : 0;
#endif
    } else {
        bits.u64 ^= *(int64_t*)key2;
        if (bits.u64 == 0)
            return -1;
        return __builtin_clzll(bits.u64);
    }
}
#endif

/* vim: set syn=c.doxygen cin et sw=4 ts=4 tw=80 fo=croqmM: */
