#include "diskarray.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

//// DEBUG HELPERS

char* diskarray_debug_extent_str(diskarray_t* na, int extent_index) {
    // Start with initial buffer size
    size_t buffer_size = 4096;
    char* debug_str = malloc(buffer_size);
    if (!debug_str) return NULL;
    size_t pos = 0;
    // Validate extent index
    if (extent_index < 0 || extent_index >= na->num_extents) {
            snprintf(debug_str, buffer_size, "Invalid extent index %d (valid range: 0-%d)\n", 
                extent_index, na->num_extents - 1);
        return debug_str;
    }
    // Get actual values from extent data
    long rm = 0, rx = 0;
    if (na->count[extent_index] > 0) {
        void* extent_buffer = NULL;
        int result = diskarray_map_extent_exportable(na, &extent_buffer, extent_index);
        if (result == DISKARRAY_IO_SUCCESS && extent_buffer != NULL) {
            rm = *(long*)extent_buffer;
            rx = *(long*)((char*)extent_buffer + ((na->count[extent_index] - 1) * na->node_size));
        }
        diskarray_unmap_buffer_exportable(na, &extent_buffer);
    }
    // Get extent lookup values for comparison
    long tuple_min = LONG_MAX, tuple_max = LONG_MAX;
    if (na->count[extent_index] > 0) {
        tuple_min = *(long*)((char*)na->mins + extent_index * na->node_size);
        tuple_max = *(long*)((char*)na->maxs + extent_index * na->node_size);
    }
    // Add header info
    int written = snprintf(debug_str + pos, buffer_size - pos, 
                        "DEBUG Extent [%d]: data(min=%ld, max=%ld, count=%d) lookup(min=%ld, max=%ld) capacity=%d, empty=%s, dextent=%d\n", 
                        extent_index, rm, rx, na->count[extent_index], tuple_min, tuple_max, 
                        na->extent_size, na->count[extent_index] == 0 ? "YES" : "NO", na->extent_to_dextent[extent_index]);
    pos += written;
    // Add data elements
    for (int i = 0; i < na->count[extent_index]; i++) {
        // Check if we need more space (estimate ~50 chars per data line)
        if (pos + 100 > buffer_size) {
            buffer_size *= 2;
            char* new_str = realloc(debug_str, buffer_size);
            if (!new_str) {
                free(debug_str);
                return NULL;
            }
            debug_str = new_str;
        }
        void* extent_buffer = NULL;
        int result = diskarray_map_extent_exportable(na, &extent_buffer, extent_index);
        long value = 0;
        if (result == DISKARRAY_IO_SUCCESS && extent_buffer != NULL) {
            value = *(long*)((char*)extent_buffer + i * na->node_size);
        }
        written = snprintf(debug_str + pos, buffer_size - pos, 
                    "  DEBUG Extent [%d]: data[%d] = %ld\n", extent_index, i, value);
        pos += written;
        diskarray_unmap_buffer_exportable(na, &extent_buffer);
    }
    return debug_str;
}

void diskarray_debug_extent(diskarray_t* na, int extent_index) {
    char* debug_str = diskarray_debug_extent_str(na, extent_index);
    if (debug_str) {
        printf("%s", debug_str);
        free(debug_str);
    }
}


inline bool diskarray_debug_stats(diskarray_t* na) {
    bool ret = true;
    printf("NextArray Stats:\n");
    printf("  Total extents: %d\n", na->num_extents);
    printf("  Data extents: %d\n", na->num_data_extents);
    printf("  Total count: %ld\n", na->total_count);

    int used_extents = 0;
    int last_extent_idx = -1;
    // extents in key order (the directory's), named by id
    for (int t = 0; t < na->ndirs; t++)
    for (int e = 0; e < na->dirs[t]->n; e++) {
        int i = na->dirs[t]->id[e];
        if (na->count[i] > 0) {
            used_extents++;
            long min_val = *(long*)((char*)na->mins + i * na->node_size);
            long max_val = *(long*)((char*)na->maxs + i * na->node_size);
            printf("  Extent %d: count=%d, min=%ld, max=%ld\n", 
                   i, na->count[i], min_val, max_val);
            if (last_extent_idx >= 0) { 
                long last_max = *(long*)((char*)na->maxs + last_extent_idx * na->node_size);
                if (last_max > min_val) {
                    printf("  overlap between extents %d and %d\n", last_extent_idx, i);
                    ret = false;
                }
            }
            last_extent_idx = i;
        }
    }
    printf("  Used extents: %d/%d\n", used_extents, na->num_extents);
    return ret;
}



