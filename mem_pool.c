/*
 * Created by Ivo Georgiev on 2/9/16.
 */

#include <stdlib.h>
#include <assert.h>
#include <stdio.h> // for perror()

#include "mem_pool.h"

/*************/
/*           */
/* Constants */
/*           */
/*************/
static const float      MEM_FILL_FACTOR                 = 0.75;
static const unsigned   MEM_EXPAND_FACTOR               = 2;

static const unsigned   MEM_POOL_STORE_INIT_CAPACITY    = 20;
static const float      MEM_POOL_STORE_FILL_FACTOR      = 0.75;
static const unsigned   MEM_POOL_STORE_EXPAND_FACTOR    = 2;

static const unsigned   MEM_NODE_HEAP_INIT_CAPACITY     = 40;
static const float      MEM_NODE_HEAP_FILL_FACTOR       = 0.75;
static const unsigned   MEM_NODE_HEAP_EXPAND_FACTOR     = 2;

static const unsigned   MEM_GAP_IX_INIT_CAPACITY        = 40;
static const float      MEM_GAP_IX_FILL_FACTOR          = 0.75;
static const unsigned   MEM_GAP_IX_EXPAND_FACTOR        = 2;



/*********************/
/*                   */
/* Type declarations */
/*                   */
/*********************/
typedef struct _node {
    alloc_t alloc_record;
    unsigned used;
    unsigned allocated;
    struct _node *next, *prev; // doubly-linked list for gap deletion
} node_t, *node_pt;

typedef struct _gap {
    size_t size;
    node_pt node;
} gap_t, *gap_pt;

typedef struct _pool_mgr {
    pool_t pool;
    node_pt node_heap;
    unsigned total_nodes;
    unsigned used_nodes;
    gap_pt gap_ix;
    unsigned gap_ix_capacity;
} pool_mgr_t, *pool_mgr_pt;



/***************************/
/*                         */
/* Static global variables */
/*                         */
/***************************/
static pool_mgr_pt *pool_store = NULL; // an array of pointers, only expand
static unsigned pool_store_size = 0;
static unsigned pool_store_capacity = 0;



/********************************************/
/*                                          */
/* Forward declarations of static functions */
/*                                          */
/********************************************/
static alloc_status _mem_resize_pool_store();
static alloc_status _mem_resize_node_heap(pool_mgr_pt pool_mgr);
static alloc_status _mem_resize_gap_ix(pool_mgr_pt pool_mgr);
static alloc_status
        _mem_add_to_gap_ix(pool_mgr_pt pool_mgr,
                           size_t size,
                           node_pt node);
static alloc_status
        _mem_remove_from_gap_ix(pool_mgr_pt pool_mgr,
                                size_t size,
                                node_pt node);
static alloc_status _mem_sort_gap_ix(pool_mgr_pt pool_mgr);



/****************************************/
/*                                      */
/* Definitions of user-facing functions */
/*                                      */
/****************************************/
alloc_status mem_init() {
    // ensure that it's called only once until mem_free
    // allocate the pool store with initial capacity
    // note: holds pointers only, other functions to allocate/deallocate

    if (pool_store == NULL){
        pool_store = calloc(MEM_POOL_STORE_INIT_CAPACITY, sizeof(pool_mgr_pt));
        pool_store_capacity = MEM_POOL_STORE_INIT_CAPACITY;
        pool_store_size = 0;
        return ALLOC_OK;
    } else {
        return ALLOC_CALLED_AGAIN;
    }

}

alloc_status mem_free() {
    // ensure that it's called only once mem_new_ for each mem_init
    // make sure all pool managers have been deallocated
    // can free the pool store array
    // update static variables

    if (pool_store != NULL){
        free(pool_store);
        pool_store = NULL;
        return ALLOC_OK;
    } else {
        return ALLOC_CALLED_AGAIN;
    }

}

