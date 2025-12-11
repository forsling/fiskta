const std = @import("std");

const lib_sources = [_][]const u8{
    "src/parse.c",
    "src/fiskta.c",
    "src/engine.c",
    "src/fileio.c",
    "src/search_literal.c",
    "src/regex_vm.c",
    "src/regex_prog.c",
    "src/util.c",
};

const cli_extra_sources = [_][]const u8{
    "src/main.c",
};

pub fn build(b: *std.Build) !void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const global_cache = b.pathJoin(&.{ b.pathFromRoot(".zig-cache"), "global" });

    const version = b.option([]const u8, "version", "FISKTA version string") orelse blk: {
        // Try git describe (shows commits since last tag + dirty state)
        var code: u8 = undefined;
        const git_describe = b.runAllowFail(
            &.{ "git", "describe", "--tags", "--dirty", "--always" },
            &code,
            .Ignore,
        ) catch break :blk "unknown";
        const trimmed = std.mem.trim(u8, git_describe, &std.ascii.whitespace);
        // Strip leading 'v' if present (git tags are usually v1.2, v1.3, etc)
        break :blk if (trimmed.len > 0 and trimmed[0] == 'v') trimmed[1..] else trimmed;
    };

    const version_parts = parseVersionParts(version);

    const out_dir = "zig-out/bin";
    try std.fs.cwd().makePath(out_dir);

    const host_step = b.step("build", "Build fiskta (host)");
    const linkage_override = b.option(std.builtin.LinkMode, "linkage", "Linkage for host build") orelse .dynamic;

    const host_triple = try target.result.zigTriple(b.allocator);
    const host_os = target.result.os.tag;
    const host_shrink = optimize != .Debug;
    const host_cmd = createBuildStep(
        b,
        host_triple,
        host_os,
        linkage_override,
        optimize,
        version,
        out_dir,
        "fiskta",
        "",
        host_shrink,
        false,
        version_parts,
    );
    host_cmd.setEnvironmentVariable("ZIG_GLOBAL_CACHE_DIR", global_cache);
    host_step.dependOn(&host_cmd.step);

    if (host_os == .linux) {
        const musl_cmd = createBuildStep(
            b,
            "x86_64-linux-musl",
            .linux,
            .static,
            optimize,
            version,
            out_dir,
            "fiskta-musl",
            "",
            host_shrink,
            false,
            version_parts,
        );
        musl_cmd.setEnvironmentVariable("ZIG_GLOBAL_CACHE_DIR", global_cache);
        host_step.dependOn(&musl_cmd.step);
    }

    b.getInstallStep().dependOn(host_step);

    const asan_step = b.step("asan", "Build fiskta with AddressSanitizer (for fuzzing)");
    const asan_cmd = createAsanBuildStep(
        b,
        host_triple,
        host_os,
        version,
        out_dir,
        version_parts,
        "fiskta-asan",
        "",
    );
    asan_cmd.setEnvironmentVariable("ZIG_GLOBAL_CACHE_DIR", global_cache);
    asan_step.dependOn(&asan_cmd.step);

    const wrapper_step = b.step("wrapper", "Build library wrapper for testing");
    const wrapper_cmd = b.addSystemCommand(&.{
        "zig",
        "cc",
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-I",
        "src",
        "tools/fiskta_library_wrapper.c",
        "-o",
        "zig-out/bin/fiskta_library_wrapper",
    });
    wrapper_cmd.setEnvironmentVariable("ZIG_GLOBAL_CACHE_DIR", global_cache);
    addLibrarySources(wrapper_cmd);
    wrapper_cmd.addArg(b.fmt("-DFISKTA_VERSION=\"{s}\"", .{version}));
    wrapper_cmd.addArgs(&.{ "-target", host_triple });
    wrapper_cmd.addArg(b.fmt("-DFISKTA_ABI_MAJOR={d}", .{version_parts.major}));
    wrapper_cmd.addArg(b.fmt("-DFISKTA_ABI_MINOR={d}", .{version_parts.minor}));
    wrapper_cmd.addArg("-DFISKTA_BUILD");
    if (optimize == .Debug) {
        wrapper_cmd.addArgs(&.{ "-g", "-O0", "-DDEBUG" });
    } else {
        wrapper_cmd.addArg("-O3");
    }
    wrapper_step.dependOn(&wrapper_cmd.step);

    const run_cmd = b.addSystemCommand(&.{"zig-out/bin/fiskta"});
    if (b.args) |args| {
        run_cmd.addArgs(args);
    }
    const run_step = b.step("run", "Run fiskta");
    run_step.dependOn(host_step);
    run_step.dependOn(&run_cmd.step);

    const test_step = b.step("test", "Run test suite (CLI)");

    const test_cli_cmd = b.addSystemCommand(&.{ "sh", "-c", "python3 tools/test.py --exe zig-out/bin/fiskta | grep -v '\\[PASS\\]'" });
    test_cli_cmd.setCwd(b.path("."));
    test_cli_cmd.setEnvironmentVariable("PATH", b.pathJoin(&.{ b.pathFromRoot(out_dir), ":", std.posix.getenv("PATH") orelse "" }));
    test_cli_cmd.setEnvironmentVariable("ZIG_GLOBAL_CACHE_DIR", global_cache);
    test_cli_cmd.step.dependOn(host_step);
    test_step.dependOn(&test_cli_cmd.step);

    const test_lib_step = b.step("test-lib", "Run test suite through library wrapper");
    const test_lib_cmd = b.addSystemCommand(&.{ "sh", "-c", "python3 tools/test.py --exe zig-out/bin/fiskta_library_wrapper | grep -v '\\[PASS\\]'" });
    test_lib_cmd.setCwd(b.path("."));
    test_lib_cmd.setEnvironmentVariable("PATH", b.pathJoin(&.{ b.pathFromRoot(out_dir), ":", std.posix.getenv("PATH") orelse "" }));
    test_lib_cmd.setEnvironmentVariable("ZIG_GLOBAL_CACHE_DIR", global_cache);
    test_lib_cmd.step.dependOn(wrapper_step);
    test_lib_step.dependOn(&test_lib_cmd.step);

    const release_root = "zig-out/release";
    try std.fs.cwd().makePath(release_root);
    const release_step = b.step("release", "Build release packages (CLI + libs + header) for all platforms");
    const release_targets = [_]struct {
        triple: []const u8,
        os: std.Target.Os.Tag,
        linkage: std.builtin.LinkMode,
        name: []const u8,
        ext: []const u8,
        use_lto: bool,
        build_shared: bool,
    }{
        .{ .triple = "x86_64-linux-gnu", .os = .linux, .linkage = .dynamic, .name = "fiskta-linux-x86_64", .ext = "", .use_lto = true, .build_shared = true },
        .{ .triple = "x86_64-linux-musl", .os = .linux, .linkage = .static, .name = "fiskta-linux-x86_64-musl", .ext = "", .use_lto = true, .build_shared = false },
        .{ .triple = "aarch64-macos", .os = .macos, .linkage = .dynamic, .name = "fiskta-macos-arm64", .ext = "", .use_lto = false, .build_shared = true },
        .{ .triple = "x86_64-windows", .os = .windows, .linkage = .dynamic, .name = "fiskta-windows-x86_64", .ext = ".exe", .use_lto = false, .build_shared = true },
    };

    for (release_targets) |entry| {
        const pkg_root = b.fmt("{s}/{s}", .{ release_root, entry.name });
        try std.fs.cwd().makePath(pkg_root);
        const bin_dir = b.fmt("{s}/bin", .{pkg_root});
        const lib_dir = b.fmt("{s}/lib", .{pkg_root});
        const include_dir = b.fmt("{s}/include", .{pkg_root});
        try std.fs.cwd().makePath(bin_dir);
        try std.fs.cwd().makePath(lib_dir);
        try std.fs.cwd().makePath(include_dir);

        const copy_fiskta = b.addSystemCommand(&.{
            "cp",
            "src/fiskta.h",
            b.fmt("{s}/fiskta.h", .{include_dir}),
        });
        release_step.dependOn(&copy_fiskta.step);
        const copy_fiskta_types = b.addSystemCommand(&.{
            "cp",
            "src/fiskta_types.h",
            b.fmt("{s}/fiskta_types.h", .{include_dir}),
        });
        release_step.dependOn(&copy_fiskta_types.step);

        const cli_cmd = createBuildStep(
            b,
            entry.triple,
            entry.os,
            entry.linkage,
            .ReleaseFast,
            version,
            bin_dir,
            "fiskta",
            entry.ext,
            true,
            entry.use_lto,
            version_parts,
        );
        cli_cmd.setEnvironmentVariable("ZIG_GLOBAL_CACHE_DIR", global_cache);
        release_step.dependOn(&cli_cmd.step);

        const static_name = switch (entry.os) {
            .windows => b.fmt("fiskta-static-{d}-{d}.lib", .{ version_parts.major, version_parts.minor }),
            else => "libfiskta.a",
        };
        const static_cmd = createStaticLibStep(
            b,
            entry.triple,
            .ReleaseFast,
            version,
            version_parts,
            lib_dir,
            static_name,
        );
        static_cmd.setEnvironmentVariable("ZIG_GLOBAL_CACHE_DIR", global_cache);
        release_step.dependOn(&static_cmd.step);

        if (entry.build_shared) {
            const shared_name = switch (entry.os) {
                .linux => b.fmt("libfiskta.so.{d}.{d}", .{ version_parts.major, version_parts.minor }),
                .macos => b.fmt("libfiskta.{d}.{d}.dylib", .{ version_parts.major, version_parts.minor }),
                .windows => b.fmt("fiskta-{d}-{d}.dll", .{ version_parts.major, version_parts.minor }),
                else => b.fmt("libfiskta.so.{d}.{d}", .{ version_parts.major, version_parts.minor }),
            };
            const implib_name = if (entry.os == .windows)
                b.fmt("fiskta-{d}-{d}.lib", .{ version_parts.major, version_parts.minor })
            else
                null;
            const shared_cmd = createSharedLibStep(
                b,
                entry.triple,
                entry.os,
                .ReleaseFast,
                version,
                version_parts,
                lib_dir,
                shared_name,
                implib_name,
            );
            shared_cmd.setEnvironmentVariable("ZIG_GLOBAL_CACHE_DIR", global_cache);
            release_step.dependOn(&shared_cmd.step);
        }
    }
}

