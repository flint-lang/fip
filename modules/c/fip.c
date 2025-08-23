#define NO_FIP_LIB
#define FIP_SLAVE
#define FIP_IMPLEMENTATION
#include "fip.h"

#include <assert.h>
#include <stdlib.h>
#include <time.h>

/*
 * ==============================================
 * THIS IS THE `fip-c` INTEROP MODULE SOURCE CODE
 * ==============================================
 * The `fip-c` Interop Module (IM) is used to
 * detect annotated function definitions from C
 * source files, which then compiles those source
 * files and provides the `.o` file of the
 * compiled C source files which contain the
 * symbols the FIP requested. This FIP module is
 * targetted at only compiling / finding C
 * definitions, not the definitions of any other
 * language.
 * ==============================================
 */

uint32_t ID;
int SOCKET_FD;
fip_slave_config_t CONFIG;

const char *c_type_names[] = {
    "unsigned char",
    "unsigned short",
    "unsigned int",
    "unsigned long",
    "char",
    "short",
    "int",
    "long",
    "float",
    "double",
    "bool",
    "char *",
};

typedef struct {
    char source_file_path[512];
    int line_number;
    fip_msg_symbol_type_t type;
    union {
        fip_sig_fn_t fn;
    } signature;
} fip_c_symbol_t;

#define MAX_SYMBOLS 100
#define MODULE_NAME "fip-c"

fip_c_symbol_t symbols[MAX_SYMBOLS]; // Simple array for now
int symbol_count = 0;

bool parse_fip_function_line( //
    const char *line,         //
    const char *file_path,    //
    int line_num              //
) {
    // Exmaple input: "int FIP_FN foo(int x, float y) {"
    // We need to extract: return_type="int", name="foo", parameters="int,float"
    fip_c_symbol_t symbol = {0};
    strcpy(symbol.source_file_path, file_path);
    symbol.line_number = line_num;
    symbol.type = FIP_SYM_FUNCTION;
    fip_sig_fn_t fn_sig = {0};

    // Remove leading whitespaces
    char const *start = line;
    while (*start == ' ' || *start == '\t') {
        start++;
    }

    // Find FIP_FN
    char *fip_fn_pos = strstr(start, "FIP_FN");
    if (!fip_fn_pos) {
        return false;
    }
    char *func_start = fip_fn_pos + 6; // Skip "FIP_FN"

    // Remove all whitespaces from the return type
    fip_fn_pos--;
    while (*fip_fn_pos == ' ' || *fip_fn_pos == '\t') {
        fip_fn_pos--;
    }
    size_t ret_type_len = fip_fn_pos - start + 1;
    // Extract return type(s) (everything before FIP_FN)
    if (!fip_parse_type_string(ID, start, c_type_names,      //
            0, ret_type_len, &fn_sig.rets, &fn_sig.rets_len) //
    ) {
        fip_print(ID, "Could not parse return types of '%s'", line);
        return false;
    }

    // Find function name
    while (*func_start == ' ' || *func_start == '\t') {
        func_start++;
    }
    char *paren_open = strchr(func_start, '(');
    char *paren_close = strchr(func_start, ')');
    if (!paren_open || !paren_close) {
        fip_print(ID, "Invalid function syntax on line %d", line_num);
        return false;
    }

    // Store function name
    memcpy(fn_sig.name, func_start, paren_open - func_start);

    // Extract all parameter types one by one
    start = paren_open + 1;
    uint32_t type_len = 0;

    // For now everything until the first space is the first parameter and then
    // everything until , or ) is considered a non-type
    bool is_at_end = paren_open + 1 == paren_close;
    while (!is_at_end) {
        switch (*(start + type_len)) {
            case ',':
            case ')': {
                const bool is_r_paren = *(start + type_len) == ')';
                // First we decrement the type_len until we reach the first
                // non-alphanumeric character
                // Everything until there is the type
                uint32_t comma_idx = type_len;
                type_len--;
                char c = *(start + type_len);
                while (is_alpha_num(c)) {
                    type_len--;
                    c = *(start + type_len);
                }
                if (!fip_parse_type_string(ID, start, c_type_names,  //
                        0, type_len, &fn_sig.args, &fn_sig.args_len) //
                ) {
                    fip_print(ID, "Could not parse arg types of '%s'", line);
                    return false;
                }
                start = start + comma_idx + 1;
                type_len = 0;

                if (is_r_paren) {
                    is_at_end = true;
                }
                break;
            }
            default:
                type_len++;
                break;
        }
    }

    symbol.signature.fn = fn_sig;
    symbols[symbol_count] = symbol;
    return true;
}

bool scan_c_file_for_fip_exports(const char *file_path) {
    FILE *file = fopen(file_path, "r");
    if (!file) {
        fip_print(ID, "Could not open %s", file_path);
        return false;
    }

    char line[1024];
    int line_num = 0;

    fip_print(ID, "Scanning %s for FIP_FN functions...", file_path);

    while (fgets(line, sizeof(line), file) && symbol_count < MAX_SYMBOLS) {
        line_num++;

        // Look for FIP_FN in the line
        if (strstr(line, "FIP_FN")) {
            if (parse_fip_function_line(line, file_path, line_num)) {
                fip_print(ID, "Found: '%s' at",
                    symbols[symbol_count].signature.fn.name);
                fip_print(                                                //
                    ID, "  LineNo: %u", symbols[symbol_count].line_number //
                );
                fip_print(ID, "  Source: '%s'",            //
                    symbols[symbol_count].source_file_path //
                );
                fip_print_sig_fn(ID, &(symbols[symbol_count].signature.fn));
                symbol_count++;
            }
        }
    }

    fclose(file);
    fip_print(ID, "Found %d FIP functions in %s", symbol_count, file_path);
    return symbol_count > 0;
}

