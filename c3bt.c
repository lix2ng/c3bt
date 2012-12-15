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
 * - If MSB (0x80) is set, low bits is the index to a user object pointer.
 * - If next bit (0x40) is set, low bits is the index to a cell pointer.
 * - If both bits clear, it's an index of a c3bt_node within the same cell.  A
 *   special value, 0x3F, is used to mark an unoccupied node slot.
 *
 * The differing bit number (crit-bit) is stored in a byte, so keys can be
 * indexed up to 256 bits in the standard layout.
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

/* Each cell contains a 3-8 crit-bit sub-tree. */
#define CELL_MIN_NODES  3
#define CELL_MAX_NODES  8
#define CELL_MAX_PTRS  (CELL_MAX_NODES + 1)
/* In-cell depth-first traversal stack size. */
#define DFT_STACK_SIZE  6

/*
 * C3BT cell is 64B in size for 32-bit standard layout.
 *
 * Cell must be 8B-aligned to spare 3 low bits as node count.  This is already
 * satisfied by major platforms and C libraries but users of custom allocators
 * shall double check.
 *
 * Cell layout:
 * - PNC (parent & node count), 4B, [0, 3]
 * - Array of 8 crit-bit nodes, 24B, [4, 27]
 * - Array of 9 external pointers, 36B, [28, 63]
 *
 * Node[0] is always the root of the cell's subtree.
 */
typedef struct c3bt_cell c3bt_cell;

