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
}
