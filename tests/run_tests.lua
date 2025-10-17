#!/usr/bin/env luajit

-- Minimal Lua test harness prototype for fiskta

local arg = arg

local function usage()
    print("Usage: lua tests/run_tests.lua [--exe PATH] [--filter SUBSTR] [--list] [--no-fixtures] [--slow]")
end

local function dirname(path)
    local dir = path:match("^(.*)/[^/]+$") or "."
    if dir == "" then
        dir = "."
    end
    return dir
end

local SCRIPT_DIR = dirname(arg[0] or "")
local ROOT = SCRIPT_DIR .. "/.."
local FIX_DIR = ROOT .. "/tests/fixtures"
local PROGRAM_FAIL_EXIT = 1

local function ensure_dir(path)
    local ok = os.execute(string.format("mkdir -p %q", path))
    if not ok then
        error("failed to create directory: " .. path)
    end
end

local function write_file(path, data)
    local f, err = io.open(path, "wb")
    if not f then
        error("failed to open " .. path .. ": " .. err)
    end
    f:write(data)
    f:close()
end

local function make_fixtures()
    ensure_dir(FIX_DIR)
    write_file(FIX_DIR .. "/small.txt", "Header\nbody 1\nbody 2\nERROR hit\ntail\nERROR 2\nfoo\nHEADER\nbar\n")
    write_file(FIX_DIR .. "/overlap.txt", "abcdefghij")
    write_file(FIX_DIR .. "/lines.txt", table.concat({
        "L01 a\n", "L02 bb\n", "L03 ccc\n", "L04 dddd\n", "L05 eeeee\n",
        "L06 ffffff\n", "L07 ggggggg\n", "L08 hhhhhhhh\n",
        "L09 iiiiiiiii\n", "L10 jjjjjjjjjj\n",
    }))
    write_file(FIX_DIR .. "/label-offset.txt", "A\nB\nX\n")
    write_file(FIX_DIR .. "/take-until-empty.txt", "HEADtail\n")
end

local has_ffi, ffi = pcall(require, "ffi")

if not has_ffi then
    error("tests/run_tests.lua requires LuaJIT with FFI")
end

local bit_mod, bxor, band, bor, rshift, lshift, ror, bnot
do
    local ok, mod = pcall(require, "bit")
    if ok then
        bit_mod = mod
        bxor, band, bor = mod.bxor, mod.band, mod.bor
        rshift, lshift = mod.rshift, mod.lshift
        ror = mod.ror
        bnot = mod.bnot
    else
        ok, mod = pcall(require, "bit32")
        if ok then
            bit_mod = mod
            bxor, band, bor = mod.bxor, mod.band, mod.bor
            rshift, lshift = mod.rshift, mod.lshift
            ror = mod.rrotate
            bnot = mod.bnot
        end
    end
end

if not (bxor and band and bor and rshift and lshift and bnot) then
    error("requires LuaJIT bit.* library")
end

local function mask32(x)
    local r = band(x, 0xffffffff)
    if r < 0 then
        r = r + 0x100000000
    end
    return r
end

if not ror then
    local function lshift_wrap(x, n)
        return mask32(lshift(x, n))
    end
    ror = function(x, n)
        n = n % 32
        if n == 0 then
            return mask32(x)
        end
        return mask32(bor(rshift(x, n), lshift_wrap(x, 32 - n)))
    end
end

local function shell_quote(token)
    if token == "" then
        return "''"
    end
    if token:find("[^%w_@%./%-]") then
        return "'" .. token:gsub("'", "'\\''") .. "'"
    end
    return token
end

local function read_file(path)
    local f = assert(io.open(path, "rb"))
    local data = f:read("*a")
    f:close()
    return data
end