void diskarray_debug_queue(diskarray_t* na) {
    printf("=== DISKARRAY QUEUE DEBUG ===\n");
    
    // Worker threads state
    printf("  Worker threads: %d, request ring depth %d\n", na->num_worker_threads, na->num_max_bgio);
    for (int i = 0; i < na->num_worker_threads; i++) {
        worker_thread_data_t* worker = &na->workers[i];
        printf("    Worker %d: issued=%u completed=%u in flight=%d free dextents=%d/%d\n",
               i, worker->req_count, worker->cpl_head, worker->inflight, worker->dextent_count, worker->num_dextents);
    }
    printf("  Flushable: %d (watermark: %d)\n", na->flushable, na->watermark_flush);

    // Buffer state summary
    printf("  Buffer states: total %d\n", na->num_io_buffers);
    int state_counts[8] = {0}; // Index by negative buffer state + offset
    int mapped_count = 0;
    int dirty_count = 0;
    
    for (int i = 0; i < na->num_io_buffers; i++) {
        int state = na->buffer_to_extent[i];
        if (na->dirty_bits[i]) dirty_count++;
        
        if (state >= 0) {
            mapped_count++;
        } else {
            int idx = -(state + 1); // Convert to positive index
            if (idx >= 0 && idx < 8) state_counts[idx]++;
        }
    }
    
    printf("    MAPPED %d,", mapped_count);
    printf(" UNUSED %d,", state_counts[0]);        // BUFFER_UNUSED = -1
    printf(" PREFETCHING %d,", state_counts[1]);   // BUFFER_PREFETCHING = -2
    printf(" PREFETCHED %d,", state_counts[2]);    // BUFFER_PREFETCHED = -3
    printf(" FLUSHING %d,", state_counts[3]);      // BUFFER_FLUSHING = -4
    printf(" FLUSHED %d,", state_counts[4]);       // BUFFER_FLUSHED = -5
    printf(" FLUSHABLE %d,", state_counts[5]);     // BUFFER_FLUSHABLE = -6
    printf(" dirtybit %d\n", dirty_count);
    
    // Buffer pool (LRU order)
    printf("  Buffer pool (LRU to MRU order):\n");
    for (int j = 0; j < na->num_worker_threads; j++) {
        printf("   Worker %d:\n", j);
        worker_thread_data_t* worker = &na->workers[j];
        for (int i = 0; i < na->num_io_buffers_per_worker; i++) {
            int buffer_idx = worker->buffer_pool[i];
            int state = na->buffer_to_extent[buffer_idx];
            const char* state_name = "MAPPED";
            if (state < 0) {
                switch (state) {
                    case BUFFER_UNUSED: state_name = "UNUSED"; break;
                    case BUFFER_PREFETCHING: state_name = "PREFETCHING"; break;
                    case BUFFER_PREFETCHED: state_name = "PREFETCHED"; break;
                    case BUFFER_FLUSHING: state_name = "FLUSHING"; break;
                    case BUFFER_FLUSHED: state_name = "FLUSHED"; break;
                    case BUFFER_FLUSHABLE: state_name = "FLUSHABLE"; break;
                    default: state_name = "UNKNOWN"; break;
                }
            }
            printf("    [%d] buffer=%d state=%s(%d) dirty=%s\n", 
                i, buffer_idx, state_name, state, na->dirty_bits[buffer_idx] ? "Y" : "N");
        }
    }
    // Full Buffer dump
    printf("  Buffer mappings:\n");
    for (int i = 0; i < na->num_io_buffers; i++) {
        int e = na->buffer_to_extent[i];
        int eio = na->buffer_to_extent_io[i];
        printf("    Buffer %i:  e=%i  e_io=%i\n", i, e, eio);
        
    }
    // Extent to buffer mappings
    printf("  Extent mappings:\n");
    char str1[32];
    char str2[32];
    for (int i = 0; i < na->num_extents; i++) {
        int buffer = na->extent_to_buffer[i];
        int io_buffer = na->extent_to_buffer_io[i];
        int e = INT_MAX;
        snprintf(str1,sizeof(str1),"NA");
        snprintf(str2,sizeof(str2),"NA");
        if (buffer >= 0) {
            if (buffer < na->num_io_buffers) {
                e = na->buffer_to_extent[buffer];
                if (e == i) {
                    snprintf(str1,sizeof(str1),"%d",e);
                } else {
                    snprintf(str1,sizeof(str1),"INV %d",e);
                }
                
            } else {
                snprintf(str1,sizeof(str1),"BAD %d",buffer);
            }
        }
        if (io_buffer >= 0) {
            if (io_buffer < na->num_io_buffers) {
                e = na->buffer_to_extent_io[io_buffer];
                if (e == i) {
                    snprintf(str2,sizeof(str2),"%d",e);
                } else {
                    snprintf(str2,sizeof(str2),"INV %d",e);
                }
                
            } else {
                snprintf(str2,sizeof(str2),"BAD %d",io_buffer);
            }
        }
        printf("    Extent %d: b=%d - %s   b_io=%d - %s  count=%d  dextent=%d\n", 
                i, buffer,str1, io_buffer,str2, na->count[i], na->extent_to_dextent[i]);
    }
    
    // Dextent pool state
    printf("  Dextent pool:\n");
    printf("    Total dextents: %d,", na->num_dextents);
    // printf(" Free: %d,", na->dextent_count);
    // printf(" Used: %d,", na->num_dextents - na->dextent_count);
    // printf(" Head: %d, Tail: %d\n", na->dextent_head, na->dextent_tail);
    
    // Async command queue state
    size_t pending_used = (na->pending_tail - na->pending_head + na->pending_capacity) % na->pending_capacity;
    printf("  Async command queue:\n");
    printf("    Capacity: %zu,", na->pending_capacity);
    printf(" Head: %zu, Tail: %zu,", na->pending_head, na->pending_tail);
    printf(" Pending: %zu/%zu,", pending_used, na->pending_capacity);
    printf(" Next UID (cmd): %lu,", na->next_uid);
    printf(" Next UID (io): %lu\n", na->next_uid_io);
    
    printf("=== END QUEUE DEBUG ===\n");
}

void diskarray_debug_buffers(diskarray_t* na) {
    int mapped = 0;
    int flushable = 0;
    int flushing = 0;
    int flushed = 0;
    int prefetching = 0;
    int prefetched = 0;
    int unused = 0;
    int dirty = 0;
    
    for (int buffer_idx = 0; buffer_idx < na->num_io_buffers; buffer_idx++) {
        int buffer_state = na->buffer_to_extent[buffer_idx];
        
        if (na->dirty_bits[buffer_idx]) {
            dirty++;
        }
        
        if (buffer_state == BUFFER_UNUSED) {
            unused++;
        } else if (buffer_state == BUFFER_FLUSHABLE) {
            flushable++;
        } else if (buffer_state == BUFFER_FLUSHING) {
            flushing++;
        } else if (buffer_state == BUFFER_FLUSHED) {
            flushed++;
        } else if (buffer_state == BUFFER_PREFETCHING) {
            prefetching++;
        } else if (buffer_state == BUFFER_PREFETCHED) {
            prefetched++;
        } else if (buffer_state >= 0) {
            mapped++;  // Buffer is mapped to an extent
        } else {
            printf("DEBUG: Unknown buffer state %d for buffer %d\n", buffer_state, buffer_idx);
        }
    }
    printf("Buffers: mapped=%d, flushable=%d, flushing=%d, flushed=%d, prefetching=%d, prefetched=%d, unused=%d, dirty=%d\n", 
           mapped, flushable, flushing, flushed, prefetching, prefetched, unused, dirty);
}
