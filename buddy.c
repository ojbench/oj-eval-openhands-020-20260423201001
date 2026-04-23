#include "buddy.h"
#include <stdlib.h>
#include <string.h>
#define NULL ((void *)0)

#define MAX_RANK 16
#define PAGE_SIZE (4 * 1024)

typedef struct free_node {
    struct free_node *next;
    struct free_node *prev;
} free_node_t;

static void *base_addr = NULL;
static int total_pages = 0;
static free_node_t *free_lists[MAX_RANK + 1];
static signed char *page_status = NULL; 
// >0: rank of allocated block (first page)
// <0: -(rank) of free block (first page) 
// 0: part of another block

static inline int get_page_index(void *p) {
    if (p < base_addr) return -1;
    long offset = (char *)p - (char *)base_addr;
    if (offset % PAGE_SIZE != 0) return -1;
    int idx = offset / PAGE_SIZE;
    if (idx >= total_pages) return -1;
    return idx;
}

static inline void *get_page_addr(int idx) {
    return (char *)base_addr + idx * PAGE_SIZE;
}

static void mark_allocated(int idx, int rank) {
    page_status[idx] = rank;
    int block_size = (1 << (rank - 1));
    if (block_size > 1) {
        memset(&page_status[idx + 1], 0, block_size - 1);
    }
}

static void add_to_free_list(int idx, int rank) {
    page_status[idx] = -rank;
    int block_size = (1 << (rank - 1));
    if (block_size > 1) {
        memset(&page_status[idx + 1], 0, block_size - 1);
    }
    free_node_t *node = (free_node_t *)get_page_addr(idx);
    node->next = free_lists[rank];
    node->prev = NULL;
    if (free_lists[rank] != NULL) {
        free_lists[rank]->prev = node;
    }
    free_lists[rank] = node;
}

static void remove_from_free_list(int idx, int rank) {
    free_node_t *node = (free_node_t *)get_page_addr(idx);
    if (node->prev != NULL) {
        node->prev->next = node->next;
    } else {
        free_lists[rank] = node->next;
    }
    if (node->next != NULL) {
        node->next->prev = node->prev;
    }
}

int init_page(void *p, int pgcount) {
    base_addr = p;
    total_pages = pgcount;
    
    for (int i = 0; i <= MAX_RANK; i++) {
        free_lists[i] = NULL;
    }
    
    if (page_status != NULL) {
        free(page_status);
    }
    page_status = (signed char *)calloc(total_pages, sizeof(signed char));
    if (page_status == NULL) {
        return -ENOSPC;
    }
    
    int remaining = pgcount;
    int idx = 0;
    
    while (remaining > 0) {
        int rank = MAX_RANK;
        int block_size = (1 << (rank - 1));
        
        while (block_size > remaining) {
            rank--;
            block_size = (1 << (rank - 1));
        }
        
        add_to_free_list(idx, rank);
        idx += block_size;
        remaining -= block_size;
    }
    
    return OK;
}

void *alloc_pages(int rank) {
    if (rank < 1 || rank > MAX_RANK) {
        return ERR_PTR(-EINVAL);
    }
    
    if (free_lists[rank] != NULL) {
        free_node_t *node = free_lists[rank];
        free_lists[rank] = node->next;
        if (node->next != NULL) {
            node->next->prev = NULL;
        }
        int idx = get_page_index((void *)node);
        mark_allocated(idx, rank);
        return (void *)node;
    }
    
    int larger_rank = rank + 1;
    while (larger_rank <= MAX_RANK && free_lists[larger_rank] == NULL) {
        larger_rank++;
    }
    
    if (larger_rank > MAX_RANK) {
        return ERR_PTR(-ENOSPC);
    }
    
    while (larger_rank > rank) {
        free_node_t *node = free_lists[larger_rank];
        free_lists[larger_rank] = node->next;
        if (node->next != NULL) {
            node->next->prev = NULL;
        }
        
        int idx = get_page_index((void *)node);
        larger_rank--;
        
        int child_size = (1 << (larger_rank - 1));
        int buddy_idx = idx + child_size;
        
        add_to_free_list(buddy_idx, larger_rank);
        add_to_free_list(idx, larger_rank);
    }
    
    if (free_lists[rank] != NULL) {
        free_node_t *node = free_lists[rank];
        free_lists[rank] = node->next;
        if (node->next != NULL) {
            node->next->prev = NULL;
        }
        int idx = get_page_index((void *)node);
        mark_allocated(idx, rank);
        return (void *)node;
    }
    
    return ERR_PTR(-ENOSPC);
}

int return_pages(void *p) {
    int idx = get_page_index(p);
    if (idx < 0 || page_status[idx] <= 0) {
        return -EINVAL;
    }
    
    int rank = page_status[idx];
    
    while (rank < MAX_RANK) {
        int block_size = (1 << (rank - 1));
        int buddy_idx = idx ^ block_size;
        
        if (buddy_idx < 0 || buddy_idx >= total_pages) {
            break;
        }
        
        // Check if buddy is free with the same rank
        if (page_status[buddy_idx] != -rank) {
            break;
        }
        
        // Remove buddy from free list
        remove_from_free_list(buddy_idx, rank);
        
        // Merge: the lower index becomes the merged block
        if (buddy_idx < idx) {
            idx = buddy_idx;
        }
        rank++;
    }
    
    // Add merged block to free list
    add_to_free_list(idx, rank);
    
    return OK;
}

int query_ranks(void *p) {
    int idx = get_page_index(p);
    if (idx < 0) {
        return -EINVAL;
    }
    
    // If allocated, return its rank
    if (page_status[idx] > 0) {
        return page_status[idx];
    }
    
    // If it's a free block header
    if (page_status[idx] < 0) {
        return -page_status[idx];
    }
    
    // It's part of a larger block, need to find which one
    for (int rank = MAX_RANK; rank >= 1; rank--) {
        int block_size = (1 << (rank - 1));
        int block_start = (idx / block_size) * block_size;
        
        if (block_start + block_size > total_pages) {
            continue;
        }
        
        // Check if this is a free block of this rank
        if (page_status[block_start] == -rank) {
            return rank;
        }
    }
    
    return -EINVAL;
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) {
        return -EINVAL;
    }
    
    int count = 0;
    free_node_t *curr = free_lists[rank];
    while (curr) {
        count++;
        curr = curr->next;
    }
    
    return count;
}
