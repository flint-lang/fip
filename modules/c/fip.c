#include <assert.h>
#define NO_FIP_LIB
#define FIP_SLAVE
#define FIP_IMPLEMENTATION
#include "fip.h"

#include <stdlib.h>

unsigned int ID;

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

typedef enum {
    FUNCTION,
} fip_c_symbol_kind_t;

typedef struct {
    char source_file_path[512];
    int line_number;
    fip_c_symbol_kind_t kind;
    union {
        fip_fn_sig_t fn;
    } signature;
} fip_c_symbol_t;

#define MAX_SYMBOLS 100

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
    symbol.kind = FUNCTION;
    fip_fn_sig_t fn_sig = {0};

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

    // Extract function name
    size_t name_len = paren_open - func_start;
    fn_sig.name = malloc(name_len + 1);
    strncpy(fn_sig.name, func_start, name_len);
    fn_sig.name[name_len] = '\0';

    // Extract all parameter types one by one
    start = paren_open + 1;
    unsigned int type_len = 0;

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
                unsigned int comma_idx = type_len;
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
                fip_print(ID, "  Found: '%s' at",
                    symbols[symbol_count].signature.fn.name);
                fip_print(                                                //
                    ID, "  LineNo: %u", symbols[symbol_count].line_number //
                );
                fip_print(ID, "  Source: '%s'",            //
                    symbols[symbol_count].source_file_path //
                );
                fip_print_fn_sig(ID, &(symbols[symbol_count].signature.fn));
                symbol_count++;
            }
        }
    }

    fclose(file);
    fip_print(ID, "Found %d FIP functions in %s", symbol_count, file_path);
    return symbol_count > 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("The `fip-c` Interop Module needs at least one argument\n");
        printf("The argument must be the ID of the Interop Module\n");
        return 1;
    }
    char *id_str = argv[1];
    char *endptr;
    ID = (unsigned int)strtoul(id_str, &endptr, 10);
    fip_print(ID, "starting...");

    // Connect to master's socket
    int socket_fd = fip_slave_init_socket();
    if (socket_fd == -1) {
        fip_print(ID, "Failed to connect to master socket");
        return 1;
    }
    fip_print(ID, "Connected to master, waiting for messages...");

    // Parse the toml file for this module.
    fip_slave_config_t config = fip_slave_load_config(ID, C);
    fip_print(ID, "Parsed fip-c.toml file");
    assert(config.type == C);

    // Print all sources and all compile flags
    for (unsigned int i = 0; i < config.u.c.compile_flags_len; i++) {
        fip_print(ID, "compile_flags[%u]: %s", i, config.u.c.compile_flags[i]);
    }
    for (unsigned int i = 0; i < config.u.c.sources_len; i++) {
        fip_print(ID, "source[%u]: %s", i, config.u.c.sources[i]);
        scan_c_file_for_fip_exports(config.u.c.sources[i]);
    }

    // Main loop - wait for messages from master
    char buffer[256];
    while (true) {
        if (fip_slave_receive_message(socket_fd, buffer, sizeof(buffer))) {
            // Only print the first time we receive a message
            fip_print(ID, "Received message: '%s'", buffer);

            // Check for known commands
            if (strcmp(buffer, "kill") == 0) {
                fip_print(ID, "Received kill command, shutting down");
                break;
            }

            // Try to parse the function signature
            fip_fn_sig_t fn_sig = fip_parse_fn_signature(ID, buffer);
            if (fn_sig.name != NULL) {
                fip_print(ID, "Function signature could be parsed");
                fip_print_fn_sig(ID, &fn_sig);
            } else {
                fip_print(ID, "Function signature could not be parsed");
            }
        }

        // Small delay to prevent busy waiting
        usleep(50000); // 50ms
    }

    fip_slave_cleanup_socket(socket_fd);
    fip_print(ID, "ending...");
    return 0;
}
