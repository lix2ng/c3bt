#C3BT: Compact Clustered Crit-Bit Tree


##Introduction

Take Crit-Bit Tree (as CBT hereafter), squeeze the nodes into cache-line sized
cells, and we get C3BT.  The clustered layout reduces memory allocation overhead
and most importantly it reduces cache line misses.  So if you like, you may also
read C3BT as "Cache-Conscious Crit-Bit Tree".

Don't know what a CBT is?  Look here: http://cr.yp.to/critbit.html

In the 32-bit (LP32) layout, each cell is 64 bytes and contains up to 8 crit-bit
nodes.  It can index keys up to 256 bits long.  The LP64 version (TODO) uses 128
byte cells each can hold 9 nodes and supports 64K-bit keys.

C3BT also extends the functionality of CBT.  Its API should be familiar to
binary search tree users: INIT, DESTROY, ADD, REMOVE, FIND, FIRST, LAST, NEXT,
and PREV.  Keys can be fixed-length bit string, zero-terminated string, 32 and
64 bit integers signed and unsigned, all with native ordering.  Custom or
composite key data types are supported by custom "bitops" -- like a comparator
to a BST.

Most implementations of BST embed index nodes in user objects; with C3BT, the
index is separate and you add/remove user objects by reference.  The reason is
that CBT node uses a crit-bit number to bisect the remaining key value space,
which is not a part of user object (in comparison, BSTs use the user object/key
itself as separator to bisect the remaining key set).  From users' point of
view, there is no difference in the API.

A standard binary tree is not known for good memory efficiency or reference
locality and these are where C3BT has improved through its cache friendly
layout.  You get both the flexibility of a separate index and the performance of
a dense, cache-optimized data structure.

  - Memory allocation is per-cell; overhead is low.
  - Good performance due to much less cache line access.
  - Index can be created and destroyed dynamically.
  - Independent index won't dilute user data; this can make a big difference if
    user objects are small and densely stored.

Besides, C3BT uses parent pointers on the cell level but not in the nodes.  This
speeds up iteration with little cost of space.

The author hopes C3BT can help popularize CBT as an alternative to BSTs, T-Tree
and in-memory B-Tree.  CBT works on the fundamental representation of data, and
playing with bits is fun.

##Build

This version is tested under GCC on 32-bit Linux.

Just type "make". There are 3 files: c3bt.h, c3bt.c and c3bt-main.c.  The first
two are meant to be dropped in your project, and the third is an ugly ad-hoc
tester.

The code has statistics enabled by default.  If you don't need it, undefine
`C3BT_STATS` in c3bt.h.

If all you need is an associative array, you may define `C3BT_FEATURE_MIN` to
reduce code size.  You can lookup an user object by a key value; you can still
iterate through the objects, but the ordering may be incorrect.  You need to
treat your keys as bit strings in this case.

##Usage

C3BT can be used in two ways, hence two init functions are provided.  One is
`c3bt_init()` where you specify the key's data type, offset in your object and
its length.  The other, `c3bt_init_bitops()`, allows you to install a bitops
function to support custom key types.

After initialization, you may use `c3bt_add()` to add some user objects and
`c3bt_remove()` to remove them.  Again, these are all by reference: "add" won't
create or copy a object and "remove" won't free any.

Once you have some objects indexed, there are various `c3bt_find()` functions
that can be used to find an object by key value, and `c3bt_first()`,
`c3bt_last()`, `c3bt_next()` and `c3bt_prev()` can help iterate through them.

Bitops
------

Bitops, meaning "bit operations", is a function to implement a key data type.
Its job is to answer three kinds of queries about the key:

 1. Get bit: what is the key's bit value at this position?
 2. Crit-bit: given two keys, which bit is the first bit where they differ?  Or,
    how long is their common prefix?
 3. Equality: are the two keys equal?

Query #3 is covered by #2 because if we can't find a differing bit up to the
maximum key length, the keys are equal.  So bitops needs to implement two
functions: `get_bit` and `crit_bit`.  Other than these, the core C3BT algorithm
is totally ignorant about the key.

