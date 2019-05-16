# C3BT: Compact Clustered Crit-Bit Tree

## Introduction

As the name suggests, C3BT is a compact, clustered version of Crit-Bit Tree
(CBT).  The cluster layout improves memory allocation efficiency and more
importantly it reduces cache line misses.  So if you like, you may also read
C3BT as "Cache-Conscious Crit-Bit Tree".

For more information about CBT, look here: http://cr.yp.to/critbit.html

CBT nodes are clustered into cells.  Under the 32-bit (LP32) layout, each cell
is 64 bytes and contains up to 8 crit-bit nodes.  It can index keys up to 256
bits long.  The LP64 version (TODO) uses 128 byte cells each can hold 9 nodes
and supports 64K-bit keys.

Comparison: C3BT-LP32 achieves >5 user objects/cell fill factor on average;
that is 14.4B/uobj.  The plain CBT would need 24B/uobj, losing half space to
the overhead, and does 5 times the `malloc()` calls -- assuming the popular
dlmalloc.

C3BT also extends the functionality of CBT.  It has a complete, binary search
tree (BST) alike API: INIT, DESTROY, ADD, REMOVE, FIND, FIRST, LAST, NEXT, and
PREV.  Common key types are supported by default: fixed-length bit string,
zero-terminated string, 32 and 64 bit integers signed and unsigned, all with
native ordering.  Custom or composite key data types are supported by custom
"bitops" function which is analogous to a comparator of BST (explained below).

Comparing with the ubiquitous BST, C3BT probably won't beat its simplicity but
can improve reference locality bacause C3BT doesn't need to access user data
structure (uobj) during a search.  If you have a large number of objects, and
you make non-trivial use of the index (an iteration will qualify), C3BT has
much potential to perform better on a modern, memory-walled CPU.  With C3BT,
you get both the flexibility of a separate index and the performance of a dense,
cache-optimized data structure:

  - Memory allocation is per-cell; overhead is low.
  - Good performance due to _much_ less cache line access.
  - Very flexibile through bitops (see below).
  - Index can be created and destroyed on-demand.
  - Independent index won't dilute user data, which can make a big difference if
    user objects are small and densely stored.

The code in this package is a starter, a proof of concept.  It demostrates how a
clustered CBT can be done and how well it can perform.  The author hopes C3BT
can help popularize CBT as an alternative to BSTs, T-Tree and in-memory B-Tree.
CBT works on the fundamental representation of data, and playing with bits is
fun (and profitable in this case).

## Build

This version is built and tested using GCC on Linux or Cygwin.  Just type
"make".

There are 3 files: c3bt.h, c3bt.c and c3bt-main.c.  The first two are meant to
be dropped in your project, and the third is an ugly ad-hoc tester.

The code has statistics enabled by default.  If you don't need it, undefine
`C3BT_STATS` in c3bt.h.

If what you need is just an associative array, you may define `C3BT_FEATURE_MIN`
to reduce code size.  You can lookup an user object by a key value; you can
still iterate through the objects, but the ordering may be incorrect (because
they keys are treated as plain bit strings).

## Usage

C3BT supports both builtin key types and custom key type, hence two init
functions are provided.  One is `c3bt_init()` where you specify the key's data
type, its offset in your object and the length.  The other one,
`c3bt_init_bitops()`, allows you to install a bitops function for the custom
key type.

After initialization, you may use `c3bt_add()` to add some uobjs and
`c3bt_remove()` to remove them.  Again, these are all by reference: "add" won't
create or copy a object and "remove" won't free any.

Once you have some uobjs indexed, there are various `c3bt_find()` functions
that can be used to find an object by key value, and `c3bt_first()`,
`c3bt_last()`, `c3bt_next()` and `c3bt_prev()` can help iterate through them.

## Bitops

Bitops, short for "bit operations", is a function to implement a particular key
data type.  Its job is to answer three kinds of queries about the key:

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

"req" is the request number.  When req >= 0, it's a `get_bit` query.  Bitops
should return the bit at position "req" of key1.  Key2 is ignored.

If req < 0, it's a `crit_bit` query.  Bitops should return the differing bit
position between key1 and key2.  If such a bit can't be found, return -1 to
report they are equal.  The maximum number of bits to be compared can be derived
from (-req-1).

The bit number is in "writing order", that is, the order you would write the
bits on paper.  For example, 32-bit integers' MSB is bit 0 and LSB is bit 31;
for a string, character at byte offset n's MSB is 8n, and its LSB is 8n+7.  You
may call it pure big-endian.

You may have just realized that the keys in C3BT can be "virtual" since bitops
is used as the interface between keys and the algorithm.  Yes indeed; you are
free to present whatever you like to the core algorithm.  Here are some ideas:

  - A single tree can index objects of many types.  The bitops in this case
    may present a larger key space and sort the objects into non-overlapping
    ranges, like prepending a type id.  Objects of the same type would then be
    grouped together, convenient when you want to iterate by type.

  - Case insensitive string or strings with non-trivial encodings: translate
    the characters by a collation table or function before reporting the bits.

  - C3BT-LP32 limits the key to 256 bits but longer strings can be supported
    with a little compromise: bitops can compose the key as such -- take first
    28 characters as-is and append a 4 byte hash code of the remaining
    characters.  There won't be false duplications (with a decent hash) and the
    ordering is correct up to the 28th character.

  - Objects with same-valued key: append memory address of each object after the
    real key to make them unique.

