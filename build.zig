const std = @import("std");

const FIP_VERSION = @import("build.zig.zon").version;
const DEFAULT_LLVM_VERSION = "llvmorg-21.1.8";

pub fn build(b: *std.Build) !void {
    const OSTag = enum { linux, windows };
    _ = b.findProgram(&.{"git"}, &.{}) catch @panic("Git not found on this system");
    _ = b.findProgram(&.{"cmake"}, &.{}) catch @panic("CMake not found on this system");
    _ = b.findProgram(&.{"ninja"}, &.{}) catch @panic("Ninja not found on this system");
    _ = b.findProgram(&.{"python"}, &.{}) catch @panic("Python3 not found on this system");
    _ = b.findProgram(&.{"ld.lld"}, &.{}) catch @panic("LLD not found on this system");

    const host_target = b.resolveTargetQuery(.{});
    const optimize = b.standardOptimizeOption(.{});

    const llvm_version = b.option([]const u8, "llvm-version", b.fmt("LLVM version to use. Default: {s}", .{DEFAULT_LLVM_VERSION})) orelse
        DEFAULT_LLVM_VERSION;
    const force_llvm_rebuild = b.option(bool, "rebuild-llvm", "Force rebuild LLVM") orelse
        false;
    const jobs = b.option(usize, "jobs", "Number of cores to use for building LLVM") orelse
        (try std.Thread.getCpuCount() - 2);
    const target_option: OSTag = b.option(OSTag, "target", "The OS to build for") orelse
        switch (host_target.result.os.tag) {
            .linux => .linux,
            .windows => .windows,
            else => @panic("Unsupported OS"),
        };

    const target = b.resolveTargetQuery(switch (target_option) {
        .linux => .{
            .cpu_arch = .x86_64,
            .cpu_model = .baseline,
            .os_tag = .linux,
            .os_version_min = .{ .semver = .{ .major = 6, .minor = 12, .patch = 0 } },
            .abi = .gnu,
            .glibc_version = .{ .major = 2, .minor = 40, .patch = 0 },
        },
        .windows => .{
            .cpu_arch = .x86_64,
            .cpu_model = .baseline,
            .os_tag = .windows,
            .os_version_min = .{ .windows = .win7 },
            .abi = .gnu,
            .glibc_version = .{ .major = 2, .minor = 40, .patch = 0 },
        },
    });

    // Update LLVM
    const update_llvm = try updateLLVM(b, llvm_version);
    // Build LLVM
    const build_llvm = try buildLLVM(b, &update_llvm.step, target, force_llvm_rebuild, jobs);
    // Build fip-c exe
    try buildFipC(b, &build_llvm.step, target, optimize);
    // Build examples
    try buildExamples(b, target, optimize);
}

