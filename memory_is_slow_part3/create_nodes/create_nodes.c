#include "benchmark_helpers.h"
#include <getopt.h>
#include <stdbool.h>

// Global configuration
uint64_t g_count = 0;
char* g_inputfile = NULL;
char* g_outputfile = NULL;
bool g_sort = false;
int debug = 0;
int verbose = 0;

void print_usage(const char* program_name) {
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("Creating files of random nodes\n\n");
    printf("Options:\n");
    printf("  -c, --count SIZE         Total number of nodes\n");
    printf("  -i, --input FILE         Path to input file\n");
    printf("  -o, --output FILE        Path to output file\n");
    printf("  -s, --sort               Create sorted nodes file\n");
    printf("  -d, --debug LEVEL        Enable debug mode\n");
    printf("  -v, --verbose LEVEL      Enable verbose mode\n");
    printf("  -h, --help               Show this help message\n");
}

int parse_arguments(int argc, char* argv[]) {
    int c;
    static struct option long_options[] = {
        {"count",      required_argument, 0, 'c'},
        {"input",      required_argument, 0, 'i'},
        {"output",     required_argument, 0, 'o'},
        {"debug",      required_argument, 0, 'd'},
        {"verbose",    required_argument, 0, 'v'},
        {"sort",       no_argument,       0, 's'},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    while ((c = getopt_long(argc, argv, "c:i:o:d:v:sh", long_options, NULL)) != -1) {
        switch (c) {
            case 'c':
                if (parse_size_bin(optarg, &g_count))
                    return -1;
                break;
            case 'i':
                g_inputfile = optarg;   // use strdup(optarg) if you plan to free it
                break;
            case 'o':
                g_outputfile = optarg;  // use strdup(optarg) if you plan to free it
                break;
            case 's':
                g_sort = true;
                break;
            case 'd':
                debug = atoi(optarg);
                if (debug < 0) {
                    fprintf(stderr, "Error: debug level must be non-negative\n");
                    return -1;
                }
                break;
            case 'v':
                verbose = atoi(optarg);
                if (verbose < 0) {
                    fprintf(stderr, "Error: verbose level must be non-negative\n");
                    return -1;
                }
                break;
            case 'h':
                print_usage(argv[0]);
                exit(0);
            case '?':
                return -1;
            default:
                return -1;
        }
    }

    if (g_sort) {
        if (g_inputfile == NULL || g_count == 0) {
            fprintf(stderr, "Error: If we are sorting we need the count to be > 0 and a file to read from\n");
            return -1;
        }
    } else {
        if (g_inputfile != NULL) {
            fprintf(stderr, "Error: If we are not sorting we cannot read from a file\n");
            return -1;
        }
        if (g_count == 0) {
            fprintf(stderr, "Error: If we are not sorting we need the count to be > 0\n");
            return -1;
        }
        if (g_count < 1024*1024 && !g_sort) {
            fprintf(stderr, "Error: count must be at least 1MB\n");
            return -1;
        }

    }
    return 0;
}

Node* create_nodes(size_t size) {
    char str1[32];
    char str2[64];
    size_t oversized = size * 1.5; // Extra space to increase chance of unique values
    Node* sortednodes = malloc(oversized * sizeof(Node));
    if (!sortednodes) {
        fprintf(stderr, "Error: Failed to allocate sortednodes\n");
        return NULL;
    }

    long starttime = get_time_us();

    // Generate random values directly into sortednodes
    for (size_t i = 0; i < oversized; i++) {
        generate_random_node(&sortednodes[i], size*4);
    }

    snprintf_count(str1, sizeof(str1), oversized);
    snprintf(str2, sizeof(str2), "Created %s random nodes", str1);
    report_elapsed_time(str2, starttime);
    starttime = get_time_us();

    // Sort to identify duplicates
    qsort(sortednodes, oversized, sizeof(Node), compare_nodes);

    snprintf_count(str1, sizeof(str1), oversized);
    snprintf(str2, sizeof(str2), "Sorted %s nodes", str1);
    report_elapsed_time(str2, starttime);

    // Allocate unique_nodes with exact size
    Node* unique_nodes = malloc(size * sizeof(Node));
    if (!unique_nodes) {
        fprintf(stderr, "Error: Failed to allocate unique_nodes\n");
        free(sortednodes);
        return NULL;
    }

    starttime = get_time_us();

    // Populate unique_nodes with unique values
    size_t unique_count = 1; // First node is always unique
    unique_nodes[0] = sortednodes[0];
    for (size_t i = 1; i < oversized && unique_count < (size_t)size; i++) {
        if (compare_nodes(&sortednodes[i], &unique_nodes[unique_count - 1]) != 0) {
            unique_nodes[unique_count] = sortednodes[i];
            unique_count++;
        }
    }

    snprintf_count(str1, sizeof(str1), oversized);
    snprintf(str2, sizeof(str2), "Found %s unique nodes", str1);
    report_elapsed_time(str2, starttime);

    if (debug > 2) {
        dump_binary("sortednodes.bin", sortednodes, oversized * sizeof(Node), 0);
        printf("saved sortednodes.bin\n");
    }
    free(sortednodes); // Free sortednodes as it's no longer needed

    // Ensure enough unique nodes
    if (unique_count < (size_t)size) {
        fprintf(stderr, "Error: Not enough unique nodes (%zu < %zu)\n", unique_count, size);
        free(unique_nodes);
        return NULL;
    }

    starttime = get_time_us();

    // Shuffle unique_nodes (Fisher-Yates shuffle)
    for (size_t i = size - 1; i > 0; i--) {
        size_t j = rand() % (i + 1);
        Node temp = unique_nodes[i];
        unique_nodes[i] = unique_nodes[j];
        unique_nodes[j] = temp;
    }

    report_elapsed_time("Shuffled unique nodes", starttime);

    return unique_nodes;
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    if (parse_arguments(argc, argv) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    // seeding rand
    srand(time(NULL));
    char str1[32];
    char str2[64];
    char str3[128];
    char str4[256];

    Node* nodes = NULL;
    char* filename;
    long totaltime = get_time_us();

    if (g_sort) {
        Node* insert_nodes = NULL;
        Node* delete_nodes = NULL;
        size_t* indexes = NULL;

        size_t size = g_count;
        if (g_count % 2 == 0) {
           size += 1;  // round up if even for a definitive median
        }
        size_t op_count = g_count / 10;
        if (op_count > 5000000) {
            op_count = 5000000;
        }

        // Load nodes
        // nodes should have the original set of nodes + the insert_nodes
        long starttime = get_time_us();
        if (load_nodes(g_inputfile, NULL, &nodes, &insert_nodes, &delete_nodes, &indexes, &size, &op_count) != 0) {
            printf("Failed to create nodes\n");
            return 1;
        }
        free(indexes);

        // Handling filenames
        char* fnameparts[3];
        char* tempfilename = strdup(g_inputfile);
        if (split_char_strip_ext(tempfilename, '-', fnameparts, sizeof(fnameparts)/sizeof(fnameparts[0])) != 3) {
            fprintf(stderr, "Error: Failed to split input file name\n");
            return 1;
        }
        char* pathname = fnameparts[0];
        char* hash = fnameparts[1];
        no_whitespace(snprintf_bytes(str1, sizeof(str1), g_count * sizeof(Node)), sizeof(str1));
        snprintf(str2, sizeof(str2), "-sorted-%s-%s", hash, str1);
        char* basefilename = malloc(strlen(pathname) + sizeof(str2) + 1);
        int filename_len = strlen(pathname) + sizeof(str2) + 1;
        snprintf(basefilename, filename_len, "%s%s", pathname, str2);
        free(tempfilename);
        filename_len += sizeof(str2);
        char* filename = malloc(filename_len+1);

        // sort the original set of nodes
        starttime = get_time_us();
        qsort(nodes, size, sizeof(Node), compare_nodes);
        report_elapsed_time("Sorted original nodes", starttime);

        snprintf(filename, filename_len, "%s-original.bin", basefilename);
        starttime = get_time_us();
        dump_binary(filename, nodes, size * sizeof(Node), 0);
        report_elapsed_time(filename, starttime);

        // reloading nodes for insert.
        free(nodes);
        nodes = NULL;
        free(delete_nodes);
        free(insert_nodes);
        starttime = get_time_us();
        if (load_nodes_from_file(g_inputfile, &nodes, size+op_count, 0) != 0) {
            fprintf(stderr, "Error: Failed to load nodes from file 2 time: %s\n", g_inputfile);
            return 1;
        }
        report_elapsed_time("Reloaded nodes", starttime);

        // sort the nodes post insert
        starttime = get_time_us();
        qsort(nodes, size+op_count, sizeof(Node), compare_nodes);
        report_elapsed_time("Sorted nodes post insert", starttime);

        snprintf(filename, filename_len, "%s-insert.bin", basefilename);
        starttime = get_time_us();
        dump_binary(filename, nodes, (size + op_count) * sizeof(Node), 0);
        report_elapsed_time(filename, starttime);

        // reloading nodes for delete.
        free(nodes);
        nodes = NULL;
        starttime = get_time_us();
        if (load_nodes_from_file(g_inputfile, &nodes, size, op_count) != 0) {
            fprintf(stderr, "Error: Failed to load nodes from file 3 time: %s\n", g_inputfile);
            return 1;
        }
        report_elapsed_time("Reloaded nodes", starttime);

        // sort the nodes post delete
        starttime = get_time_us();
        qsort(nodes, size, sizeof(Node), compare_nodes);
        report_elapsed_time("Sorted nodes post delete", starttime);

        snprintf(filename, filename_len, "%s-delete.bin", basefilename);
        starttime = get_time_us();
        dump_binary(filename, nodes, size * sizeof(Node), 0);
        report_elapsed_time(filename, starttime);

        free(nodes);
        free(basefilename);
        free(filename);
        report_elapsed_time("Total time", totaltime);
    } else {
        // Generating nodes.
        snprintf_bytes_count(str1, sizeof(str1), g_count, sizeof(Node));
        printf("Creating Node Pool:  %s nodes (%ldB)\n", str1, sizeof(Node));

        // Create deduplicated nodes
        nodes = create_nodes(g_count);
        if (g_outputfile == NULL) {
            if (mkdir_p("data") != 0) {
                fprintf(stderr, "Error: Failed to create directory: data\n");
                return 1;
            }
            hash8((void*)nodes, str1);
            no_whitespace(snprintf_bytes(str2, sizeof(str2), g_count * sizeof(Node)), sizeof(str2));
            snprintf(str3, sizeof(str3), "data/nodes-%s-%s.bin", str1, str2);
            filename = str3;
        } else {
            filename = g_outputfile;
        }
        dump_binary(filename, nodes, g_count * sizeof(Node), 0);
        snprintf(str4, sizeof(str4), "Deduplicated nodes written to %s", filename);
        report_elapsed_time(str4, totaltime);
    }

    return 0;
}
