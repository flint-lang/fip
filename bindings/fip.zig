const std = @import("std");
const toml = @import("toml");
const defines = @import("defines");

pub const MAJOR = 0;
pub const MINOR = 4;
pub const PATCH = 1;

pub const MSG_SIZE = 4096;

pub const MAX_MODULE_NAME_LEN = 16;
pub const PATH_SIZE = 8;
pub const PATHS_SIZE = MSG_SIZE - 32;

pub extern var LOG_LEVEL: LogLevel;

/// Flexible arrays, just like in good old C
pub fn FlexibleArray(comptime T: type) type {
    return struct {
        len: usize,
        _value: [0]T,

        const Self = @This();

        pub fn value(self: *Self) []T {
            const ptr: [*c]T = @ptrCast(@alignCast(&self.*._value));
            return ptr[0..self.len];
        }

        pub fn create(len: usize) !*Self {
            const mem: *anyopaque = std.c.malloc(@sizeOf(Self) + @sizeOf(T) * len) orelse
                return error.OOM;
            const a: *Self = @ptrCast(@alignCast(mem));
            a.len = len;
            return a;
        }

        pub fn destroy(self: *Self) void {
            std.c.free(self);
        }

        comptime {
            std.debug.assert(@sizeOf(Self) == @sizeOf(usize));
        }
    };
}

pub fn EnumFromExternUnion(comptime T: type, comptime backing_int: type) type {
    comptime switch (@typeInfo(T)) {
        .@"union" => {},
        else => @compileError("Doesnt work on non-unions"),
    };
    const union_type = @typeInfo(T).@"union";
    comptime if (union_type.layout != .@"extern") {
        @compileError("Doesnt work on non-extern unions");
    };
    const field_names = std.meta.fieldNames(T);
    return @Enum(backing_int, .exhaustive, field_names, &std.simd.iota(backing_int, field_names.len));
}

/// Enum of all possible log levels of FIP
pub const LogLevel = enum(u8) {
    none,
    @"error",
    warn,
    info,
    debug,
    trace,
};

/// The struct representing a type in FIP
pub const Type = extern struct {
    is_mutable: bool = false,
    tag: Tag,
    u: U,

    pub const Tag = EnumFromExternUnion(U, u8);
    pub const U = extern union {
        primitive: Primitive,
        pointer: Pointer,
        @"struct": Struct,
        recursive: Recursive,
        @"enum": Enum,
        array: Array,
        @"opaque": Opaque,
    };

    /// Enum of all possible primitive types supported by FIP
    pub const Primitive = enum(u8) {
        /// void
        void,
        /// unsigned char
        u8,
        /// unsigned short
        u16,
        /// unsigned int
        u32,
        /// unsigned long
        u64,
        /// char
        i8,
        /// short
        i16,
        /// int
        i32,
        /// long
        i64,
        /// float
        f32,
        /// double
        f64,
        /// bool (byte)
        bool,
        /// char*
        str,
    };

    /// The struct representing a pointer type
    pub const Pointer = extern struct {
        base_type: *Type,
    };

    /// The struct representing a struct type
    pub const Struct = extern struct {
        name: [128]u8 = @splat(0),
        field_count: usize = 0,
        fields: [*]Type = undefined,
    };

    /// The struct representing recursive / repeating types
    pub const Recursive = extern struct {
        levels_back: u8 = 0,
    };

    /// The struct representing enum types
    pub const Enum = extern struct {
        name: [128]u8 = @splat(0),
        bit_width: u8 = 0,
        is_signed: bool = false,
        value_count: usize = 0,
        /// The value is a size_t because it can be anything from i1 up to an u64 / i64.
        /// The underlying enum type can differ
        values: [*]usize = undefined,
    };

    /// The struct representing fixed-size arrays
    pub const Array = extern struct {
        size: usize = 0,
        base_type: *Type,
    };

    /// @brief The struct representing a named opaque type
    pub const Opaque = extern struct {
        name: [128]u8 = @splat(0),
    };

    pub const print = print_type;
    pub const clone = clone_type;
    pub const free = free_type;
};

