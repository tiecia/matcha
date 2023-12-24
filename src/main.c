#define _XOPEN_SOURCE 500
#include "idle-inhibit-unstable-v1.h"
#include <fcntl.h>
#include <getopt.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#define HELP                                                                                       \
    "Usage: matcha [MODE] [OPTION]...\n"                                                           \
    "MODE:\n"                                                                                      \
    "  -d, --daemon     Main instance (Daemon Mode)\n"                                             \
    "  -t, --toggle     Toggle instance (Toggle Mode)\n\n"                                         \
    "Options:\n"                                                                                   \
    "  -b, --bar=[BAR]  Set the bar type to bar (default: None)\n"                                 \
    "  -o, --off        Start daemon with inhibitor off\n"                                         \
    "  -h, --help       Display this help and exit\n\n"                                            \
    "BAR: \n"                                                                                      \
    "    yambar - Only works on daemon instance\n"                                                 \
    "    waybar - Only works on toggle instance\n"

#define SHARED_MEM_NAME "/matcha-idle-inhibit"

typedef struct wl_registry wl_registry;
typedef struct wl_compositor wl_compositor;
typedef struct zwp_idle_inhibit_manager_v1 zwp_idle_inhibit_manager_v1;
typedef struct wl_surface wl_surface;
typedef struct zwp_idle_inhibitor_v1 zwp_idle_inhibitor_v1;
typedef struct wl_display wl_display;

typedef enum { INHIBIT, UNINHIBIT, KILL } SignalState;
static SignalState signal_state = INHIBIT;

typedef enum {
    NONE,
    WAYBAR,
    YAMBAR,
} Bar;

typedef struct {
    bool inhibit;
    sem_t sem;
} SharedMem;

typedef struct {
    SharedMem* shared_mem;
    wl_registry* registry;
    wl_display* display;
    wl_compositor* compositor;
    wl_surface* surface;
    zwp_idle_inhibit_manager_v1* idle_inhibit_manager;
    zwp_idle_inhibitor_v1* idle_inhibitor;
} MatchaBackend;

// destroys all componenet of the backend
static void matcha_destroy(MatchaBackend* backend) {
    fprintf(stderr, "Cleaning Up\n");
    if (backend->idle_inhibitor) {
        zwp_idle_inhibitor_v1_destroy(backend->idle_inhibitor);
    }
    if (backend->idle_inhibit_manager) {
        zwp_idle_inhibit_manager_v1_destroy(backend->idle_inhibit_manager);
    }
    if (backend->surface) {
        wl_surface_destroy(backend->surface);
    }
    if (backend->compositor) {
        wl_compositor_destroy(backend->compositor);
    }
    if (backend->registry) {
        wl_registry_destroy(backend->registry);
    }
    if (backend->display) {
        wl_display_flush(backend->display);
        wl_display_disconnect(backend->display);
    }
    if (backend->shared_mem) {
        sem_destroy(&backend->shared_mem->sem);
        munmap(backend->shared_mem, sizeof(SharedMem));
    }
    free(backend);
}

static void global_registry_handler(void* data, struct wl_registry* registry, uint32_t name,
                                    const char* interface, uint32_t version) {
    MatchaBackend* backend = (MatchaBackend*)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        fprintf(stderr, "Found A Compositor\n");
        backend->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version);
    } else if (strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name) == 0) {
        fprintf(stderr, "Found An Inhibitor Manager\n");
        backend->idle_inhibit_manager =
            wl_registry_bind(registry, name, &zwp_idle_inhibit_manager_v1_interface, version);
    }
}