Essentially, bitops fully decides what the key is; it must be constistent to
produce meaningful results.  Bitops is also called frequently, especially the
`get_bit` query.  You should make it as short and quick as possible.  If you use
only one key type, you may consider inlining it to eliminate the function call
overhead.

Although bitops is a busy function, most of the time it's used on the same
input key/uobj (query only needs `cbit`, not other uobjs).  Impacts on data
cache thus are at minimum.

## Algorithm

Clustering a non-linear structure, even as simple as a binary tree, has its
challenges.  For instance, split and merge can happen vertically between parent
and child cells but pushing nodes between siblings is not possible.  Also, tasks
like finding a split point or copying a subtree would involve tree traversal.

Fortunately the binary trees that C3BT deals with are confined in small arrays
with limited number of nodes.  This allows much simpler and light weight
solutions with bound time and space: array iteration can be used to enumerate
all the nodes, and when tree-style traversal is necessary, a non-recursive
algorithm would use only a few bytes as the work stack.  So, the code for cell
split and merge may look a bit complex, they are in fact not CPU hungry.

C3BT belongs to the family of Patricia Trie or Radix Tree data structure, which
are _not_ balanced.  However, these structures are insensitive to sequential key
inputs since a sequential number series actually have favourable crit-bit
distribution.  The most skewed input pattern won't hurt either because tree
depth is bound by key-length.

In the end, the cell fill factor and algorithm complexity of C3BT is comparable
to in-memory B-Tree, but C3BT has higher density (measured by bytes per fanout)
and more localized lookup (measured by cache lines per lookup).

### Lookup

Lookup in C3BT is straightforward: start from node 0 of the root cell, check the
bit value of key at the position indicated by the node's "cbit".  If it's 0,
follow the left child, otherwise follow the right.  If the child is a cell
pointer, continue the search from node 0 in the child cell.  If the child is an
user object pointer, get the pointer and the search is done.

However as a Patricia Trie, C3BT doesn't store the key in the tree, so a full
key comparison is necessary to confirm the found object is indeed a match.

### Iteration

C3BT iterates just like a standard BST, except that we have a parent pointer for
each cell, thus backtracking is cell-by-cell and very quick.  After locating the
cell, we follow the usual node-by-node path to locate the predecessor or
successor.

Parent pointer in most tree structures is regarded as a luxury: it speeds up
iteration but adds memory requirement and maintainance.  In C3BT however, the
per-cell parent strikes a nice balance between cost and benefit.

### Add

Adding an user object in C3BT is like in CBT: fist step is to lookup the new
object, compare the result with the new object to get their crit-bit number.
Second step is to find a position along the path as the insertion point.  The
second step should guarantee `cbit` is in ascending order from top to bottom.

If the insertion point happens to be within a full cell, we must make room for
the new node.  First attempt is to push an edge node down to a sub-cell.  If
that fails, we have to split the cell in two.

### Remove

Removing is one step: lookup the user object, then delete its reference.

But some housekeeping has to be done afterwards: if the cell is becoming
incomplete, i.e. when removing from a cell with only one node, the remaining
child should be coalesced into its parent cell then the cell should be free-ed.
This may happen in cells at the lowest level.  Otherwise, merging attempts
should be taken at this point: first try to merge the cell to its parent, then
try to merge one of the cell's sub-cells into itself.

## Future Improvements

There are many viable schemes for the cell layout:  node structure may change to
support longer keys; spare bits can be used to improve performance; and the cell
may become larger, e.g., 96B or 128B.

C3BT uses many static inline functions to abstract out the layout-dependent
operations, thus making it relatively easy to adapt to different cell layouts.

### Longer Keys
256-bit keys already cover most use cases: all primitive integer and floating
point numbers, checksums, cryptographic hashes and keys, UUID/GUID and short
string identifiers.  If even longer keys are required, you may change the "cbit"
from `uint8_t` to `uint16_t`.  The cell layout becomes: 4B header, 7 nodes, 8
external pointers.  This translates to about 12% more memory usage.

### Allocation Bitmap

Most processors today have instructions like `clz` or `fls` which can find a bit
1 very quickly.  If the layout has spare bytes, you may consider using them as
allocation map for the nodes and pointer array.  This will speed up node and
pointer allocation.

Current layout use the lowest 3 bits in the parent pointer to track the number
of nodes in a cell.  With a bitmap, you may leave the parent pointer alone and
count the number of 1s in the bitmap instead.

### Bit-Parallel Processing

If you have access to SIMD instructions (SSE, NEON, AltiVec etc.) and parallel
comparison is available, congratulations!  You can greatly speed up the
cell-level operations.  Node/pointer allocation and deallocation, finding a
node's parent in a cell, finding a cell's anchor point in its parent cell... All
these can be done in short sequences without looping.

SIMD requires the elements to be packed.  You'll need to rearrange the node
array (array of triplet [cbit, child0, child1]) into three: cbit array, left
child array and right child array.

### Read-Only Data Indexing

Due to its dense and cache-friendly nature, C3BT would be a perfect candidate
for indexing large amouts of read-only data.  The offline indexer can make the
cells larger and 100% filled up; "pointers" can be shorter after serialization,
further increasing density, etc.  The online, read-only query code, as
described, is super lightweight.  It only does bit testing; there is no partial
key or compressed key to decode; it even barely accesses the data.

vim: set ai et ts=4 tw=80 syn=markdown spell spl=en_us fo=ta:

