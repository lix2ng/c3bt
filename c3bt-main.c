#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "c3bt.h"

void print_stats(c3bt_tree* tree)
{
    printf("%d uobjs in %d cells, %d pushdowns %d splits %d pushups "
        "%d merges.\n", c3bt_nobjects(tree), c3bt_stat_cells,
        c3bt_stat_pushdowns, c3bt_stat_splits, c3bt_stat_pushups,
        c3bt_stat_mergeups);
}

void clear_stats()
{
    c3bt_stat_pushdowns = 0;
    c3bt_stat_splits = 0;
    c3bt_stat_pushups = 0;
    c3bt_stat_mergeups = 0;
}

int main()
{
#define ASIZE   100000
    c3bt_tree tree;
    c3bt_cursor cur;
    int i;
    int *array = malloc(ASIZE * sizeof(int));
    int *robj;
    struct timespec t_start, t_end;

//    srand(time(NULL) - 1354856137);
//    srand(77);
    for (i = 0; i < ASIZE; i++)
//        array[i] = rand();
        array[i] = i * 7;

    c3bt_init(&tree, C3BT_KDT_U32, 0, 0);
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    for (i = 0; i < ASIZE; i++)
        c3bt_add(&tree, array + i);
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    printf("Add %dk uobjs: %ldus\n", ASIZE / 1000,
        (t_end.tv_sec - t_start.tv_sec) * 1000000
            + (t_end.tv_nsec - t_start.tv_nsec) / 1000);
    print_stats(&tree);
    clear_stats();

    clock_gettime(CLOCK_MONOTONIC, &t_start);
    for (i = 0; i < ASIZE; i += 2)
        c3bt_remove(&tree, array + i);
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    printf("Remove %dk uobjs: %ldus\n", ASIZE / 2000,
        (t_end.tv_sec - t_start.tv_sec) * 1000000
            + (t_end.tv_nsec - t_start.tv_nsec) / 1000);
    print_stats(&tree);
    clear_stats();

    clock_gettime(CLOCK_MONOTONIC, &t_start);
    for (i = 0; i < ASIZE; i += 2)
        c3bt_add(&tree, array + i);
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    printf("Re-add %dk uobjs: %ldus\n", ASIZE / 2000,
        (t_end.tv_sec - t_start.tv_sec) * 1000000
            + (t_end.tv_nsec - t_start.tv_nsec) / 1000);
    print_stats(&tree);

    robj = c3bt_first(&tree, &cur);
    while (robj) {
        //printf("%d\n", *robj);
        robj = c3bt_next(&tree, &cur);
    }
    c3bt_destroy(&tree);
    printf("Population distribution:\n");
    for (i = 0; i < NODES_PER_CELL; i++)
        printf("cells with %d nodes: %d\n", i + 1, c3bt_stat_popdist[i]);
    free(array);
    return 0;
}

/* vim: set syn=c.doxygen cin et sw=4 ts=4 tw=80 fo=croqmM: */
