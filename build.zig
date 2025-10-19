const std = @import("std");

pub fn build(b: *std.Build) !void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const version = b.option([]const u8, "version", "FISKTA version string") orelse blk: {
        const version_file = std.fs.cwd().readFileAlloc(b.allocator, "VERSION", 100) catch break :blk "dev";
        break :blk std.mem.trim(u8, version_file, &std.ascii.whitespace);
    };

    const out_dir = "zig-out/bin";
    const mkdir_step = b.addSystemCommand(&.{ "mkdir", "-p", out_dir });

    const host_step = b.step("build", "Build fiskta (host)");
    const linkage_override = b.option(std.builtin.LinkMode, "linkage", "Linkage for host build") orelse .dynamic;

    const host_triple = try target.result.zigTriple(b.allocator);
    const host_os = target.result.os.tag;
    const host_shrink = optimize != .Debug;
    const host_cmd = createBuildStep(
        b,
        &mkdir_step.step,
        host_triple,
        host_os,
        linkage_override,
        optimize,
        version,
        "fiskta",
        "",
        host_shrink,
        false,
    );
    host_step.dependOn(&host_cmd.step);

    if (host_os == .linux) {
        const musl_cmd = createBuildStep(
            b,
            &mkdir_step.step,
            "x86_64-linux-musl",
            .linux,
            .static,
            optimize,
            version,
            "fiskta-musl",
            "",
            host_shrink,
            false,
        );
        host_step.dependOn(&musl_cmd.step);
    }

    b.getInstallStep().dependOn(host_step);

    const asan_step = b.step("asan", "Build fiskta with AddressSanitizer (for fuzzing)");
    const asan_cmd = createAsanBuildStep(
        b,
        &mkdir_step.step,
        host_triple,
        host_os,
        version,
        "fiskta-asan",
        "",
    );
    asan_step.dependOn(&asan_cmd.step);

    const run_cmd = b.addSystemCommand(&.{"zig-out/bin/fiskta"});
    if (b.args) |args| {
        run_cmd.addArgs(args);
    }
    const run_step = b.step("run", "Run fiskta");
    run_step.dependOn(host_step);
    run_step.dependOn(&run_cmd.step);

    const test_cmd = b.addSystemCommand(&.{ "python3", "tests/run_tests.py" });
    test_cmd.setCwd(b.path("."));
    test_cmd.setEnvironmentVariable("PATH", b.pathJoin(&.{ b.pathFromRoot(out_dir), ":", std.posix.getenv("PATH") orelse "" }));
    test_cmd.step.dependOn(host_step);
    const test_step = b.step("test", "Run comprehensive test suite");
    test_step.dependOn(&test_cmd.step);

    const release_step = b.step("release", "Build release binaries (<150 KiB) for all platforms");
    const release_targets = [_]struct {
        triple: []const u8,
        os: std.Target.Os.Tag,
        linkage: std.builtin.LinkMode,
        name: []const u8,
        ext: []const u8,
        use_lto: bool,
    }{
        .{ .triple = "x86_64-linux-gnu", .os = .linux, .linkage = .dynamic, .name = "fiskta-linux-x86_64", .ext = "", .use_lto = true },
        .{ .triple = "x86_64-linux-musl", .os = .linux, .linkage = .static, .name = "fiskta-linux-x86_64-musl", .ext = "", .use_lto = true },
        .{ .triple = "aarch64-macos", .os = .macos, .linkage = .dynamic, .name = "fiskta-macos-arm64", .ext = "", .use_lto = false },
        .{ .triple = "x86_64-windows", .os = .windows, .linkage = .dynamic, .name = "fiskta-x86_64", .ext = ".exe", .use_lto = false },
    };

    for (release_targets) |entry| {
        const step = createBuildStep(
            b,
            &mkdir_step.step,
            entry.triple,
            entry.os,
            entry.linkage,
            .ReleaseFast,
            version,
            entry.name,
            entry.ext,
            true,
            entry.use_lto,
        );
        release_step.dependOn(&step.step);
    }
}