/// Union representing a single FIP symbol signature
pub const Signature = extern struct {
    tag: Tag = .unknown,
    u: U = .{ .unknown = {} },

    pub const Tag = EnumFromExternUnion(U, u8);
    pub const U = extern union {
        unknown: void,
        @"fn": Fn,
        data: Data,
        @"enum": Enum,
        @"opaque": Opaque,
    };

    /// Struct representing the signature of a FIP-defined function
    pub const Fn = extern struct {
        name: [128]u8 = @splat(0),
        args_len: usize = 0,
        args: [*]Arg = undefined,
        rets_len: usize = 0,
        rets: [*]Type = undefined,

        /// Struct representing a single arugment of a FIP-defined function
        pub const Arg = extern struct {
            name: [128]u8 = @splat(0),
            type: Type,
        };

        pub const print = print_sig_fn;
        pub const clone = clone_sig_fn;
    };

    pub const Data = extern struct {
        name: [128]u8 = @splat(0),
        field_count: usize = 0,
        fields: [*]Field = undefined,

        pub const Field = extern struct {
            name: [128]u8 = @splat(0),
            type: Type,
        };

        pub const print = print_sig_data;
        pub const clone = clone_sig_data;
    };

    /// Struct representing the signature of a FIP-defined enum
    pub const Enum = extern struct {
        name: [128]u8 = @splat(0),
        type: Type.Primitive,
        value_count: usize = 0,
        values: [*]Value = undefined,

        /// Struct representing a single enum value
        pub const Value = extern struct {
            tag: [128]u8 = @splat(0),
            /// The value is a size_t because it can be anything from i1 up to an u64 / i64.
            /// The underlying enum type can differ
            value: usize = 0,
        };

        pub const print = print_sig_enum;
        pub const clone = clone_sig_enum;
    };

    /// Struct representing the signature of a FIP-defined named opaque type
    pub const Opaque = extern struct {
        name: [128]u8 = @splat(0),

        pub const print = print_sig_opaque;
        pub const clone = clone_sig_opaque;
    };

    pub fn print(self: *const Signature) void {
        switch (self.tag) {
            .unknown => void,
            .@"fn" => |@"fn"| @"fn".print(),
            .data => |data| data.print(),
            .@"enum" => |@"enum"| @"enum".print(),
            .@"opaque" => |@"opaque"| @"opaque".print(),
        }
    }

    pub fn clone(self: *const Signature) void {
        switch (self.tag) {
            .unknown => void,
            .@"fn" => |@"fn"| @"fn".clone(),
            .data => |data| data.clone(),
            .@"enum" => |@"enum"| @"enum".clone(),
            .@"opaque" => |@"opaque"| @"opaque".clone(),
        }
    }
};

/// Struct representing a list of signatures
pub const SignatureList = FlexibleArray(Signature);

/// Union representing a single FIP IPC message
pub const Message = extern struct {
    tag: Tag = .unknown,
    u: U = .{ .unknown = {} },

    pub const Tag = EnumFromExternUnion(U, u8);
    pub const U = extern union {
        /// Unknown message
        unknown: void,
        /// Slave trying to connect to master
        connect_request: ConnectRequest,
        /// Master requesting symbol resolution
        symbol_request: SymbolRequest,
        /// Slave response of FN_REQ
        symbol_response: SymbolResponse,
        /// Master requesting all slaves to compile
        compile_request: CompileRequest,
        /// Slave responding compilation with .o file
        object_response: ObjectResponse,
        /// The master requests that each IM searches for a certain tag and the symbols from that tag
        tag_request: TagRequest,
        /// The IM's response to the request. It sends whether it contains that searched-for tag.
        /// If it contains this tag then it will send all the tag symbol responses it contains.
        /// The master then reads every single sent tag symbol response of the IM
        tag_present_response: TagPresentResponse,
        /// The master request a single IM to give it the next symbol in it's symbol list of
        /// the previously requested module tag. The slave will respond with symbol responses
        /// until it reaches the end of the list, responding with an empty symbol response
        tag_next_symbol_request: void,
        /// The IM's response to the tag request. It sends one symbol at a time and
        /// whether that was the last symbol it provides
        tag_symbol_response: TagSymbolResponse,
        /// Kill command comes last
        kill: Kill,
    };

    /// Struct representing all information from a connection request
    pub const ConnectRequest = extern struct {
        setup_ok: bool = 0,
        version: extern struct {
            major: u8 = 0,
            minor: u8 = 0,
            patch: u8 = 0,
        },
        module_name: [MAX_MODULE_NAME_LEN]u8 = @splat(0),
    };

    /// Struct representing the symbol request message
    pub const SymbolRequest = extern struct {
        sig: Signature,
    };

    /// Struct representing the symbol response message
    pub const SymbolResponse = extern struct {
        found: bool = false,
        module_name: [MAX_MODULE_NAME_LEN]u8 = @splat(0),
        sig: Signature,
    };

    /// Struct representing the compile request message
    pub const CompileRequest = extern struct {
        target: extern struct {
            arch: [16]u8 = @splat(0),
            sub: [16]u8 = @splat(0),
            vendor: [16]u8 = @splat(0),
            sys: [16]u8 = @splat(0),
            abi: [16]u8 = @splat(0),
        },
    };

    /// Struct representing the object response message
    pub const ObjectResponse = extern struct {
        has_obj: bool = false,
        compilation_failed: bool = false,
        module_name: [MAX_MODULE_NAME_LEN]u8 = @splat(0),
        path_count: usize = 0,
        paths: [PATHS_SIZE]u8 = @splat(0),
    };

    /// Struct representing the tag request message
    pub const TagRequest = extern struct {
        tag: [128]u8 = @splat(0),
    };

    /// Struct representing the tag present response message
    pub const TagPresentResponse = extern struct {
        is_present: bool = false,
    };

    /// Struct representing the tag symbol response message
    pub const TagSymbolResponse = extern struct {
        is_empty: bool = false,
        sig: Signature,
    };

    /// Struct representing the kill message
    pub const Kill = extern struct {
        reason: enum(u8) {
            finish,
            version_mismatch,
        },
    };

    pub const print = print_msg;
    pub const free = free_msg;
    pub const encode = encode_msg;
    pub const decode = decode_msg;
};