fn createBuildStep(
    b: *std.Build,
    target_triple: []const u8,
    target_os: std.Target.Os.Tag,
    linkage: std.builtin.LinkMode,
    optimize: std.builtin.OptimizeMode,
    version: []const u8,
    out_dir: []const u8,
    out_name: []const u8,
    out_ext: []const u8,
    shrink: bool,
    use_lto: bool,
    version_parts: VersionParts,
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
        "-o",
        b.fmt("{s}/{s}{s}", .{ out_dir, out_name, out_ext }),
    });

    addCliSources(cmd);
    cmd.addArg(b.fmt("-DFISKTA_VERSION=\"{s}\"", .{version}));
    cmd.addArg(b.fmt("-DFISKTA_ABI_MAJOR={d}", .{version_parts.major}));
    cmd.addArg(b.fmt("-DFISKTA_ABI_MINOR={d}", .{version_parts.minor}));
    cmd.addArg("-DFISKTA_BUILD");
    cmd.addArgs(&.{ "-target", target_triple });

    addOptimizeArgs(cmd, optimize);

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

    if (optimize != .Debug and target_os != .windows) {
        cmd.addArg("-s");
    }

    if (target_os == .macos) {
        cmd.addArg("-Wl,-dead_strip");
    } else if (target_os == .windows) {
        // Windows linkers do not accept GNU-style --gc-sections
        // Note: -s (strip) disabled for Windows cross-compile due to zig lld limitation
    } else {
        cmd.addArg("-Wl,--gc-sections");
    }

    if (linkage == .static) {
        cmd.addArg("-static");
    }
    return cmd;
}

