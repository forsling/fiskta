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

local function run_command(exe, tokens, input_path, extra_args, stdin_data)
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
    if ok == true or ok == 0 then
        exit_code = 0
    elseif why == "exit" then
        exit_code = status
    else
        exit_code = status or 1
    end

    return exit_code, stdout, stderr
end

local function sha256_hex(data)
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

local tests = {
    {
        id = "gram-001-clause-sep",
        tokens = {"take", "+3b", "THEN", "take", "+2b"},
        input_file = "overlap.txt",
        expect = { stdout = "abcde", exit = 0 },
    },
    {
        id = "gram-002-signed-skip",
        tokens = {"skip", "-5b"},
        input_file = "overlap.txt",
        expect = { stdout = "", exit = 0 },
    },
    {
        id = "gram-005-view-inline-offsets",
        tokens = {"view", "BOF+2b", "BOF+5b", "take", "+3b"},
        input_file = "overlap.txt",
        expect = { stdout = "cde", exit = 0 },
    },
    {
        id = "take-stdout-len",
        tokens = {"take", "+3b"},
        input_file = "overlap.txt",
        expect = { stdout_len = 3, exit = 0 },
    },
    {
        id = "gram-008-print-hex",
        tokens = {"print", [[\x00\xFA]]},
        input_file = "overlap.txt",
        expect = {
            stdout_sha256 = "d96bdf2090bd7dafe1ab0d9f7ffc4720d002c07abbf48df3969af497b1edbfb9",
            exit = 0,
        },
    },
    {
        id = "io-001-stdin-forward",
        tokens = {"take", "+5b"},
        input_file = "-",
        stdin = "Hello\nWorld\n",
        expect = { stdout = "Hello", exit = 0 },
    },
    {
        id = "logic-023-or-second-executes",
        tokens = {"find", "xyz", "take", "+2b", "OR", "take", "+2b"},
        input_file = "overlap.txt",
        expect = { stdout = "ab", exit = 0 },
    },
    {
        id = "error-005-invalid-location",
        tokens = {"skip", "to", "NOTEXIST"},
        input_file = "small.txt",
        expect = { stdout = "", exit = PROGRAM_FAIL_EXIT },
    },
    {
        id = "logic-016-then-sequential",
        tokens = {"find", "abc", "THEN", "skip", "3b", "THEN", "take", "+3b"},
        input_file = "overlap.txt",
        expect = { stdout = "def", exit = 0 },
    },
    {
        id = "logic-017-then-with-failure",
        tokens = {"find", "abc", "THEN", "find", "xyz", "THEN", "take", "+3b"},
        input_file = "overlap.txt",
        expect = { stdout = "abc", exit = 0 },
    },
    {
        id = "loop-slow-follow-001-existing-data",
        tokens = {"take", "1l"},
        input_file = "small.txt",
        extra_args = {"--follow", "-u", "50ms"},
        expect = { stdout = "Header\n", exit = 0 },
        slow = true,
    },
    {
        id = "fail-005-then-continues",
        tokens = {"fail", "Failed", "THEN", "take", "+3b"},
        input_file = "overlap.txt",
        expect = { stdout = "abc", stderr = "Failed", exit = 0 },
    },
    {
        id = "edge-001-inline-offset-label",
        tokens = {"label", "HERE", "THEN", "skip", "to", "HERE+2l", "THEN", "take", "1l"},
        input_file = "label-offset.txt",
        expect = { stdout = "X\n", exit = 0 },
    },
    {
        id = "edge-003-take-until-empty-span",
        tokens = {"take", "until", "HEAD", "THEN", "take", "4b"},
        input_file = "take-until-empty.txt",
        expect = { stdout = "HEAD", exit = 0 },
    },
    {
        id = "lines-negative",
        tokens = {"skip", "7b", "take", "-1l"},
        input_file = "lines.txt",
        expect = { stdout = "L01 a\n", exit = 0 },
    },
}

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
    return #actual == 0, string.format("expected empty stderr, got %d bytes", #actual)
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