pool_pt mem_pool_open(size_t size, alloc_policy policy) {
    // make sure that the pool store is allocated
    if (pool_store == NULL){
        return NULL;
    }

    // expand the pool store, if necessary
    _mem_resize_pool_store();

    // allocate a new mem pool mgr
    pool_mgr_pt my_pool_mgr = calloc(1, sizeof(pool_mgr_t));

    // check success, on error return null
    if (my_pool_mgr == NULL) return NULL;

    // allocate a new memory pool
    my_pool_mgr->pool.mem = (char*) calloc(size, sizeof(char));

    // check success, on error deallocate mgr and return null
    if (my_pool_mgr->pool.mem == NULL){
        free(my_pool_mgr);
        return NULL;
    }

    // allocate a new node heap
    my_pool_mgr->node_heap = (node_t*) calloc(MEM_NODE_HEAP_INIT_CAPACITY, sizeof(node_t));

    // check success, on error deallocate mgr/pool and return null
    if (my_pool_mgr->node_heap == NULL) {
        free(my_pool_mgr->pool.mem);
        free(my_pool_mgr);
        return NULL;
    }

    // allocate a new gap index
    my_pool_mgr->gap_ix = (gap_t*) calloc(MEM_GAP_IX_INIT_CAPACITY, sizeof(gap_t));

    // check success, on error deallocate mgr/pool/heap and return null
    if (my_pool_mgr->gap_ix == NULL){
        free(my_pool_mgr->node_heap);
        free(my_pool_mgr->pool.mem);
        free(my_pool_mgr);
        my_pool_mgr = NULL;
        return NULL;
    }

    // assign all the pointers and update meta data:
    //   initialize pool mgr
    my_pool_mgr->pool.policy = policy;
    my_pool_mgr->pool.total_size = size;
    my_pool_mgr->pool.num_gaps = 1;
    my_pool_mgr->total_nodes = MEM_NODE_HEAP_INIT_CAPACITY;
    my_pool_mgr->gap_ix_capacity = MEM_GAP_IX_INIT_CAPACITY;


    //   initialize top node of node heap
    //   initialize top node of gap index
    node_pt my_node_heap = my_pool_mgr->node_heap; //for shortening following assignments

    my_node_heap->used = 1;
    my_node_heap->allocated = 0;
    my_node_heap->next = NULL;
    my_node_heap->prev = NULL;
    my_node_heap->alloc_record.size = size;
    my_node_heap->alloc_record.mem = my_pool_mgr->pool.mem;

    //   link pool mgr to pool store
    pool_store[pool_store_size] = my_pool_mgr;
    pool_store_size++;
    // return the address of the mgr, cast to (pool_pt)

    return (pool_pt)&my_pool_mgr->pool;
} //end mem_pool_open

alloc_status mem_pool_close(pool_pt pool) {
    // get mgr from pool by casting the pointer to (pool_mgr_pt)
    pool_mgr_pt my_pool_mgr = (pool_mgr_pt) pool;
    // check if this pool is allocated

    if (my_pool_mgr == NULL) {
        return ALLOC_NOT_FREED;
    }

    // check if pool has only one gap
    // check if it has zero allocations
    // free memory pool
    // free node heap
    // free gap index
    // find mgr in pool store and set to null
    // note: don't decrement pool_store_size, because it only grows
    // free mgr
    if (my_pool_mgr->pool.num_gaps == 1 && my_pool_mgr->pool.num_allocs==0) {

    } else {
        return ALLOC_NOT_FREED;
    }



    return ALLOC_OK;
}