// =====================
// GENERAL FUNCTIONALITY
// =====================

/// @function `fip_print`
/// @brief Prints a message from the given module ID from the format string and
/// the variadic arguments which will be forwarded to printf
///
/// @param `id` The id of the process to print the message from
/// @param `log_level` The log level of the current message to print
/// @param `format` The format string of the printed message
/// @param `...` The variadic values to put into the formatted output
extern fn fip_print(id: u32, log_level: LogLevel, format: [*c]const u8, ...) void;
pub const print = fip_print;

/// @function `fip_print_msg`
/// @brief Prints the given message from the given module ID
///
/// @param `message` The message to print
/// @param `id` The id of the process to print the message from
extern fn fip_print_msg(message: *const Message, id: u32) void;
pub const print_msg = fip_print_msg;

/// @function `fip_encode_msg`
/// @brief Encodes a given message into a string and stores it in the internal buffer
///
/// @param `message` The message to encode into the internal buffer
extern fn fip_encode_msg(message: *const Message) void;
pub const encode_msg = fip_encode_msg;

/// @function `fip_decode_msg`
/// @brief Tries to decode a message from the internal buffer and create a message
/// from it
///
/// @param `message` Pointer to the message where the result is stored
extern fn fip_decode_msg(message: *Message) void;
pub const decode_msg = fip_decode_msg;

/// @function `fip_free_type`
/// @brief Frees the given type
///
/// @param `type` The type to free
extern fn fip_free_type(@"type": *Type) void;
pub const free_type = fip_free_type;

/// @function `fip_free_msg`
/// @brief Frees a given message
///
/// @param `message` The message to free
extern fn fip_free_msg(message: *Message) void;
pub const free_msg = fip_free_msg;

/// @function `fip_free_sig_list`
/// @brief Frees a given signature list
///
/// @param `list` The list to free
extern fn fip_free_sig_list(list: *SignatureList) void;
pub const free_sig_list = fip_free_sig_list;

/// @function `fip_create_hash`
/// @brief Creates a 8 Byte character hash from the given file path to make
/// differentiating between different files predictable in size. Each character
/// in the 8 character hash is one of 63 possible characters (A-Z, a-z, 0-9, _)
/// which means that the 8 Byte hash can have roughly as many unique hashes as a
/// equivalent 48 bit number.
///
/// @param `hash` The buffer in which to write the hash
/// @param `file_path` The file path to turn into a 8 Byte hash
extern fn fip_create_hash(hash: *[PATH_SIZE]u8, file_path: [*c]const u8) void;
pub const create_hash = fip_create_hash;

/// @function `fip_print_type`
/// @brief "Prints" a given type into the internal buffer which then can be used to print
/// the whole type in one fip_print call
///
/// @param `type` The type to print into the internal buffer
extern fn fip_print_type(@"type": *const Type) void;
pub const print_type = fip_print_type;

