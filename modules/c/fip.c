#define FIP_SLAVE
#define FIP_IMPLEMENTATION
#include "fip.h"

#ifdef DEBUG_BUILD
fip_log_level_e LOG_LEVEL = FIP_DEBUG;
#else
fip_log_level_e LOG_LEVEL = FIP_WARN;
#endif

#include <clang-c/Index.h>

#include <assert.h>
#include <stdalign.h>
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

/*
 * ==============================================
 * GLOBAL DEFINITIONS
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
    char source_file_path[512];
    int line_number;
    fip_msg_symbol_type_e type;
    union {
        fip_sig_fn_t fn;
        fip_sig_data_t data;
        fip_sig_enum_t enum_t;
    } sig;
} fip_c_symbol_t;

typedef struct {
    char tag[128];
    uint32_t headers_len;
    char **headers;
    uint32_t command_len;
    char **command;
    char output[FIP_PATH_SIZE + 1];
} fip_module_config_t;

typedef struct {
    size_t count;
    fip_module_config_t *configs;
} fip_modules_config_t;

#define MAX_SYMBOLS 1000
typedef struct {
    /// @var `needed`
    /// @brief Whether this whole collection is needed, for example when the tag
    /// is non-empty and we write `use Fip.tagname` in Flint then the whole
    /// module is requested to be compiled. Or, if the tag is empty and a single
    /// (or more) symbol(s) of that module are used then the whole module is
    /// compiled and returned.
    bool needed;
    char tag[128];
    size_t symbol_count;
    fip_c_symbol_t symbols[MAX_SYMBOLS];
} fip_c_symbol_collection_t;

typedef struct {
    size_t count;
    fip_c_symbol_collection_t *collection;
} fip_c_symbol_list_t;

typedef struct {
    fip_type_t *fields;
    int current_index;
    uint32_t module_id;
} field_visitor_data;

typedef struct {
    char **field_names;
    int current_index;
    uint32_t module_id;
} field_name_visitor_data;

typedef struct {
    size_t *values;
    int current_index;
    uint32_t module_id;
} enum_visitor_data;

typedef struct {
    char **tags;
    size_t *values;
    int current_index;
    uint32_t module_id;
} enum_sig_visitor_data;

/*
 * ==============================================
 * TYPE STACK Definitions and Helper Functions
 * ==============================================
 * Because C types may be self-referential and
 * recursive we need to handle such types and
 * cases explicitely, since otherwise we would
 * have infinite looping scenarios. To add
 * support for recursive types I have implemented
 * a "type stack" to track which types we already
 * came across within the type conversion
 * function
 * ==============================================
 */

typedef struct cx_type_stack {
    CXType *items;
    uint32_t len;
    uint32_t cap;
} cx_type_stack;

void stack_clear(cx_type_stack *s) {
    if (s->items != NULL) {
        free(s->items);
    }
    s->items = NULL;
    s->len = 0;
    s->cap = 0;
}

void stack_push(cx_type_stack *s, CXType t) {
    if (s->len == s->cap) {
        s->cap = s->cap == 0 ? 8 : s->cap * 2;
        s->items = (CXType *)realloc(s->items, sizeof(CXType) * s->cap);
    }
    s->items[s->len++] = t;
}

void stack_pop(cx_type_stack *s) {
    if (s->len > 0) {
        s->len--;
        if (s->len < s->cap / 2 && s->cap > 8) {
            s->items = (CXType *)realloc(             //
                s->items, sizeof(CXType) * s->cap / 2 //
            );
            s->cap = s->cap / 2;
        }
    }
}

int stack_find_equal(cx_type_stack *s, CXType canonical) {
    for (uint32_t i = 0; i < s->len; ++i) {
        if (clang_equalTypes(s->items[i], canonical)) {
            return i;
        }
    }
    return -1;
}

/*
 * ==============================================
 * GLOBAL VARIABLES
 * ==============================================
 */

#define MODULE_NAME "fip-c"

uint32_t ID;
fip_c_symbol_list_t symbol_list;
fip_c_symbol_collection_t *curr_coll;
fip_modules_config_t CONFIGS;
cx_type_stack stack;