alloc_pt mem_new_alloc(pool_pt pool, size_t size) {
    // get mgr from pool by casting the pointer to (pool_mgr_pt)
    pool_mgr_pt my_pool_mgr = (pool_mgr_pt) pool;


    // check if any gaps, return null if none
    if (my_pool_mgr->pool.num_gaps == 0){
        return NULL;
    }

    // expand heap node, if necessary, quit on error
    if (_mem_resize_node_heap(my_pool_mgr) == ALLOC_FAIL){
        return NULL;
    }

    // check used nodes fewer than total nodes, quit on error
    if (my_pool_mgr->used_nodes > my_pool_mgr->total_nodes){
        return NULL;
    }
    // get a node for allocation:
    node_pt my_node = NULL;

    // if FIRST_FIT, then find the first sufficient node in the node heap
    if (pool->policy == FIRST_FIT) {
        for(int i = 0; i < my_pool_mgr->total_nodes; i++) {
            //if node is <= size, is used and isn't allocated


            if (my_pool_mgr->node_heap[i].alloc_record.size >= size && my_pool_mgr->node_heap[i].used == 1
                    && my_pool_mgr->node_heap[i].allocated == 0) {
                my_node = &my_pool_mgr->node_heap[i];
                my_pool_mgr->node_heap[i] = *my_node;
                break;
            }

        }

    } else if (pool->policy == BEST_FIT) {
        // if BEST_FIT, then find the first sufficient node in the gap index


    }

    // check if node found
    if (my_node == NULL){
        return NULL;
    }
    // update metadata (num_allocs, alloc_size)
    my_pool_mgr->pool.num_allocs++;
    my_pool_mgr->pool.alloc_size += size;


    // calculate the size of the remaining gap, if any
    size_t gap_remainder = my_node->alloc_record.size - size;
    // remove node from gap index
    _mem_remove_from_gap_ix(my_pool_mgr, size, my_node);
    // convert gap_node to an allocation node of given size
    my_node->allocated = 1;
    my_node->alloc_record.mem = calloc(1,size);
    my_node->alloc_record.size = size;


    // adjust node heap:
    //   if remaining gap, need a new node
    node_pt new_node = NULL;
    if (gap_remainder > 0) {

        for (int i = 0; i < my_pool_mgr->total_nodes; i++) {

            if (my_pool_mgr->node_heap[i].used == 0) { //find an unused one in the node heap
                new_node = &my_pool_mgr->node_heap[i];
                break;
            }
        }

        if (new_node == NULL) { //   make sure one was found
            return NULL;
        }
        //   initialize it to a gap node
        new_node->alloc_record.size = gap_remainder;
        new_node->alloc_record.mem = my_node->alloc_record.mem + size;
        new_node->used = 1;
        new_node->allocated = 0;
        //   update metadata (used_nodes)
        my_pool_mgr->used_nodes++;
        //   update linked list (new node right after the node for allocation)
        my_node->next = new_node;
        new_node->prev = my_node;
        //   add to gap index
        if (_mem_add_to_gap_ix(my_pool_mgr, gap_remainder, new_node) == ALLOC_OK) {

        }
    }






    //   check if successful
    // return allocation record by casting the node to (alloc_pt)

    return (alloc_pt )my_node;
} //end mem_new_alloc

alloc_status mem_del_alloc(pool_pt pool, alloc_pt alloc) {
    // get mgr from pool by casting the pointer to (pool_mgr_pt)
    pool_mgr_pt my_pool_mgr = (pool_mgr_pt) pool;
    // get node from alloc by casting the pointer to (node_pt)
    node_pt remove_node = (node_pt) alloc;
    // find the node in the node heap
    node_pt find_node = NULL;

    for (int i = 0; i < my_pool_mgr->total_nodes; i++) {
        if( remove_node == &my_pool_mgr->node_heap[i]) {
            find_node = remove_node;
        }
    }

    // this is node-to-delete
    // make sure it's found
    if (find_node == NULL) { //if not found
        return ALLOC_FAIL;
    }
    // convert to gap node
    remove_node->allocated = 0;
    // update metadata (num_allocs, alloc_size)
    my_pool_mgr->pool.num_allocs--;
    my_pool_mgr->pool.alloc_size -= remove_node->alloc_record.size;
    // if the next node in the list is also a gap, merge into node-to-delete
    if (remove_node->next != NULL && remove_node->next->allocated == 0){
        return (_mem_remove_from_gap_ix(my_pool_mgr, 0, remove_node->next ));
    }
    //   remove the next node from gap index
    //   check success
    //   add the size to the node-to-delete
    //   update node as unused
    remove_node->used = 0;
    //   update metadata (used nodes)
    my_pool_mgr->used_nodes--;
    //   update linked list:
    /*
                    if (next->next) {
                        next->next->prev = node_to_del;
                        node_to_del->next = next->next;
                    } else {
                        node_to_del->next = NULL;
                    }
                    next->next = NULL;
                    next->prev = NULL;
     */

    // this merged node-to-delete might need to be added to the gap index
    // but one more thing to check...
    // if the previous node in the list is also a gap, merge into previous!
    //   remove the previous node from gap index
    //   check success
    //   add the size of node-to-delete to the previous
    //   update node-to-delete as unused
    //   update metadata (used_nodes)
    //   update linked list
    /*
                    if (node_to_del->next) {
                        prev->next = node_to_del->next;
                        node_to_del->next->prev = prev;
                    } else {
                        prev->next = NULL;
                    }
                    node_to_del->next = NULL;
                    node_to_del->prev = NULL;
     */
    //   change the node to add to the previous node!
    // add the resulting node to the gap index
    // check success

    return ALLOC_OK;
}