struct c3bt_cell {
    c3bt_cell *pnc;
    c3bt_node N[CELL_MAX_NODES];
    c3bt_cell *P[CELL_MAX_PTRS];
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
uint c3bt_stat_mergeups;
uint c3bt_stat_mergedowns;
uint c3bt_stat_failed_merges;
uint c3bt_stat_shortcuts;
uint c3bt_stat_popdist[CELL_MAX_NODES];
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
bool _osize c3bt_init(c3bt_tree *c3bt, uint kdt, uint koffset, uint kbits)
{
    c3bt_tree_impl *tree;

    CT_ASSERT(sizeof(c3bt_node) == 3);
    CT_ASSERT(sizeof(c3bt_cell) == 64);
    CT_ASSERT(sizeof(c3bt_tree) == sizeof(c3bt_tree_impl));
    CT_ASSERT(sizeof(c3bt_cursor) == sizeof(c3bt_cursor_impl));

    if (c3bt == NULL)
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

    if (c3bt == NULL || bitops == NULL)
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
    if (cell == NULL)
        return NULL;
    assert(((intptr_t)cell & 7) == 0);
    memset(cell, 0, sizeof(c3bt_cell));
    for (i = 0; i < CELL_MAX_NODES; i++)
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

    if (cell == NULL)
        return NULL;

    for (n = 0; n < CELL_MAX_NODES; n++) {
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
    c3bt_cell *head, *next, *del, *tmp;

    if (c3bt == NULL)
        return false;
    /*
     * Iterative Post-order Traversal of N-way Tree With Delayed Node Access.
     */
    head = ((c3bt_tree_impl*)c3bt)->root;
    del = NULL;
    while (head) {
        next = cell_delist_subcell(head);
        if (next == NULL) {
            cell_free(del);
            del = head;
#ifdef C3BT_STATS
            cell_update_popdist(head);
#endif
            next = cell_delist_subcell(cell_parent(head));
            if (next == NULL) {
                while (cell_parent(head)) {
                    next = cell_parent(head);
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
                        head = next;
                        next = NULL;
                    }
                }
            }
        }
        head = next;
    }
    cell_free(del);
    memset(c3bt, 0, sizeof(c3bt_tree_impl));
    return true;
}

uint c3bt_nobjects(c3bt_tree *tree)
{
    if (tree == NULL)
        return 0;
    return (((c3bt_tree_impl*)tree)->n_objects);
}

/*
 * Tree lookup by key.
 *
 * Lookup from top of the tree trying to find the key, but it won't verify the
 * result.  Cursor is updated if specified. Special cases:
 * - Empty tree: return NULL, cur->cell is NULL, nid and cid undefined.
 * - Singleton tree: always return the uobj and cur is set as (nid=0, cid=0).
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

    if (key == NULL || tree == NULL || tree->key_type != C3BT_KDT_BITS)
        return NULL;

    robj = tree_lookup(tree, key, NULL);
    if (robj == NULL)
        return NULL;
    if (memcmp(key, (char*)robj + tree->key_offset, (tree->key_nbits + 7) / 8)
        == 0)
        return robj;
    return NULL;
}

#ifdef C3BT_WITH_INTS
/* Common for all integers "find-by-value" functions. */
static _noinline void *c3bt_find_integer(c3bt_tree *c3bt, uint64_t key,
    uint kdt)
{
    union {
        uint32_t u32;
        uint64_t u64;
    } bits;
    void *robj;
    c3bt_tree_impl *tree = (c3bt_tree_impl*)c3bt;

    if (tree == NULL || tree->key_type != kdt)
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
    if (robj == NULL)
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

    if (key == NULL || c3bt == NULL)
        return NULL;

    tree = (c3bt_tree_impl*)c3bt;
    if (tree->key_type == C3BT_KDT_PSTR) {
        str = key;
        robj = tree_lookup(tree, &str, NULL);
    } else if (tree->key_type == C3BT_KDT_STR)
        robj = tree_lookup(tree, key, NULL);
    else
        return NULL;
    if (robj == NULL)
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

    if (c3bt == NULL || uobj == NULL)
        return NULL;

    tree = (c3bt_tree_impl*)c3bt;
    robj = tree_lookup(tree, (char*)uobj + tree->key_offset,
        (c3bt_cursor_impl*)cur);
    if (robj == NULL)
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
static _noinline void *tree_rush_down(c3bt_tree_impl *tree,
    c3bt_cursor_impl *start, int dir)
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
static _noinline void *tree_extreme(c3bt_tree_impl *tree, c3bt_cursor *cur,
    int dir)
{
    c3bt_cursor_impl start;
    void * robj;

    if (tree == NULL || tree->root == NULL)
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
static _noinline void *tree_step(c3bt_tree_impl *tree, c3bt_cursor_impl *cur,
    int dir)
{
    c3bt_cell *cell;
    void *uobj;
    int upper, lower, bit, cur_cbit;

    /* Nowhere to step if tree is null, empty or singleton. */
    if (cur == NULL || tree == NULL || tree->n_objects < 2)
        return NULL;

    /* The easy case: the other sibling is on the desired path. */
    if (cur->cid != dir)
        goto down;
    /*
     * The hard case: find an ancestor from where we can rush down.
     * Climbing up is cell by cell using the parent pointer; within each
     * cell it's key-guided descent.
     */
    cur_cbit = cur->cell->N[cur->nid].cbit;
    cell = cur->cell;
    uobj = cell->P[cell->N[cur->nid].child[cur->cid] & INDEX_MASK];
    while (cell != NULL) {
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
 * Find node's parent in a cell (input node can't be 0).
 *
 * Let P be the parent of node N.  This function returns 2*P if N is P's left
 * child, or 2*P+1 if it's the right child.
 */
static int cell_node_parent(c3bt_cell *cell, int node)
{
    int n, c;

    for (n = 0; n < CELL_MAX_NODES; n++) {
        if (cell_node_is_vacant(cell, n))
            continue;
        for (c = 0; c < 2; c++)
            if (cell->N[n].child[c] == node)
                goto done;
    }
    done:

    return n * 2 + c;
}

/*
 * Find a split point for a fully populated cell.  Return the would-be new
 * cell-root and a bitmap representing the nodes need to be moved.
 */
static uint cell_find_split(c3bt_cell *cell, int *bitmap)
{
    uint8_t stack[DFT_STACK_SIZE];
    int i, top, cid, n, count, alt, alt_bmp;

    alt = -1;
    alt_bmp = 0;
    for (i = 1; i < CELL_MAX_NODES; i++) {
        if (!CHILD_IS_NODE(cell->N[i].child[0])
            && !CHILD_IS_NODE(cell->N[i].child[1]))
            continue;
        /*
         * Pre-order traversal to count number of children of a node.
         * Cell-root and leaves are excluded; only 3~6 nodes to check.
         */
        count = 0;
        *bitmap = 0;
        stack[0] = i;
        top = 0;
        while (top >= 0) {
            n = stack[top--];
            *bitmap |= 0x80u >> n;
            for (cid = 1; cid >= 0; cid--)
                if (CHILD_IS_NODE(cell->N[n].child[cid])) {
                    stack[++top] = cell->N[n].child[cid];
                    count++;
                }
        }
        /* First choice: 4+4. */
        if (count == 3) {
//        if (count >= 2 && count <= 4) {
            return i;
        }
        /* Second choice: 3+5. */
        if (count == 2 || count == 4) {
            alt = i;
            alt_bmp = *bitmap;
        }
    }
    *bitmap = alt_bmp;
    return alt;
}

/*
 * Split a full cell in two.  New cell will become original cell's sub-cell.
 */
static bool cell_split(c3bt_cell *cell)
{
    c3bt_cell *new_cell;
    int i, n, p, cid, new_root, count, bitmap = 0;

    new_cell = cell_malloc();
    if (new_cell == NULL)
        return false;

    new_root = cell_find_split(cell, &bitmap);
    count = 0;
    for (i = 0; i < CELL_MAX_NODES; i++) {
        if (!(bitmap & (0x80u >> i)))
            continue;
        /* Copy node[i] and its ptr to new cell (same location) */
        new_cell->N[i] = cell->N[i];
        for (cid = 0; cid < 2; cid++) {
            p = cell->N[i].child[cid];
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
    n = cell_node_parent(cell, new_root);
    cell->N[n >> 1].child[n & 1] = CHILD_CELL_BIT | p;
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

    for (n = 1; n < CELL_MAX_NODES; n++) {
        if (cell_node_is_vacant(cell, n))
            continue;
        for (c = 0; c < 2; c++) {
            /* Only edge nodes can be pushed down. */
            if (CHILD_IS_CELL(cell->N[n].child[c])
                && !CHILD_IS_NODE(cell->N[n].child[1 - c])) {
                sub = cell->P[cell->N[n].child[c] & INDEX_MASK];
                /* Only push down when sub has at least 2 vacancies. */
                if (cell_ncount(sub) < CELL_MAX_NODES - 1) {
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
    c3bt_cell *cell;
    void *robj;
    int cbit_nr, bit, new_node, new_ptr, upper, lower, dir = 0;
    if (c3bt == NULL || uobj == NULL)
        return false;

    tree = (c3bt_tree_impl*)c3bt;
    /* Empty -> singleton. */
    if (tree->root == NULL) {
        cell = cell_malloc();
        if (cell == NULL)
            return false;
        tree->root = cell;
        cell->N[0].child[0] = CHILD_UOBJ_BIT | 0;
        cell->N[0].child[1] = CHILD_CELL_BIT | 1;
        cell->P[0] = uobj;
        /* Redundant for a new cell.
         * cell->P[1] = NULL;
         * cell->pnc = cell_make_pnc(NULL, 1);
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
    /* Shortcut if no need to search from root. */
    if (cbit_nr > cur.cell->N[cur.nid].cbit) {
        c3bt_stat_shortcuts++;
        cell = cur.cell;
        upper = cur.nid;
        dir = cur.cid;
        lower = cell->N[upper].child[dir];
        if (cell_ncount(cell) == CELL_MAX_NODES)
            goto make_room;
        goto do_insert;
    }
    /* Find location for new node (from tree root). */
    cell = tree->root;

    next:

    upper = INVALID_NODE;
    lower = 0;
    while (!CHILD_IS_UOBJ(lower)) {
        if (cell->N[lower].cbit > cbit_nr)
            break;
        upper = lower;
        dir = tree->bitops(cell->N[lower].cbit, (char*)uobj + tree->key_offset,
            NULL);
        lower = cell->N[lower].child[dir];
        if (CHILD_IS_CELL(lower)) {
            cell = cell->P[lower & INDEX_MASK];
            goto next;
        }
    }

    /* Make room for a full cell. */
    if (cell_ncount(cell) == CELL_MAX_NODES) {
        make_room:
        /* Try to push down a node first. It's cheaper. */
        if (cell_push_down(cell))
            goto next;
        /* Then we have to split. */
        if (!cell_split(cell))
            return false;
#ifdef C3BT_STATS
        c3bt_stat_cells++;
        c3bt_stat_splits++;
#endif
        goto next;
    }

    do_insert:

    new_node = cell_alloc_node(cell);
    new_ptr = cell_alloc_ptr(cell);
    cell_inc_ncount(cell, 1);
    cell->P[new_ptr] = uobj;
    if (upper == INVALID_NODE) {
        /* Insert as cell root. */
        cell->N[new_node] = cell->N[0];
        lower = new_node;
        new_node = 0;
    }
    /* Insert between upper and lower. */
    cell->N[new_node].cbit = cbit_nr;
    if (upper != INVALID_NODE)
        cell->N[upper].child[dir] = new_node;
    cell->N[new_node].child[bit] = new_ptr | CHILD_UOBJ_BIT;
    cell->N[new_node].child[1 - bit] = lower;

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
    for (nid = 0; nid < CELL_MAX_NODES; nid++) {
        if (cell_node_is_vacant(parent, nid))
            continue;
        for (cid = 0; cid < 2; cid++)
            if (parent->N[nid].child[cid] == i)
                goto found;
    }

    found:

    return nid << 1 | cid;
}

static int cell_copy_ptr(c3bt_cell *src, c3bt_cell *dest, int child)
{
    int pid, new_ptr;

    new_ptr = cell_alloc_ptr(dest);
    pid = child & INDEX_MASK;
    dest->P[new_ptr] = src->P[pid];
    if (CHILD_IS_CELL(child))
        cell_set_parent(src->P[pid], dest);
    return (child & FLAGS_MASK) | new_ptr;
}

static int cell_copy_node(c3bt_cell *src, c3bt_cell *dest, int nid)
{
    int c, child, new_node;

    new_node = cell_alloc_node(dest);
    cell_inc_ncount(dest, 1);
    dest->N[new_node].cbit = src->N[nid].cbit;
    for (c = 0; c < 2; c++) {
        child = src->N[nid].child[c];
        if (CHILD_IS_NODE(child))
            dest->N[new_node].child[c] = cell_copy_node(src, dest, child);
        else
            dest->N[new_node].child[c] = cell_copy_ptr(src, dest, child);
    }
    return new_node;
}

/*
 * Merge a cell into its parent cell.
 * (Recursive)
 */
static void cell_merge_up(c3bt_cell *cell, c3bt_cell *parent)
{
    int anchor, c, child, new_node;

    anchor = cell_find_anchor(cell, parent);
    cell_free_ptr(parent,
        parent->N[anchor >> 1].child[anchor & 1] & INDEX_MASK);
    new_node = cell_alloc_node(parent);
    cell_inc_ncount(parent, 1);
    parent->N[anchor >> 1].child[anchor & 1] = new_node;
    parent->N[new_node].cbit = cell->N[0].cbit;
    for (c = 0; c < 2; c++) {
        child = cell->N[0].child[c];
        if (CHILD_IS_NODE(child))
            parent->N[new_node].child[c] = cell_copy_node(cell, parent, child);
        else
            parent->N[new_node].child[c] = cell_copy_ptr(cell, parent, child);
    }
    cell_free(cell);
    return;
}

bool c3bt_remove(c3bt_tree *c3bt, void *uobj)
{
    c3bt_tree_impl *tree;
    c3bt_cursor_impl loc;
    c3bt_cell *parent;
    uint8_t *pap;
    int n, sibling, anchor;

    if (c3bt_locate(c3bt, uobj, (c3bt_cursor*)&loc) == NULL)
        return false;
    tree = (c3bt_tree_impl*)c3bt;
    parent = cell_parent(loc.cell);
    cell_free_ptr(loc.cell, loc.cell->N[loc.nid].child[loc.cid] & INDEX_MASK);
    if (loc.nid == 0) {
        /* Remove from cell root. */
        sibling = loc.cell->N[0].child[1 - loc.cid];
        if (CHILD_IS_NODE(sibling)) {
            loc.cell->N[0] = loc.cell->N[sibling];
            cell_free_node(loc.cell, sibling);
        } else if (CHILD_IS_UOBJ(sibling) && parent == NULL) {
            /* Turn to singleton tree. */
            loc.cell->P[0] = loc.cell->P[sibling & INDEX_MASK];
            loc.cell->P[1] = NULL;
            loc.cell->N[0].child[0] = CHILD_UOBJ_BIT | 0;
            loc.cell->N[0].child[1] = CHILD_CELL_BIT | 1;
            tree->n_objects--;
            return true;
        } else {
            /* Cell is becoming incomplete; it must be pushed up then free-ed. */
            if (parent == NULL) {
                tree->root = loc.cell->P[sibling & INDEX_MASK];
                if (tree->root != NULL)
                    cell_set_parent(tree->root, 0);
            } else {
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
            tree->n_objects--;
            return true;
        }
    } else {
        /* Not from cell root. */
        n = cell_node_parent(loc.cell, loc.nid);
        loc.cell->N[n >> 1].child[n & 1] = loc.cell->N[loc.nid].child[1
            - loc.cid];
        cell_free_node(loc.cell, loc.nid);
    }
    cell_dec_ncount(loc.cell, 1);
    tree->n_objects--;

    /*
     * Since we expect a node has no less than CELL_MIN_NODES nodes, there's
     * no need to try nodes with more than (CELL_MAX_NODES - CELL_MIN_NODES).
     */
    if (cell_ncount(loc.cell) > CELL_MAX_NODES - CELL_MIN_NODES)
        return true;
    /* Try merging up to parent. */
    if (parent != NULL
        && cell_ncount(loc.cell) + cell_ncount(parent) <= CELL_MAX_NODES) {
        cell_merge_up(loc.cell, parent);
#ifdef C3BT_STATS
        c3bt_stat_mergeups++;
#endif
        goto merge_done;
    }
    /* Record a merge failure. */
#ifdef C3BT_STATS
    c3bt_stat_failed_merges++;
#endif
    return true;

    merge_done:

#ifdef C3BT_STATS
    c3bt_stat_cells--;
#endif
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
        /*
         * For crit_bit request, req is passed with -(tree->key_nbits+1), hence
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
        /*
         * Compare up to specified number of bits (exactly), if can't find a
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
        /*
         * Return the position of first differing bit, or -1 if the two keys
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

    /*
     * Shifting from [INT_MIN, INT_MAX] to [0, UINT_MAX] to maintain proper
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