bool parse_toml_file(toml_result_t toml) {
    // Validate top-level is a table (toml.toptab)
    if (toml.toptab.type != TOML_TABLE) {
        fip_print(ID, FIP_ERROR, "TOML root is not a table");
        return false;
    }

    // First pass: count top-level tables (each is a module config)
    const int32_t top_count = toml.toptab.u.tab.size;
    if (top_count <= 0) {
        fip_print(ID, FIP_ERROR, "No top-level entries in TOML");
        return false;
    }

    for (int32_t i = 0; i < top_count; ++i) {
        toml_datum_t v = toml.toptab.u.tab.value[i];
        if (v.type != TOML_TABLE) {
            fip_print(ID, FIP_ERROR, "Incorrect top-level entry in TOML");
            return false;
        }
    }

    // Allocate CONFIGS
    CONFIGS.count = (size_t)top_count;
    CONFIGS.configs = (fip_module_config_t *)malloc( //
        sizeof(fip_module_config_t) * CONFIGS.count  //
    );
    memset(CONFIGS.configs, 0, sizeof(fip_module_config_t) * CONFIGS.count);

    // Second pass: fill CONFIGS
    size_t cfg_idx = 0;
    for (int32_t i = 0; i < top_count; ++i) {
        toml_datum_t v = toml.toptab.u.tab.value[i];
        const char *keyname = toml.toptab.u.tab.key[i];
        int keylen = toml.toptab.u.tab.len[i];

        fip_module_config_t *cfg = &CONFIGS.configs[cfg_idx];
        memset(cfg, 0, sizeof(fip_module_config_t));
        // copy tag (may be empty)
        int copy_len = keylen < (int)sizeof(cfg->tag) - 1
            ? keylen
            : (int)sizeof(cfg->tag) - 1;
        if (copy_len > 0) {
            memcpy(cfg->tag, keyname, (size_t)copy_len);
        }
        cfg->tag[copy_len] = '\0';

        // module table datum, this is the table itself
        toml_datum_t module = v;

        // headers (required, array of strings)
        toml_datum_t headers_d = toml_get(module, "headers");
        if (headers_d.type != TOML_ARRAY) {
            fip_print(ID, FIP_ERROR,
                "Missing or invalid 'headers' in table '%s'", cfg->tag);
            goto fail;
        }
        size_t headers_len = headers_d.u.arr.size;
        if (headers_len < 0) {
            headers_len = 0;
        }
        cfg->headers = (char **)malloc(sizeof(char *) * (size_t)headers_len);
        cfg->headers_len = (uint32_t)headers_len;
        toml_datum_t *header_elems = headers_d.u.arr.elem;
        for (size_t j = 0; j < headers_len; ++j) {
            if (header_elems[j].type != TOML_STRING) {
                fip_print(ID, FIP_ERROR,
                    "Non-string in 'headers' of table '%s'", cfg->tag);
                // free allocated so far for this cfg
                for (size_t k = 0; k < j; ++k) {
                    free(cfg->headers[k]);
                }
                free(cfg->headers);
                cfg->headers = NULL;
                cfg->headers_len = 0;
                goto fail;
            }
            int32_t slen = header_elems[j].u.str.len;
            cfg->headers[j] = (char *)malloc((size_t)slen + 1);
            memcpy(cfg->headers[j], header_elems[j].u.str.ptr, (size_t)slen);
            cfg->headers[j][slen] = '\0';
        }

        // sources (optional, array of strings)
        toml_datum_t sources_d = toml_get(module, "sources");
        size_t sources_len = 0;
        char **sources = NULL;
        const bool sources_present = sources_d.type == TOML_ARRAY;
        if (sources_present) {
            sources_len = sources_d.u.arr.size;
            if (sources_len < 0) {
                sources_len = 0;
            }
            sources = (char **)malloc(               //
                sizeof(char *) * (size_t)sources_len //
            );
            toml_datum_t *elems = sources_d.u.arr.elem;
            for (size_t j = 0; j < sources_len; ++j) {
                if (elems[j].type != TOML_STRING) {
                    fip_print(ID, FIP_ERROR,
                        "Non-string in 'sources' of table '%s'", cfg->tag);
                    // free allocated so far for this cfg
                    for (size_t k = 0; k < j; ++k) {
                        free(sources[k]);
                    }
                    free(sources);
                    sources = NULL;
                    sources_len = 0;
                    goto fail;
                }
                int32_t slen = elems[j].u.str.len;
                sources[j] = (char *)malloc((size_t)slen + 1);
                memcpy(sources[j], elems[j].u.str.ptr, (size_t)slen);
                sources[j][slen] = '\0';
            }
        }

        // command (optional, array of strings)
        // Fails if command is present but no sources are present
        toml_datum_t command_d = toml_get(module, "command");
        const bool command_present = command_d.type == TOML_ARRAY;
        bool command_sources_substituted = false;
        bool command_output_substituted = false;
        if (command_present) {
            if (!sources_present || sources_len == 0) {
                fip_print(ID, FIP_ERROR,
                    "Missing, invalid or empty 'sources' in table '%s'",
                    cfg->tag);
                goto fail;
            }
            size_t command_len = command_d.u.arr.size;
            if (command_len < 0) {
                command_len = 0;
            }
            // The command needs to be the size of the actual command strings +
            // the number of sources - 1 (since the `__SOURCES__` string is
            // substituted with the sources)
            command_len += sources_len - 1;
            cfg->command = (char **)malloc(          //
                sizeof(char *) * (size_t)command_len //
            );
            cfg->command_len = (uint32_t)command_len;
            toml_datum_t *elems = command_d.u.arr.elem;
            for (size_t j = 0; j < command_len - sources_len + 1; ++j) {
                size_t cmd_idx = command_sources_substituted //
                    ? j + sources_len - 1                    //
                    : j;
                if (elems[j].type != TOML_STRING) {
                    fip_print(ID, FIP_ERROR,
                        "Non-string in 'command' of table '%s'", cfg->tag);
                    // Clean up all command string so far
                    for (size_t k = 0; k < cmd_idx; ++k) {
                        free(cfg->command[k]);
                    }
                    free(cfg->command);
                    cfg->command = NULL;
                    cfg->command_len = 0;
                    for (size_t k = 0; k < sources_len; ++k) {
                        free(sources[k]);
                    }
                    free(sources);
                    sources = NULL;
                    sources_len = 0;
                    goto fail;
                }
                int32_t slen = elems[j].u.str.len;
                if (strncmp(                                                  //
                        elems[j].u.str.ptr, "__SOURCES__", (size_t)slen) == 0 //
                ) {
                    if (command_sources_substituted) {
                        // Substituting the sources twice is not allowed
                        fip_print(ID, FIP_ERROR,
                            "Substituting '__SOURCES__' twice in 'command' in table '%s'",
                            cfg->tag);
                        // Clean up all command string so far
                        for (size_t k = 0; k < cmd_idx; ++k) {
                            free(cfg->command[k]);
                        }
                        free(cfg->command);
                        cfg->command = NULL;
                        cfg->command_len = 0;
                        for (size_t k = 0; k < sources_len; ++k) {
                            free(sources[k]);
                        }
                        free(sources);
                        goto fail;
                    }
                    // Substitute the sources' content into the command by
                    // simply copying over the pointer shallowly and then just
                    // freeing the `sources` itself instead of freeing the
                    // strings themselves.
                    assert(cmd_idx == j);
                    for (size_t k = 0; k < sources_len; k++) {
                        cfg->command[j + k] = sources[k];
                    }
                    free(sources);
                    sources = NULL;
                    command_sources_substituted = true;
                } else if (strncmp(elems[j].u.str.ptr, "__OUTPUT__", //
                               (size_t)slen) == 0                    //
                ) {
                    if (command_output_substituted) {
                        // Substituting the sources twice is not allowed
                        fip_print(ID, FIP_ERROR,
                            "Substituting '__OUTPUT__' twice in 'command' in table '%s'",
                            cfg->tag);
                        // Clean up all command string so far
                        for (size_t k = 0; k < j; ++k) {
                            free(cfg->command[k]);
                        }
                        free(cfg->command);
                        cfg->command = NULL;
                        cfg->command_len = 0;
                        for (size_t k = 0; k < sources_len; ++k) {
                            free(sources[k]);
                        }
                        free(sources);
                        goto fail;
                    }
                    // Substitution of the `__OUTPUT__` string works by hashing
                    // the first header for now
                    // TODO: Implement a more sophisticated hashing
                    const char *cache_dir = ".fip/cache/";
                    const size_t cache_dir_len = 11;
#ifdef __WIN32__
                    const char *file_ext = ".obj";
                    const size_t ext_len = 4;
                    const size_t output_len = //
                        cache_dir_len         // The length of the cache dir
                        + FIP_PATH_SIZE       // The length of the hash itself
                        + ext_len             // '.obj'
                        + 1;                  // Zero-terminator
#else
                    const char *file_ext = ".o";
                    const size_t ext_len = 2;
                    const size_t output_len = //
                        cache_dir_len         // The length of the cache dir
                        + FIP_PATH_SIZE       // The length of the hash itself
                        + ext_len             // '.o'
                        + 1;                  // Zero-terminator
#endif
                    cfg->command[cmd_idx] = (char *)malloc(output_len);
                    char *insert_ptr = cfg->command[cmd_idx];
                    memcpy(insert_ptr, cache_dir, cache_dir_len);
                    insert_ptr += cache_dir_len;
                    fip_create_hash(insert_ptr, cfg->headers[0]);
                    memcpy(cfg->output, insert_ptr, FIP_PATH_SIZE);
                    insert_ptr += FIP_PATH_SIZE;
                    memcpy(insert_ptr, file_ext, ext_len);
                    cfg->command[cmd_idx][output_len - 1] = '\0';
                    cfg->output[FIP_PATH_SIZE] = '\0';
                    command_output_substituted = true;
                } else {
                    cfg->command[cmd_idx] = (char *)malloc((size_t)slen + 1);
                    memcpy(                    //
                        cfg->command[cmd_idx], //
                        elems[j].u.str.ptr,    //
                        (size_t)slen           //
                    );
                    cfg->command[cmd_idx][slen] = '\0';
                }
            }
        }

        if (command_present && !command_sources_substituted) {
            fip_print(ID, FIP_ERROR,
                "Missing substitute '__SOURCES__' in 'command' in table '%s'",
                cfg->tag);
            if (sources_present) {
                for (size_t j = 0; j < sources_len; ++j) {
                    free(sources[j]);
                }
                free(sources);
            }
            goto fail;
        }
        if (command_present && !command_output_substituted) {
            fip_print(ID, FIP_ERROR,
                "Missing substitute '__OUTPUT__' in 'command' in table '%s'",
                cfg->tag);
            if (sources_present) {
                for (size_t j = 0; j < sources_len; ++j) {
                    free(sources[j]);
                }
                free(sources);
            }
            goto fail;
        }

        // Check if 'sources' are present but 'command' is not. In this case we
        // need to free the 'sources' manually here since otherwise they would
        // leak since nothin consumed them
        if (sources_present && !command_present) {
            fip_print(ID, FIP_ERROR,
                "Missing or invalid 'command' in table '%s'", cfg->tag);
            for (size_t j = 0; j < sources_len; ++j) {
                free(sources[j]);
            }
            free(sources);
            goto fail;
        }

        // finished filling this cfg
        cfg_idx++;
    }

    // Success: cfg_idx should equal CONFIGS.count
    if (cfg_idx != CONFIGS.count) {
        // Shouldn't happen; defensive handling
        fip_print(ID, FIP_WARN, "Parsed %zu module tables but expected %zu",
            cfg_idx, CONFIGS.count);
        CONFIGS.count = cfg_idx;
    }

    return true;

// On error: free everything allocated in CONFIGS
fail:
    for (size_t i = 0; i < CONFIGS.count; ++i) {
        fip_module_config_t *c = &CONFIGS.configs[i];
        if (c->headers) {
            for (uint32_t j = 0; j < c->headers_len; ++j) {
                free(c->headers[j]);
            }
            free(c->headers);
            c->headers = NULL;
            c->headers_len = 0;
        }
        if (c->command) {
            for (uint32_t j = 0; j < c->command_len; ++j)
                free(c->command[j]);
            free(c->command);
            c->command = NULL;
            c->command_len = 0;
        }
    }
    free(CONFIGS.configs);
    CONFIGS.configs = NULL;
    CONFIGS.count = 0;
    return false;
}