const VersionParts = struct {
    major: u32,
    minor: u32,
};

fn parseVersionParts(version: []const u8) VersionParts {
    var parts = VersionParts{ .major = 0, .minor = 0 };
    var it = std.mem.splitScalar(u8, version, '.');
    var idx: usize = 0;
    while (it.next()) |segment| {
        const trimmed = std.mem.trim(u8, segment, &std.ascii.whitespace);
        if (trimmed.len == 0) {
            continue;
        }
        const value = std.fmt.parseUnsigned(u32, trimmed, 10) catch break;
        if (idx == 0) {
            parts.major = value;
        } else if (idx == 1) {
            parts.minor = value;
            break;
        }
        idx += 1;
    }
    return parts;
}

fn addLibrarySources(cmd: *std.Build.Step.Run) void {
    for (lib_sources) |src| {
        cmd.addArg(src);
    }
}

fn addCliSources(cmd: *std.Build.Step.Run) void {
    addLibrarySources(cmd);
    for (cli_extra_sources) |src| {
        cmd.addArg(src);
    }
}

fn addOptimizeArgs(cmd: *std.Build.Step.Run, optimize: std.builtin.OptimizeMode) void {
    switch (optimize) {
        .Debug => cmd.addArgs(&.{ "-g", "-O0", "-DDEBUG" }),
        .ReleaseFast => cmd.addArg("-O3"),
        .ReleaseSafe => cmd.addArg("-O2"),
        .ReleaseSmall => cmd.addArg("-Os"),
    }
}

