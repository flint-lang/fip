#define FIP_SLAVE
#define FIP_IMPLEMENTATION
#include "fip.h"

fip_log_level_t LOG_LEVEL = FIP_INFO;

#include <clang-c/Index.h>

#include <assert.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#include <fcntl.h> // _O_BINARY
#include <io.h>    // _setmode, _fileno
#endif

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
    "void",
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

typedef struct {
    fip_type_t *fields;
    int current_index;
    uint32_t module_id;
} field_visitor_data;

#define MAX_SYMBOLS 1000
#define MODULE_NAME "fip-c"

fip_c_symbol_t symbols[MAX_SYMBOLS]; // Simple array for now
uint32_t symbol_count = 0;
uint32_t ID;
fip_module_config_t CONFIG;

bool parse_toml_file(toml_result_t toml) {
    // === COMPILER ===
    toml_datum_t compiler_d = toml_seek(toml.toptab, "compiler");
    if (compiler_d.type != TOML_STRING) {
        fip_print(ID, FIP_ERROR, "Missing 'compiler' field");
        return false;
    }
    const int compiler_len = compiler_d.u.str.len;
    const char *compiler_ptr = compiler_d.u.str.ptr;
    memcpy(CONFIG.compiler, compiler_ptr, compiler_len);

    // === SOURCES ===
    toml_datum_t sources_d = toml_seek(toml.toptab, "sources");
    if (sources_d.type != TOML_ARRAY) {
        fip_print(ID, FIP_ERROR, "Missing 'sources' field");
        return false;
    }
    int32_t arr_len = sources_d.u.arr.size;
    toml_datum_t *sources_elems_d = sources_d.u.arr.elem;
    CONFIG.sources = (char **)malloc(sizeof(char *) * arr_len);
    CONFIG.sources_len = arr_len;
    for (int32_t i = 0; i < arr_len; i++) {
        if (sources_elems_d[i].type != TOML_STRING) {
            fip_print(ID, FIP_ERROR,
                "'sources' does contain a non-string value");
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
        fip_print(ID, FIP_ERROR, "Missing 'compile_flags' field");
        return false;
    }
    arr_len = compile_flags_d.u.arr.size;
    toml_datum_t *compile_flags_elems_d = compile_flags_d.u.arr.elem;
    CONFIG.compile_flags = (char **)malloc(sizeof(char *) * arr_len);
    CONFIG.compile_flags_len = arr_len;
    for (int32_t i = 0; i < arr_len; i++) {
        if (compile_flags_elems_d[i].type != TOML_STRING) {
            fip_print(ID, FIP_ERROR,
                "'compile_flags' does contain a non-string value");
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

enum CXChildVisitResult count_struct_fields_visitor( //
    CXCursor cursor,                                 //
    [[maybe_unused]] CXCursor parent,                //
    CXClientData client_data                         //
) {
    int *field_count = (int *)client_data;
    if (clang_getCursorKind(cursor) == CXCursor_FieldDecl) {
        (*field_count)++;
    }
    return CXChildVisit_Continue;
}

bool clang_type_to_fip_type(CXType clang_type, fip_type_t *fip_type);

enum CXChildVisitResult struct_field_visitor( //
    CXCursor cursor,                          //
    [[maybe_unused]] CXCursor parent,         //
    CXClientData client_data                  //
) {
    field_visitor_data *data = (field_visitor_data *)client_data;

    if (clang_getCursorKind(cursor) == CXCursor_FieldDecl) {
        CXType field_type = clang_getCursorType(cursor);

        if (!clang_type_to_fip_type(                            //
                field_type, &data->fields[data->current_index]) //
        ) {
            fip_print(data->module_id, FIP_TRACE,
                "Failed to convert field type at index %d",
                data->current_index);
            return CXChildVisit_Break;
        }

        data->current_index++;
    }

    return CXChildVisit_Continue;
}

bool clang_type_to_fip_type(CXType clang_type, fip_type_t *fip_type) {
    fip_type->is_mutable = !clang_isConstQualifiedType(clang_type);

    switch (clang_type.kind) {
        case CXType_UChar:
            fip_type->type = FIP_TYPE_PRIMITIVE;
            fip_type->u.prim = FIP_U8;
            return true;
        case CXType_UShort:
            fip_type->type = FIP_TYPE_PRIMITIVE;
            fip_type->u.prim = FIP_U16;
            return true;
        case CXType_UInt:
            fip_type->type = FIP_TYPE_PRIMITIVE;
            fip_type->u.prim = FIP_U32;
            return true;
        case CXType_ULong:
        case CXType_ULongLong:
            fip_type->type = FIP_TYPE_PRIMITIVE;
            fip_type->u.prim = FIP_U64;
            return true;
        case CXType_Char_S:
        case CXType_SChar:
            fip_type->type = FIP_TYPE_PRIMITIVE;
            fip_type->u.prim = FIP_I8;
            return true;
        case CXType_Short:
            fip_type->type = FIP_TYPE_PRIMITIVE;
            fip_type->u.prim = FIP_I16;
            return true;
        case CXType_Int:
            fip_type->type = FIP_TYPE_PRIMITIVE;
            fip_type->u.prim = FIP_I32;
            return true;
        case CXType_Long:
        case CXType_LongLong:
            fip_type->type = FIP_TYPE_PRIMITIVE;
            fip_type->u.prim = FIP_I64;
            return true;
        case CXType_Float:
            fip_type->type = FIP_TYPE_PRIMITIVE;
            fip_type->u.prim = FIP_F32;
            return true;
        case CXType_Double:
            fip_type->type = FIP_TYPE_PRIMITIVE;
            fip_type->u.prim = FIP_F64;
            return true;
        case CXType_Bool:
            fip_type->type = FIP_TYPE_PRIMITIVE;
            fip_type->u.prim = FIP_BOOL;
            return true;
        case CXType_Void:
            fip_type->type = FIP_TYPE_PRIMITIVE;
            fip_type->u.prim = FIP_VOID;
            return true;
        case CXType_Pointer: {
            CXType pointee = clang_getPointeeType(clang_type);
            if (pointee.kind == CXType_Char_S || pointee.kind == CXType_SChar) {
                // char* -> treat as C string
                fip_type->is_mutable = !clang_isConstQualifiedType(pointee);
                fip_type->type = FIP_TYPE_PRIMITIVE;
                fip_type->u.prim = FIP_STR;
                return true;
            } else {
                // Other pointer types
                fip_type->type = FIP_TYPE_PTR;
                fip_type->u.ptr.base_type = malloc(sizeof(fip_type_t));
                const bool success = clang_type_to_fip_type( //
                    pointee, fip_type->u.ptr.base_type       //
                );
                if (success) {
                    fip_type->is_mutable =
                        fip_type->u.ptr.base_type->is_mutable;
                }
                return success;
            }
        }
        case CXType_Elaborated: {
            // For elaborated types (like 'struct Foo', 'union Bar'), get the
            // named type
            CXType named_type = clang_Type_getNamedType(clang_type);

            CXString elaborated_name = clang_getTypeSpelling(clang_type);
            CXString named_name = clang_getTypeSpelling(named_type);
            fip_print(                                      //
                ID, FIP_TRACE,                              //
                "Elaborated '%s' -> named '%s' (kind: %d)", //
                clang_getCString(elaborated_name),          //
                clang_getCString(named_name),               //
                named_type.kind                             //
            );
            clang_disposeString(elaborated_name);
            clang_disposeString(named_name);

            // Recursively process the named type
            if (clang_type_to_fip_type(named_type, fip_type)) {
                fip_type->is_mutable = !clang_isConstQualifiedType(clang_type);
                return true;
            } else {
                return false;
            }
        }
        case CXType_Typedef: {
            // For typedef'ed types, get the canonical (underlying) type
            CXType canonical_type = clang_getCanonicalType(clang_type);

            CXString typedef_name = clang_getTypeSpelling(clang_type);
            CXString canonical_name = clang_getTypeSpelling(canonical_type);
            fip_print(                            //
                ID, FIP_TRACE,                    //
                "Typedef '%s' -> canonical '%s'", //
                clang_getCString(typedef_name),   //
                clang_getCString(canonical_name)  //
            );
            clang_disposeString(typedef_name);
            clang_disposeString(canonical_name);

            // Recursively process the canonical type
            if (clang_type_to_fip_type(canonical_type, fip_type)) {
                fip_type->is_mutable = !clang_isConstQualifiedType(clang_type);
                return true;
            } else {
                return false;
            }
        }
        case CXType_Record: { // CXType_Record represents structs/unions
            fip_print(ID, FIP_TRACE, "Processing struct/record type");
            fip_type->type = FIP_TYPE_STRUCT;

            // Get the struct declaration cursor
            CXCursor type_cursor = clang_getTypeDeclaration(clang_type);

            // Count fields first
            int field_count = 0;
            clang_visitChildren(                                       //
                type_cursor, count_struct_fields_visitor, &field_count //
            );
            fip_print(ID, FIP_TRACE, "Struct has %d fields", field_count);
            if (field_count <= 0 || field_count > 255) {
                fip_print(ID, FIP_WARN, "Invalid struct field count: %d",
                    field_count);
                return false;
            }

            fip_type->u.struct_t.field_count = (uint8_t)field_count;
            fip_type->u.struct_t.fields = malloc( //
                sizeof(fip_type_t) * field_count  //
            );

            // Structure to pass data to the visitor
            field_visitor_data visitor_data = {
                .fields = fip_type->u.struct_t.fields,
                .current_index = 0,
                .module_id = ID,
            };

            // Visit each field and convert its type
            clang_visitChildren(                                 //
                type_cursor, struct_field_visitor, &visitor_data //
            );

            // Check if we processed the expected number of fields
            if (visitor_data.current_index != field_count) {
                fip_print(                                               //
                    ID, FIP_WARN, "Processed %d fields but expected %d", //
                    visitor_data.current_index, field_count              //
                );
                free(fip_type->u.struct_t.fields);
                return false;
            }

            fip_print(ID, FIP_TRACE,
                "Successfully processed struct with %d fields", field_count);
            return true;
        }
        case CXType_Complex: {
            // TODO: Handle complex types if needed
            fip_print(ID, FIP_ERROR, "Complex types not yet supported");
            return false;
        }
        default:
            return false;
    }
}

bool extract_function_signature(CXCursor cursor, fip_sig_fn_t *fn_sig) {
    // Get function name
    CXString name = clang_getCursorSpelling(cursor);
    const char *name_cstr = clang_getCString(name);
    strncpy(fn_sig->name, name_cstr, sizeof(fn_sig->name) - 1);
    clang_disposeString(name);

    CXType function_type = clang_getCursorType(cursor);

    // Get return type
    CXType return_type = clang_getResultType(function_type);
    if (return_type.kind != CXType_Void) {
        fn_sig->rets_len = 1;
        fn_sig->rets = malloc(sizeof(fip_type_t));
        if (!clang_type_to_fip_type(return_type, &fn_sig->rets[0])) {
            fip_print(ID, FIP_WARN, "Unsupported return type for function %s",
                fn_sig->name);
            free(fn_sig->rets);
            return false;
        }
    } else {
        fn_sig->rets_len = 0;
        fn_sig->rets = NULL;
    }

    // Get parameter count and types
    int num_args = clang_getNumArgTypes(function_type);
    if (num_args < 0) {
        fip_print(                                                        //
            ID, FIP_WARN, "Could not get argument count for function %s", //
            fn_sig->name                                                  //
        );
        free(fn_sig->rets);
        return false;
    }

    fn_sig->args_len = num_args;
    if (num_args > 0) {
        fn_sig->args = malloc(sizeof(fip_type_t) * num_args);
        for (int i = 0; i < num_args; i++) {
            CXType arg_type = clang_getArgType(function_type, i);
            if (!clang_type_to_fip_type(arg_type, &fn_sig->args[i])) {
                fip_print(                                          //
                    ID, FIP_WARN,                                   //
                    "Unsupported argument type %d for function %s", //
                    i, fn_sig->name                                 //
                );
                free(fn_sig->args);
                free(fn_sig->rets);
                return false;
            }
        }
    } else {
        fn_sig->args = NULL;
    }

    return true;
}

enum CXChildVisitResult visit_ast_node( //
    CXCursor cursor,                    //
    [[maybe_unused]] CXCursor parent,   //
    CXClientData client_data            //
) {
    const char *file_path = (const char *)client_data;

    // Only process nodes from the file we're parsing (not from #includes)
    CXSourceLocation location = clang_getCursorLocation(cursor);
    CXFile file;
    unsigned line, column, offset;
    clang_getExpansionLocation(location, &file, &line, &column, &offset);

    if (file) {
        CXString filename = clang_getFileName(file);
        const char *filename_cstr = clang_getCString(filename);

        // Skip if this cursor is not from our target file
        if (strcmp(filename_cstr, file_path) != 0) {
            clang_disposeString(filename);
            return CXChildVisit_Continue;
        }
        clang_disposeString(filename);
    }

    enum CXCursorKind kind = clang_getCursorKind(cursor);

    // We're looking for function definitions
    if (kind == CXCursor_FunctionDecl) {
        // Get function name first to check if it's main
        CXString name = clang_getCursorSpelling(cursor);
        const char *name_cstr = clang_getCString(name);
        fip_print(ID, FIP_TRACE, "Visit AST Node FunctionDecl: %s", name_cstr);
        // Skip main function
        if (strcmp(name_cstr, "main") == 0) {
            clang_disposeString(name);
            return CXChildVisit_Continue;
        }
        clang_disposeString(name);

        // Check if function has external linkage
        enum CXLinkageKind linkage = clang_getCursorLinkage(cursor);
        if (linkage != CXLinkage_External) {
            fip_print(ID, FIP_TRACE, "Has no external linkage");
            return CXChildVisit_Continue;
        }

        if (symbol_count >= MAX_SYMBOLS) {
            fip_print(                                                  //
                ID, FIP_WARN,                                           //
                "Maximum symbols reached, skipping remaining functions" //
            );
            return CXChildVisit_Break;
        }

        // Extract function information
        fip_c_symbol_t symbol = {0};
        strncpy(symbol.source_file_path, file_path,
            sizeof(symbol.source_file_path) - 1);
        symbol.needed = false;
        symbol.line_number = (int)line;
        symbol.type = FIP_SYM_FUNCTION;

        if (extract_function_signature(cursor, &symbol.signature.fn)) {
            symbols[symbol_count] = symbol;

            fip_print(                                                  //
                ID, FIP_INFO, "Found extern function: '%s' at line %d", //
                symbol.signature.fn.name, symbol.line_number            //
            );
            fip_print_sig_fn(ID, &symbol.signature.fn);

            symbol_count++;
        }
    }

    return CXChildVisit_Recurse;
}

void parse_c_file(const char *c_file) {
    CXIndex index = clang_createIndex(0, 0);
    FILE *fp = popen("gcc -print-file-name=include", "r");
    char buf[512];
    if (fgets(buf, sizeof(buf), fp)) {
        buf[strcspn(buf, "\n")] = 0; // strip newline
        // buf now holds the include path, e.g.
        // "/usr/lib/gcc/x86_64-pc-linux-gnu/15.2.1/include"
    }
    pclose(fp);
    const char *base_args[] = {
        "-x",
        "c",
        "-std=gnu23",
        "-I",
        buf,
    };
    const int base_args_len = 5;
    const int num_args = base_args_len + CONFIG.compile_flags_len;
    const char **args = (const char **)malloc(sizeof(char *) * num_args);
    for (int i = 0; i < base_args_len; i++) {
        args[i] = base_args[i];
    }
    for (int i = 0; i < (int)CONFIG.compile_flags_len; i++) {
        args[base_args_len + i] = CONFIG.compile_flags[i];
    }
    fip_print(ID, FIP_DEBUG, "Clang Parse Arguments:");
    for (int i = 0; i < num_args; i++) {
        fip_print(ID, FIP_DEBUG, "  %s", args[i]);
    }
    CXTranslationUnit unit = clang_parseTranslationUnit(               //
        index, c_file, args, num_args, NULL, 0, CXTranslationUnit_None //
    );

    if (unit == NULL) {
        fip_print(ID, FIP_WARN, "Unable to parse file %s", c_file);
        return;
    }

    fip_print(                                                             //
        ID, FIP_INFO,                                                      //
        "Scanning %s for extern functions with implementations...", c_file //
    );

    CXCursor cursor = clang_getTranslationUnitCursor(unit);
    clang_visitChildren(cursor, visit_ast_node, (CXClientData)c_file);

    clang_disposeTranslationUnit(unit);
    clang_disposeIndex(index);

    fip_print(                                           //
        ID, FIP_INFO, "Found %d extern functions in %s", //
        symbol_count, c_file                             //
    );
}

void handle_symbol_request(    //
    char buffer[FIP_MSG_SIZE], //
    const fip_msg_t *message   //
) {
    assert(message->u.sym_req.type == FIP_SYM_FUNCTION);
    fip_print(ID, FIP_INFO, "Symbol Request Received");
    const fip_sig_fn_t *msg_fn = &message->u.sym_req.sig.fn;
    fip_print_sig_fn(ID, msg_fn);
    fip_msg_t response = {0};
    response.type = FIP_MSG_SYMBOL_RESPONSE;
    fip_msg_symbol_response_t *sym_res = &response.u.sym_res;
    strncpy(sym_res->module_name, MODULE_NAME,
        sizeof(sym_res->module_name) - 1);
    sym_res->module_name[sizeof(sym_res->module_name) - 1] = '\0';
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
                memcpy(sym_res->sig.fn.name, sym_fn->name, 128);
                break;
            }
        }
    }
    sym_res->found = sym_match;
    fip_slave_send_message(ID, buffer, &response);
}

bool compile_file(                                    //
    uint8_t *path_count,                              //
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
    char hash[FIP_PATH_SIZE + 1] = {0};
    fip_create_hash(hash, file_path);
    // This is only needed for the below strstr function
    hash[FIP_PATH_SIZE] = '\0';

    // Check if the hash is already part of the paths, if it is we already
    // compiled the file
    if (strstr(paths, hash)) {
        return true;
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
    char output_path[64] = {0};
    snprintf(output_path, sizeof(output_path), ".fip/cache/%.8s.o", hash);

    char compile_cmd[1024] = {0};
    int ret =
        snprintf(compile_cmd, sizeof(compile_cmd), "%s -x c -c %s %s -o %s",
            CONFIG.compiler, compile_flags, file_path, output_path);
    if (ret >= (int)sizeof(compile_cmd)) {
        fip_print(ID, FIP_ERROR, "Compile command too long, truncated");
        return false;
    }

    fip_print(ID, FIP_INFO, "Executing: %s", compile_cmd);

    char *compile_output = NULL;
    int exit_code = fip_execute_and_capture(&compile_output, compile_cmd);
    if (exit_code != 0) {
        if (compile_output && compile_output[0]) {
            fip_print(ID, FIP_ERROR, "%s", compile_output);
        }
        fip_print(ID, FIP_ERROR, "Compiling file '%s' failed with exit code %d",
            file_path, exit_code);
        free(compile_output);
        return false;
    } else {
        if (compile_output && compile_output[0]) {
            fip_print(ID, FIP_INFO, "%s", compile_output);
        }
        free(compile_output);
    }

    fip_print(ID, FIP_INFO, "Compiled '%.8s' successfully", hash);
    // Add to paths array. For this we need to find the first null-byte
    // character in the paths array, that's where we will place our hash at.
    // The good thing is that we only need to check multiples of 8 so this
    // check is rather easy.
    // Because we know how many paths there already are in the paths string we
    // can just offset by path_count * FIP_PATH_SIZE and increment path_count
    // afterwards, as simple as that
    const uint16_t offset = *path_count * FIP_PATH_SIZE;
    if (offset >= FIP_PATHS_SIZE) {
        fip_print(                                          //
            ID, FIP_ERROR, "The Paths array is full: %.*s", //
            FIP_PATHS_SIZE, paths                           //
        );
        fip_print(ID, FIP_ERROR, "Could not store hash '%s' in it", hash);
        return false;
    }
    memcpy(paths + offset, hash, FIP_PATH_SIZE);
    (*path_count)++;
    return true;
}

void handle_compile_request(   //
    char buffer[FIP_MSG_SIZE], //
    const fip_msg_t *message   //
) {
    assert(message->type == FIP_MSG_COMPILE_REQUEST);
    fip_print(ID, FIP_INFO, "Compile Request Received");
    fip_msg_t response = {0};
    response.type = FIP_MSG_OBJECT_RESPONSE;
    fip_msg_object_response_t *obj_res = &response.u.obj_res;
    obj_res->has_obj = false;
    obj_res->compilation_failed = false;
    strncpy(obj_res->module_name, MODULE_NAME,
        sizeof(obj_res->module_name) - 1);
    obj_res->module_name[sizeof(obj_res->module_name) - 1] = '\0';

    // We need to go through all symbols and see whether they need to be
    // compiled
    for (uint32_t i = 0; i < symbol_count; i++) {
        if (symbols[i].needed) {
            if (!compile_file(                            //
                    &obj_res->path_count, obj_res->paths, //
                    symbols[i].source_file_path, message) //
            ) {
                obj_res->has_obj = false;
                obj_res->compilation_failed = true;
                break;
            }
            obj_res->has_obj = true;
        }
    }

    fip_slave_send_message(ID, buffer, &response);
}

int main(int argc, char *argv[]) {
#ifdef _WIN32
    // Disable CRLF <-> LF translations so FIP messages are sent as raw bytes.
    // Must be done before any FIP stdio I/O happens.
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    if (argc < 2) {
        printf("The '%s' Interop Module needs at least one argument\n",
            MODULE_NAME);
        printf("-- The first argument must be the ID of the Interop Module\n");
        printf("-- The optional second argument is the log level of the IM\n");
        printf("   If no log level (0-4) is provided it is set to 1 (INFO)\n");
        return 1;
    }
    if (strcmp("--version", argv[1]) == 0) {
        printf("fip-c version: v%d.%d.%d\n", FIP_MAJOR, FIP_MINOR, FIP_PATCH);
        return 0;
    }
    char *id_str = argv[1];
    char *endptr;
    ID = (uint32_t)strtoul(id_str, &endptr, 10);
    if (argc == 3) {
        char *log_str = argv[2];
        LOG_LEVEL = (fip_log_level_t)strtoul(log_str, &endptr, 10);
    }
    fip_print(ID, FIP_INFO, "starting...");

    char msg_buf[1024] = {0};

    // Initialize slave for stdio communication
    if (!fip_slave_init(ID)) {
        fip_print(ID, FIP_ERROR, "Failed to initialize slave");
        return 1;
    }
    fip_print(ID, FIP_INFO, "Successfully initialized slave communication");

    fip_msg_t msg = {0};
    msg.type = FIP_MSG_CONNECT_REQUEST;
    msg.u.con_req.setup_ok = true;
    msg.u.con_req.version.major = FIP_MAJOR;
    msg.u.con_req.version.minor = FIP_MINOR;
    msg.u.con_req.version.patch = FIP_PATCH;
    strncpy(msg.u.con_req.module_name, MODULE_NAME,
        sizeof(msg.u.con_req.module_name) - 1);
    msg.u.con_req.module_name[sizeof(msg.u.con_req.module_name) - 1] = '\0';

    // Parse the toml file for this module.
    toml_result_t toml = fip_slave_load_config(ID, MODULE_NAME);
    if (!parse_toml_file(toml)) {
        fip_print(                                                 //
            ID, FIP_ERROR, "The %s.toml file could not be parsed", //
            MODULE_NAME                                            //
        );
        msg.u.con_req.setup_ok = false;
        toml_free(toml);
        goto send;
    }
    toml_free(toml);
    if (CONFIG.sources_len == 0) {
        fip_print(                                                            //
            ID, FIP_ERROR,                                                    //
            "The '%s' module does not have any sources declared", MODULE_NAME //
        );
        goto send;
    }
    fip_print(ID, FIP_INFO, "Parsed %s.toml file", MODULE_NAME);

send:
    // Send the connect message to the master now, as we are now able to
    // connect to it
    if (!msg.u.con_req.setup_ok) {
        fip_print(ID, FIP_INFO, "Sending shutdown request to master...");
        fip_slave_send_message(ID, msg_buf, &msg);
        goto kill;
    }
    fip_print(ID, FIP_INFO, "Sending connect request to master...");
    fip_slave_send_message(ID, msg_buf, &msg);

    // Print all sources and all compile flags and parse all source files
    for (uint32_t i = 0; i < CONFIG.compile_flags_len; i++) {
        fip_print(                                  //
            ID, FIP_DEBUG, "compile_flags[%u]: %s", //
            i, CONFIG.compile_flags[i]              //
        );
    }
    for (uint32_t i = 0; i < CONFIG.sources_len; i++) {
        fip_print(ID, FIP_DEBUG, "source[%u]: %s", i, CONFIG.sources[i]);
        parse_c_file(CONFIG.sources[i]);
    }

    // Main loop - wait for messages from master
    bool is_running = true;
    while (is_running) {
        if (fip_slave_receive_message(msg_buf)) {
            // Only print the first time we receive a message
            fip_print(ID, FIP_DEBUG, "Received message");
            fip_msg_t message = {0};
            fip_decode_msg(msg_buf, &message);

            switch (message.type) {
                case FIP_MSG_UNKNOWN:
                    fip_print(ID, FIP_WARN, "Received unknown message");
                    break;
                case FIP_MSG_CONNECT_REQUEST:
                    // The slave should not receive a message it sends
                    assert(false);
                    break;
                case FIP_MSG_SYMBOL_REQUEST:
                    handle_symbol_request(msg_buf, &message);
                    break;
                case FIP_MSG_SYMBOL_RESPONSE:
                    // The slave should not receive a message it sends
                    assert(false);
                    break;
                case FIP_MSG_COMPILE_REQUEST:
                    handle_compile_request(msg_buf, &message);
                    break;
                case FIP_MSG_OBJECT_RESPONSE:
                    // The slave should not receive a message it sends
                    assert(false);
                    break;
                case FIP_MSG_KILL:
                    fip_print(                                    //
                        ID, FIP_INFO,                             //
                        "Received Kill Command, shutting down..." //
                    );
                    is_running = false;
                    break;
            }

            // Free the decoded message
            fip_free_msg(&message);
        }

        // Just wait for N milliseconds before the new loop iteration
        msleep(FIP_SLAVE_DELAY_MS);
    }

kill:
    fip_slave_cleanup();
    fip_print(ID, FIP_INFO, "ending...");
    return 0;
}