/*
 * ==============================================
 * CLANG TO FIP TYPE Conversion Functions
 * ==============================================
 * The conversion functions work with a visitor
 * pattern of functions which get executed
 * depending on the type at hand. The `fip-c`
 * Interop Module is single-threaded only which
 * means we can have a global type stack to
 * detect recursion effectively. This uses the
 * above recursion handling functions extensively
 * to keep track of the current type and all
 * types which came before.
 * ==============================================
 */

static bool symbol_name_exists(const char *name) {
    if (strlen(name) == 0) {
        return false;
    }
    for (size_t i = 0; i < curr_coll->symbol_count; i++) {
        const char *existing_name = NULL;
        switch (curr_coll->symbols[i].type) {
            case FIP_SYM_DATA:
                existing_name = curr_coll->symbols[i].sig.data.name;
                break;
            case FIP_SYM_ENUM:
                existing_name = curr_coll->symbols[i].sig.enum_t.name;
                break;
            default:
                continue;
        }
        if (strcmp(existing_name, name) == 0) {
            return true;
        }
    }
    return false;
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

enum CXChildVisitResult struct_field_name_visitor( //
    CXCursor cursor,                               //
    [[maybe_unused]] CXCursor parent,              //
    CXClientData client_data                       //
) {
    field_name_visitor_data *data = (field_name_visitor_data *)client_data;

    if (clang_getCursorKind(cursor) == CXCursor_FieldDecl) {
        CXString field_name = clang_getCursorSpelling(cursor);
        const char *field_name_cstr = clang_getCString(field_name);
        size_t name_len = strlen(field_name_cstr);

        data->field_names[data->current_index] = (char *)malloc(name_len + 1);
        strcpy(data->field_names[data->current_index], field_name_cstr);
        clang_disposeString(field_name);

        data->current_index++;
    }

    return CXChildVisit_Continue;
}

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

enum CXChildVisitResult enum_value_visitor( //
    CXCursor cursor,                        //
    [[maybe_unused]] CXCursor parent,       //
    CXClientData client_data                //
) {
    enum_visitor_data *data = (enum_visitor_data *)client_data;

    if (clang_getCursorKind(cursor) == CXCursor_EnumConstantDecl) {
        data->values[data->current_index++] =
            (size_t)clang_getEnumConstantDeclValue(cursor);
    }

    return CXChildVisit_Continue;
}

enum CXChildVisitResult enum_sig_visitor( //
    CXCursor cursor,                      //
    [[maybe_unused]] CXCursor parent,     //
    CXClientData client_data              //
) {
    enum_sig_visitor_data *data = (enum_sig_visitor_data *)client_data;

    if (clang_getCursorKind(cursor) == CXCursor_EnumConstantDecl) {
        CXString name_str = clang_getCursorSpelling(cursor);
        const char *name = clang_getCString(name_str);
        size_t len = strlen(name);
        data->tags[data->current_index] = (char *)malloc(len + 1);
        strcpy(data->tags[data->current_index], name);
        clang_disposeString(name_str);
        data->values[data->current_index] =
            (size_t)clang_getEnumConstantDeclValue(cursor);
        data->current_index++;
    }

    return CXChildVisit_Continue;
}

enum CXChildVisitResult count_enum_constants_visitor( //
    CXCursor cursor,                                  //
    [[maybe_unused]] CXCursor parent,                 //
    CXClientData client_data                          //
) {
    int *count = (int *)client_data;
    if (clang_getCursorKind(cursor) == CXCursor_EnumConstantDecl) {
        (*count)++;
    }
    return CXChildVisit_Continue;
}

bool clang_type_to_fip_type(CXType clang_type, fip_type_t *fip_type) {
    CXType canonical = clang_getCanonicalType(clang_type);
    fip_print(ID, FIP_DEBUG, "Resolving type at depth %u", stack.len);

    fip_type->is_mutable = !clang_isConstQualifiedType(clang_type);

    int found = stack_find_equal(&stack, canonical);
    if (found >= 0) {
        fip_type->type = FIP_TYPE_RECURSIVE;
        fip_type->u.recursive.levels_back = (uint8_t)(stack.len - found);
        return true;
    }

    // Type not seen yet, push and process
    stack_push(&stack, canonical);

    switch (canonical.kind) {
        case CXType_UChar:
            fip_type->type = FIP_TYPE_PRIMITIVE;
            fip_type->u.prim = FIP_U8;
            goto ok;
        case CXType_UShort:
            fip_type->type = FIP_TYPE_PRIMITIVE;
            fip_type->u.prim = FIP_U16;
            goto ok;
        case CXType_UInt:
            fip_type->type = FIP_TYPE_PRIMITIVE;
            fip_type->u.prim = FIP_U32;
            goto ok;
        case CXType_ULong:
        case CXType_ULongLong:
            fip_type->type = FIP_TYPE_PRIMITIVE;
            fip_type->u.prim = FIP_U64;
            goto ok;
        case CXType_Char_S:
        case CXType_SChar:
            fip_type->type = FIP_TYPE_PRIMITIVE;
            fip_type->u.prim = FIP_I8;
            goto ok;
        case CXType_Short:
            fip_type->type = FIP_TYPE_PRIMITIVE;
            fip_type->u.prim = FIP_I16;
            goto ok;
        case CXType_Int:
            fip_type->type = FIP_TYPE_PRIMITIVE;
            fip_type->u.prim = FIP_I32;
            goto ok;
        case CXType_Long:
        case CXType_LongLong:
            fip_type->type = FIP_TYPE_PRIMITIVE;
            fip_type->u.prim = FIP_I64;
            goto ok;
        case CXType_Float:
            fip_type->type = FIP_TYPE_PRIMITIVE;
            fip_type->u.prim = FIP_F32;
            goto ok;
        case CXType_Double:
            fip_type->type = FIP_TYPE_PRIMITIVE;
            fip_type->u.prim = FIP_F64;
            goto ok;
        case CXType_Bool:
            fip_type->type = FIP_TYPE_PRIMITIVE;
            fip_type->u.prim = FIP_BOOL;
            goto ok;
        case CXType_Void:
            fip_type->type = FIP_TYPE_PRIMITIVE;
            fip_type->u.prim = FIP_VOID;
            goto ok;
        case CXType_Pointer: {
            CXType pointee = clang_getPointeeType(canonical);
            if (pointee.kind == CXType_Char_S || pointee.kind == CXType_SChar) {
                // char* -> treat as C string
                fip_type->is_mutable = !clang_isConstQualifiedType(pointee);
                fip_type->type = FIP_TYPE_PRIMITIVE;
                fip_type->u.prim = FIP_STR;
                goto ok;
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
                    goto ok;
                }
                goto fail;
            }
        }
        case CXType_Elaborated: {
            // For elaborated types (like 'struct Foo', 'union Bar'), get the
            // named type
            CXType named_type = clang_Type_getNamedType(canonical);

            CXString elaborated_name = clang_getTypeSpelling(canonical);
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
                fip_type->is_mutable = !clang_isConstQualifiedType(canonical);
                goto ok;
            } else {
                goto fail;
            }
        }
        case CXType_Typedef: {
            // For typedef'ed types, get the canonical (underlying) type
            CXType canonical_type = clang_getCanonicalType(canonical);

            CXString typedef_name = clang_getTypeSpelling(canonical);
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
                fip_type->is_mutable = !clang_isConstQualifiedType(canonical);
                goto ok;
            } else {
                goto fail;
            }
        }
        case CXType_Record: { // CXType_Record represents structs/unions
            fip_print(ID, FIP_TRACE, "Processing struct/record type");
            fip_type->type = FIP_TYPE_STRUCT;

            // Get the struct declaration cursor
            CXCursor type_cursor = clang_getTypeDeclaration(canonical);

            // Extracting the type name
            CXString struct_name = clang_getCursorSpelling(type_cursor);
            const char *name_cstr = clang_getCString(struct_name);
            const size_t name_len = strlen(name_cstr);
            memset(                               //
                fip_type->u.struct_t.name, 0,     //
                sizeof(fip_type->u.struct_t.name) //
            );
            memcpy(fip_type->u.struct_t.name, name_cstr, name_len);
            clang_disposeString(struct_name);

            // Count fields first
            int field_count = 0;
            clang_visitChildren(                                       //
                type_cursor, count_struct_fields_visitor, &field_count //
            );
            fip_print(ID, FIP_TRACE, "Struct has %d fields", field_count);
            if (field_count <= 0 || field_count > 255) {
                fip_print(ID, FIP_WARN, "Invalid struct field count: %d",
                    field_count);
                goto fail;
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
                goto fail;
            }

            fip_print(ID, FIP_TRACE,
                "Successfully processed struct with %d fields", field_count);
            goto ok;
        }
        case CXType_Enum: {
            fip_print(ID, FIP_TRACE, "Processing enum type");
            fip_type->type = FIP_TYPE_ENUM;

            CXCursor type_cursor = clang_getTypeDeclaration(canonical);
            CXType integer_type = clang_getEnumDeclIntegerType(type_cursor);
            CXType canon_int = clang_getCanonicalType(integer_type);

            // Extracting the type name
            CXString enum_name = clang_getCursorSpelling(type_cursor);
            const char *name_cstr = clang_getCString(enum_name);
            const size_t name_len = strlen(name_cstr);
            memset(                             //
                fip_type->u.enum_t.name, 0,     //
                sizeof(fip_type->u.enum_t.name) //
            );
            memcpy(fip_type->u.enum_t.name, name_cstr, name_len);
            clang_disposeString(enum_name);

            // Determine is_signed
            uint8_t is_signed = 1;
            switch (canon_int.kind) {
                case CXType_Char_U:
                case CXType_UChar:
                case CXType_UShort:
                case CXType_UInt:
                case CXType_ULong:
                case CXType_ULongLong:
                case CXType_UInt128:
                    is_signed = 0;
                    break;
                default:
                    is_signed = 1;
            }
            fip_type->u.enum_t.is_signed = is_signed;

            // bit_width, for now enums are just 'int' values internally, so 4
            // byte signed
            long long byte_size = clang_Type_getSizeOf(canon_int);
            if (byte_size <= 0) {
                fip_print(ID, FIP_WARN, "Invalid enum underlying type size");
                goto fail;
            }
            fip_type->u.enum_t.bit_width = (uint8_t)(byte_size * 8);

            // Count constants first
            int value_count = 0;
            clang_visitChildren(                                        //
                type_cursor, count_enum_constants_visitor, &value_count //
            );
            fip_print(ID, FIP_TRACE, "Enum has %d constants", value_count);
            if (value_count <= 0 || value_count > 255) {
                fip_print(ID, FIP_WARN, "Invalid enum constant count: %d",
                    value_count);
                goto fail;
            }

            fip_type->u.enum_t.value_count = (uint8_t)value_count;
            fip_type->u.enum_t.values = malloc( //
                sizeof(size_t) * value_count    //
            );

            // Structure to pass data to the visitor
            enum_visitor_data visitor_data = {
                .values = fip_type->u.enum_t.values,
                .current_index = 0,
                .module_id = ID,
            };

            // Visit each constant and collect its value
            clang_visitChildren(                               //
                type_cursor, enum_value_visitor, &visitor_data //
            );

            // Check if we processed the expected number of constants
            if (visitor_data.current_index != value_count) {
                fip_print(                                                  //
                    ID, FIP_WARN, "Processed %d constants but expected %d", //
                    visitor_data.current_index, value_count                 //
                );
                free(fip_type->u.enum_t.values);
                goto fail;
            }

            fip_print(ID, FIP_TRACE,
                "Successfully processed enum with %d constants", value_count);
            goto ok;
        }
        case CXType_Complex: {
            // TODO: Handle complex types if needed
            fip_print(ID, FIP_ERROR, "Complex types not yet supported");
            goto fail;
        }
        default:
            goto fail;
    }