fn addOptimizeArgsForBuildLib(cmd: *std.Build.Step.Run, optimize: std.builtin.OptimizeMode) void {
    switch (optimize) {
        .Debug => cmd.addArgs(&.{ "-g", "-O0", "-DDEBUG" }),
        .ReleaseFast => cmd.addArg("-OReleaseFast"),
        .ReleaseSafe => cmd.addArg("-OReleaseSafe"),
        .ReleaseSmall => cmd.addArg("-OReleaseSmall"),
    }
}

fn createAsanBuildStep(
    b: *std.Build,
    target_triple: []const u8,
    target_os: std.Target.Os.Tag,
    version: []const u8,
    out_dir: []const u8,
    version_parts: VersionParts,
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
        "-o",
        b.fmt("{s}/{s}{s}", .{ out_dir, out_name, out_ext }),
    });

    addCliSources(cmd);
    cmd.addArg(b.fmt("-DFISKTA_VERSION=\"{s}\"", .{version}));
    cmd.addArg(b.fmt("-DFISKTA_ABI_MAJOR={d}", .{version_parts.major}));
    cmd.addArg(b.fmt("-DFISKTA_ABI_MINOR={d}", .{version_parts.minor}));
    cmd.addArg("-DFISKTA_BUILD");
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
    return cmd;
}