The prototype is defined as:

    int bitops(int req, void *key1, void *key2);

"req" is the request number.  When req>=0, it's a `get_bit` query.  Bitops
should return the bit at position "req" of key1.  Key2 is ignored.

If req < 0, it's a `crit_bit` query.  Bitops should return the differing bit
position between key1 and key2.  If such a bit can't be found, return -1 to
report they are equal.  The maximum number of bits to be compared can be derived
from (-req-1).

Bit position is numbered in "written order", that is, the order that you would
write it on paper.  For example, 32-bit integers' MSB is bit #0 and LSB is bit
31; for a string, character at byte offset n's MSB is nx8, and its LSB is nx8+7.
You may call it pure big-endian.

You may find many creative use of bitops.  Here are some ideas:

  - A single tree can index objects of different types.  The bitops in this case
    should present a larger key space and sort the objects into non-overlapping
    ranges.  One solution could be prepending a type id or domain id.
  - Case insensitive string or strings with non-trivial encodings: translate
    the characters by a collation table.
  - C3BT in LP32 mode limits the key to 256 bits but longer strings can be
    supported with a little compromise: bitops can compose the key as such --
    take first 28 characters and append a 4 byte hash code of the remaining
    characters.  There won't be false duplications (with a good hash) and the
    ordering is correct up to the 28th character.
  - Objects with same valued key: append memory address of object after the real
    key to make it unique.

Bitops is called frequently, especially the `get_bit` query.  You should make it
as short and fast as possible.  If you plan to use only one key type, you may
consider inlining it to eliminate the function call overhead.

##Algorithm

Clustering a non-linear structure, even as simple as a binary tree, has some
challenges.  For instance, split and merge can happen vertically between parent
and child but pushing nodes between siblings is not possible.  Also, tasks like
finding a split point or copying a subtree would involve tree traversal.

Fortunately the binary trees that C3BT deals with are confined in small arrays
with limited number of nodes.  This allows much simpler and light weight
solutions with bound time and space: array iteration can be used to enumerate
all the nodes, and when tree-style traversal is necessary, a non-recursive
algorithm would use only a few bytes as the stack.

In the end, the cell fill factor and algorithm complexity of C3BT is comparable
to in-memory B-Tree, but C3BT has higher density (measured by bytes per fanout)
and very localized lookup (measured by cache lines per lookup).

On average C3BT in LP32 mode achieves 5.5 uobjs/cell.  Memory consumption can be
estimated as: Nobj/5.5x64 (bytes).

###Lookup
Lookup in C3BT is straightforward: start from tree root, check the bit value of
key at the position indicated by current node's "cbit".  If it's 0, follow the
left child, otherwise follow the right.  If the child is a cell pointer,
continue the search from node 0 in the new cell.  If the child is an user object
pointer, return the pointer and the lookup is done.

However as a patricia trie, C3BT doesn't store the key in the tree, so a full
key comparison is necessary to confirm the found object is indeed a match.

###Iteration
C3BT iterates just like a standard BST, except that since the parent pointer is
per-cell, when doing backtrack, the algorithm climbs up the tree cell by cell
and descent node by node within the cells to locate a suitable ancestor.
Profiling has shown it's quite efficient.

###Add
Adding an user object in C3BT is like in CBT: fist step is to lookup the new
object, compare the result with the new object to get their crit-bit number.
Second step is to find a position along the path as the insertion point.  The
second step should guarantee "cbit" is in ascending order from top to down.

If the insertion point is in a full cell, we must make room for the new node.
First attempt is to push an edge node down to a sub-cell.  If that fails, we
split the cell in two.

###Remove
Removing is one step: lookup the user object, then delete its belonging node.

If the cell is becoming incomplete, i.e., when removing from a cell with only
one node, the remaining child should be absorbed by the parent cell then the
cell should be free-ed.  This may happen in cells at the lowest level.

After the removal, C3BT merges the cell to increase fill factor.  First try is
to merge the cell to its parent, then try to merge one of the cell's sub-cells
into it.

vim: set ai et ts=4 tw=80 syn=markdown spell spl=en_us fo=ta:

