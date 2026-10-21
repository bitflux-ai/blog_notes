#ifndef TARGETS_H
#define TARGETS_H

#include <stdint.h>
#include <stddef.h>

// One NVMe-oF namespace from targets.yml, as seen by this (initiator) host.
typedef struct {
    char nqn[256];
    unsigned nsid;
    uint64_t size_bytes;    // from sysfs (filled by targets_resolve)
    char path[64];          // local block device, e.g. /dev/nvme3n1 (filled by targets_resolve)
} target_drive_t;

// Parse the serial-keyed targets.yml (wargy_nvmeof `just export`) and return
// the drives in file order, NQN derived from host + serial.  Returns the
// number of drives (caller frees *out), or -1 on error.
int targets_read(const char* yml_path, target_drive_t** out);

// Map each drive's NQN + nsid to its local /dev/nvme* path and size by reading
// /sys/class/nvme-subsystem, /sys/class/nvme and /sys/block (text files only;
// no device I/O).
// Returns 0 if every drive resolved, -1 otherwise (unresolved ones are reported).
int targets_resolve(target_drive_t* drives, int n);

#endif
