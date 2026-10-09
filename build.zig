const std = @import("std");

const FIP_VERSION = @import("build.zig.zon").version;
const DEFAULT_LLVM_VERSION = "llvmorg-21.1.8";

const OSTag = enum {
    linux,
    windows,
};

pub const LibMode = enum {
    none,
    master,
    slave,
};

pub fn build(b: *std.Build) !void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const lib_mode = b.option(LibMode, "lib-mode", "Mode to build the library in. Default: none (neither master nor slave)") orelse .none;

    const toml_dep = b.dependency("tomlc17", .{});
    _ = mkfip(b, target, optimize, lib_mode);

    const toml_c = b.addTranslateC(.{
        .target = target,
        .optimize = optimize,
        .root_source_file = toml_dep.path("src/tomlc17.h"),
    });
    const toml_module = toml_c.createModule();

    const tests_step = b.step("test", "Run tests");
    inline for (.{ .master, .slave }) |mode| {
        const mode_lib = mkfip(b, target, optimize, mode);

        var mode_options = b.addOptions();
        mode_options.addOption(LibMode, "lib_mode", mode);

        const mode_tests = b.addTest(.{
            .root_module = b.createModule(.{
                .root_source_file = b.path("bindings/fip.zig"),
                .target = b.graph.host,
                .optimize = optimize,
                .link_libc = true,
                .imports = &.{
                    .{ .name = "defines", .module = mode_options.createModule() },
                    .{ .name = "toml", .module = toml_module },
                },
            }),
            .use_llvm = true,
            .use_lld = true,
        });
        mode_tests.root_module.linkLibrary(mode_lib);

        tests_step.dependOn(&b.addRunArtifact(mode_tests).step);
    }
}

fn mkfip(b: *std.Build, target: std.Build.ResolvedTarget, optimize: std.builtin.OptimizeMode, lib_mode: LibMode) *std.Build.Step.Compile {
    const toml_dep = b.dependency("tomlc17", .{});
    const lib = b.addLibrary(.{
        .name = "fip",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });

    lib.root_module.addIncludePath(b.path("."));
    lib.root_module.addIncludePath(toml_dep.path("src"));
    lib.root_module.addCSourceFile(.{
        .file = b.path("fip.h"),
        .flags = &.{
            "-DFIP_IMPLEMENTATION",
            switch (lib_mode) {
                .none => "",
                .master => "-DFIP_MASTER",
                .slave => "-DFIP_SLAVE",
            },
        },
        .language = .c,
    });
    lib.root_module.addCSourceFile(.{
        .file = toml_dep.path("src/tomlc17.c"),
    });

    b.installArtifact(lib);
    lib.installHeader(b.path("fip.h"), "fip.h");
    lib.installHeader(toml_dep.path("src/tomlc17.h"), "tomlc17.h");
    return lib;
}
