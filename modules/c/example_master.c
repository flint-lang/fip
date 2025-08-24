#define NO_FIP_LIB
#define FIP_MASTER
#define FIP_IMPLEMENTATION
#include "fip.h"

#include <unistd.h>

int main() {
    fip_interop_modules_t interop_modules = {0};

    // Initialize socket first
    int socket_fd = fip_master_init_socket();
    if (socket_fd == -1) {
        fip_print(0, "Failed to initialize socket, exiting");
        return 1;
    }

    // Create a buffer used for sending messages
    char msg_buf[FIP_MSG_SIZE] = {0};

    // TODO: Change it to dynamically discovering the interop modules from the
    // `.fip/modules` path

    // First parse the config file (fip.toml)
    fip_master_config_t config_file = fip_master_load_config();

    // Create a single message which will be re-used for all messages
    fip_msg_t msg = {0};

    // Start all enabled interop modules
    for (uint8_t i = 0; i < config_file.enabled_count; i++) {
        const char *mod = config_file.enabled_modules[i];
        fip_print(0, "Starting the %s module...", mod);
        char module_path[13 + FIP_MAX_MODULE_NAME_LEN] = {0};
        snprintf(module_path, sizeof(module_path), ".fip/modules/%s", mod);
        fip_spawn_interop_module(&interop_modules, module_path);
    }

    // Give the fip-c IM time to connect
    fip_print(0, "Waiting for fip-c to connect...");

    // Broadcast a few definitions. These definitions are normally not created
    // by hand but gathered in the compiler in a list or something similar, so
    // we actually would not need to to the same as here.
    msg.type = FIP_MSG_SYMBOL_REQUEST;
    msg.u.sym_req.type = FIP_SYM_FUNCTION;
    strncpy(msg.u.sym_req.sig.fn.name, "foo", 3);
    msg.u.sym_req.sig.fn.rets_len = 1;
    msg.u.sym_req.sig.fn.rets = malloc(sizeof(fip_sig_type_t));
    msg.u.sym_req.sig.fn.rets[0].is_mutable = false;
    msg.u.sym_req.sig.fn.rets[0].type = FIP_I32;
    // "foo()->i32"
    nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 10000000}, NULL);
    if (!fip_master_symbol_request(msg_buf, &msg)) {
        fip_print(0, "Goto kill");
        goto kill;
    }

    strncpy(msg.u.sym_req.sig.fn.name, "bar", 3);
    msg.u.sym_req.sig.fn.args_len = 2;
    msg.u.sym_req.sig.fn.args = malloc(sizeof(fip_sig_type_t) * 2);
    msg.u.sym_req.sig.fn.args[0].is_mutable = false;
    msg.u.sym_req.sig.fn.args[0].type = FIP_I32;
    msg.u.sym_req.sig.fn.args[1].is_mutable = false;
    msg.u.sym_req.sig.fn.args[1].type = FIP_I32;
    // "bar(i32,i32)->i32"
    nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 10000000}, NULL);
    if (!fip_master_symbol_request(msg_buf, &msg)) {
        fip_print(0, "Goto kill");
        goto kill;
    }

    // If we came here all definitions have been found in one of the interop
    // modules. This means that we now can request all modules to compile their
    // files and give us back the .o files as the responses
    fip_free_msg(&msg);
    msg.type = FIP_MSG_COMPILE_REQUEST;
    if (!fip_master_compile_request(msg_buf, &msg)) {
        fip_print(0, "Goto kill");
        goto kill;
    }

kill:
    // Broadcast kill message
    fip_free_msg(&msg);
    msg.type = FIP_MSG_KILL;
    msg.u.kill.reason = FIP_KILL_FINISH;
    nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 10000000}, NULL);
    fip_master_broadcast_message(socket_fd, msg_buf, &msg);

    // Clean up
    nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 100000000}, NULL);
    fip_master_cleanup_socket(socket_fd);
    fip_terminate_all_slaves(&interop_modules); // Fallback cleanup

    fip_print(0, "Master shutting down");
    return 0;
}