/// @function `fip_print_sig_fn`
/// @brief Prints a parsed function signature to the console
///
/// @param `sig` The function signature to print
/// @param `id` The id of the process in which the signature is printed
extern fn fip_print_sig_fn(sig: *const Signature.Fn, id: u32) void;
pub const print_sig_fn = fip_print_sig_fn;

/// @function `fip_print_sig_data`
/// @brief Prints a parsed data definition signature to the console
///
/// @param `sig` The data signature to print
/// @param `id` The id of the process in which the signature is printed
extern fn fip_print_sig_data(sig: *const Signature.Data, id: u32) void;
pub const print_sig_data = fip_print_sig_data;

/// @function `fip_print_sig_enum`
/// @brief Prints a parsed enum definition signature to the console
///
/// @param `sig` The enum signature to print
/// @param `id` The id of the process in which the signature is printed
extern fn fip_print_sig_enum(sig: *const Signature.Enum, id: u32) void;
pub const print_sig_enum = fip_print_sig_enum;

/// @function `fip_print_sig_opaque`
/// @brief Prints a parsed opaque type signature to the console
///
/// @param `sig` The opaque signature to print
/// @param `id` The id of the process in which the signature is printed
extern fn fip_print_sig_opaque(sig: *const Signature.Opaque, id: u32) void;
pub const print_sig_opaque = fip_print_sig_opaque;

/// @function `fip_clone_sig_fn`
/// @brief Clones a given function signature from the source to the destination
///
/// @param `src` The source to clone
/// @param `dest` The signature to fill
extern fn fip_clone_sig_fn(src: *const Signature.Fn, dest: *Signature.Fn) void;
pub const clone_sig_fn = fip_clone_sig_fn;

/// @function `fip_clone_sig_data`
/// @brief Clones a given data signature from the source to the destination
///
/// @param `src` The source to clone
/// @param `dest` The signature to fill
extern fn fip_clone_sig_data(src: *const Signature.Data, dest: *Signature.Data) void;
pub const clone_sig_data = fip_clone_sig_data;

/// @function `fip_clone_sig_enum`
/// @brief Clones a given enum signature from the source to the destination
///
/// @param `src` The source to clone
/// @param `dest` The signature to fill
extern fn fip_clone_sig_enum(src: *const Signature.Enum, dest: *Signature.Enum) void;
pub const clone_sig_enum = fip_clone_sig_enum;

/// @function `fip_clone_sig_opaque`
/// @brief Clones a given opaque signature from the source to the destination
///
/// @param `src` The source to clone
/// @param `dest` The signature to fill
extern fn fip_clone_sig_opaque(src: *const Signature.Opaque, dest: *Signature.Opaque) void;
pub const clone_sig_opaque = fip_clone_sig_opaque;

/// @function `fip_clone_type`
/// @brief Clones a given type from the source to the destination
///
/// @param `src` The source to clone
/// @param `dest` The type to fill
extern fn fip_clone_type(src: *const Type, dest: *Type) void;
pub const clone_type = fip_clone_type;

/// @function `fip_execute_and_capture`
/// @brief Executes the given command and captures both stdout and stderr in the
/// output string. Returns the exit code of the executed command
///
/// @param `output` The output parameter where the output of the command gets written to
/// @param `command` The command to execute
/// @return `int` The exit code of the executed command
extern fn fip_execute_and_caputre(output: *allowzero [*c]const u8, command: [*c]const u8) c_int;
pub const execute_and_capture = fip_execute_and_caputre;