fn createStaticLibStep(
    b: *std.Build,
    target_triple: []const u8,
    optimize: std.builtin.OptimizeMode,
    version: []const u8,
    version_parts: VersionParts,
    lib_dir: []const u8,
    file_name: []const u8,
) *std.Build.Step.Run {
    const out_path = b.pathJoin(&.{ b.pathFromRoot(lib_dir), file_name });
    const cmd = b.addSystemCommand(&.{
        "zig",
        "build-lib",
        "-static",
        "-lc",
        "-I",
        "src",
        b.fmt("-femit-bin={s}", .{out_path}),
    });
    addLibrarySources(cmd);
    cmd.addArg(b.fmt("-DFISKTA_VERSION=\"{s}\"", .{version}));
    cmd.addArg(b.fmt("-DFISKTA_ABI_MAJOR={d}", .{version_parts.major}));
    cmd.addArg(b.fmt("-DFISKTA_ABI_MINOR={d}", .{version_parts.minor}));
    cmd.addArg("-DFISKTA_BUILD");
    cmd.addArgs(&.{ "-target", target_triple });
    addOptimizeArgsForBuildLib(cmd, optimize);
    return cmd;
}

fn createSharedLibStep(
    b: *std.Build,
    target_triple: []const u8,
    target_os: std.Target.Os.Tag,
    optimize: std.builtin.OptimizeMode,
    version: []const u8,
    version_parts: VersionParts,
    lib_dir: []const u8,
    file_name: []const u8,
    implib_name: ?[]const u8,
) *std.Build.Step.Run {
    // For Windows, use zig build-lib which supports -femit-implib
    if (target_os == .windows) {
        const out_path = b.pathJoin(&.{ b.pathFromRoot(lib_dir), file_name });
        const cmd = b.addSystemCommand(&.{
            "zig",
            "build-lib",
            "-dynamic",
            "-lc",
            "-I",
            "src",
            b.fmt("-femit-bin={s}", .{out_path}),
        });
        if (implib_name) |libname| {
            const implib_path = b.pathJoin(&.{ b.pathFromRoot(lib_dir), libname });
            cmd.addArg(b.fmt("-femit-implib={s}", .{implib_path}));
        }
        addLibrarySources(cmd);
        cmd.addArg(b.fmt("-DFISKTA_VERSION=\"{s}\"", .{version}));
        cmd.addArg(b.fmt("-DFISKTA_ABI_MAJOR={d}", .{version_parts.major}));
        cmd.addArg(b.fmt("-DFISKTA_ABI_MINOR={d}", .{version_parts.minor}));
        cmd.addArg("-DFISKTA_BUILD");
        cmd.addArgs(&.{ "-target", target_triple });
        addOptimizeArgsForBuildLib(cmd, optimize);
        return cmd;
    }

    // For Linux/macOS, use zig cc -shared
    const out_path = b.pathJoin(&.{ b.pathFromRoot(lib_dir), file_name });
    const cmd = b.addSystemCommand(&.{
        "zig",
        "cc",
        "-shared",
        "-std=c11",
        "-I",
        "src",
        "-o",
        out_path,
    });

    addLibrarySources(cmd);
    cmd.addArg(b.fmt("-DFISKTA_VERSION=\"{s}\"", .{version}));
    cmd.addArg(b.fmt("-DFISKTA_ABI_MAJOR={d}", .{version_parts.major}));
    cmd.addArg(b.fmt("-DFISKTA_ABI_MINOR={d}", .{version_parts.minor}));
    cmd.addArg("-DFISKTA_BUILD");
    cmd.addArgs(&.{ "-target", target_triple });

    // Optimization
    addOptimizeArgs(cmd, optimize);

    // Platform-specific flags
    if (target_os == .linux) {
        cmd.addArg("-fPIC");
        cmd.addArg("-fvisibility=hidden");
        cmd.addArg(b.fmt("-Wl,-soname,{s}", .{file_name}));
        cmd.addArg(b.fmt("-Wl,--version-script={s}/fiskta.map", .{b.pathFromRoot(".")}));
    } else if (target_os == .macos) {
        cmd.addArg("-fPIC");
        cmd.addArg("-fvisibility=hidden");
        cmd.addArg(b.fmt("-Wl,-install_name,@rpath/{s}", .{file_name}));
        // Note: -fvisibility=hidden + FISKTA_API attributes control symbol visibility
        // zig lld doesn't support -exported_symbols_list in cross-compile mode
    }
    return cmd;
}