fn buildFipC(b: *std.Build, previous_step: *std.Build.Step, target: std.Build.ResolvedTarget, optimize: std.builtin.OptimizeMode) !void {
    const exe = b.addExecutable(.{
        .name = "fip-c",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libcpp = true,
        }),
        .version = try .parse(FIP_VERSION),
    });
    b.installArtifact(exe);
    if (optimize == .Debug) {
        exe.root_module.addCMacro("DEBUG_BUILD", "");
    }
    exe.link_function_sections = true;
    exe.link_data_sections = true;
    exe.compress_debug_sections = .zlib;
    exe.build_id = .fast;

    const llvm_dir = switch (target.result.os.tag) {
        .linux => "vendor/llvm-linux",
        .windows => "vendor/llvm-mingw",
        else => return error.TargetNeedsToBeLinuxOrWindows,
    };

    // Add Include paths
    exe.root_module.addSystemIncludePath(b.path(b.fmt("{s}/include", .{llvm_dir})));
    exe.root_module.addIncludePath(b.path(""));

    // Add Library paths
    exe.root_module.addLibraryPath(b.path(b.fmt("{s}/lib", .{llvm_dir})));

    // zig fmt: off
    // Add C++ src files
    exe.root_module.addCSourceFile(.{
        .file = b.path(
            "modules/c/fip.c"
        ),
        .flags = &[_][]const u8{
            "-std=c17",                     // Set C standard to C17
            "-Werror",                      // Treat warnings as errors
            "-Wall",                        // Enable most warnings
            "-Wextra",                      // Enable extra warnings
            "-Wshadow",                     // Warn about shadow variables
            "-Wcast-align",                 // Warn about pointer casts that increase alignment requirement
            "-Wcast-qual",                  // Warn about casts that remove const qualifier
            "-Wunused",                     // Warn about unused variables
            "-Wold-style-cast",             // Warn about C-style casts
            "-Wdouble-promotion",           // Warn about float being implicitly promoted to double
            "-Wformat=2",                   // Warn about printf/scanf/strftime/strfmon format string issue
            "-Wundef",                      // Warn if an undefined identifier is evaluated in an #if
            "-Wpointer-arith",              // Warn about sizeof(void) and add/sub with void*
            "-Wunreachable-code",           // Warn about unreachable code
            "-Wno-deprecated-declarations", // Ignore deprecation warnings
            "-Wno-deprecated",              // Ignore general deprecation warnings
            "-fno-omit-frame-pointer",      // Prevent omitting frame pointer for debugging and stack unwinding
            "-funwind-tables",              // Generate unwind tables for stack unwinding
            "-ffunction-sections",          // Place each function in its own section
            "-fdata-sections",              // Place each data object in its own section
            "-fstandalone-debug",           // Emit standalone debug information
            "-Wdeprecated-declarations",    // Warn about deprecated declarations
        },
    });
    // zig fmt: on

    // Add toml C src file for FIP
    exe.root_module.addCSourceFile(.{
        .file = b.path("toml/tomlc17.c"),
    });

    // Link libraries
    if (target.result.os.tag == .windows) {
        exe.root_module.linkSystemLibrary("ole32", .{});
        exe.root_module.linkSystemLibrary("version", .{});
    }
    try linkWithClangLibs(b, previous_step, exe, b.fmt("{s}/lib", .{llvm_dir}));
}

fn buildExamples(b: *std.Build, target: std.Build.ResolvedTarget, optimize: std.builtin.OptimizeMode) !void {
    const exe = b.addExecutable(.{
        .name = "example_master",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
        .version = try .parse(FIP_VERSION),
    });
    b.installArtifact(exe);
    if (optimize == .Debug) {
        exe.root_module.addCMacro("DEBUG_BUILD", "");
    }
    exe.link_function_sections = true;
    exe.link_data_sections = true;
    exe.compress_debug_sections = .zlib;
    exe.build_id = .fast;

    // Add Include paths
    exe.root_module.addIncludePath(b.path(""));

    // zig fmt: off
    // Add C++ src files
    exe.root_module.addCSourceFile(.{
        .file = b.path(
            "modules/c/example_master.c"
        ),
        .flags = &[_][]const u8{
            "-std=c17",                     // Set C standard to C17
            "-Werror",                      // Treat warnings as errors
            "-Wall",                        // Enable most warnings
            "-Wextra",                      // Enable extra warnings
            "-Wshadow",                     // Warn about shadow variables
            "-Wcast-align",                 // Warn about pointer casts that increase alignment requirement
            "-Wcast-qual",                  // Warn about casts that remove const qualifier
            "-Wunused",                     // Warn about unused variables
            "-Wold-style-cast",             // Warn about C-style casts
            "-Wdouble-promotion",           // Warn about float being implicitly promoted to double
            "-Wformat=2",                   // Warn about printf/scanf/strftime/strfmon format string issue
            "-Wundef",                      // Warn if an undefined identifier is evaluated in an #if
            "-Wpointer-arith",              // Warn about sizeof(void) and add/sub with void*
            "-Wunreachable-code",           // Warn about unreachable code
            "-Wno-deprecated-declarations", // Ignore deprecation warnings
            "-Wno-deprecated",              // Ignore general deprecation warnings
            "-fno-omit-frame-pointer",      // Prevent omitting frame pointer for debugging and stack unwinding
            "-funwind-tables",              // Generate unwind tables for stack unwinding
            "-ffunction-sections",          // Place each function in its own section
            "-fdata-sections",              // Place each data object in its own section
            "-fstandalone-debug",           // Emit standalone debug information
            "-Wdeprecated-declarations",    // Warn about deprecated declarations
        },
    });
    // zig fmt: on

    // Add toml C src file for FIP
    exe.root_module.addCSourceFile(.{
        .file = b.path("toml/tomlc17.c"),
    });
}

