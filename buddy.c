#include "buddy.h"
#include <stdlib.h>
#define NULL ((void *)0)

#define MAX_RANK 16
#define PAGE_SIZE (4 * 1024)

// Free list node structure
typedef struct free_node {
    struct free_node *next;
} free_node_t;

// Global variables
static void *base_addr = NULL;
static int total_pages = 0;
static free_node_t *free_lists[MAX_RANK + 1]; // free_lists[rank] for each rank
static char *page_ranks = NULL; // Store rank for each 4K page (0 means free/part of larger block)

// Helper function to get page index from address
static inline int get_page_index(void *p) {
    if (p < base_addr) return -1;
    long offset = (char *)p - (char *)base_addr;
    if (offset % PAGE_SIZE != 0) return -1;
    int idx = offset / PAGE_SIZE;
    if (idx >= total_pages) return -1;
    return idx;
}

// Helper function to get address from page index
static inline void *get_page_addr(int idx) {
    return (char *)base_addr + idx * PAGE_SIZE;
}

// Helper function to get buddy index
static inline int get_buddy_index(int idx, int rank) {
    int block_size = (1 << (rank - 1)); // 2^(rank-1) pages
    return idx ^ block_size;
}

// Check if a block is free (not allocated)
static int is_block_free(int idx, int rank) {
    int block_size = (1 << (rank - 1));
    for (int i = 0; i < block_size; i++) {
        if (page_ranks[idx + i] != 0) return 0;
    }
    return 1;
}

// Mark a block as allocated with given rank
static void mark_allocated(int idx, int rank) {
    int block_size = (1 << (rank - 1));
    for (int i = 0; i < block_size; i++) {
        page_ranks[idx + i] = (i == 0) ? rank : -rank; // First page stores rank, others store -rank
    }
}

// Mark a block as free
static void mark_free(int idx, int rank) {
    int block_size = (1 << (rank - 1));
    for (int i = 0; i < block_size; i++) {
        page_ranks[idx + i] = 0;
    }
}

// Add block to free list
static void add_to_free_list(int idx, int rank) {
    free_node_t *node = (free_node_t *)get_page_addr(idx);
    node->next = free_lists[rank];
    free_lists[rank] = node;
}

// Remove block from free list
static void remove_from_free_list(int idx, int rank) {
    void *addr = get_page_addr(idx);
    free_node_t **curr = &free_lists[rank];
    while (*curr) {
        if ((void *)*curr == addr) {
            *curr = (*curr)->next;
            return;
        }
        curr = &((*curr)->next);
    }
}

int init_page(void *p, int pgcount) {
    base_addr = p;
    total_pages = pgcount;
    
    // Initialize free lists
    for (int i = 0; i <= MAX_RANK; i++) {
        free_lists[i] = NULL;
    }
    
    // Allocate metadata separately using malloc
    if (page_ranks != NULL) {
        free(page_ranks);
    }
    page_ranks = (char *)malloc(total_pages * sizeof(char));
    if (page_ranks == NULL) {
        return -ENOSPC;
    }
    
    for (int i = 0; i < total_pages; i++) {
        page_ranks[i] = 0;
    }
    
    // Find the largest rank that fits
    int remaining = pgcount;
    int idx = 0;
    
    while (remaining > 0) {
        int rank = MAX_RANK;
        int block_size = (1 << (rank - 1));
        
        // Find largest block that fits
        while (block_size > remaining) {
            rank--;
            block_size = (1 << (rank - 1));
        }
        
        // Add this block to free list
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
    
    // Try to find a free block of the requested rank
    if (free_lists[rank] != NULL) {
        free_node_t *node = free_lists[rank];
        free_lists[rank] = node->next;
        int idx = get_page_index((void *)node);
        mark_allocated(idx, rank);
        return (void *)node;
    }
    
    // Need to split a larger block
    int larger_rank = rank + 1;
    while (larger_rank <= MAX_RANK && free_lists[larger_rank] == NULL) {
        larger_rank++;
    }
    
    if (larger_rank > MAX_RANK) {
        return ERR_PTR(-ENOSPC);
    }
    
    // Split blocks down to the requested rank
    while (larger_rank > rank) {
        free_node_t *node = free_lists[larger_rank];
        free_lists[larger_rank] = node->next;
        
        int idx = get_page_index((void *)node);
        larger_rank--;
        
        // When splitting a rank (larger_rank+1) block, we get two rank larger_rank blocks
        // The second block starts at idx + 2^(larger_rank-1)
        int child_size = (1 << (larger_rank - 1));
        int buddy_idx = idx + child_size;
        
        // Add in reverse order so that the first block (lower address) is at the head
        add_to_free_list(buddy_idx, larger_rank);
        add_to_free_list(idx, larger_rank);
    }
    
    // Now allocate from the requested rank
    if (free_lists[rank] != NULL) {
        free_node_t *node = free_lists[rank];
        free_lists[rank] = node->next;
        int idx = get_page_index((void *)node);
        mark_allocated(idx, rank);
        return (void *)node;
    }
    
    return ERR_PTR(-ENOSPC);
}

int return_pages(void *p) {
    int idx = get_page_index(p);
    if (idx < 0 || page_ranks[idx] <= 0) {
        return -EINVAL;
    }
    
    int rank = page_ranks[idx];
    mark_free(idx, rank);
    
    // Try to merge with buddy
    while (rank < MAX_RANK) {
        int buddy_idx = get_buddy_index(idx, rank);
        int block_size = (1 << (rank - 1));
        
        // Check if buddy is within bounds and free
        if (buddy_idx < 0 || buddy_idx + block_size > total_pages) {
            break;
        }
        
        if (!is_block_free(buddy_idx, rank)) {
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
    if (page_ranks[idx] > 0) {
        return page_ranks[idx];
    }
    
    // If free, find the maximum rank this page belongs to
    for (int rank = MAX_RANK; rank >= 1; rank--) {
        int block_size = (1 << (rank - 1));
        // Find the start of the block of this rank that contains idx
        int block_start = (idx / block_size) * block_size;
        
        if (block_start + block_size > total_pages) {
            continue;
        }
        
        // Check if this entire block is free
        if (is_block_free(block_start, rank)) {
            // Check if it's in the free list
            free_node_t *curr = free_lists[rank];
            void *block_addr = get_page_addr(block_start);
            while (curr) {
                if ((void *)curr == block_addr) {
                    return rank;
                }
                curr = curr->next;
            }
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