pub const master = if (defines.lib_mode != .master) @compileError("lib_mode != .master, master namespace unavailable") else struct {
    pub const MAX_ENABLED_MODULES = 16;
    pub const MAX_SLAVES = 64;

    extern var master_state: State;

    /// A list of all active interop modules spawned by the master
    pub const InteropModules = extern struct {
        active_count: u8,
        pids: [MAX_SLAVES]std.c.pid_t,
    };

    /// The structure containing the whole state of the entire master
    pub const State = extern struct {
        slave_stdin: [MAX_SLAVES]*std.c.FILE,
        slave_stdout: [MAX_SLAVES]*std.c.FILE,
        slave_stderr: [MAX_SLAVES]*std.c.FILE,
        slave_count: u32,
        reponses: [MAX_SLAVES]Message,
        response_count: u32,
    };

    /// A simple enum desciring the exit code of the `fip_master_tag_request` function
    pub const TagRequestStatus = enum(u8) {
        ok,
        err_faulty,
        err_unknown_tag,
        err_ambiguous_tag,
        err_write,
    };

    /// A structure containing the result type of the `fip_master_tag_request` function being an exit status and a list of symbols
    pub const TagRequestResult = extern struct {
        status: TagRequestStatus,
        list: *SignatureList,
    };

    /// The structure containing the results of the parsed toml file
    pub const Config = extern struct {
        ok: bool,
        enabled_modules: [MAX_ENABLED_MODULES][MAX_MODULE_NAME_LEN]u8,
        enabled_count: u8,
    };

    /// @function `fip_copy_stream_lines`
    /// @brief Copies all lines from the `src` stream into the `dest` stream. Only
    /// copies full lines, and copies the lines as one unit. So, all leftover
    /// characters are, well, left, in the `src` and only full lines are written to
    /// `dest`
    ///
    /// @param `src` The source stream from which lines are read
    /// @param `dest` The destination stream to which lines are copied to
    extern fn fip_copy_stream_lines(src: *std.c.FILE, dest: *std.c.FILE) void;
    pub const copy_stream_lines = fip_copy_stream_lines;

    /// @function `fip_print_slave_streams`
    /// @brief Prints all the `stderr` streams from all slaves into the `stderr`
    /// stream of the master, to gather all the debug output from all slaves
    extern fn fip_print_slave_streams() void;
    pub const print_slave_streams = fip_print_slave_streams;

    /// @function `fip_spawn_interop_module`
    /// @brief Creates a new interop module and adds it's process ID to the list of
    /// modules in the interop modules parameter
    ///
    /// @param `modules` A pointer to the structure containing all the module PIDs
    /// @param `root_path` The root path of the project (the directory where the
    /// .fip directory is contained). The `module` program will be started in this
    /// directory
    /// @param `module` The interop module to start
    /// @return `bool` Whether the interop module process creation was successful
    extern fn fip_spawn_interop_module(modules: *InteropModules, root_path: [*c]const u8, module: [*c]const u8) bool;
    pub const spawn_interop_module = fip_spawn_interop_module;

    /// @function `fip_terminate_all_slaves`
    /// @brief Terminates all currently running slaves if they have not been
    /// terminated yet
    ///
    /// @param `modules` A pointer to the structure containing all the module PIDs
    extern fn fip_terminate_all_slaves(modules: *InteropModules) void;
    pub const terminate_all_slaves = fip_terminate_all_slaves;

    /// @function `fip_master_init`
    /// @brief Initializes the master for stdio-based communication
    ///
    /// @param `modules` The interop modules structure containing slave PIDs
    /// @return `bool` Whether initialization was successful
    extern fn fip_master_init(modules: *InteropModules) bool;
    pub const init = fip_master_init;

    /// @function `fip_master_broadcast_message`
    /// @brief Broadcasts a given message to stdout
    ///
    /// @param `message` The message to send
    extern fn fip_master_broadcast_message(message: *const Message) void;
    pub const broadcast_message = fip_master_broadcast_message;

    /// @function `fip_master_await_responses`
    /// @brief Waits for all slaves to respond with a message from stdin
    ///
    /// @param `responses` The responses of all slaves where the ID of the response
    /// in the array corresponds to the ID of the slave itself
    /// @param `response_count` How many responses we got
    /// @param `expected_msg_type` The type of the expected message
    /// @return `uint8_t` How many responses were faulty (unable to be read) or had
    /// the wrong type
    extern fn fip_master_await_responses(
        responses: *[MAX_SLAVES]Message,
        response_count: *u32,
        expected_msg_type: Message.Tag,
    ) u8;
    pub const await_responses = fip_master_await_responses;

    /// @function `fip_master_symbol_request`
    /// @brief Broadcasts a symbol request message and then awaits all
    /// symbol response messages and returns whether the requested
    /// symbol was found
    ///
    /// @param `message` The symbol request message to send
    /// @return `bool` Whether the requested symbol was found
    ///
    /// @note This function asserts the message type to be FIP_MSG_SYMBOL_REQUEST
    extern fn fip_master_symbol_request(message: *const Message) bool;
    pub const symbol_request = fip_master_symbol_request;

    /// @function `fip_master_compile_request`
    /// @brief Broadcasts a compile request message and then awaits
    /// all object response messages and returns whether all modules
    /// were able to compile their sources
    ///
    /// @param `message` The compile request message to send
    /// @return `bool` Whether all interop modules were able to compile their
    /// source files
    ///
    /// @note This function asserts the message type to be FIP_MSG_COMPILE_REQUEST
    extern fn fip_master_compile_request(message: *const Message) bool;
    pub const compile_request = fip_master_compile_request;

    /// @function `fip_master_tag_request`
    /// @brief Broadcasts a tag request message and then collects all the symbols of
    /// all interop modules
    ///
    /// @param `message` The tag request message to send
    /// @return `fip_sig_list_t *` A list of all collected signatures from the tag
    ///
    /// @note This function asserts the message type to be FIP_MSG_TAG_REQUEST
    extern fn fip_master_tag_request(message: *const Message) TagRequestResult;
    pub const tag_request = fip_master_tag_request;

    /// @function `fip_master_receive_message_from`
    /// @brief Reads a message from stdin from a given IM id and stores it in the internal buffer
    ///
    /// @param `id` The id of the slave to get the message from
    /// @return `bool` Whether a message was recieved
    extern fn fip_master_receive_message_from(id: u32) bool;
    pub const recieve_message_from = fip_master_receive_message_from;

    /// @function `fip_master_send_message_to`
    /// @brief Sends a message to the stdout of a given interop module
    ///
    /// @param `message` The message which will be sent
    /// @param `id` The id of the slave to send the message to
    extern fn fip_master_send_message_to(message: *const Message, id: u32) void;
    pub const send_message_to = fip_master_send_message_to;

    /// @function `fip_master_cleanup`
    /// @brief Cleans up the master
    extern fn fip_master_cleanup() void;
    pub const cleanup = fip_master_cleanup;

    /// @function `fip_master_load_config`
    /// @brief Loads the master config from the file at the `config_path`
    ///
    /// @param `config_path` The path to the `fip.toml` config file located in
    /// `<ProjectPath>/.fip/config/.toml`
    /// @return `fip_master_config_t` The loaded configuration
    extern fn fip_master_load_config(config_path: [*c]const u8) Config;
    pub const load_config = fip_master_load_config;
};