fn buildLLVM(b: *std.Build, previous_step: *std.Build.Step, target: std.Build.ResolvedTarget, force_rebuild: bool, jobs: usize) !*std.Build.Step.Run {
    const build_name: []const u8 = switch (target.result.os.tag) {
        .linux => "linux",
        .windows => "mingw",
        else => return error.TargetNeedsToBeLinuxOrWindows,
    };
    const llvm_build_dir = b.fmt(".zig-cache/llvm-{s}", .{build_name});
    const install_dir = b.fmt("vendor/llvm-{s}", .{build_name});

    if (std.fs.cwd().openDir(install_dir, .{})) |_| {
        // LLVM is already built, rebuilt only if requested
        if (force_rebuild) {
            try std.fs.cwd().deleteTree(install_dir);
        } else {
            return makeEmptyStep(b);
        }
    } else |_| {}
    if (force_rebuild) {
        try std.fs.cwd().deleteTree(llvm_build_dir);
    }

    std.debug.print("-- Building LLVM for {s}\n", .{build_name});

    // Setup LLVM
    const setup_llvm = b.addSystemCommand(&[_][]const u8{
        "cmake",
        "-S",
        "vendor/sources/llvm-project/llvm",
        "-B",
        llvm_build_dir,
        "-G",
        "Ninja",
        b.fmt("-DCMAKE_INSTALL_PREFIX={s}", .{install_dir}),
        "-DCMAKE_BUILD_TYPE=MinSizeRel",
        b.fmt("-DCMAKE_C_COMPILER={s}", .{switch (target.result.os.tag) {
            .linux => "zig;cc",
            .windows => "zig;cc;-target;x86_64-windows-gnu",
            else => return error.TargetNeedsToBeLinuxOrWindows,
        }}),
        b.fmt("-DCMAKE_CXX_COMPILER={s}", .{switch (target.result.os.tag) {
            .linux => "zig;c++",
            .windows => "zig;c++;-target;x86_64-windows-gnu",
            else => return error.TargetNeedsToBeLinuxOrWindows,
        }}),
        b.fmt("-DCMAKE_ASM_COMPILER={s}", .{switch (target.result.os.tag) {
            .linux => "zig;cc",
            .windows => "zig;cc;-target;x86_64-windows-gnu",
            else => return error.TargetNeedsToBeLinuxOrWindows,
        }}),
        if (b.resolveTargetQuery(.{}).result.os.tag == target.result.os.tag) "" else switch (target.result.os.tag) {
            .linux => "-DCMAKE_SYSTEM_NAME=Linux",
            .windows => "-DCMAKE_SYSTEM_NAME=Windows",
            else => return error.TargetNeedsToBeLinuxOrWindows,
        },
        "-DBUILD_SHARED_LIBS=OFF",

        "-DLLVM_TARGET_ARCH=X86",
        "-DLLVM_TARGETS_TO_BUILD=X86",

        "-DLLVM_ENABLE_PROJECTS=clang",
        "-DLLVM_ENABLE_ASSERTIONS=ON",
        "-DLLVM_ENABLE_CURL=OFF",
        "-DLLVM_ENABLE_HTTPLIB=OFF",
        "-DLLVM_ENABLE_FFI=OFF",
        "-DLLVM_ENABLE_LIBEDIT=OFF",
        "-DLLVM_ENABLE_LIBXML2=OFF",
        "-DLLVM_ENABLE_PIC=OFF", // To avoid "error: unsupported linker arg:", "-Bsymbolic-functions"
        "-DLLVM_ENABLE_Z3_SOLVER=OFF",
        "-DLLVM_ENABLE_ZLIB=OFF",
        "-DLLVM_ENABLE_ZSTD=OFF",

        "-DLLVM_INCLUDE_BENCHMARKS=OFF",
        "-DLLVM_INCLUDE_DOCS=OFF",
        "-DLLVM_INCLUDE_EXAMPLES=OFF",
        "-DLLVM_INCLUDE_RUNTIMES=OFF",
        "-DLLVM_INCLUDE_TESTS=OFF",
        "-DLLVM_INCLUDE_UTILS=OFF",

        "-DLLVM_BUILD_STATIC=ON",
        "-DLLVM_BUILD_BENCHMARKS=OFF",
        "-DLLVM_BUILD_DOCS=OFF",
        "-DLLVM_BUILD_EXAMPLES=OFF",
        "-DLLVM_BUILD_RUNTIME=OFF",
        "-DLLVM_BUILD_TESTS=OFF",
        "-DLLVM_BUILD_UTILS=OFF",

        "-DLIBCLANG_BUILD_STATIC=ON",

        "-DCLANG_BUILD_EXAMPLES=OFF",
        "-DCLANG_BUILD_TOOLS=OFF",
        "-DCLANG_ENABLE_OBJC_REWRITER=OFF",
        "-DCLANG_ENABLE_STATIC_ANALYZER=OFF",
        "-DCLANG_INCLUDE_DOCS=OFF",
        "-DCLANG_INCLUDE_TESTS=OFF",
        "-DCLANG_PLUGIN_SUPPORT=OFF",

        // https://github.com/ziglang/zig/issues/23546
        // https://codeberg.org/ziglang/zig/pulls/30073
        "-DCMAKE_LINK_DEPENDS_USE_LINKER=FALSE", // To avoid "error: unsupported linker arg:", "--dependency-file"

        "-DCMAKE_C_FLAGS=-mcpu=baseline",
        "-DCMAKE_CXX_FLAGS=-mcpu=baseline",

        // "-DCMAKE_VERBOSE_MAKEFILE=ON", // Increased build log verbosity
        "-DCMAKE_INSTALL_MESSAGE=NEVER",
        b.fmt("-DLLVM_PARALLEL_COMPILE_JOBS={d}", .{jobs}),
        b.fmt("-DLLVM_PARALLEL_LINK_JOBS={d}", .{jobs}),
    });
    setup_llvm.setEnvironmentVariable("CC", "zig;cc");
    setup_llvm.setEnvironmentVariable("CXX", "zig;cxx");
    setup_llvm.setEnvironmentVariable("ASM", "zig;cc");
    setup_llvm.setName("llvm_setup");
    setup_llvm.step.dependOn(previous_step);

    // Build main LLVM
    const components = [_][]const u8{
        "llvm-headers",
        "llvm-libraries",
        "install-llvm-libraries",
        "clang-headers",
        "clang-libraries",
        "install-clang-libraries",
        "libclang-headers",
        "libclang",
        "install-libclang",
    };
    const build_llvm = b.addSystemCommand(&[_][]const u8{
        "cmake",                 "--build",  llvm_build_dir,
        b.fmt("-j{d}", .{jobs}), "--target",
    } ++ components);
    build_llvm.setName("llvm_build");
    build_llvm.step.dependOn(&setup_llvm.step);

    // Install main LLVM
    var install_runs: [components.len]*std.Build.Step.Run = undefined;
    for (components, 0..) |comp, i| {
        const cmd = b.addSystemCommand(&[_][]const u8{
            "cmake", "--install", llvm_build_dir, "--component", comp,
        });
        cmd.setName(b.fmt("llvm_install_{s}", .{comp}));
        if (i == 0) {
            cmd.step.dependOn(&build_llvm.step);
        } else {
            cmd.step.dependOn(&install_runs[i - 1].step);
        }
        install_runs[i] = cmd;
    }

    return install_runs[install_runs.len - 1];
}

