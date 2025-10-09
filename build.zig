const std = @import("std");

pub fn build(b: *std.Build) !void {
    // Standard Zig CLI options
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // Our options
    const version = b.option([]const u8, "version", "FISKTA version string") orelse blk: {
        const version_file = std.fs.cwd().readFileAlloc(b.allocator, "VERSION", 100) catch break :blk "dev";
        break :blk std.mem.trim(u8, version_file, &std.ascii.whitespace);
    };
    const linkage = b.option(std.builtin.LinkMode, "linkage", "static or dynamic") orelse .dynamic;

    // Use zig cc directly via system command
    const exe_step = b.step("build", "Build fiskta using zig cc");

    // Create output directory
    const mkdir_cmd = b.addSystemCommand(&.{ "mkdir", "-p", "zig-out/bin" });

    const zig_cc_cmd = b.addSystemCommand(&.{
        "zig",            "cc",
        "-std=c11",       "-Wall",
        "-Wextra",        "-Wconversion",
        "-Wshadow",       "-I",
        "src",            "src/main.c",
        "src/parse.c",    "src/engine.c",
        "src/iosearch.c", "src/reprog.c",
        "-o",             "zig-out/bin/fiskta",
    });

    // Add version define
    zig_cc_cmd.addArg(b.fmt("-DFISKTA_VERSION=\"{s}\"", .{version}));

    // Set target
    zig_cc_cmd.addArg("-target");
    zig_cc_cmd.addArg(b.fmt("{s}", .{try target.result.zigTriple(b.allocator)}));

    // Set optimization
    switch (optimize) {
        .Debug => {
            zig_cc_cmd.addArgs(&.{ "-g", "-O0", "-DDEBUG" });
        },
        .ReleaseFast => {
            zig_cc_cmd.addArg("-O3");
        },
        .ReleaseSafe => {
            zig_cc_cmd.addArg("-O2");
        },
        .ReleaseSmall => {
            zig_cc_cmd.addArg("-Os");
        },
    }

    // Set linkage
    if (linkage == .static) {
        zig_cc_cmd.addArg("-static");
    }

    zig_cc_cmd.step.dependOn(&mkdir_cmd.step);
    exe_step.dependOn(&zig_cc_cmd.step);

    // Make install step depend on our build step
    b.getInstallStep().dependOn(exe_step);

    // Run step
    const run_cmd = b.addSystemCommand(&.{"zig-out/bin/fiskta"});
    if (b.args) |args| {
        run_cmd.addArgs(args);
    }
    const run_step = b.step("run", "Run fiskta");
    run_step.dependOn(exe_step);
    run_step.dependOn(&run_cmd.step);

    // Test step
    const test_cmd = b.addSystemCommand(&.{ "python3", "tests/run_tests.py" });
    test_cmd.setCwd(b.path("."));
    test_cmd.setEnvironmentVariable("PATH", b.pathJoin(&.{ b.pathFromRoot("zig-out/bin"), ":", std.posix.getenv("PATH") orelse "" }));
    test_cmd.step.dependOn(exe_step);
    const test_step = b.step("test", "Run comprehensive test suite");
    test_step.dependOn(&test_cmd.step);

    // Multi-target build step
    const all_step = b.step("all", "Build fiskta for multiple platforms");

    // Linux x86_64 dynamic
    const linux_dynamic = createBuildStep(b, "x86_64-linux-gnu", .dynamic, "linux-x86_64", version, "");
    all_step.dependOn(&linux_dynamic.step);

    // Linux x86_64 static (musl)
    const linux_static = createBuildStep(b, "x86_64-linux-musl", .static, "linux-x86_64-musl", version, "");
    all_step.dependOn(&linux_static.step);

    // macOS ARM64
    const macos_arm = createBuildStep(b, "aarch64-macos", .dynamic, "macos-arm64", version, "");
    all_step.dependOn(&macos_arm.step);

    // Windows x86_64
    const windows_x64 = createBuildStep(b, "x86_64-windows", .dynamic, "x86_64", version, ".exe");
    all_step.dependOn(&windows_x64.step);
}

fn createBuildStep(b: *std.Build, target_str: []const u8, linkage: std.builtin.LinkMode, output_name: []const u8, version: []const u8, ext: []const u8) *std.Build.Step.Run {
    // Create output directory
    const mkdir_cmd = b.addSystemCommand(&.{ "mkdir", "-p", "zig-out/bin" });

    const zig_cc_cmd = b.addSystemCommand(&.{
        "zig",            "cc",
        "-std=c11",       "-Wall",
        "-Wextra",        "-Wconversion",
        "-Wshadow",       "-I",
        "src",            "src/main.c",
        "src/parse.c",    "src/engine.c",
        "src/iosearch.c", "src/reprog.c",
        "-o",             b.fmt("zig-out/bin/fiskta-{s}{s}", .{ output_name, ext }),
    });

    // Add version define
    zig_cc_cmd.addArg(b.fmt("-DFISKTA_VERSION=\"{s}\"", .{version}));

    // Set target
    zig_cc_cmd.addArg("-target");
    zig_cc_cmd.addArg(target_str);

    // Set optimization to ReleaseSmall for smaller binaries
    zig_cc_cmd.addArg("-Os");

    // Set linkage
    if (linkage == .static) {
        zig_cc_cmd.addArg("-static");
    }

    zig_cc_cmd.step.dependOn(&mkdir_cmd.step);
    return zig_cc_cmd;
}