ok:
    stack_pop(&stack);
    return true;
fail:
    stack_pop(&stack);
    return false;
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
        stack_clear(&stack);
        if (!clang_type_to_fip_type(return_type, &fn_sig->rets[0])) {
            fip_print(ID, FIP_WARN, "Unsupported return type for function %s",
                fn_sig->name);
            free(fn_sig->rets);
            stack_clear(&stack);
            return false;
        }
        stack_clear(&stack);
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
        fn_sig->args = malloc(sizeof(fip_sig_fn_arg_t) * num_args);
        for (int i = 0; i < num_args; i++) {
            CXType arg_type = clang_getArgType(function_type, i);
            stack_clear(&stack);
            CXCursor arg_cursor = clang_Cursor_getArgument(cursor, i);
            CXString arg_name_str = clang_getCursorSpelling(arg_cursor);
            const char *arg_name = clang_getCString(arg_name_str);
            strncpy(                             //
                fn_sig->args[i].name,            //
                arg_name,                        //
                sizeof(fn_sig->args[i].name) - 1 //
            );
            clang_disposeString(arg_name_str);
            if (!clang_type_to_fip_type(arg_type, &fn_sig->args[i].type)) {
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
        stack_clear(&stack);
    } else {
        fn_sig->args = NULL;
    }

    return true;
}

