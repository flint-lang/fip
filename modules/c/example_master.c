#define FIP_MASTER
#define FIP_IMPLEMENTATION
#include "fip.h"

fip_log_level_t LOG_LEVEL = FIP_TRACE;
fip_master_state_t master_state = {0};

#include <unistd.h>

int main() {
    fip_interop_modules_t interop_modules = {0};

    // Create a buffer used for sending messages
    char msg_buf[FIP_MSG_SIZE] = {0};

    // First parse the config file (fip.toml)
    fip_master_config_t config_file = fip_master_load_config();
    if (!config_file.ok) {
        goto kill;
    }

    // Create a single message which will be re-used for all messages
    fip_msg_t msg = {0};

    // Start all enabled interop modules
    for (uint8_t i = 0; i < config_file.enabled_count; i++) {
        const char *mod = config_file.enabled_modules[i];
        fip_print(0, FIP_INFO, "Starting the %s module...", mod);
        char module_path[13 + FIP_MAX_MODULE_NAME_LEN] = {0};
        snprintf(module_path, sizeof(module_path), ".fip/modules/%s", mod);
        fip_spawn_interop_module(&interop_modules, module_path);
    }

    // Initialize master with the spawned modules
    if (!fip_master_init(&interop_modules)) {
        fip_print(0, FIP_ERROR, "Failed to initialize master, exiting");
        goto kill;
    }

    // Wait for all connect messages from the IMs
    fip_print(0, FIP_INFO, "Waiting for all connect requests...");
    fip_master_await_responses(       //
        msg_buf,                      //
        master_state.responses,       //
        &master_state.response_count, //
        FIP_MSG_CONNECT_REQUEST       //
    );

    // Check if each interop module has the correct version and whether it's
    // setup was ok
    for (uint8_t i = 0; i < master_state.response_count; i++) {
        const fip_msg_t *response = &master_state.responses[i];
        const fip_msg_connect_request_t *req = &response->u.con_req;
        assert(response->type == FIP_MSG_CONNECT_REQUEST);
        if (req->version.major != FIP_MAJOR    //
            || req->version.minor != FIP_MINOR //
            || req->version.patch != FIP_PATCH //
        ) {
            fip_print(                                             //
                0, FIP_ERROR, "Version mismatch with module '%s'", //
                response->u.con_req.module_name                    //
            );
            fip_print(                                                     //
                0, FIP_ERROR,                                              //
                "  Expected 'v%d.%d.%d' but got 'v%d.%d.%d'",              //
                FIP_MAJOR, FIP_MINOR, FIP_PATCH,                           //
                req->version.major, req->version.minor, req->version.patch //
            );
            goto kill;
        }
        if (!req->setup_ok) {
            fip_print(0, FIP_ERROR, "Module '%s' failed it's setup",
                req->module_name);
            goto kill;
        }
    }

    // Broadcast a few definitions. These definitions are normally not created
    // by hand but gathered in the compiler in a list or something similar, so
    // we actually would not need to to the same as here.
    msg.type = FIP_MSG_SYMBOL_REQUEST;
    msg.u.sym_req.type = FIP_SYM_FUNCTION;
    strcpy(msg.u.sym_req.sig.fn.name, "ClearBackground");
    msg.u.sym_req.sig.fn.rets_len = 0;
    msg.u.sym_req.sig.fn.rets = NULL;
    msg.u.sym_req.sig.fn.args_len = 1;
    msg.u.sym_req.sig.fn.args = malloc(sizeof(fip_type_t));
    msg.u.sym_req.sig.fn.args[0].type = FIP_TYPE_STRUCT;
    msg.u.sym_req.sig.fn.args[0].is_mutable = true;
    msg.u.sym_req.sig.fn.args[0].u.struct_t.field_count = 4;
    msg.u.sym_req.sig.fn.args[0].u.struct_t.fields =
        malloc(sizeof(fip_type_t) * 4);
    msg.u.sym_req.sig.fn.args[0].u.struct_t.fields[0].type = FIP_TYPE_PRIMITIVE;
    msg.u.sym_req.sig.fn.args[0].u.struct_t.fields[0].is_mutable = true;
    msg.u.sym_req.sig.fn.args[0].u.struct_t.fields[0].u.prim = FIP_U8;
    msg.u.sym_req.sig.fn.args[0].u.struct_t.fields[1].type = FIP_TYPE_PRIMITIVE;
    msg.u.sym_req.sig.fn.args[0].u.struct_t.fields[1].is_mutable = true;
    msg.u.sym_req.sig.fn.args[0].u.struct_t.fields[1].u.prim = FIP_U8;
    msg.u.sym_req.sig.fn.args[0].u.struct_t.fields[2].type = FIP_TYPE_PRIMITIVE;
    msg.u.sym_req.sig.fn.args[0].u.struct_t.fields[2].is_mutable = true;
    msg.u.sym_req.sig.fn.args[0].u.struct_t.fields[2].u.prim = FIP_U8;
    msg.u.sym_req.sig.fn.args[0].u.struct_t.fields[3].type = FIP_TYPE_PRIMITIVE;
    msg.u.sym_req.sig.fn.args[0].u.struct_t.fields[3].is_mutable = true;
    msg.u.sym_req.sig.fn.args[0].u.struct_t.fields[3].u.prim = FIP_U8;
    // "ClearBackground(Color)"
    if (!fip_master_symbol_request(msg_buf, &msg)) {
        fip_print(0, FIP_INFO, "Goto kill");
        goto kill;
    }

    // If we came here all definitions have been found in one of the interop
    // modules. This means that we now can request all modules to compile their
    // files and give us back the .o files as the responses
    fip_free_msg(&msg);
    msg.type = FIP_MSG_COMPILE_REQUEST;
    if (!fip_master_compile_request(msg_buf, &msg)) {
        fip_print(0, FIP_INFO, "Goto kill");
        goto kill;
    }

kill:
    // Broadcast kill message
    fip_free_msg(&msg);
    msg.type = FIP_MSG_KILL;
    msg.u.kill.reason = FIP_KILL_FINISH;
    fip_master_broadcast_message(msg_buf, &msg);

    // Clean up
    nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 100000000}, NULL);
    fip_master_cleanup();
    fip_terminate_all_slaves(&interop_modules); // Fallback cleanup

    fip_print(0, FIP_INFO, "Master shutting down");
    return 0;
}