void handle_symbol_request(    //
    char buffer[FIP_MSG_SIZE], //
    const fip_msg_t *message   //
) {
    assert(message->u.sym_req.type == FIP_SYM_FUNCTION);
    fip_print(ID, "Requested Function");
    const fip_sig_fn_t *msg_fn = &message->u.sym_req.sig.fn;
    fip_print_sig_fn(ID, msg_fn);
    fip_msg_t response = {0};
    response.type = FIP_MSG_SYMBOL_RESPONSE;
    fip_msg_symbol_response_t *sym_res = &response.u.sym_res;
    memcpy(sym_res->module_name, MODULE_NAME, sizeof(MODULE_NAME));
    sym_res->type = FIP_SYM_FUNCTION;

    bool sym_match = false;
    for (uint32_t i = 0; i < symbol_count; i++) {
        const fip_sig_fn_t *sym_fn = &symbols[i].signature.fn;
        if (strcmp(sym_fn->name, msg_fn->name) == 0 //
            && sym_fn->args_len == msg_fn->args_len //
            && sym_fn->rets_len == msg_fn->rets_len //
        ) {
            sym_match = true;
            // Now we need to check if the arg and ret types match
            for (uint32_t j = 0; j < sym_fn->args_len; j++) {
                if (sym_fn->args[j].type != msg_fn->args[j].type ||
                    sym_fn->args[j].is_mutable != msg_fn->args[j].is_mutable) {
                    sym_match = false;
                }
            }
            for (uint32_t j = 0; j < sym_fn->rets_len; j++) {
                if (sym_fn->rets[j].type != msg_fn->rets[j].type ||
                    sym_fn->rets[j].is_mutable != msg_fn->rets[j].is_mutable) {
                    sym_match = false;
                }
            }
            if (sym_match) {
                // We found the requested symbol
                fip_clone_sig_fn(&sym_res->sig.fn, sym_fn);
                break;
            }
        }
    }
    sym_res->found = sym_match;
    fip_slave_send_message(ID, SOCKET_FD, buffer, &response);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("The `fip-c` Interop Module needs at least one argument\n");
        printf("The argument must be the ID of the Interop Module\n");
        return 1;
    }
    char *id_str = argv[1];
    char *endptr;
    ID = (uint32_t)strtoul(id_str, &endptr, 10);
    fip_print(ID, "starting...");

    char msg_buf[1024] = {0};

    // Connect to master's socket
    SOCKET_FD = fip_slave_init_socket();
    if (SOCKET_FD == -1) {
        fip_print(ID, "Failed to connect to master socket");
        return 1;
    }
    fip_print(ID, "Connected to master, waiting for messages...");

    // Parse the toml file for this module.
    CONFIG = fip_slave_load_config(ID, C);
    fip_print(ID, "Parsed fip-c.toml file");
    assert(CONFIG.type == C);

    // Print all sources and all compile flags
    for (uint32_t i = 0; i < CONFIG.u.c.compile_flags_len; i++) {
        fip_print(ID, "compile_flags[%u]: %s", i, CONFIG.u.c.compile_flags[i]);
    }
    for (uint32_t i = 0; i < CONFIG.u.c.sources_len; i++) {
        fip_print(ID, "source[%u]: %s", i, CONFIG.u.c.sources[i]);
        scan_c_file_for_fip_exports(CONFIG.u.c.sources[i]);
    }

    // Main loop - wait for messages from master
    bool is_running = true;
    while (is_running) {
        struct timespec start_time;
        clock_gettime(CLOCK_MONOTONIC, &start_time);

        if (fip_slave_receive_message(SOCKET_FD, msg_buf)) {
            // Only print the first time we receive a message
            fip_print(ID, "Received message");
            fip_msg_t message = {0};
            fip_decode_msg(msg_buf, &message);

            switch (message.type) {
                case FIP_MSG_UNKNOWN:
                    fip_print(ID, "Recieved unknown message");
                    break;
                case FIP_MSG_CONNECT_REQUEST:
                    fip_print(ID, "Connect Request");
                    break;
                case FIP_MSG_SYMBOL_REQUEST:
                    fip_print(ID, "Symbol Request");
                    handle_symbol_request(msg_buf, &message);
                    break;
                case FIP_MSG_SYMBOL_RESPONSE:
                    // The slave should not recieve a message it sends
                    assert(false);
                    break;
                case FIP_MSG_COMPILE_REQUEST:
                    fip_print(ID, "Compile Request");
                    break;
                case FIP_MSG_OBJECT_RESPONSE:
                    // The slave should not recieve a message it sends
                    assert(false);
                    break;
                case FIP_MSG_KILL:
                    fip_print(ID, "Kill Command, shutting down");
                    is_running = false;
                    break;
            }
        }

        // Calculate elapsed time and adjust sleep to maintain 1ms intervals
        struct timespec end_time;
        clock_gettime(CLOCK_MONOTONIC, &end_time);
        long elapsed_ns = (end_time.tv_sec - start_time.tv_sec) * 1000000000 +
            (end_time.tv_nsec - start_time.tv_nsec);

        long remaining_ns = FIP_SLAVE_DELAY - elapsed_ns;
        if (remaining_ns > 0) {
            nanosleep(                                                    //
                &(struct timespec){.tv_sec = 0, .tv_nsec = remaining_ns}, //
                NULL                                                      //
            );
        }
    }

    fip_slave_cleanup_socket(SOCKET_FD);
    fip_print(ID, "ending...");
    return 0;
}