fn linkWithClangLibs(b: *std.Build, previous_step: *std.Build.Step, exe: *std.Build.Step.Compile, llvm_libdir: []const u8) !void {
    const LinkClangLibsStep = struct {
        step: std.Build.Step,
        exe: *std.Build.Step.Compile,
        llvm_libdir: []const u8,

        pub fn make(step: *std.Build.Step, _: std.Build.Step.MakeOptions) !void {
            const self: *@This() = @fieldParentPtr("step", step);

            var llvm_dir = try std.fs.cwd().openDir(self.llvm_libdir, .{ .iterate = true });
            defer llvm_dir.close();

            const static_lib_prefix = "lib";
            const static_lib_suffix = ".a";

            var iter = llvm_dir.iterate();
            while (try iter.next()) |entry| {
                std.debug.assert(entry.name.len > 0);
                std.debug.assert(std.mem.startsWith(u8, entry.name, static_lib_prefix));

                const lib_name = entry.name[static_lib_prefix.len .. entry.name.len - static_lib_suffix.len];
                self.exe.root_module.linkSystemLibrary(lib_name, .{});
            }
        }
    };

    const link_clang_libs_step = try b.allocator.create(LinkClangLibsStep);
    link_clang_libs_step.* = .{
        .step = std.Build.Step.init(.{
            .id = .custom,
            .name = "Link Clang libraries",
            .owner = b,
            .makeFn = LinkClangLibsStep.make,
        }),
        .exe = exe,
        .llvm_libdir = llvm_libdir,
    };
    link_clang_libs_step.step.dependOn(previous_step);
    exe.step.dependOn(&link_clang_libs_step.step);
}

