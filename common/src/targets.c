#include "targets.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Minimal parser for the serial-keyed targets.yml that wargy_nvmeof's
// `just export` writes: each "- host:" item lists "- serial:" drives.  Every
// drive is its own subsystem with namespace 1, NQN derived as
// nqn.2026-04.labtwo:<host>:<serial> (wargy never stores it).  Other keys
// (address, port, model, size, ...) are ignored; the size comes from sysfs
// once the drive is connected (targets_resolve).

#define NQN_PREFIX "nqn.2026-04.labtwo:"

static char* trim(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    return s;
}

int targets_read(const char* yml_path, target_drive_t** out) {
    FILE* f = fopen(yml_path, "r");
    if (!f) {
        fprintf(stderr, "Failed to open %s\n", yml_path);
        return -1;
    }

    target_drive_t* ents = NULL;
    int n = 0, cap = 0;
    char host[128] = "", line[1024];
    while (fgets(line, sizeof(line), f)) {
        char* s = trim(line);
        if (*s == '#' || *s == 0) continue;
        if (s[0] == '-' && (s[1] == ' ' || s[1] == 0)) s = trim(s + 1);  // list item marker

        char* colon = strchr(s, ':');
        if (!colon) continue;
        *colon = 0;
        const char* key = s;
        char* val = trim(colon + 1);

        if (strcmp(key, "subsystems") == 0 || strcmp(key, "nqn") == 0) {
            fprintf(stderr, "%s is the old pre-serial format; re-run: just export\n", yml_path);
            fclose(f); free(ents);
            return -1;
        } else if (strcmp(key, "host") == 0) {
            snprintf(host, sizeof(host), "%s", val);
        } else if (strcmp(key, "serial") == 0) {
            if (!*host) {
                fprintf(stderr, "%s: drive %s is not under a host\n", yml_path, val);
                fclose(f); free(ents);
                return -1;
            }
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                target_drive_t* grown = realloc(ents, cap * sizeof(target_drive_t));
                if (!grown) { fclose(f); free(ents); return -1; }
                ents = grown;
            }
            target_drive_t* t = &ents[n++];
            memset(t, 0, sizeof(*t));
            snprintf(t->nqn, sizeof(t->nqn), NQN_PREFIX "%s:%s", host, val);
            t->nsid = 1;
        }
    }
    fclose(f);
    *out = ents;
    return n;
}

static int read_sysfs_line(const char* path, char* buf, size_t len) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    char* r = fgets(buf, (int)len, f);
    fclose(f);
    if (!r) return -1;
    buf[strcspn(buf, "\r\n")] = 0;
    return 0;
}

// Look for a namespace block device of subsystem `nqn` with id `nsid` under
// <root>/<subdir_prefix>*/.  Namespaces appear as children named nvme<K>n<M>.
static int scan_root(const char* root, const char* subdir_prefix,
                     const char* nqn, unsigned nsid, char* out, size_t outlen) {
    DIR* d = opendir(root);
    if (!d) return -1;
    struct dirent* e;
    int found = -1;
    while (found < 0 && (e = readdir(d))) {
        if (strncmp(e->d_name, subdir_prefix, strlen(subdir_prefix)) != 0) continue;
        char sub[512], path[1024], val[512];
        snprintf(sub, sizeof(sub), "%s/%s", root, e->d_name);
        snprintf(path, sizeof(path), "%s/subsysnqn", sub);
        if (read_sysfs_line(path, val, sizeof(val)) != 0 || strcmp(val, nqn) != 0) continue;

        DIR* sd = opendir(sub);
        if (!sd) continue;
        struct dirent* c;
        while ((c = readdir(sd))) {
            unsigned k, m;
            int used = 0;
            // exact "nvme<K>n<M>"; skips nvme<K>c<C>n<M> multipath paths
            if (sscanf(c->d_name, "nvme%un%u%n", &k, &m, &used) != 2 || c->d_name[used] != 0) continue;
            snprintf(path, sizeof(path), "%s/%s/nsid", sub, c->d_name);
            if (read_sysfs_line(path, val, sizeof(val)) != 0 || strtoul(val, NULL, 10) != nsid) continue;
            snprintf(out, outlen, "nvme%un%u", k, m);
            found = 0;
            break;
        }
        closedir(sd);
    }
    closedir(d);
    return found;
}

int targets_resolve(target_drive_t* drives, int n) {
    int bad = 0;
    for (int i = 0; i < n; i++) {
        target_drive_t* t = &drives[i];
        char name[32], path[96], val[64];
        if (scan_root("/sys/class/nvme-subsystem", "nvme-subsys", t->nqn, t->nsid, name, sizeof(name)) != 0 &&
            scan_root("/sys/class/nvme", "nvme", t->nqn, t->nsid, name, sizeof(name)) != 0) {
            fprintf(stderr, "No local block device for %s (namespace %u) - is it connected?\n", t->nqn, t->nsid);
            bad++;
            continue;
        }
        snprintf(t->path, sizeof(t->path), "/dev/%s", name);
        // /sys/block/<dev>/size is in 512-byte sectors regardless of the LBA size
        snprintf(path, sizeof(path), "/sys/block/%s/size", name);
        if (read_sysfs_line(path, val, sizeof(val)) != 0 || (t->size_bytes = strtoull(val, NULL, 10) * 512) == 0) {
            fprintf(stderr, "Cannot read the size of %s from %s\n", t->path, path);
            bad++;
        }
    }
    return bad ? -1 : 0;
}
