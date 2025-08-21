#define NO_FIP_LIB
#define FIP_MASTER
#define FIP_IMPLEMENTATION
#include "fip.h"

#include <unistd.h>

int main() {
    interop_modules_t interop_modules = {0};

    // Initialize socket first
    int socket_fd = fip_master_init_socket();
    if (socket_fd == -1) {
        fip_print(0, "Failed to initialize socket, exiting");
        return 1;
    }

    // First parse the config file (fip.toml)
    fip_master_config_t config_file = fip_master_load_config();

    // Only start the fip-c IM if it's enabled in the fip.toml config file
    if (config_file.fip_c_enabled) {
        fip_print(0, "Starting the fip-c module...");
        spawn_interop_module(&interop_modules, "./modules/c/fip-c");
    }

    // Give the fip-c IM time to connect
    fip_print(0, "Waiting for fip-c to connect...");

    // Broadcast a few definitions
    fip_master_broadcast_message(socket_fd, "foo()->i32");
    // fip_master_broadcast_message(socket_fd, "foo(f32)->i32");
    // fip_master_broadcast_message(socket_fd, "foo(f32)");
    // fip_master_broadcast_message(socket_fd, "foo()");
    // fip_master_broadcast_message(socket_fd, "bar(u64,f32)->i32");
    fip_master_broadcast_message(socket_fd, "bar(u64,f32)");

    // Broadcast kill message
    fip_master_broadcast_message(socket_fd, "kill");

    // Clean up
    fip_master_cleanup_socket(socket_fd);
    terminate_all_slaves(&interop_modules); // Fallback cleanup

    fip_print(0, "Master shutting down");
    return 0;
}