fn updateLLVM(b: *std.Build, llvm_version: []const u8) !*std.Build.Step.Run {
    std.debug.print("-- Updating the 'llvm-project' repository\n", .{});
    // 1. Check if llvm-project exists in vendor directory
    if (std.fs.cwd().openDir("vendor/sources/llvm-project", .{})) |_| {
        // 2. Check for internet connection
        if (!hasInternetConnection(b)) {
            std.debug.print("-- No internet connection found, skipping updating 'llvm-project'...\n", .{});
            return makeEmptyStep(b);
        }

        // 3. Reset hard
        const reset_llvm_cmd = b.addSystemCommand(&[_][]const u8{ "git", "reset", "--hard" });
        reset_llvm_cmd.setName("reset_llvm");
        reset_llvm_cmd.setCwd(b.path("vendor/sources/llvm-project"));

        // 4. Fetch llvm-project
        const fetch_llvm_cmd = b.addSystemCommand(&[_][]const u8{ "git", "fetch", "-fq", "--depth", "1", "origin", "tag", llvm_version });
        fetch_llvm_cmd.setName("fetch_llvm");
        fetch_llvm_cmd.setCwd(b.path("vendor/sources/llvm-project"));
        fetch_llvm_cmd.step.dependOn(&reset_llvm_cmd.step);

        // 5. Checkout llvm-project at tag of `llvm_version`
        const checkout_llvm_cmd = b.addSystemCommand(&[_][]const u8{ "git", "checkout", "-fq", llvm_version });
        checkout_llvm_cmd.setName("checkout_llvm");
        checkout_llvm_cmd.setCwd(b.path("vendor/sources/llvm-project"));
        checkout_llvm_cmd.step.dependOn(&fetch_llvm_cmd.step);

        return checkout_llvm_cmd;
    } else |_| {
        // 2. Check for internet connection
        if (!hasInternetConnection(b)) {
            std.debug.print("-- No internet connection found, unable to clone dependency 'llvm-project'...\n", .{});
            return error.NoInternetConnection;
        }

        // 3. Clone llvm
        const clone_llvm_step = b.addSystemCommand(&[_][]const u8{ "git", "clone", "--depth", "1", "--branch", llvm_version, "https://github.com/llvm/llvm-project.git", "vendor/sources/llvm-project" });
        clone_llvm_step.setName("clone_llvm");

        return clone_llvm_step;
    }
}

/// Create a no-op Run step that meets the return type requirements
fn makeEmptyStep(b: *std.Build) !*std.Build.Step.Run {
    const run_step = b.addSystemCommand(&[_][]const u8{ "zig", "version" });
    run_step.setName("make_empty_step");
    _ = run_step.captureStdOut();
    return run_step;
}

fn hasInternetConnection(b: *std.Build) bool {
    const s = std.net.tcpConnectToHost(b.allocator, "google.com", 443) catch return false;
    s.close();
    return true;
}