fn createBuildStep(
    b: *std.Build,
    mkdir_step: *std.Build.Step,
    target_triple: []const u8,
    target_os: std.Target.Os.Tag,
    linkage: std.builtin.LinkMode,
    optimize: std.builtin.OptimizeMode,
    version: []const u8,
    out_name: []const u8,
    out_ext: []const u8,
    shrink: bool,
    use_lto: bool,
) *std.Build.Step.Run {
    const cmd = b.addSystemCommand(&.{
        "zig",
        "cc",
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Wconversion",
        "-Wshadow",
        "-Wcast-qual",
        "-Wpointer-arith",
        "-Wbad-function-cast",
        "-Wundef",
        "-pedantic",
        "-Wcast-align",
        "-Wmissing-declarations",
        "-Wwrite-strings",
        "-Wstrict-aliasing=2",
        "-ffunction-sections",
        "-fdata-sections",
        "-I",
        "src",
        "src/main.c",
        "src/parse.c",
        "src/runtime.c",
        "src/engine.c",
        "src/iosearch.c",
        "src/reprog.c",
        "src/util.c",
        "src/error.c",
        "-o",
        b.fmt("zig-out/bin/{s}{s}", .{ out_name, out_ext }),
    });

    cmd.addArg(b.fmt("-DFISKTA_VERSION=\"{s}\"", .{version}));
    cmd.addArgs(&.{ "-target", target_triple });

    switch (optimize) {
        .Debug => cmd.addArgs(&.{ "-g", "-O0", "-DDEBUG" }),
        .ReleaseFast => cmd.addArg("-O3"),
        .ReleaseSafe => cmd.addArg("-O2"),
        .ReleaseSmall => cmd.addArg("-Os"),
    }

    if (shrink) {
        if (use_lto) {
            cmd.addArg("-flto");
        }
        cmd.addArgs(&.{
            "-fomit-frame-pointer",
            "-fno-stack-protector",
            "-fno-unwind-tables",
            "-fno-asynchronous-unwind-tables",
        });
    }

    if (optimize != .Debug) {
        cmd.addArg("-s");
    }

    if (target_os == .macos) {
        cmd.addArg("-Wl,-dead_strip");
    } else {
        cmd.addArg("-Wl,--gc-sections");
    }

    if (linkage == .static) {
        cmd.addArg("-static");
    }

    cmd.step.dependOn(mkdir_step);
    return cmd;
}

fn createAsanBuildStep(
    b: *std.Build,
    mkdir_step: *std.Build.Step,
    target_triple: []const u8,
    target_os: std.Target.Os.Tag,
    version: []const u8,
    out_name: []const u8,
    out_ext: []const u8,
) *std.Build.Step.Run {
    const cmd = b.addSystemCommand(&.{
        "zig",
        "cc",
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Wconversion",
        "-Wshadow",
        "-Wcast-qual",
        "-Wpointer-arith",
        "-Wbad-function-cast",
        "-Wundef",
        "-pedantic",
        "-Wcast-align",
        "-Wmissing-declarations",
        "-Wwrite-strings",
        "-Wstrict-aliasing=2",
        "-I",
        "src",
        "src/main.c",
        "src/parse.c",
        "src/runtime.c",
        "src/engine.c",
        "src/iosearch.c",
        "src/reprog.c",
        "src/util.c",
        "src/error.c",
        "-o",
        b.fmt("zig-out/bin/{s}{s}", .{ out_name, out_ext }),
    });

    cmd.addArg(b.fmt("-DFISKTA_VERSION=\"{s}\"", .{version}));
    cmd.addArgs(&.{ "-target", target_triple });

    // ASAN build: optimized with sanitizers
    cmd.addArgs(&.{
        "-g",
        "-O1",
        "-DDEBUG",
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
    });

    if (target_os == .macos) {
        cmd.addArg("-Wl,-dead_strip");
    } else {
        cmd.addArg("-Wl,--gc-sections");
    }

    cmd.step.dependOn(mkdir_step);
    return cmd;
}