local function run_command_fallback(exe, tokens, input_path, extra_args, stdin_data)
    local stdout_tmp = os.tmpname()
    local stderr_tmp = os.tmpname()
    local stdin_tmp

    local parts = { exe }
    if extra_args then
        for _, v in ipairs(extra_args) do
            parts[#parts + 1] = v
        end
    end
    if input_path then
        parts[#parts + 1] = "--input"
        parts[#parts + 1] = input_path
    end
    for _, t in ipairs(tokens) do
        parts[#parts + 1] = t
    end

    for i = 1, #parts do
        parts[i] = shell_quote(parts[i])
    end

    local cmd = table.concat(parts, " ")

    if stdin_data then
        stdin_tmp = os.tmpname()
        write_file(stdin_tmp, stdin_data)
        cmd = string.format("cat %s | %s", shell_quote(stdin_tmp), cmd)
    end

    cmd = string.format("%s >%s 2>%s", cmd, shell_quote(stdout_tmp), shell_quote(stderr_tmp))

    local ok, why, status = os.execute(cmd)

    local stdout = read_file(stdout_tmp)
    local stderr = read_file(stderr_tmp)

    os.remove(stdout_tmp)
    os.remove(stderr_tmp)
    if stdin_tmp then
        os.remove(stdin_tmp)
    end

    local exit_code
    if type(ok) == "number" then
        exit_code = math.floor(ok / 256)
    elseif ok == true or ok == 0 then
        exit_code = 0
    elseif why == "exit" then
        exit_code = status
    else
        exit_code = status or 1
    end

    return exit_code, stdout, stderr
end

local run_command_impl

if has_ffi and ffi.os ~= "Windows" then
    ffi.cdef[[
        typedef long ssize_t;
        typedef unsigned long size_t;
        typedef int pid_t;
        int pipe(int pipefd[2]);
        pid_t fork(void);
        int dup2(int oldfd, int newfd);
        int close(int fd);
        int execvp(const char *file, char *const argv[]);
        ssize_t read(int fd, void *buf, size_t count);
        ssize_t write(int fd, const void *buf, size_t count);
        pid_t waitpid(pid_t pid, int *status, int options);
        void _exit(int status);
        int errno;
    ]]

    local EINTR = 4

    local function build_argv(exe, tokens, input_path, extra_args)
        local args = { exe }
        if extra_args then
            for _, v in ipairs(extra_args) do
                args[#args + 1] = tostring(v)
            end
        end
        if input_path then
            args[#args + 1] = "--input"
            args[#args + 1] = input_path
        end
        for _, t in ipairs(tokens) do
            args[#args + 1] = tostring(t)
        end
        return args
    end

    local function make_argv(args)
        local count = #args
        local argv = ffi.new("char *[?]", count + 1)
        local storage = {}
        for i = 1, count do
            local s = args[i]
            local buf = ffi.new("char[?]", #s + 1)
            ffi.copy(buf, s)
            storage[i] = buf
            argv[i - 1] = buf
        end
        argv[count] = nil
        return argv, storage
    end

    local function read_fd(fd)
        local chunks = {}
        local buf = ffi.new("unsigned char[4096]")
        while true do
            local n = ffi.C.read(fd, buf, 4096)
            if n == 0 then
                break
            end
            if n < 0 then
                local err = ffi.errno()
                if err ~= EINTR then
                    ffi.C.close(fd)
                    error("read error: errno=" .. err)
                end
            else
                chunks[#chunks + 1] = ffi.string(buf, n)
            end
        end
        ffi.C.close(fd)
        return table.concat(chunks)
    end

    local function write_all(fd, data)
        if not data or #data == 0 then
            ffi.C.close(fd)
            return
        end
        local len = #data
        local ptr = ffi.cast("const unsigned char*", data)
        local written = 0
        while written < len do
            local n = ffi.C.write(fd, ptr + written, len - written)
            if n < 0 then
                if ffi.errno() == EINTR then
                    n = 0
                else
                    ffi.C.close(fd)
                    error("write error: errno=" .. ffi.errno())
                end
            end
            written = written + n
        end
        ffi.C.close(fd)
    end

    run_command_impl = function(exe, tokens, input_path, extra_args, stdin_data)
        local args = build_argv(exe, tokens, input_path, extra_args)
        local argv, storage = make_argv(args)

        local stdout_pipe = ffi.new("int[2]")
        local stderr_pipe = ffi.new("int[2]")
        if ffi.C.pipe(stdout_pipe) ~= 0 or ffi.C.pipe(stderr_pipe) ~= 0 then
            error("pipe failed: errno=" .. ffi.errno())
        end

        local stdin_pipe
        if stdin_data then
            stdin_pipe = ffi.new("int[2]")
            if ffi.C.pipe(stdin_pipe) ~= 0 then
                error("pipe failed: errno=" .. ffi.errno())
            end
        end

        local pid = ffi.C.fork()
        if pid == 0 then
            if stdin_pipe then
                ffi.C.close(stdin_pipe[1])
                ffi.C.dup2(stdin_pipe[0], 0)
                ffi.C.close(stdin_pipe[0])
            end

            ffi.C.close(stdout_pipe[0])
            ffi.C.close(stderr_pipe[0])
            ffi.C.dup2(stdout_pipe[1], 1)
            ffi.C.dup2(stderr_pipe[1], 2)
            ffi.C.close(stdout_pipe[1])
            ffi.C.close(stderr_pipe[1])

            ffi.C.execvp(argv[0], argv)
            local msg = "execvp failed\n"
            ffi.C.write(2, msg, #msg)
            ffi.C._exit(127)
        elseif pid < 0 then
            error("fork failed: errno=" .. ffi.errno())
        end

        ffi.C.close(stdout_pipe[1])
        ffi.C.close(stderr_pipe[1])

        if stdin_pipe then
            ffi.C.close(stdin_pipe[0])
            write_all(stdin_pipe[1], stdin_data)
        end

        local stdout = read_fd(stdout_pipe[0])
        local stderr = read_fd(stderr_pipe[0])

        local status = ffi.new("int[1]")
        while true do
            local wp = ffi.C.waitpid(pid, status, 0)
            if wp == pid then
                break
            end
            if wp == -1 then
                if ffi.errno() ~= EINTR then
                    error("waitpid failed: errno=" .. ffi.errno())
                end
            end
        end

        local code
        local st = tonumber(status[0])
        if band(st, 0x7f) ~= 0 then
            code = 128 + band(st, 0x7f)
        else
            code = band(rshift(st, 8), 0xff)
        end

        return code, stdout, stderr
    end
else
    run_command_impl = run_command_fallback
end

local function run_command(...)
    return run_command_impl(...)
end

local function add32(...)
    local sum = 0
    for i = 1, select("#", ...) do
        sum = sum + select(i, ...)
    end
    return mask32(sum)
end

local K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
}

local sha256_hex

if has_ffi and ffi.os ~= "Windows" then
    local crypto
    local has_crypto = false
    do
        local candidates = { "crypto", "libcrypto.so.3", "libcrypto.so", "libcrypto" }
        for _, name in ipairs(candidates) do
            local ok, lib = pcall(ffi.load, name)
            if ok then
                crypto = lib
                has_crypto = true
                break
            end
        end
    end

    local function sha256_hex_pure(data)
        local len = #data
        local ptr = ffi.cast("const unsigned char*", data)
        local w = {}

        local h0, h1, h2, h3 = 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a
        local h4, h5, h6, h7 = 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19

        local function compress(block_ptr)
            for j = 0, 15 do
                local base = j * 4
                local b0 = tonumber(block_ptr[base])
                local b1 = tonumber(block_ptr[base + 1])
                local b2 = tonumber(block_ptr[base + 2])
                local b3 = tonumber(block_ptr[base + 3])
                w[j] = mask32(bor(
                    lshift(b0, 24),
                    lshift(b1, 16),
                    lshift(b2, 8),
                    b3
                ))
            end
            for j = 16, 63 do
                local v = w[j - 15]
                local s0 = bxor(ror(v, 7), ror(v, 18), rshift(v, 3))
                v = w[j - 2]
                local s1 = bxor(ror(v, 17), ror(v, 19), rshift(v, 10))
                w[j] = add32(w[j - 16], s0, w[j - 7], s1)
            end

            local a, b, c, d = h0, h1, h2, h3
            local e, f, g, hh = h4, h5, h6, h7

            for j = 0, 63 do
                local S1 = bxor(ror(e, 6), ror(e, 11), ror(e, 25))
                local ch = bxor(band(e, f), band(mask32(bnot(e)), g))
                local temp1 = add32(hh, S1, ch, K[j + 1], w[j])
                local S0 = bxor(ror(a, 2), ror(a, 13), ror(a, 22))
                local maj = bxor(band(a, b), band(a, c), band(b, c))
                local temp2 = add32(S0, maj)

                hh = g
                g = f
                f = e
                e = add32(d, temp1)
                d = c
                c = b
                b = a
                a = add32(temp1, temp2)
            end

            h0 = add32(h0, a)
            h1 = add32(h1, b)
            h2 = add32(h2, c)
            h3 = add32(h3, d)
            h4 = add32(h4, e)
            h5 = add32(h5, f)
            h6 = add32(h6, g)
            h7 = add32(h7, hh)
        end

        local processed = 0
        local full_blocks = math.floor(len / 64)
        for _ = 1, full_blocks do
            compress(ptr + processed)
            processed = processed + 64
        end

        local tail = ffi.new("unsigned char[128]")
        ffi.fill(tail, 128, 0)
        local rem = len - processed
        if rem > 0 then
            ffi.copy(tail, ptr + processed, rem)
        end
        tail[rem] = 0x80

        local total_blocks = (rem <= 55) and 1 or 2
        local bit_len = len * 8
        local high = math.floor(bit_len / 0x100000000)
        local low = bit_len % 0x100000000
        local len_pos = total_blocks * 64 - 8
        tail[len_pos + 0] = band(rshift(high, 24), 0xff)
        tail[len_pos + 1] = band(rshift(high, 16), 0xff)
        tail[len_pos + 2] = band(rshift(high, 8), 0xff)
        tail[len_pos + 3] = band(high, 0xff)
        tail[len_pos + 4] = band(rshift(low, 24), 0xff)
        tail[len_pos + 5] = band(rshift(low, 16), 0xff)
        tail[len_pos + 6] = band(rshift(low, 8), 0xff)
        tail[len_pos + 7] = band(low, 0xff)

        for i = 0, total_blocks - 1 do
            compress(tail + i * 64)
        end

        return string.format("%08x%08x%08x%08x%08x%08x%08x%08x",
            mask32(h0), mask32(h1), mask32(h2), mask32(h3),
            mask32(h4), mask32(h5), mask32(h6), mask32(h7))
    end

    if has_crypto then
        ffi.cdef[[unsigned char *SHA256(const void *d, size_t n, unsigned char *md);]]
        sha256_hex = function(data)
            local out = ffi.new("unsigned char[32]")
            crypto.SHA256(data, #data, out)
            local t = {}
            for i = 0, 31 do
                t[#t + 1] = string.format("%02x", tonumber(out[i]))
            end
            return table.concat(t)
        end
    else
        sha256_hex = sha256_hex_pure
    end
else
    sha256_hex = function(data)
        local tmp_path = os.tmpname()
        write_file(tmp_path, data)
        local cmd = string.format("sha256sum %s", shell_quote(tmp_path))
        local fh = assert(io.popen(cmd, "r"))
        local out = fh:read("*a") or ""
        fh:close()
        os.remove(tmp_path)
        local hash = out:match("^[0-9a-fA-F]+")
        return hash and hash:lower() or ""
    end
end

local function load_generated_tests()
    local generated_path = SCRIPT_DIR .. "/tests_suite.lua"
    local chunk, err = loadfile(generated_path)
    if not chunk then
        error("tests/tests_suite.lua missing; add the canonical test table before running")
    end
    local ok, data = pcall(chunk)
    if not ok then
        error("failed to load tests/tests_suite.lua: " .. tostring(data))
    end
    return data
end

local tests = load_generated_tests()

local function expect_stdout(actual, expect)
    if expect.stdout ~= nil then
        if actual == expect.stdout then
            return true, ""
        end
        return false, string.format("stdout mismatch (want %q, got %q)", expect.stdout, actual)
    end
    if expect.stdout_startswith ~= nil then
        local prefix = expect.stdout_startswith
        if actual:sub(1, #prefix) == prefix then
            return true, ""
        end
        return false, string.format("stdout prefix mismatch (want %q)", prefix)
    end
    if expect.stdout_len ~= nil then
        local want_len = tonumber(expect.stdout_len)
        if #actual == want_len then
            return true, ""
        end
        return false, string.format("stdout length mismatch (want %d, got %d)", want_len, #actual)
    end
    if expect.stdout_sha256 ~= nil then
        local got = sha256_hex(actual)
        local want = expect.stdout_sha256:lower()
        if got == want then
            return true, ""
        end
        return false, string.format("stdout sha256 mismatch (want %s, got %s)", want, got)
    end
    return #actual == 0, string.format("expected empty stdout, got %d bytes", #actual)
end

local function expect_stderr(actual, expect)
    if expect.stderr ~= nil then
        if actual == expect.stderr then
            return true, ""
        end
        return false, string.format("stderr mismatch (want %q, got %q)", expect.stderr, actual)
    end
    return true, ""
end

local function parse_args()
    local opts = {
        exe = ROOT .. "/fiskta",
        filter = nil,
        list = false,
        no_fixtures = false,
        slow = false,
    }

    local i = 1
    while i <= #arg do
        local a = arg[i]
        if a == "--exe" then
            i = i + 1
            if i > #arg then
                usage()
                os.exit(1)
            end
            opts.exe = arg[i]
        elseif a == "--filter" then
            i = i + 1
            if i > #arg then
                usage()
                os.exit(1)
            end
            opts.filter = arg[i]
        elseif a == "--list" then
            opts.list = true
        elseif a == "--no-fixtures" then
            opts.no_fixtures = true
        elseif a == "--slow" then
            opts.slow = true
        elseif a == "--help" or a == "-h" then
            usage()
            os.exit(0)
        else
            print("Unknown option: " .. tostring(a))
            usage()
            os.exit(1)
        end
        i = i + 1
    end
    return opts
end

local function main()
    local opts = parse_args()

    if not opts.no_fixtures then
        make_fixtures()
    end

    local selected = {}
    for _, t in ipairs(tests) do
        if (not t.slow or opts.slow) and (not opts.filter or t.id:find(opts.filter, 1, true)) then
            selected[#selected + 1] = t
        end
    end

    if opts.list then
        for _, t in ipairs(selected) do
            print(t.id)
        end
        return 0
    end

    local passed, failed = 0, 0

    for _, t in ipairs(selected) do
        local input_path
        if t.input_file == nil then
            input_path = nil
        elseif t.input_file == "-" then
            input_path = "-"
        else
            input_path = FIX_DIR .. "/" .. t.input_file
        end

        local code, out, err = run_command(opts.exe, t.tokens, input_path, t.extra_args, t.stdin)

        local ok_stdout, msg_stdout = expect_stdout(out, t.expect)
        local ok_stderr, msg_stderr = expect_stderr(err, t.expect)
        local ok_exit = (code == t.expect.exit)

        if ok_stdout and ok_stderr and ok_exit then
            print(string.format("[PASS] %s", t.id))
            passed = passed + 1
        else
            print(string.format("[FAIL] %s", t.id))
            if not ok_exit then
                print(string.format("  exit: want %d, got %d", t.expect.exit, code))
            end
            if not ok_stdout then
                print("  " .. msg_stdout)
                if #out > 0 then
                    print("  stdout data: " .. out:gsub("\n", "\\n"))
                end
            end
            if not ok_stderr then
                print("  " .. msg_stderr)
            elseif #err > 0 then
                print("  stderr: " .. err:gsub("\n", "\\n"))
            end
            failed = failed + 1
        end
    end

    local total = passed + failed
    print(string.format("\nSummary: %d/%d passed, %d failed", passed, total, failed))

    if failed > 0 then
        return 1
    end
    return 0
end

local rc = main()
os.exit(rc)
