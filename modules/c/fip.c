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
    bool needed;
    char source_file_path[512];
    int line_number;
    fip_msg_symbol_type_t type;
    union {
        fip_sig_fn_t fn;
    } signature;
} fip_c_symbol_t;

typedef struct {
    char compiler[128];
    uint32_t sources_len;
    char **sources;
    uint32_t compile_flags_len;
    char **compile_flags;
} fip_module_config_t;

#define MAX_SYMBOLS 100
#define MODULE_NAME "fip-c"

fip_c_symbol_t symbols[MAX_SYMBOLS]; // Simple array for now
uint32_t symbol_count = 0;
uint32_t ID;
int SOCKET_FD;
fip_module_config_t CONFIG;

bool parse_toml_file(toml_result_t toml) {
    // === COMPILER ===
    toml_datum_t compiler_d = toml_seek(toml.toptab, "compiler");
    if (compiler_d.type != TOML_STRING) {
        fip_print(ID, "Missing 'compiler' field");
        return false;
    }
    const int compiler_len = compiler_d.u.str.len;
    const char *compiler_ptr = compiler_d.u.str.ptr;
    memcpy(CONFIG.compiler, compiler_ptr, compiler_len);

    // === SOURCES ===
    toml_datum_t sources_d = toml_seek(toml.toptab, "sources");
    if (sources_d.type != TOML_ARRAY) {
        fip_print(ID, "Missing 'sources' field");
        return false;
    }
    int32_t arr_len = sources_d.u.arr.size;
    toml_datum_t *sources_elems_d = sources_d.u.arr.elem;
    CONFIG.sources = (char **)malloc(sizeof(char *) * arr_len);
    CONFIG.sources_len = arr_len;
    for (int32_t i = 0; i < arr_len; i++) {
        if (sources_elems_d[i].type != TOML_STRING) {
            fip_print(ID, "'sources' does contain a non-string value");
            return false;
        }
        int32_t strlen = sources_elems_d[i].u.str.len;
        CONFIG.sources[i] = (char *)malloc(strlen + 1);
        memcpy(                           //
            CONFIG.sources[i],            //
            sources_elems_d[i].u.str.ptr, //
            strlen                        //
        );
        CONFIG.sources[i][strlen] = '\0';
    }

    // === COMPILE_FLAGS ===
    toml_datum_t compile_flags_d = toml_seek(toml.toptab, "compile_flags");
    if (compile_flags_d.type != TOML_ARRAY) {
        fip_print(ID, "Missing 'compile_flags' field");
        return false;
    }
    arr_len = compile_flags_d.u.arr.size;
    toml_datum_t *compile_flags_elems_d = compile_flags_d.u.arr.elem;
    CONFIG.compile_flags = (char **)malloc(sizeof(char *) * arr_len);
    CONFIG.compile_flags_len = arr_len;
    for (int32_t i = 0; i < arr_len; i++) {
        if (compile_flags_elems_d[i].type != TOML_STRING) {
            fip_print(ID, "'compile_flags' does contain a non-string value");
            return false;
        }
        int32_t strlen = compile_flags_elems_d[i].u.str.len;
        CONFIG.compile_flags[i] = (char *)malloc(strlen + 1);
        memcpy(                                 //
            CONFIG.compile_flags[i],            //
            compile_flags_elems_d[i].u.str.ptr, //
            strlen                              //
        );
        CONFIG.compile_flags[i][strlen] = '\0';
    }
    return true;
}

bool parse_fip_function_line( //
    const char *line,         //
    const char *file_path,    //
    int line_num              //
) {
    // Exmaple input: "int FIP_FN foo(int x, float y) {"
    // We need to extract: return_type="int", name="foo", parameters="int,float"
    fip_c_symbol_t symbol = {0};
    strcpy(symbol.source_file_path, file_path);
    symbol.needed = false;
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
                symbols[i].needed = true;
                fip_clone_sig_fn(&sym_res->sig.fn, sym_fn);
                snprintf(sym_res->sig.fn.name, 128, "__fip_c_%s", sym_fn->name);
                break;
            }
        }
    }
    sym_res->found = sym_match;
    fip_slave_send_message(ID, SOCKET_FD, buffer, &response);
}

