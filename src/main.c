#include "idle-inhibit-unstable-v1.h"

#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

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
    bool* inhibit;
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
    free(backend);
}

static void global_registry_handler(void* data, struct wl_registry* registry, uint32_t id,
                                    const char* interface, uint32_t version) {
    MatchaBackend* backend = (MatchaBackend*)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        fprintf(stderr, "Found A Compositor\n");
        backend->compositor = wl_registry_bind(registry, id, &wl_compositor_interface, version);
    }
    // inhibit manager
    else if (strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name) == 0) {
        fprintf(stderr, "Found An Inhibitor Manager\n");
        backend->idle_inhibit_manager =
            wl_registry_bind(registry, id, &zwp_idle_inhibit_manager_v1_interface, version);
    }
}

static const struct wl_registry_listener registry_listener = {global_registry_handler, NULL};

// create shared memory
static bool* create_shared_mem() {
    int shm_fd = shm_open(SHARED_MEM_NAME, O_CREAT | O_RDWR | O_EXCL, 0660);
    if (shm_fd == -1) {
        perror("Failed to initialize Matcha, Other instances might be running\nERR");
        signal_state = KILL;
        return NULL;
    }
    if (ftruncate(shm_fd, sizeof(bool)) == -1) {
        close(shm_fd);
        perror("Failed to truncate shared memory");
        signal_state = KILL;
        return NULL;
    }
    bool* data = (bool*)mmap(0, sizeof(bool), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (data == MAP_FAILED) {
        close(shm_fd);
        perror("Failed to map shared memory");
        signal_state = KILL;
        return NULL;
    }
    close(shm_fd);
    *data = true;
    return data;
}

static bool* access_shared_mem() {
    int shm_fd = shm_open(SHARED_MEM_NAME, O_RDWR, 0660);
    if (shm_fd == -1) {
        fprintf(stderr, "Failed to attach to the main process, make sure matcha is running\n");
        exit(1);
    }

    bool* data = (bool*)mmap(0, sizeof(bool), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (data == MAP_FAILED) {
        perror("Failed to map shared memory");
        exit(1);
    }
    close(shm_fd);
    return data;
}

// destroy shared memory
static void destroy_shared_mem() {
    if (shm_unlink(SHARED_MEM_NAME) == -1) {
        perror("Failed to unlink shared memory");
        exit(1);
    }
}

// signal handler
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

int main(int argc, char** argv) {
    // check if we are in waybar or yambar mode
    Bar bar = NONE;
    // use getopt to parse arguments
    struct option long_options[] = {
        {"bar", required_argument, 0, 'b'},
        {"help", no_argument, 0, 'h'},
        {"toggle", no_argument, 0, 't'},
        {"off", no_argument, 0, 'o'},
        {0, 0, 0, 0},
    };
    int option_index = 0;
    int opt;
    bool off_flag = false;
    bool toggle_flag = false;
    while ((opt = getopt_long(argc, argv, "mht:", long_options, &option_index)) != -1) {
        switch (opt) {
        case 'b':
            if (strcmp(optarg, "waybar") == 0) {
                bar = WAYBAR;
            } else if (strcmp(optarg, "yambar") == 0) {
                bar = YAMBAR;
            } else {
                fprintf(stderr, "Invalid mode\n");
                return EXIT_FAILURE;
            }
            break;
        case 'o':
            off_flag = true;
            break;
        case 't':
            toggle_flag = true;
            break;
        case 'h':
        default:
            printf("Usage: matcha [OPTION]...\n");
            printf("Options:\n");
            printf("  -m, --bar=[BAR]  Set the bar type to bar (default: None)\n");
            printf("  -o, --off        Run main instance with inhibitor disabled\n");
            printf("  -t, --toggle     Toggle the inhibit state\n");
            printf("  -h, --help       Display this help and exit\n");
            printf("\nBAR: \n"
                   "    Yambar - Only works on main instance\n"
                   "    Waybar - Only works on toggle instance\n");
            return EXIT_SUCCESS;
        }
    }
    if (toggle_flag) {
        bool* inhibit = access_shared_mem();
        *inhibit = !*inhibit;
        if (bar == WAYBAR) {
            // print to waybar the result (i3 style)
            char* waybar_on = getenv("MATCHA_WAYBAR_ON");
            char* waybar_off = getenv("MATCHA_WAYBAR_OFF");
            if (*inhibit) {
                printf("%s\n%s\n\n", waybar_on ? waybar_on : "🍵", *inhibit ? "Enabled" : "Disabled");
            } else {
                printf("%s\n%s\n\n", waybar_off ? waybar_off : "💤", *inhibit ? "Enabled" : "Disabled");
            }
        }
        return EXIT_SUCCESS;
    }

    struct sigaction sig_action;

    memset(&sig_action, 0, sizeof(sig_action));
    sig_action.sa_sigaction = signal_handler;
    sig_action.sa_flags = SA_SIGINFO;
    // Setting up the signal handler for SIGUSR1
    int sig_ret = sigaction(SIGUSR1, &sig_action, NULL);
    sig_ret |= sigaction(SIGINT, &sig_action, NULL);
    sig_ret |= sigaction(SIGTERM, &sig_action, NULL);
    if (sig_ret == -1) {
        fprintf(stderr, "Failed to set up A Signal handler\n");
    }

    MatchaBackend* backend = (MatchaBackend*)malloc(sizeof(MatchaBackend));
    if (!backend) {
        fprintf(stderr, "Failed to allocate memory for backend\n");
        return EXIT_FAILURE;
    }
    backend->display = wl_display_connect(NULL);
    if (!backend->display) {
        fprintf(stderr, "Failed to connect to Wayland server\n");
        free(backend);
        return EXIT_FAILURE;
    }

    backend->registry = wl_display_get_registry(backend->display);
    wl_registry_add_listener(backend->registry, &registry_listener, backend);

    // Roundtrip to ensure we processed all events and got the compositor.
    wl_display_roundtrip(backend->display);

    if (!backend->compositor) {
        fprintf(stderr, "Failed to get wl_compositor\n");
        matcha_destroy(backend);
        return EXIT_FAILURE;
    }

    backend->surface = wl_compositor_create_surface(backend->compositor);
    if (!backend->surface) {
        fprintf(stderr, "Failed to create Wayland surface\n");
        matcha_destroy(backend);
        return EXIT_FAILURE;
    }

    backend->inhibit = create_shared_mem();
    if (backend->inhibit == NULL) {
        matcha_destroy(backend);
        return EXIT_SUCCESS;
    }
    if (off_flag) {
        *backend->inhibit = false;
    }
    // create a new inhibitor
    backend->idle_inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(
        backend->idle_inhibit_manager, backend->surface);
    wl_surface_commit(backend->surface);
    wl_display_roundtrip(backend->display);

    while (signal_state != KILL) {
        if (!*backend->inhibit || signal_state == UNINHIBIT) {
            if (bar == YAMBAR) {
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
            if (bar == YAMBAR) {
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
        // sleep for 1 second
        usleep(500000);
    }

    matcha_destroy(backend);
    destroy_shared_mem();
    return EXIT_SUCCESS;
}