bool extract_struct_signature(CXCursor cursor, fip_sig_data_t *data_sig) {
    CXString cname = clang_getCursorSpelling(cursor);
    const char *name_cstr = clang_getCString(cname);
    if (strlen(name_cstr) == 0) {
        clang_disposeString(cname);
        return false;
    }
    strncpy(data_sig->name, name_cstr, sizeof(data_sig->name) - 1);
    clang_disposeString(cname);

    // Count fields first
    int field_count = 0;
    clang_visitChildren(cursor, count_struct_fields_visitor, &field_count);
    if (field_count <= 0 || field_count > 255) {
        fip_print(ID, FIP_WARN, "Invalid struct field count: %d", field_count);
        return false;
    }
    data_sig->value_count = (uint8_t)field_count;

    if (field_count > 0) {
        data_sig->value_names = (char **)malloc(sizeof(char *) * field_count);
        data_sig->value_types =
            (fip_type_t *)malloc(sizeof(fip_type_t) * field_count);

        // Extract field types
        field_visitor_data type_visitor_data = {
            .fields = data_sig->value_types,
            .current_index = 0,
            .module_id = ID,
        };
        clang_visitChildren(cursor, struct_field_visitor, &type_visitor_data);

        // Extract field names
        field_name_visitor_data name_visitor_data = {
            .field_names = data_sig->value_names,
            .current_index = 0,
            .module_id = ID,
        };
        clang_visitChildren(cursor, struct_field_name_visitor,
            &name_visitor_data);

        // Verify we processed all fields
        if (type_visitor_data.current_index != field_count ||
            name_visitor_data.current_index != field_count) {
            fip_print(ID, FIP_WARN,
                "Processed %d types and %d names but expected %d",
                type_visitor_data.current_index,
                name_visitor_data.current_index, field_count);

            // Clean up allocated memory
            for (int i = 0; i < name_visitor_data.current_index; i++) {
                free(data_sig->value_names[i]);
            }
            free(data_sig->value_names);
            free(data_sig->value_types);
            return false;
        }
    } else {
        data_sig->value_names = NULL;
        data_sig->value_types = NULL;
    }

    return true;
}