// remover, empty
static void global_registry_remover(void* data, struct wl_registry* registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {global_registry_handler,
                                                              global_registry_remover};

// create shared memory
static SharedMem* create_shared_mem(void) {
    int shm_fd = shm_open(SHARED_MEM_NAME, O_CREAT | O_RDWR, 0660);
    if (shm_fd == -1) {
        perror("Failed to initialize Matcha, Other instances might be running\nERR");
        signal_state = KILL;
        return NULL;
    }
    if (ftruncate(shm_fd, sizeof(SharedMem)) == -1) {
        close(shm_fd);
        perror("Failed to truncate shared memory");
        signal_state = KILL;
        return NULL;
    }
    SharedMem* data =
        (SharedMem*)mmap(0, sizeof(SharedMem), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (data == MAP_FAILED) {
        close(shm_fd);
        perror("Failed to map shared memory");
        signal_state = KILL;
        return NULL;
    }
    close(shm_fd);
    sem_init(&data->sem, 1, 1);
    data->inhibit = true;
    return data;
}

static SharedMem* access_shared_mem(void) {
    int shm_fd = shm_open(SHARED_MEM_NAME, O_RDWR, 0660);
    if (shm_fd == -1) {
        fprintf(stderr, "Failed to attach to the main process, make sure matcha is running\n");
        exit(1);
    }

    SharedMem* data =
        (SharedMem*)mmap(0, sizeof(SharedMem), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (data == MAP_FAILED) {
        perror("Failed to map shared memory");
        exit(1);
    }
    close(shm_fd);
    return data;
}

static void signal_handler(int signum, siginfo_t* info, void* context) {
    (void)info;
    (void)context;
    switch (signum) {
    case SIGUSR1:
        fprintf(stderr, "SIGUSR1 received, Toggling...\n");
        if (signal_state == INHIBIT) {
            signal_state = UNINHIBIT;
        } else {
            signal_state = INHIBIT;
        }
        break;
    case SIGINT:
    case SIGTERM:
        fprintf(stderr, "SIGINT/SIGTERM received, Killing Matcha...\n");
        signal_state = KILL;
        break;
    default:
        fprintf(stderr, "Unknown signal received\n");
        break;
    }
}

typedef struct {
    bool off_flag;
    bool toggle_mode;
    bool daemon_mode;
    Bar bar;
} Args;

Args parse_args(int argc, char** argv) {

    Bar bar = NONE;
    // use getopt to parse arguments
    struct option long_options[] = {
        {"bar", required_argument, 0, 'b'}, {"daemon", no_argument, 0, 'd'},
        {"help", no_argument, 0, 'h'},      {"toggle", no_argument, 0, 't'},
        {"off", no_argument, 0, 'o'},       {0, 0, 0, 0},
    };
    int option_index = 0;
    int opt;
    bool off_flag = false;
    bool toggle_mode = false;
    bool daemon_mode = false;
    while ((opt = getopt_long(argc, argv, "b:dhto", long_options, &option_index)) != -1) {
        switch (opt) {
        case 'b':
            if (strcmp(optarg, "waybar") == 0) {
                bar = WAYBAR;
            } else if (strcmp(optarg, "yambar") == 0) {
                bar = YAMBAR;
            } else {
                fprintf(stderr, "Invalid mode\n");
                exit(EXIT_FAILURE);
            }
            break;
        case 'd':
            daemon_mode = true;
            break;
        case 'o':
            off_flag = true;
            break;
        case 't':
            toggle_mode = true;
            break;
        case 'h':
        default:
            printf(HELP);

            exit(EXIT_FAILURE);
        }
    }

    if (toggle_mode == daemon_mode) {
        fprintf(stderr, "ERROR: You must specify either --daemon or --toggle\n\n%s", HELP);
        exit(EXIT_FAILURE);
    }
    if (bar == YAMBAR && toggle_mode) {
        fprintf(stderr, "ERROR: Yambar only works on daemon side (--daemon)\n");
        exit(EXIT_FAILURE);
    }
    if (bar == WAYBAR && daemon_mode) {
        fprintf(stderr, "ERROR: Waybar only works on toggle side (--toggle)\n");
        exit(EXIT_FAILURE);
    }
    Args ret = {
        .off_flag = off_flag,
        .daemon_mode = daemon_mode,
        .toggle_mode = toggle_mode,
        .bar = bar,
    };
    return ret;
}

MatchaBackend* init_backend(bool off_flag) {

    MatchaBackend* backend = (MatchaBackend*)calloc(1, sizeof(MatchaBackend));
    if (!backend) {
        fprintf(stderr, "Failed to allocate memory for backend\n");
        return NULL;
    }
    backend->display = wl_display_connect(NULL);
    if (!backend->display) {
        fprintf(stderr, "Failed to connect to Wayland server\n");
        free(backend);
        return NULL;
    }

    backend->registry = wl_display_get_registry(backend->display);
    wl_registry_add_listener(backend->registry, &registry_listener, backend);

    wl_display_roundtrip(backend->display);

    if (!backend->compositor) {
        fprintf(stderr, "Failed to get wl_compositor\n");
        matcha_destroy(backend);
        return NULL;
    }

    backend->surface = wl_compositor_create_surface(backend->compositor);
    if (!backend->surface) {
        fprintf(stderr, "Failed to create Wayland surface\n");
        matcha_destroy(backend);
        return NULL;
    }

    backend->shared_mem = create_shared_mem();
    if (backend->shared_mem == NULL) {
        matcha_destroy(backend);
        return NULL;
    }
    backend->shared_mem->inhibit = !off_flag;

    backend->idle_inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(
        backend->idle_inhibit_manager, backend->surface);
    wl_surface_commit(backend->surface);
    wl_display_roundtrip(backend->display);

    return backend;
}

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);

    if (args.toggle_mode) {
        SharedMem* shm = access_shared_mem();
        shm->inhibit = !shm->inhibit;
        if (args.bar == WAYBAR) {
            // print to waybar the result (i3 style)
            char* waybar_on = getenv("MATCHA_WAYBAR_ON");
            char* waybar_off = getenv("MATCHA_WAYBAR_OFF");
            if (shm->inhibit) {
                printf("%s\n%s\n\n", waybar_on ? waybar_on : "🍵", "Enabled");
            } else {
                printf("%s\n%s\n\n", waybar_off ? waybar_off : "💤", "Disabled");
            }
        }
        sem_post(&shm->sem);
        munmap(shm, sizeof(SharedMem));
        return EXIT_SUCCESS;
    }

    struct sigaction sig_action;

    memset(&sig_action, 0, sizeof(sig_action));
    sig_action.sa_sigaction = signal_handler;
    sig_action.sa_flags = SA_SIGINFO;
    // Setting up the signal handler for SIGUSR1, SIGINT, SIGTERM
    int sig_ret = sigaction(SIGUSR1, &sig_action, NULL);
    sig_ret |= sigaction(SIGINT, &sig_action, NULL);
    sig_ret |= sigaction(SIGTERM, &sig_action, NULL);
    if (sig_ret == -1) {
        fprintf(stderr, "Failed to set up A Signal handler\n");
    }

    MatchaBackend* backend = init_backend(args.off_flag);
    if (backend == NULL)
        return EXIT_FAILURE;
    while (signal_state != KILL) {
        sem_wait(&backend->shared_mem->sem);
        if (!backend->shared_mem->inhibit || signal_state == UNINHIBIT) {
            if (args.bar == YAMBAR) {
                printf("inhibit|bool|false\n\n");
                fflush(stdout);
            }
            if (backend->idle_inhibitor) {
                fprintf(stderr, "Pausing Matcha\n");
                zwp_idle_inhibitor_v1_destroy(backend->idle_inhibitor);
                wl_surface_commit(backend->surface);
                wl_display_roundtrip(backend->display);
                backend->idle_inhibitor = NULL;
            }
        } else {
            if (args.bar == YAMBAR) {
                printf("inhibit|bool|true\n\n");
                fflush(stdout);
            }
            if (!backend->idle_inhibitor) {
                fprintf(stderr, "Starting Matcha\n");
                backend->idle_inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(
                    backend->idle_inhibit_manager, backend->surface);
                wl_surface_commit(backend->surface);
                wl_display_roundtrip(backend->display);
            }
        }
    }
    matcha_destroy(backend);
    return EXIT_SUCCESS;
}