bool compile_file(                                    //
    char paths[FIP_PATHS_SIZE],                       //
    const char *file_path,                            //
    [[maybe_unused]] const fip_msg_t *compile_message //
) {
    // TODO: Use the target information from the compile_message
    //
    // First we need to calculate a hash of the source file's path and then the
    // compiled object will be stored in the `.fip/cache/` directory as
    // `HASH.o`. This way each "file path" is not the absolute path to the
    // object file but a simple hash instead, making it predictable how many
    // file paths / hashes can fit inside the paths array!
    //
    // We need to know the hash before even compiling the file at all
    char hash[9] = {0};
    fip_create_hash(hash, file_path);
    hash[8] = '\0'; // This is only needed for the below strstr function

    // Check if the hash is already part of the paths, if it is we already
    // compiled the file
    if (strstr(paths, hash)) {
        fip_print(ID, "Hash '%.8s' already part of paths", hash);
        return true;
    }

    // Add all mangling definitions to prevent symbol collisions
    char defines[1024] = {0};
    strcat(defines, "-Dmain=__fip_c_main ");
    for (uint32_t i = 0; i < symbol_count; i++) {
        if (symbols[i].needed) {
            char define_flag[64];
            snprintf(define_flag, sizeof(define_flag), " -D%s=__fip_c_%s",
                symbols[i].signature.fn.name, symbols[i].signature.fn.name);
            strcat(defines, define_flag);
        }
    }

    // Build compile command with flags
    char compile_flags[1024] = {0};
    // Add all compile flags
    for (uint32_t i = 0; i < CONFIG.compile_flags_len; i++) {
        strcat(compile_flags, CONFIG.compile_flags[i]);
        if (i + 1 < CONFIG.compile_flags_len) {
            strcat(compile_flags, " ");
        }
    }
    // Add source and output
    char temp[256];
    char output_path[64] = {0};
    snprintf(output_path, sizeof(output_path), ".fip/cache/%.8s.o", hash);
    snprintf(temp, sizeof(temp), "%s -o %s", file_path, output_path);

    char compile_cmd[1024] = {0};
    snprintf(compile_cmd, sizeof(compile_cmd), "%s -c %s %s %s -o %s",
        CONFIG.compiler, compile_flags, defines, file_path, output_path);
    strcat(compile_cmd, temp);

    fip_print(ID, "Executing: %s", compile_cmd);

    if (system(compile_cmd) == 0) {
        fip_print(ID, "Compiled '%.8s' successfully", hash);
        // Add to paths array. For this we need to find the first null-byte
        // character in the paths array, that's where we will place our hash at.
        // The good thing is that we only need to check multiples of 8 so this
        // check is rather easy.
        char *dest = paths;
        uint16_t occupied_bytes = 0;
        while (*dest != '\0' && occupied_bytes < FIP_PATHS_SIZE) {
            dest += 8;
            occupied_bytes += 8;
        }
        if (occupied_bytes >= FIP_PATHS_SIZE) {
            fip_print(ID, "The Paths array is full: %.*s", FIP_PATHS_SIZE,
                paths);
            fip_print(ID, "Could not store hash '%s' in it", hash);
            return false;
        }
        memcpy(dest, hash, 8);
        return true;
    }

    return false;
}

void handle_compile_request(   //
    char buffer[FIP_MSG_SIZE], //
    const fip_msg_t *message   //
) {
    assert(message->type == FIP_MSG_COMPILE_REQUEST);
    fip_print(ID, "Handle Compile Request");
    fip_msg_t response = {0};
    response.type = FIP_MSG_OBJECT_RESPONSE;
    fip_msg_object_response_t *obj_res = &response.u.obj_res;
    obj_res->has_obj = false;
    memcpy(obj_res->module_name, MODULE_NAME, sizeof(MODULE_NAME));

    // We need to go through all symbols and see whether they need to be
    // compiled
    for (uint8_t i = 0; i < symbol_count; i++) {
        if (symbols[i].needed) {
            if (!compile_file(                                            //
                    obj_res->paths, symbols[i].source_file_path, message) //
            ) {
                obj_res->has_obj = false;
                break;
            }
            obj_res->has_obj = true;
        }
    }

    fip_slave_send_message(ID, SOCKET_FD, buffer, &response);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("The '%s' Interop Module needs at least one argument\n",
            MODULE_NAME);
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
    toml_result_t toml = fip_slave_load_config(ID, MODULE_NAME);
    if (!parse_toml_file(toml)) {
        fip_print(ID, "The %s.toml file could not be parsed", MODULE_NAME);
        toml_free(toml);
        goto kill;
    }
    toml_free(toml);
    if (CONFIG.sources_len == 0) {
        fip_print(ID, "The '%s' module does not have any sources declared",
            MODULE_NAME);
        goto kill;
    }
    fip_print(ID, "Parsed %s.toml file", MODULE_NAME);

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
                    handle_compile_request(msg_buf, &message);
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