void mem_inspect_pool(pool_pt pool,
                      pool_segment_pt *segments,
                      unsigned *num_segments) {
    // get the mgr from the pool
    pool_mgr_pt my_pool_mgr = (pool_mgr_pt) pool;
    // allocate the segments array with size == used_nodes
    pool_segment_pt segs = (pool_segment_pt) calloc(my_pool_mgr->used_nodes, sizeof(pool_segment_t));
    // check successful
    if (segs == NULL){
        return;
    }

    node_pt my_node;
    // loop through the node heap and the segments array
    for (int i = 0; i < my_pool_mgr->used_nodes ; i++) {
        //    for each node, write the size and allocated in the segment
        my_node = &my_pool_mgr->node_heap[i];
        if (my_node != NULL){
            segs[i].allocated = my_node->allocated;
            segs[i].size = my_node->alloc_record.size;
        }

    }

    *segments = segs;
    *num_segments = my_pool_mgr->used_nodes;

}



/***********************************/
/*                                 */
/* Definitions of static functions */
/*                                 */
/***********************************/
static alloc_status _mem_resize_pool_store() {
    // check if necessary
    if (((float) pool_store_size / pool_store_capacity)
        > MEM_POOL_STORE_FILL_FACTOR)
    {
        pool_store_size = pool_store_size * MEM_EXPAND_FACTOR;
        pool_store_capacity = pool_store_capacity * MEM_EXPAND_FACTOR;
        pool_store = (pool_mgr_pt*) realloc(pool_store, sizeof(pool_mgr_t));

    }

    // don't forget to bring a towel

    return ALLOC_OK;
}

static alloc_status _mem_resize_node_heap(pool_mgr_pt pool_mgr) {
    // see above




    return ALLOC_OK;
}

static alloc_status _mem_resize_gap_ix(pool_mgr_pt pool_mgr) {
    // see above
    if (((float) pool_mgr->pool.num_gaps / pool_mgr->gap_ix_capacity)
        > MEM_GAP_IX_EXPAND_FACTOR)
    {
        pool_mgr->gap_ix_capacity = pool_mgr->gap_ix_capacity * MEM_GAP_IX_EXPAND_FACTOR;
        pool_mgr->gap_ix = realloc(pool_store_capacity, sizeof(gap_t));
    }

    return ALLOC_OK;
}

static alloc_status _mem_add_to_gap_ix(pool_mgr_pt pool_mgr,
                                       size_t size,
                                       node_pt node) {

    // expand the gap index, if necessary (call the function)
    _mem_resize_gap_ix(pool_mgr);
    // add the entry at the end
    unsigned lastgap = pool_mgr->pool.num_gaps;
    gap_pt my_gap = &pool_mgr->gap_ix[lastgap];
    // update metadata (num_gaps)
    pool_mgr->pool.num_gaps++;
    // sort the gap index (call the function)
    return (_mem_sort_gap_ix(pool_mgr));
    // check success

    return ALLOC_OK;
}

static alloc_status _mem_remove_from_gap_ix(pool_mgr_pt pool_mgr,
                                            size_t size,
                                            node_pt node) {

    int location = -42;
    // find the position of the node in the gap index
    for (int i = 0; i < pool_mgr->gap_ix_capacity; i++) {
        if (node == pool_mgr->gap_ix[i].node) {
            location = i; //save location
            break;
        }
    }
    if (location == -42) {
        return ALLOC_FAIL;
    }
    // loop from there to the end of the array:
    //    pull the entries (i.e. copy over) one position up
    //    this effectively deletes the chosen node
    for (int i = location; i < pool_mgr->gap_ix_capacity; i++) {
        pool_mgr->gap_ix[i] = pool_mgr->gap_ix[i+1];
    }
    // update metadata (num_gaps)
    pool_mgr->pool.num_gaps--;
    // zero out the element at position num_gaps!
    pool_mgr->gap_ix[pool_mgr->pool.num_gaps].size = 0;
    pool_mgr->gap_ix[pool_mgr->pool.num_gaps].node = NULL;

    return ALLOC_OK;
}

// note: only called by _mem_add_to_gap_ix, which appends a single entry
static alloc_status _mem_sort_gap_ix(pool_mgr_pt pool_mgr) {
    // the new entry is at the end, so "bubble it up"
    // loop from num_gaps - 1 until but not including 0:
    //    if the size of the current entry is less than the previous (u - 1)
    //    or if the sizes are the same but the current entry points to a
    //    node with a lower address of pool allocation address (mem)
    //       swap them (by copying) (remember to use a temporary variable)

    return ALLOC_FAIL;
}