bool extract_enum_signature(CXCursor cursor, fip_sig_enum_t *enum_sig) {
    CXString cname = clang_getCursorSpelling(cursor);
    const char *name_cstr = clang_getCString(cname);
    if (strlen(name_cstr) == 0) {
        clang_disposeString(cname);
        return false;
    }
    strncpy(enum_sig->name, name_cstr, sizeof(enum_sig->name) - 1);
    clang_disposeString(cname);

    CXType integer_type = clang_getEnumDeclIntegerType(cursor);
    CXType canon = clang_getCanonicalType(integer_type);
    fip_type_prim_e prim;
    switch (canon.kind) {
        case CXType_Char_U:
        case CXType_UChar:
            prim = FIP_U8;
            break;
        case CXType_UShort:
            prim = FIP_U16;
            break;
        case CXType_UInt:
            prim = FIP_U32;
            break;
        case CXType_ULong:
        case CXType_ULongLong:
            prim = FIP_U64;
            break;
        case CXType_Char_S:
        case CXType_SChar:
            prim = FIP_I8;
            break;
        case CXType_Short:
            prim = FIP_I16;
            break;
        case CXType_Int:
            prim = FIP_I32;
            break;
        case CXType_Long:
        case CXType_LongLong:
            prim = FIP_I64;
            break;
        default:
            fip_print(ID, FIP_WARN, "Unsupported underlying enum type for %s",
                enum_sig->name);
            return false;
    }
    enum_sig->type = prim;

    int value_count = 0;
    clang_visitChildren(cursor, count_enum_constants_visitor, &value_count);
    if (value_count <= 0 || value_count > 255) {
        fip_print(ID, FIP_WARN, "Invalid enum constant count: %d", value_count);
        return false;
    }
    enum_sig->value_count = (uint8_t)value_count;

    if (value_count > 0) {
        enum_sig->tags = (char **)malloc(sizeof(char *) * value_count);
        enum_sig->values = (size_t *)malloc(sizeof(size_t) * value_count);
        enum_sig_visitor_data data = {.tags = enum_sig->tags,
            .values = enum_sig->values,
            .current_index = 0,
            .module_id = ID};
        clang_visitChildren(cursor, enum_sig_visitor, &data);
        if (data.current_index != value_count) {
            for (int i = 0; i < data.current_index; ++i) {
                free(enum_sig->tags[i]);
            }
            free(enum_sig->tags);
            free(enum_sig->values);
            return false;
        }
    } else {
        enum_sig->tags = NULL;
        enum_sig->values = NULL;
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
    switch (kind) {
        default:
            return CXChildVisit_Recurse;
        case CXCursor_FunctionDecl: {
            // Get function name first to check if it's main
            CXString name = clang_getCursorSpelling(cursor);
            const char *name_cstr = clang_getCString(name);
            fip_print(ID, FIP_TRACE, "Visit AST Node FunctionDecl: %s",
                name_cstr);
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

            if (curr_coll->symbol_count >= MAX_SYMBOLS) {
                fip_print(                                                  //
                    ID, FIP_WARN,                                           //
                    "Maximum symbols reached, skipping remaining functions" //
                );
                return CXChildVisit_Break;
            }

            // Extract function information
            fip_c_symbol_t symbol = {0};
            strncpy(                                //
                symbol.source_file_path, file_path, //
                sizeof(symbol.source_file_path) - 1 //
            );
            symbol.line_number = (int)line;
            symbol.type = FIP_SYM_FUNCTION;

            if (extract_function_signature(cursor, &symbol.sig.fn)) {
                curr_coll->symbols[curr_coll->symbol_count] = symbol;

                fip_print(                                                  //
                    ID, FIP_INFO, "Found extern function: '%s' at line %d", //
                    symbol.sig.fn.name, symbol.line_number                  //
                );
                fip_print_sig_fn(ID, &symbol.sig.fn);

                curr_coll->symbol_count++;
            }
            break;
        }
        case CXCursor_StructDecl: {
            if (!clang_isCursorDefinition(cursor)) {
                return CXChildVisit_Continue;
            }

            if (curr_coll->symbol_count >= MAX_SYMBOLS) {
                fip_print(                                                //
                    ID, FIP_WARN,                                         //
                    "Maximum symbols reached, skipping remaining structs" //
                );
                return CXChildVisit_Break;
            }

            // Extract struct information
            fip_c_symbol_t symbol = {0};
            strncpy(                                //
                symbol.source_file_path, file_path, //
                sizeof(symbol.source_file_path) - 1 //
            );
            symbol.line_number = (int)line;
            symbol.type = FIP_SYM_DATA;

            if (extract_struct_signature(cursor, &symbol.sig.data) //
                && !symbol_name_exists(symbol.sig.data.name)       //
            ) {
                curr_coll->symbols[curr_coll->symbol_count] = symbol;
                fip_print(                                         //
                    ID, FIP_INFO, "Found struct: '%s' at line %d", //
                    symbol.sig.data.name, symbol.line_number       //
                );
                curr_coll->symbol_count++;
            }
            break;
        }
        case CXCursor_EnumDecl: {
            if (!clang_isCursorDefinition(cursor)) {
                return CXChildVisit_Continue;
            }

            if (curr_coll->symbol_count >= MAX_SYMBOLS) {
                fip_print(                                              //
                    ID, FIP_WARN,                                       //
                    "Maximum symbols reached, skipping remaining enums" //
                );
                return CXChildVisit_Break;
            }

            // Extract enum information
            fip_c_symbol_t symbol = {0};
            strncpy(                                //
                symbol.source_file_path, file_path, //
                sizeof(symbol.source_file_path) - 1 //
            );
            symbol.line_number = (int)line;
            symbol.type = FIP_SYM_ENUM;

            if (extract_enum_signature(cursor, &symbol.sig.enum_t) //
                && !symbol_name_exists(symbol.sig.enum_t.name)     //
            ) {
                curr_coll->symbols[curr_coll->symbol_count] = symbol;
                fip_print(                                       //
                    ID, FIP_INFO, "Found enum: '%s' at line %d", //
                    symbol.sig.enum_t.name, symbol.line_number   //
                );
                curr_coll->symbol_count++;
            }
            break;
        }
    }

    return CXChildVisit_Recurse;
}

void parse_c_file(char *c_file) {
    CXIndex index = clang_createIndex(0, 0);
    FILE *fp = popen("gcc -print-file-name=include", "r");
    char buf[512];
    if (fgets(buf, sizeof(buf), fp)) {
        buf[strcspn(buf, "\n")] = 0; // strip newline
        // buf now holds the include path, e.g.
        // "/usr/lib/gcc/x86_64-pc-linux-gnu/15.2.1/include"
    }
    pclose(fp);
    const char *args[] = {
        "-x",
        "c",
        "-std=gnu23",
        "-I",
        buf,
    };
    const size_t num_args = 5;
    fip_print(ID, FIP_DEBUG, "Clang Parse Arguments:");
    for (size_t i = 0; i < num_args; i++) {
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

    fip_print(                                  //
        ID, FIP_INFO, "Found %d symbols in %s", //
        curr_coll->symbol_count, c_file         //
    );
}

void handle_function_symbol_request(         //
    const fip_msg_t *message,                //
    fip_msg_symbol_response_t *const sym_res //
) {
    assert(message->u.sym_req.type == FIP_SYM_FUNCTION);
    const fip_sig_fn_t *msg_fn = &message->u.sym_req.sig.fn;
    fip_print_sig_fn(ID, msg_fn);
    sym_res->type = FIP_SYM_FUNCTION;

    bool sym_match = false;
    fip_print(ID, FIP_DEBUG, "symbol_list.count=%lu", symbol_list.count);
    for (size_t i = 0; i < symbol_list.count; i++) {
        fip_c_symbol_collection_t *const collection =
            &symbol_list.collection[i];
        fip_print(ID, FIP_DEBUG, "collection->symbol_count=%lu",
            collection->symbol_count);
        for (size_t j = 0; j < collection->symbol_count; j++) {
            const fip_c_symbol_t *symbol = &collection->symbols[j];
            if (symbol->type != FIP_SYM_FUNCTION) {
                continue;
            }
            fip_print(ID, FIP_DEBUG, "Checking function");
            fip_print_sig_fn(ID, &symbol->sig.fn);
            const fip_sig_fn_t *sym_fn = &symbol->sig.fn;
            if (strcmp(sym_fn->name, msg_fn->name) == 0 //
                && sym_fn->args_len == msg_fn->args_len //
                && sym_fn->rets_len == msg_fn->rets_len //
            ) {
                sym_match = true;
                // Now we need to check if the arg and ret types match
                for (uint32_t k = 0; k < sym_fn->args_len; k++) {
                    if (sym_fn->args[k].type.type !=
                            msg_fn->args[k].type.type ||
                        sym_fn->args[k].type.is_mutable !=
                            msg_fn->args[k].type.is_mutable) {
                        sym_match = false;
                    }
                }
                for (uint32_t k = 0; k < sym_fn->rets_len; k++) {
                    if (sym_fn->rets[k].type != msg_fn->rets[k].type ||
                        sym_fn->rets[k].is_mutable !=
                            msg_fn->rets[k].is_mutable) {
                        sym_match = false;
                    }
                }
                if (sym_match) {
                    // We found the requested symbol
                    collection->needed = true;
                    fip_clone_sig_fn(&sym_res->sig.fn, sym_fn);
                    memcpy(sym_res->sig.fn.name, sym_fn->name, 128);
                    break;
                }
            }
        }
        if (sym_match) {
            break;
        }
    }
    sym_res->found = sym_match;
}

void handle_data_symbol_request(             //
    const fip_msg_t *message,                //
    fip_msg_symbol_response_t *const sym_res //
) {
    assert(message->u.sym_req.type == FIP_SYM_DATA);
    const fip_sig_data_t *msg_data = &message->u.sym_req.sig.data;
    sym_res->type = FIP_SYM_DATA;

    bool sym_match = false;
    fip_print(ID, FIP_DEBUG, "symbol_list.count=%lu", symbol_list.count);
    for (size_t i = 0; i < symbol_list.count; i++) {
        fip_c_symbol_collection_t *const collection =
            &symbol_list.collection[i];
        fip_print(ID, FIP_DEBUG, "collection->symbol_count=%lu",
            collection->symbol_count);
        for (size_t j = 0; j < collection->symbol_count; j++) {
            const fip_c_symbol_t *symbol = &collection->symbols[j];
            if (symbol->type != FIP_SYM_DATA) {
                continue;
            }
            fip_print(ID, FIP_DEBUG, "Checking data");
            const fip_sig_data_t *sym_data = &symbol->sig.data;
            if (strcmp(sym_data->name, msg_data->name) == 0       //
                && sym_data->value_count == msg_data->value_count //
            ) {
                sym_match = true;
                // Check if field types match
                for (uint32_t k = 0; k < sym_data->value_count; k++) {
                    if (sym_data->value_types[k].type !=
                            msg_data->value_types[k].type ||
                        sym_data->value_types[k].is_mutable !=
                            msg_data->value_types[k].is_mutable) {
                        sym_match = false;
                    }
                }
                if (sym_match) {
                    // We found the requested symbol
                    collection->needed = true;
                    fip_clone_sig_data(&sym_res->sig.data, sym_data);
                    memcpy(sym_res->sig.data.name, sym_data->name, 128);
                    break;
                }
            }
        }
        if (sym_match) {
            break;
        }
    }
    sym_res->found = sym_match;
}

void handle_enum_symbol_request(             //
    const fip_msg_t *message,                //
    fip_msg_symbol_response_t *const sym_res //
) {
    assert(message->u.sym_req.type == FIP_SYM_ENUM);
    const fip_sig_enum_t *msg_enum = &message->u.sym_req.sig.enum_t;
    sym_res->type = FIP_SYM_ENUM;

    bool sym_match = false;
    fip_print(ID, FIP_DEBUG, "symbol_list.count=%lu", symbol_list.count);
    for (size_t i = 0; i < symbol_list.count; i++) {
        fip_c_symbol_collection_t *const collection =
            &symbol_list.collection[i];
        fip_print(ID, FIP_DEBUG, "collection->symbol_count=%lu",
            collection->symbol_count);
        for (size_t j = 0; j < collection->symbol_count; j++) {
            const fip_c_symbol_t *symbol = &collection->symbols[j];
            if (symbol->type != FIP_SYM_ENUM) {
                continue;
            }
            fip_print(ID, FIP_DEBUG, "Checking enum");
            const fip_sig_enum_t *sym_enum = &symbol->sig.enum_t;
            if (strcmp(sym_enum->name, msg_enum->name) == 0       //
                && sym_enum->type == msg_enum->type               //
                && sym_enum->value_count == msg_enum->value_count //
            ) {
                sym_match = true;
                // Check if values match
                for (uint32_t k = 0; k < sym_enum->value_count; k++) {
                    if (sym_enum->values[k] != msg_enum->values[k]) {
                        sym_match = false;
                    }
                }
                if (sym_match) {
                    // We found the requested symbol
                    collection->needed = true;
                    fip_clone_sig_enum(&sym_res->sig.enum_t, sym_enum);
                    memcpy(sym_res->sig.enum_t.name, sym_enum->name, 128);
                    break;
                }
            }
        }
        if (sym_match) {
            break;
        }
    }
    sym_res->found = sym_match;
}

void handle_symbol_request(    //
    char buffer[FIP_MSG_SIZE], //
    const fip_msg_t *message   //
) {
    assert(message->type == FIP_MSG_SYMBOL_REQUEST);
    fip_print(ID, FIP_INFO, "Symbol Request Received");
    // Create the response structure
    fip_msg_t response = {0};
    response.type = FIP_MSG_SYMBOL_RESPONSE;
    fip_msg_symbol_response_t *const sym_res = &response.u.sym_res;
    strncpy(sym_res->module_name, MODULE_NAME,
        sizeof(sym_res->module_name) - 1);
    sym_res->module_name[sizeof(sym_res->module_name) - 1] = '\0';

    switch (message->u.sym_req.type) {
        case FIP_SYM_UNKNOWN:
            fip_print(ID, FIP_DEBUG, "Not implemented yet");
            return;
        case FIP_SYM_FUNCTION:
            handle_function_symbol_request(message, sym_res);
            break;
        case FIP_SYM_DATA:
            handle_data_symbol_request(message, sym_res);
            break;
        case FIP_SYM_ENUM:
            handle_enum_symbol_request(message, sym_res);
            break;
    }
    fip_slave_send_message(ID, buffer, &response);
}

bool compile_module(                                  //
    uint8_t *path_count,                              //
    char paths[FIP_PATHS_SIZE],                       //
    fip_module_config_t *config,                      //
    [[maybe_unused]] const fip_msg_t *compile_message //
) {
    // Ensure .fip/cache directory exists
#ifdef __WIN32__
    if (CreateDirectoryA(".fip", NULL) ||
        GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!CreateDirectoryA(".fip/cache", NULL) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            fip_print(ID, FIP_ERROR, "Failed to create .fip/cache directory");
            return false;
        }
    } else {
        fip_print(ID, FIP_ERROR, "Failed to create .fip directory");
        return false;
    }
#else
    if (mkdir(".fip", 0755) != 0 && errno != EEXIST) {
        fip_print(ID, FIP_ERROR, "Failed to create .fip directory: %s",
            strerror(errno));
        return false;
    }
    if (mkdir(".fip/cache", 0755) != 0 && errno != EEXIST) {
        fip_print(ID, FIP_ERROR, "Failed to create .fip/cache directory: %s",
            strerror(errno));
        return false;
    }
#endif
    // TODO: Use the target information from the compile_message
    // compile_message->u.com_req.target

    // Check if the hash is already part of the paths, if it is we already
    // compiled the module
    const char *const hash = config->output;
    if (strstr(paths, hash)) {
        return true;
    }

    size_t command_size = 0;
    for (size_t i = 0; i < config->command_len; i++) {
        command_size += strlen(config->command[i]) + 1;
    }
    char *const command = (char *)malloc(command_size);
    size_t idx = 0;
    for (size_t i = 0; i < config->command_len; i++) {
        const size_t len = strlen(config->command[i]);
        memcpy(command + idx, config->command[i], len);
        idx += len;
        command[idx] = ' ';
        idx++;
    }
    command[command_size - 1] = '\0';
    fip_print(ID, FIP_INFO, "Executing: %s", command);

    char *compile_output = NULL;
    int exit_code = fip_execute_and_capture(&compile_output, command);
    if (exit_code != 0) {
        if (compile_output && compile_output[0]) {
            fip_print(ID, FIP_ERROR, "%s", compile_output);
        }
        fip_print(ID, FIP_ERROR,                              //
            "Compiling module '%s' failed with exit code %d", //
            config->tag, exit_code                            //
        );
        free(compile_output);
        free(command);
        return false;
    } else {
        if (compile_output && compile_output[0]) {
            fip_print(ID, FIP_INFO, "%s", compile_output);
        }
        free(compile_output);
        free(command);
    }

    fip_print(ID, FIP_INFO, "Compiled '%s' successfully", hash);
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

    // We need to go through all modules and see whether they need to be
    // compiled
    for (size_t i = 0; i < symbol_list.count; i++) {
        fip_c_symbol_collection_t *const coll = &symbol_list.collection[i];
        if (!coll->needed) {
            continue;
        }
        if (!compile_module(                          //
                &obj_res->path_count, obj_res->paths, //
                &CONFIGS.configs[i], message)         //
        ) {
            obj_res->has_obj = false;
            obj_res->compilation_failed = true;
            break;
        }
        obj_res->has_obj = true;
    }

    fip_slave_send_message(ID, buffer, &response);
}

void handle_tag_request(       //
    char buffer[FIP_MSG_SIZE], //
    const fip_msg_t *message   //
) {
    assert(message->type == FIP_MSG_TAG_REQUEST);
    fip_print(ID, FIP_INFO, "Tag Request Received");
    const char *msg_tag = message->u.tag_req.tag;

    fip_msg_t response = {0};
    response.type = FIP_MSG_TAG_PRESENT_RESPONSE;

    size_t coll_id = 0;
    bool is_present = false;
    for (; coll_id < symbol_list.count; coll_id++) {
        fip_c_symbol_collection_t *coll = &symbol_list.collection[coll_id];
        if (strcmp(coll->tag, msg_tag) == 0) {
            is_present = true;
            break;
        }
    }
    response.u.tag_pres_res.is_present = is_present;
    fip_slave_send_message(ID, buffer, &response);
    fip_free_msg(&response);

    if (!is_present) {
        return;
    }

    // Wait for `FIP_MSG_TAG_NEXT_SYMBOL_REQUEST` from master to tell us that it
    // wants to have the next symbol. We simply ping-pong between the master and
    // the slave. The master requests new symbols until we send it an empy
    // symbol as we reached the end of the list. If we reached the end of the
    // list then the slave will automatically fall back to it's normal
    // execution, the master does not need to send any other message to us in
    // this case.
    fip_c_symbol_collection_t *const coll = &symbol_list.collection[coll_id];
    coll->needed = true;
    for (size_t i = 0; i < coll->symbol_count; i++) {
        fip_print(ID, FIP_INFO, "Sending symbol %u/%u", i, coll->symbol_count);
        // Wait for master to request the next symbol
        while (!fip_slave_receive_message(buffer)) {
            fip_print(ID, FIP_WARN, "No message from master yet...");
        }
        fip_msg_t next_message = {0};
        fip_decode_msg(buffer, &next_message);
        switch (next_message.type) {
            default:
                fip_print(ID, FIP_ERROR,
                    "Unexpected message from master: %s, expected %s",
                    fip_msg_type_str[next_message.type],
                    fip_msg_type_str[FIP_MSG_TAG_NEXT_SYMBOL_REQUEST]);
                fip_free_msg(&next_message);
                return;
            case FIP_MSG_TAG_NEXT_SYMBOL_REQUEST:
                break;
        }
        fip_c_symbol_t *const sym = &coll->symbols[i];
        response = (fip_msg_t){0};
        response.type = FIP_MSG_TAG_SYMBOL_RESPONSE;
        response.u.tag_sym_res.is_empty = false;
        response.u.tag_sym_res.type = sym->type;
        switch (sym->type) {
            case FIP_SYM_UNKNOWN:
                continue;
            case FIP_SYM_FUNCTION:
                fip_clone_sig_fn(                   //
                    &response.u.tag_sym_res.sig.fn, //
                    &sym->sig.fn                    //
                );
                break;
            case FIP_SYM_DATA:
                fip_clone_sig_data(                   //
                    &response.u.tag_sym_res.sig.data, //
                    &sym->sig.data                    //
                );
                break;
            case FIP_SYM_ENUM:
                fip_clone_sig_enum(                     //
                    &response.u.tag_sym_res.sig.enum_t, //
                    &sym->sig.enum_t                    //
                );
                break;
        }
        // Send the next symbol to the master
        fip_slave_send_message(ID, buffer, &response);
        fip_free_msg(&response);
    }
    // Wait for master to request the next symbol before sending the empty
    // symbol to it
    while (!fip_slave_receive_message(buffer)) {
        fip_print(ID, FIP_WARN, "No message from master yet...");
    }
    // Only print the first time we receive a message
    fip_msg_t next_message = {0};
    fip_decode_msg(buffer, &next_message);
    switch (next_message.type) {
        default:
            fip_print(ID, FIP_ERROR,
                "Unexpected message from master: %s, expected %s",
                fip_msg_type_str[next_message.type],
                fip_msg_type_str[FIP_MSG_TAG_NEXT_SYMBOL_REQUEST]);
            fip_free_msg(&next_message);
            return;
        case FIP_MSG_TAG_NEXT_SYMBOL_REQUEST:
            break;
    }
    // Send "end of list" message
    response = (fip_msg_t){0};
    response.type = FIP_MSG_TAG_SYMBOL_RESPONSE;
    response.u.tag_sym_res.is_empty = true;
    response.u.tag_sym_res.type = FIP_SYM_UNKNOWN;
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
        LOG_LEVEL = (fip_log_level_e)strtoul(log_str, &endptr, 10);
    }
    fip_print(ID, FIP_INFO, "starting...");

    char msg_buf[FIP_MSG_SIZE] = {0};

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

    symbol_list.count = CONFIGS.count;
    symbol_list.collection = (fip_c_symbol_collection_t *)malloc( //
        sizeof(fip_c_symbol_collection_t) * CONFIGS.count         //
    );

    // Print all tags of the config and all headers and the command of it
    for (size_t i = 0; i < CONFIGS.count; i++) {
        fip_module_config_t *config = &CONFIGS.configs[i];
        curr_coll = &symbol_list.collection[i];
        strcpy(curr_coll->tag, config->tag);
        curr_coll->symbol_count = 0;

        fip_print(ID, FIP_DEBUG, "[%s]", config->tag);
        for (size_t j = 0; j < config->headers_len; j++) {
            fip_print(ID, FIP_DEBUG, "headers[%lu]: %s", j, config->headers[j]);
            fip_print(                                                      //
                ID, FIP_DEBUG, "parsing header '%s'...", config->headers[j] //
            );
            clock_t start = clock();
            parse_c_file(config->headers[j]);
            clock_t end = clock();
            double parse_time = ((double)(end - start)) / CLOCKS_PER_SEC;
            fip_print(ID, FIP_DEBUG, "parsing '%s' took %f s",
                config->headers[j], parse_time);
        }
        for (size_t j = 0; j < config->command_len; j++) {
            fip_print(ID, FIP_DEBUG, "command[%lu]: %s", j, config->command[j]);
        }
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
                case FIP_MSG_TAG_REQUEST:
                    handle_tag_request(msg_buf, &message);
                    break;
                case FIP_MSG_TAG_PRESENT_RESPONSE:
                    // The slave should not receive a message it sends
                    assert(false);
                    break;
                case FIP_MSG_TAG_NEXT_SYMBOL_REQUEST:
                    // We should never get this message in the main loop. It can
                    // only be recieved when we are in the `handle_tag_request`
                    // function
                    assert(false);
                    break;
                case FIP_MSG_TAG_SYMBOL_RESPONSE:
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