pub const slave = if (defines.lib_mode != .slave) @compileError("lib_mode != .slave, slave namespace unavailable") else struct {
    /// @function `fip_slave_init`
    /// @brief Initializes the slave for stdio-based communication with named pipes
    ///
    /// @param `slave_id` The ID of this slave process
    /// @return `bool` Whether initialization was successful
    extern fn fip_slave_init(slave_id: u32) bool;
    pub const init = fip_slave_init;

    /// @function `fip_slave_receive_message`
    /// @brief Reads a message from stdin and stores it in the internal buffer
    ///
    /// @return `bool` Whether a message was recieved
    extern fn fip_slave_receive_message() bool;
    pub const recieve_message = fip_slave_receive_message;

    /// @function `fip_slave_send_message`
    /// @brief Sends a message to stdout
    ///
    /// @param `message` The message which will be sent
    /// @param `id` The id of the slave who tries to send the message
    extern fn fip_slave_send_message(message: *const Message, id: u32) void;
    pub const send_message = fip_slave_send_message;

    /// @function `fip_slave_cleanup`
    /// @brief Cleans up the slave
    extern fn fip_slave_cleanup() void;
    pub const cleanup = fip_slave_cleanup;

    /// @function `fip_slave_load_config`
    /// @brief Loads the slave config from the `.fip/config/X.toml` where `X` is
    /// the name of the module (for example `fip-c`)
    ///
    /// @param `id` The ID of the slave that tries to load the config
    /// @param `module_name` The name of the module to load
    /// @return `toml_result_t` The loaded configuration toml file. Interpreting the
    /// content of this file is the responsibility of each interop module itself,
    /// the FIP protocol itself stays purely language-independant
    extern fn fip_slave_load_config(id: u32, module_name: [*c]const u8) toml.toml_result_t;
    pub const load_config = fip_slave_load_config;
};

test "test" {
    inline for (comptime std.meta.declarations(@This())) |decl| {
        if (comptime std.mem.eql(u8, decl.name, "master") or
            std.mem.eql(u8, decl.name, "slave"))
        {
            continue;
        }
        _ = &@field(@This(), decl.name);
    }

    if (defines.lib_mode == .master) {
        std.testing.refAllDecls(master);
    } else if (defines.lib_mode == .slave) {
        std.testing.refAllDecls(slave);
    }
}
