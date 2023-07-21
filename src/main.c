#include "idle-inhibit-unstable-v1.h"

#include <fcntl.h>
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
        printf("Found A Compositor\n");
        backend->compositor = wl_registry_bind(registry, id, &wl_compositor_interface, version);
    }
    // inhibit manager
    else if (strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name) == 0) {
        printf("Found An Inhibitor Manager\n");
        backend->idle_inhibit_manager =
            wl_registry_bind(registry, id, &zwp_idle_inhibit_manager_v1_interface, version);
    }
}

static void global_registry_remover(void* data, struct wl_registry* registry, uint32_t id) {
    // do nothing
    printf("Removing %d\n", id);
    (void)data;
    (void)registry;
    (void)id;
}

static const struct wl_registry_listener registry_listener = {global_registry_handler,
                                                              global_registry_remover};

// create shared memory
static bool* create_shared_mem() {
    int shm_fd = shm_open(SHARED_MEM_NAME, O_CREAT | O_RDWR, 0660);
    if (shm_fd == -1) {
        perror("Failed to create shared memory");
        exit(1);
    }
    if (ftruncate(shm_fd, sizeof(bool)) == -1) {
        perror("Failed to truncate shared memory");
        exit(1);
    }
    bool* data = (bool*)mmap(0, sizeof(bool), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (data == MAP_FAILED) {
        perror("Failed to map shared memory");
        exit(1);
    }
    close(shm_fd);
    *data = true;
    return data;
}

static bool* access_shared_mem() {
    int shm_fd = shm_open(SHARED_MEM_NAME, O_RDWR, 0660);
    if (shm_fd == -1) {
        perror("Failed to access shared memory");
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

int main(int argc, char** argv) {
    // toggle is used for toggling the state of the inhibitor
    if (argc == 2 && strcmp(argv[1], "--toggle") == 0) {
        bool* inhibit = access_shared_mem();
        *inhibit = !*inhibit;
        return EXIT_SUCCESS;
    } else if (argc != 1) {
        fprintf(stderr, "Usage: %s [--toggle]\n", argv[0]);
        return EXIT_FAILURE;
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

    while (1) {
        if (!*backend->inhibit) {
            if (backend->idle_inhibitor) {
                printf("Pausing Matcha\n");
                zwp_idle_inhibitor_v1_destroy(backend->idle_inhibitor);
                wl_surface_commit(backend->surface);
                wl_display_roundtrip(backend->display);
                backend->idle_inhibitor = NULL;
            }
        } else {
            if (!backend->idle_inhibitor) {
                printf("Starting Matcha\n");
                backend->idle_inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(
                    backend->idle_inhibit_manager, backend->surface);
                wl_surface_commit(backend->surface);
                wl_display_roundtrip(backend->display);
            }
        }
        sleep(1);
    }

    matcha_destroy(backend);
    return EXIT_SUCCESS;
}
