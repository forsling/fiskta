#!/usr/bin/env luajit
-- fiskta grammar-guided fuzzer (standalone, no dependencies)
-- Just run: ./fuzz
-- Or with args: ./fuzz --cases 1000 --workers 2

local ffi = require("ffi")
local bit = require("bit")

-- ========= POSIX FFI =========
ffi.cdef[[
typedef int pid_t;
typedef long time_t;
typedef void (*sighandler_t)(int);
int     kill(pid_t pid, int sig);
pid_t   fork(void);
int     execvp(const char *file, char *const argv[]);
pid_t   waitpid(pid_t pid, int *status, int options);
int     usleep(unsigned int usec);
int     open(const char *pathname, int flags, ...);
int     dup2(int oldfd, int newfd);
int     close(int fd);
time_t  time(time_t *t);
int     sysconf(int name);
char*   getenv(const char *name);
int     setenv(const char *name, const char *value, int overwrite);
sighandler_t signal(int signum, sighandler_t handler);
extern int errno;
const char *strerror(int errnum);
]]

local SIGINT = 2
local SIGKILL = 9
local WNOHANG = 1
local O_WRONLY, O_CREAT, O_TRUNC = 1, 64, 512
local S_IRUSR, S_IWUSR, S_IRGRP, S_IROTH = 256,128,32,4 -- 0644
local _SC_NPROCESSORS_ONLN = 84 -- Linux; macOS uses 58

-- ========= System utils =========
local function file_exists(path)
  local f = io.open(path, "r")
  if f then f:close(); return true end
  return false
end

local function get_cpu_count()
  local n = ffi.C.sysconf(_SC_NPROCESSORS_ONLN)
  if n > 0 then return tonumber(n) end
  -- Fallback: try macOS value
  n = ffi.C.sysconf(58)
  if n > 0 then return tonumber(n) end
  return 1 -- default
end

local function auto_detect_binary()
  if file_exists("./fiskta-asan") then return "./fiskta-asan", true end
  if file_exists("./fiskta") then return "./fiskta", false end
  io.stderr:write("Error: No fiskta binary found. Run ./build.sh first\n")
  os.exit(2)
end

local function setup_asan_env()
  if not ffi.C.getenv("ASAN_OPTIONS") then
    ffi.C.setenv("ASAN_OPTIONS", "abort_on_error=1:detect_leaks=1:symbolize=1", 1)
  end
  if not ffi.C.getenv("UBSAN_OPTIONS") then
    ffi.C.setenv("UBSAN_OPTIONS", "print_stacktrace=1", 1)
  end
end

-- ========= Args =========
local function parse_args()
  local binary, is_asan = auto_detect_binary()
  if is_asan then setup_asan_env() end
  local cpu_count = get_cpu_count()

  local cfg = {
    fiskta_path = binary,
    artifacts = "artifacts",
    readme = "README.md",
    grammar = nil,
    cases = nil,  -- nil = continuous mode
    seed = tonumber(ffi.C.time(nil)),
    timeout_ms = 1500,
    max_depth = 12,
    max_repeat = 3,
    minimize = true,  -- enabled by default
    use_corpus = true,  -- corpus-based fuzzing enabled by default
    corpus_dir = "tests/fixtures",
    workers = cpu_count,  -- default to CPU count
    repro_case = nil,
    repro_input = nil,
    repro_ops = nil,
    is_asan = is_asan,
    min_ops = nil,  -- nil = no minimum
    max_ops = nil,  -- nil = no maximum
  }

  local i = 1
  while i <= #arg do
    local a = arg[i]
    if a == "--fiskta-path" then i=i+1; cfg.fiskta_path = arg[i]
    elseif a == "--artifacts" then i=i+1; cfg.artifacts = arg[i]
    elseif a == "--grammar" then i=i+1; cfg.grammar = arg[i]
    elseif a == "--readme" then i=i+1; cfg.readme = arg[i]
    elseif a == "--cases" then i=i+1; cfg.cases = tonumber(arg[i])
    elseif a == "--seed" then i=i+1; cfg.seed = tonumber(arg[i])
    elseif a == "--timeout-ms" then i=i+1; cfg.timeout_ms = tonumber(arg[i])
    elseif a == "--max-depth" then i=i+1; cfg.max_depth = tonumber(arg[i])
    elseif a == "--max-repeat" then i=i+1; cfg.max_repeat = tonumber(arg[i])
    elseif a == "--minimize" then cfg.minimize = true
    elseif a == "--no-minimize" then cfg.minimize = false
    elseif a == "--corpus" then cfg.use_corpus = true
    elseif a == "--no-corpus" then cfg.use_corpus = false
    elseif a == "--corpus-dir" then i=i+1; cfg.corpus_dir = arg[i]
    elseif a == "--workers" then i=i+1; cfg.workers = tonumber(arg[i])
    elseif a == "--min-ops" then i=i+1; cfg.min_ops = tonumber(arg[i])
    elseif a == "--max-ops" then i=i+1; cfg.max_ops = tonumber(arg[i])
    elseif a == "--quick" then cfg.cases = 10000; cfg.workers = 2  -- quick mode
    elseif a == "--repro-case" then i=i+1; cfg.repro_case = tonumber(arg[i])
    elseif a == "--repro" then i=i+1; cfg.repro_input = arg[i]; i=i+1; cfg.repro_ops = arg[i]
    elseif a == "-h" or a == "--help" then
      io.write([[
fiskta grammar-guided fuzzer

Usage:
  ./fuzz                              Run with smart defaults (continuous mode)
  ./fuzz --cases 10000                Run fixed number of cases
  ./fuzz --quick                      Quick mode (10k cases, 2 workers)
  ./fuzz --repro-case N               Reproduce crash case N
  ./fuzz --repro <in.bin> <ops.txt>   Reproduce specific case

Options:
  --fiskta-path <path>     Binary to test (default: auto-detect)
  --artifacts <dir>        Output directory (default: artifacts/)
  --readme <path>          Grammar source (default: README.md)
  --cases N                Fixed case count (default: continuous)
  --minimize               Minimize crashes (default: enabled)
  --no-minimize            Disable minimization
  --corpus                 Use corpus-based fuzzing (default: enabled)
  --no-corpus              Pure generation mode (no corpus)
  --corpus-dir <path>      Corpus directory (default: tests/fixtures/)
  --timeout-ms N           Per-case timeout (default: 1500ms)
  --seed N                 RNG seed (default: current time)
  --workers N              Parallel workers (default: CPU count)
  --min-ops N              Minimum operations per program (default: no limit)
  --max-ops N              Maximum operations per program (default: no limit)

Examples:
  ./fuzz                   # Continuous fuzzing, Ctrl-C to stop
  ./fuzz --cases 50000     # Run 50k cases then exit
  ./fuzz --quick           # Quick test (10k cases)
  ./fuzz --min-ops 20 --max-ops 40   # Long operation chains

]])
      os.exit(0)
    else
      io.stderr:write("Unknown arg: "..a.."\n")
      os.exit(1)
    end
    i = i + 1
  end

  return cfg
end

local cfg = parse_args()
math.randomseed(cfg.seed)

-- ========= FS utils =========
local function ensure_dir(dir)
  os.execute(string.format("mkdir -p %q", dir))
end

local function read_all(path)
  local f, e = io.open(path, "rb")
  if not f then return nil, e end
  local s = f:read("*a")
  f:close()
  return s
end

local function write_all(path, data)
  ensure_dir(cfg.artifacts)
  local f, e = io.open(path, "wb")
  if not f then error(e) end
  f:write(data)
  f:close()
end

local function read_lines(path)
  local t = {}
  for line in io.lines(path) do t[#t+1] = line end
  return t
end

-- ========= Grammar extract (last fenced block) =========
local function grammar_from_readme(readme_path)
  local s, e = read_all(readme_path)
  if not s then error(e) end
  local last_start, last_end
  local i = 1
  while true do
    local a, b = s:find("```", i, true)
    if not a then break end
    local c, d = s:find("```", b+1, true)
    if not c then break end
    last_start, last_end = b+1, c-1
    i = d + 1
  end
  if not last_start then error("No fenced code block found in README") end
  return s:sub(last_start, last_end)
end

local grammar_src = cfg.grammar and read_all(cfg.grammar) or grammar_from_readme(cfg.readme)

-- ========= EBNF tokenize/parse (subset) =========
local function tokenize(src)
  local tokens = {}
  local i, n = 1, #src
  local function isalpha(c) return c:match("%a") end
  local function isalnum_(c) return c:match("[%w_%-%/]") end
  while i <= n do
    local c = src:sub(i,i)
    if c:match("[%s]") then
      i = i + 1
    elseif c == '"' then
      local j = i + 1
      while j <= n and src:sub(j,j) ~= '"' do j = j + 1 end
      local s = src:sub(i+1, j-1)
      table.insert(tokens, {kind="Str", lex=s})
      i = (j < n) and (j+1) or (n+1)
    elseif c == "=" or c == "." or c == "|" or c == ":" or c == "(" or c == ")" or c == "[" or c == "]" or c == "{" or c == "}" then
      table.insert(tokens, {kind=c, lex=c})
      i = i + 1
    elseif isalpha(c) then
      local j = i + 1
      while j <= n and isalnum_((src:sub(j,j))) do j = j + 1 end
      table.insert(tokens, {kind="Ident", lex=src:sub(i, j-1)})
      i = j
    else
      i = i + 1
    end
  end
  table.insert(tokens, {kind="Eof", lex=""})
  return tokens
end

local toks = tokenize(grammar_src)
local pos = 1
local function peek() return toks[pos] end
local function take(k)
  local t = toks[pos]
  if t.kind ~= k then error("parse error: expected "..k.." got "..t.kind) end
  pos = pos + 1
  return t
end

local function parse_expr()
  local function parse_primary()
    local t = peek()
    if t.kind == "Str" then take("Str"); return {tag="Terminal", v=t.lex}
    elseif t.kind == "Ident" then take("Ident"); return {tag="Nonterm", v=t.lex}
    elseif t.kind == "(" then take("("); local e = parse_expr(); take(")"); return e
    elseif t.kind == "[" then take("["); local e = parse_expr(); take("]"); return {tag="Opt", v=e}
    elseif t.kind == "{" then take("{"); local e = parse_expr(); take("}"); return {tag="Repeat", v=e}
    else error("parse_primary: unexpected "..t.kind) end
  end
  local function parse_term()
    local list = {}
    while true do
      local k = peek().kind
      if k=="Str" or k=="Ident" or k=="(" or k=="[" or k=="{" then
        table.insert(list, parse_primary())
      else break end
    end
    if #list == 1 then return list[1] end
    return {tag="Seq", v=list}
  end
  local first = parse_term()
  local choices = {first}
  while peek().kind == "|" do
    take("|")
    table.insert(choices, parse_term())
  end
  if #choices == 1 then return first end
  return {tag="Choice", v=choices}
end

local rules = {}
while peek().kind ~= "Eof" do
  local name = take("Ident").lex
  take("=")
  local def = parse_expr()
  take(".")
  rules[name] = def
end

local start_rule = rules["Program"] and "Program" or (function()
  for k,_ in pairs(rules) do return k end
end)()

-- ========= Random helpers =========
local function rand(n) return math.random(n) end
local function rand_bool() return math.random(0,1)==1 end
local function rand_from(t)
  if #t == 0 then return "x" end
  return t[rand(#t)]
end
local function pickchar(pool)
  local k = rand(#pool)
  return pool:sub(k, k)
end
local function rand_digits(n)
  local s = {}
  for i=1,n do s[i] = string.char(string.byte('0') + rand(10)-1) end
  return table.concat(s)
end
local function rand_number() return rand_digits(1 + rand(5)) end
local function rand_unit() return rand_from({"b","l","c"}) end
local function rand_signed_number()
  local sign = rand_bool() and "-" or ""
  return sign .. rand_number()
end
local function rand_offset()
  local sign = rand_bool() and "-" or "+"
  return sign .. rand_number() .. rand_unit()
end
local function rand_upper_letter()
  return string.char(string.byte('A') + rand(26)-1)
end
local function rand_name()
  local len = rand(9)
  local s = { string.char(string.byte('A') + rand(26)-1) }
  local pool = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-"
  for i=1,len do
    s[#s+1] = pickchar(pool)
  end
  return table.concat(s)
end

local function rand_string_token()
  local n = 1 + rand(25)
  local pool = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 \t.:;,_-+/\\[]{}()|?*^$@!~\n\r"
  local t = {}
  for i=1,n do t[i] = pickchar(pool) end
  local s = table.concat(t):gsub("^[ \n\r\t]+",""):gsub("[ \n\r\t]+$","")
  if s == "" then s = "x" end
  return s
end

local function rand_hex_string()
  local pairs = 1 + rand(12)
  local hexd = "0123456789ABCDEF"
  local t = {}
  for i=1,pairs do
    if i>1 and rand_bool() then t[#t+1] = " " end
    t[#t+1] = pickchar(hexd)
    t[#t+1] = pickchar(hexd)
  end
  return table.concat(t)
end

-- ========= Generator (raw tokens) =========
local function gen_node(node, out, depth, limits)
  if not node then return end
  limits = limits or {max_depth=cfg.max_depth, max_repeat=cfg.max_repeat}
  local tag = node.tag
  if tag == "Terminal" then table.insert(out, node.v); return end
  if tag == "Nonterm" then
    local nm = node.v
    if nm == "Digit" then table.insert(out, tostring(rand(10)-1)); return end
    if nm == "UpperLetter" then table.insert(out, rand_upper_letter()); return end
    if nm == "Number" then table.insert(out, rand_number()); return end
    if nm == "SignedNumber" then table.insert(out, rand_signed_number()); return end
    if nm == "Unit" then table.insert(out, rand_unit()); return end
    if nm == "Name" then table.insert(out, rand_name()); return end
    if nm == "String" or nm == "ShellString" then
      if rand(6)==1 then table.insert(out, rand_hex_string()) else table.insert(out, rand_string_token()) end
      return
    end
    if nm == "Offset" then table.insert(out, rand_offset()); return end
    if nm == "Location" then
      local bases = {"cursor","BOF","EOF","match-start","match-end","line-start","line-end"}
      if rand(4)==1 then table.insert(out, rand_name()) else table.insert(out, rand_from(bases)) end
      return
    end
    if nm == "LocationExpr" then
      gen_node({tag="Nonterm", v="Location"}, out, depth+1, limits)
      if rand_bool() then gen_node({tag="Nonterm", v="Offset"}, out, depth+1, limits) end
      return
    end
    if nm == "AtExpr" then
      local bases = {"match-start","match-end","line-start","line-end"}
      table.insert(out, rand_from(bases))
      if rand_bool() then table.insert(out, rand_offset()) end
      return
    end
    if depth > limits.max_depth then return end
    local sub = rules[nm]
    if not sub then return end
    return gen_node(sub, out, depth+1, limits)
  elseif tag == "Seq" then
    for _,p in ipairs(node.v) do gen_node(p, out, depth+1, limits) end
  elseif tag == "Choice" then
    local i = 1 + rand(#node.v)
    gen_node(node.v[i], out, depth+1, limits)
  elseif tag == "Opt" then
    if rand_bool() then gen_node(node.v, out, depth+1, limits) end
  elseif tag == "Repeat" then
    local n = rand(cfg.max_repeat+1)
    for _=1,n do gen_node(node.v, out, depth+1, limits) end
  end
end

-- ========= Rewriter (fiskta-friendly tokens) =========
local function is_label(s) return s:match("^[A-Z][A-Z0-9_%-]*$") ~= nil end
local function is_location_base(s)
  return s=="cursor" or s=="BOF" or s=="EOF" or s=="match-start" or s=="match-end" or s=="line-start" or s=="line-end" or is_label(s)
end
local function is_at_base(s)
  return s=="match-start" or s=="match-end" or s=="line-start" or s=="line-end"
end
local function is_num(s) return s:match("^%d+$") ~= nil end
local function is_unit(s) return s=="b" or s=="l" or s=="c" end
local function looks_like_offset(s) return s:match("^[%+%-]%d+[blc]$") ~= nil end
local function looks_like_signednum(s) return s:match("^%-?%d+$") ~= nil end

local function rewrite_tokens(toks)
  local out = {}
  local i = 1
  while i <= #toks do
    local a = toks[i]
    local b = toks[i+1]
    local c = toks[i+2]
    if a and is_location_base(a) and b and looks_like_offset(b) then
      table.insert(out, a .. b); i = i + 2
    elseif a and is_at_base(a) and b and looks_like_offset(b) then
      table.insert(out, a .. b); i = i + 2
    elseif a == "take" and b and is_num(b) and c and is_unit(c) then
      table.insert(out, a); table.insert(out, b..c); i = i + 3
    elseif a == "take" and b and looks_like_signednum(b) and c and is_unit(c) then
      table.insert(out, a); table.insert(out, b..c); i = i + 3
    elseif b and is_num(a) and is_unit(b) then
      table.insert(out, a..b); i = i + 2
    else
      table.insert(out, a or ""); i = i + 1
    end
  end
  local pruned = {}
  local drop = {["="]=1, ["."]=1, ["|"]=1, [":"]=1, ["("]=1, [")"]=1, ["["]=1, ["]"]=1, ["{"]=1, ["}"]=1, [""]=1}
  for _,x in ipairs(out) do if not drop[x] then table.insert(pruned, x) end end
  return pruned
end

-- ========= fiskta exec =========
local function run_fiskta(ops_tokens, input_path, stdout_path, stderr_path)
  local argc = 3 + 1 + #ops_tokens + 1
  local argv = ffi.new("char*[?]", argc)
  local idx = 0
  local function set(s) argv[idx] = ffi.cast("char*", ffi.new("char[?]", #s+1, s)); idx = idx + 1 end
  set(cfg.fiskta_path); set("--input"); set(input_path); set("--")
  for _,t in ipairs(ops_tokens) do set(t) end
  argv[argc-1] = nil

  local pid = ffi.C.fork()
  if pid == 0 then
    local mode = bit.bor(S_IRUSR,S_IWUSR,S_IRGRP,S_IROTH)
    do
      local fd
      if stdout_path and #stdout_path > 0 then
        fd = ffi.C.open(stdout_path, bit.bor(O_WRONLY,O_CREAT,O_TRUNC), mode)
      else
        fd = ffi.C.open("/dev/null", O_WRONLY)
      end
      if fd >= 0 then ffi.C.dup2(fd, 1); ffi.C.close(fd) end
    end
    do
      local fd
      if stderr_path and #stderr_path > 0 then
        fd = ffi.C.open(stderr_path, bit.bor(O_WRONLY,O_CREAT,O_TRUNC), mode)
      else
        fd = ffi.C.open("/dev/null", O_WRONLY)
      end
      if fd >= 0 then ffi.C.dup2(fd, 2); ffi.C.close(fd) end
    end
    ffi.C.execvp(argv[0], argv)
    os.exit(127)
  end

  local waited, status = 0, ffi.new("int[1]", 0)
  local deadline_us = cfg.timeout_ms * 1000
  while true do
    local r = ffi.C.waitpid(pid, status, WNOHANG)
    if r == pid then
      local st = status[0]
      if bit.band(st, 0x7f) == 0 then
        return {crashed=false, timed_out=false, exit=bit.rshift(bit.band(st, 0xff00), 8), signal=nil}
      else
        return {crashed=true, timed_out=false, exit=128 + bit.band(st, 0x7f), signal=bit.band(st, 0x7f)}
      end
    elseif r == 0 then
      if waited >= deadline_us then
        ffi.C.kill(pid, SIGKILL)
        ffi.C.waitpid(pid, status, 0)
        return {crashed=false, timed_out=true, exit=-2, signal=nil}
      end
      ffi.C.usleep(1000)
      waited = waited + 1000
    else
      return {crashed=true, timed_out=false, exit=-3, signal=nil}
    end
  end
end

local function save_case(id, ops_tokens, input_data, res)
  local base = string.format("%s/case_%d", cfg.artifacts, id)
  ensure_dir(cfg.artifacts)
  write_all(base..".ops.txt", table.concat(ops_tokens, "\n"))
  write_all(base..".input.bin", input_data)
  local meta = string.format("exit=%d\nsignal=%s\ntimed_out=%s\n",
    res.exit or -999, tostring(res.signal), tostring(res.timed_out))
  write_all(base..".meta.txt", meta)
end

-- ========= Minimizer =========
local function same_failure(a, b)
  if a.timed_out and b.timed_out then return true end
  if a.crashed and b.crashed then return true end
  return (a.exit == b.exit)
end

local function minimize_tokens(orig_tokens, input_path, want_res)
  local best = {}
  for i,t in ipairs(orig_tokens) do best[i] = t end
  local improved = true
  while improved do
    improved = false
    local i = 1
    while i <= #best do
      if #best <= 1 then break end
      local trial = {}
      for j=1,#best do if j ~= i then trial[#trial+1] = best[j] end end
      local res = run_fiskta(trial, input_path, nil, nil)
      if same_failure(want_res, res) then
        best = trial
        improved = true
        i = 1
      else
        i = i + 1
      end
    end
  end
  return best
end

-- ========= Corpus loading =========
local corpus = {}

local function load_corpus()
  if not cfg.use_corpus then return end

  local max_size = 512 * 1024  -- Skip files > 512KB
  local loaded = 0

  local handle = io.popen("find " .. cfg.corpus_dir .. " -type f 2>/dev/null")
  if not handle then return end

  for path in handle:lines() do
    local f = io.open(path, "rb")
    if f then
      local size = f:seek("end")
      if size and size > 0 and size <= max_size then
        f:seek("set", 0)
        local content = f:read("*a")
        if content and #content > 0 then
          table.insert(corpus, content)
          loaded = loaded + 1
        end
      end
      f:close()
    end
  end
  handle:close()

  if loaded > 0 then
    io.write(string.format("Loaded %d corpus files from %s\n", loaded, cfg.corpus_dir))
  end
end

-- ========= Mutation strategies =========
local function mutate_bit_flip(data)
  if #data == 0 then return data end
  local pos = rand(#data)
  local bit_pos = rand(8) - 1
  local byte = data:byte(pos)
  local flipped = bit.bxor(byte, bit.lshift(1, bit_pos))
  return data:sub(1, pos - 1) .. string.char(flipped) .. data:sub(pos + 1)
end

local function mutate_byte_flip(data)
  if #data == 0 then return data end
  local pos = rand(#data)
  local new_byte = rand(256) - 1
  return data:sub(1, pos - 1) .. string.char(new_byte) .. data:sub(pos + 1)
end

local function mutate_byte_insert(data)
  local pos = rand(#data + 1)
  local byte = string.char(rand(256) - 1)
  return data:sub(1, pos - 1) .. byte .. data:sub(pos)
end

local function mutate_byte_delete(data)
  if #data <= 1 then return data end
  local pos = rand(#data)
  return data:sub(1, pos - 1) .. data:sub(pos + 1)
end

local function mutate_chunk_insert(data)
  local chunk_size = rand(16)
  local chunk = {}
  for i=1,chunk_size do chunk[i] = string.char(rand(256) - 1) end
  local pos = rand(#data + 1)
  return data:sub(1, pos - 1) .. table.concat(chunk) .. data:sub(pos)
end

local function mutate_chunk_delete(data)
  if #data <= 1 then return data end
  local chunk_size = math.min(rand(32), #data)
  local pos = rand(#data - chunk_size + 1)
  return data:sub(1, pos - 1) .. data:sub(pos + chunk_size)
end

local function mutate_splice(data)
  if #corpus < 2 then return data end
  local other = corpus[rand(#corpus)]
  local split1 = rand(#data + 1)
  local split2 = rand(#other + 1)
  return data:sub(1, split1 - 1) .. other:sub(split2)
end

local function mutate_repeat(data)
  if #data == 0 then return data end
  local chunk_size = math.min(rand(16), #data)
  local pos = rand(#data - chunk_size + 1)
  local chunk = data:sub(pos, pos + chunk_size - 1)
  local count = rand(4)
  local insert_pos = rand(#data + 1)
  return data:sub(1, insert_pos - 1) .. chunk:rep(count) .. data:sub(insert_pos)
end

local mutations = {
  mutate_bit_flip,
  mutate_byte_flip,
  mutate_byte_insert,
  mutate_byte_delete,
  mutate_chunk_insert,
  mutate_chunk_delete,
  mutate_splice,
  mutate_repeat,
}

local function apply_mutations(data, count)
  for i=1,count do
    local mutator = mutations[rand(#mutations)]
    data = mutator(data)
  end
  return data
end

-- ========= Template library (known-good programs) =========
-- These templates stress specific runtime features with valid syntax
local templates = {
  -- Regex engine stress tests (custom implementation, likely bug source)
  {"find:re", "a.*b", "take", "20b"},
  {"find:re", "[0-9]+", "take", "to", "match-end"},
  {"find:re", "(ERROR|WARNING|FATAL):", "take", "to", "line-end"},
  {"find:re", "user=[a-z]+", "take", "to", "match-end"},
  {"find:re", "^BEGIN", "take", "to", "line-end"},
  {"find:re", "END$", "take", "to", "line-end"},
  {"find:re", "a{3,5}", "take", "10b"},
  {"find:re", ".{1,100}", "take", "to", "match-end"},
  {"find:re", "\\w+@\\w+", "take", "to", "match-end"},
  {"find:re", "[^\\s]+", "take", "to", "match-end"},
  {"find:re", "(a|b|c)+", "take", "to", "match-end"},
  {"find:re", "a*b*c*", "take", "to", "match-end"},
  {"find:re", "\\d{3}-\\d{4}", "take", "to", "match-end"},

  -- Boundary operations (view limits, offsets)
  {"view", "BOF", "EOF-100b", "find:bin", "20", "take", "10b"},
  {"view", "BOF+50b", "EOF", "find", "ERROR", "take", "to", "line-end"},
  {"view", "BOF", "BOF+1000b", "find:re", "[a-z]+", "take", "to", "match-end"},
  {"skip", "100b", "view", "cursor", "EOF", "find:bin", "0A", "take", "5b"},
  {"find:bin", "0A", "view", "match-start", "match-end+50b", "take", "10b"},

  -- Label operations
  {"find", "ERROR", "label", "ERR", "skip", "10b", "skip", "to", "ERR", "take", "20b"},
  {"find:re", "BEGIN", "label", "START", "find:re", "END", "label", "END", "take", "to", "END"},
  {"find:bin", "0A", "label", "L1", "skip", "5b", "label", "L2", "skip", "to", "L1", "take", "to", "L2"},

  -- UTF-8 character boundaries
  {"find:bin", "0A", "skip", "1c", "take", "10c"},
  {"view", "BOF", "BOF+500c", "find", "ERROR", "take", "to", "line-end"},
  {"skip", "100c", "take", "20c"},

  -- Literal hex searches
  {"find:bin", "0A", "take", "to", "line-end"},
  {"find:bin", "0D0A", "take", "to", "line-end"},
  {"find:bin", "FF", "take", "10b"},
  {"find:bin", "00", "skip", "1b", "take", "50b"},

  -- Complex clause linking
  {"find", "ERROR", "take", "to", "line-end", "THEN", "skip", "1l", "take", "to", "line-end"},
  {"find", "WARNING", "take", "to", "line-end", "OR", "find", "disk", "take", "to", "line-end"},
  {"find", "READY", "take", "to", "line-end", "OR", "find", "STATE", "take", "to", "line-end"},

  -- Take operations
  {"take", "to", "BOF+100b"},
  {"find:bin", "0A", "take", "to", "match-end"},
  {"find", "ERROR", "take", "to", "line-end"},

  -- Until operations
  {"take", "until", "ERROR"},
  {"take", "until:re", "[0-9]+"},
  {"take", "until:bin", "0A"},

  -- Edge cases
  {"take", "0b"},
  {"skip", "0b", "take", "10b"},
  {"find:bin", "0A", "skip", "0l", "take", "1l"},
}

-- Extreme values for injection
local extreme_values = {
  offsets = {
    "BOF+999999999b", "EOF-999999999b",
    "BOF+999999999c", "EOF-999999999c",
    "BOF+999999999l", "EOF-999999999l",
    "cursor+999999999b", "cursor-999999999b",
    "match-start+999999999b", "match-end-999999999b",
  },
  sizes = {
    "0b", "0c", "0l",
    "999999999b", "999999999c", "999999999l",
    "-1b", "-1c", "-1l",
  },
  regexes = {
    "()",          -- empty capture
    "a*",          -- zero-or-more at start
    ".*$",         -- greedy to end
    ".{0,999999}", -- huge quantifier
    "a{999,}",     -- huge minimum
    "((((a))))",   -- nested groups
    "(a|){100}",   -- empty alt with repeat
  },
}

local function rand_extreme_offset()
  return extreme_values.offsets[rand(#extreme_values.offsets)]
end

local function rand_extreme_size()
  return extreme_values.sizes[rand(#extreme_values.sizes)]
end

local function rand_extreme_regex()
  return extreme_values.regexes[rand(#extreme_values.regexes)]
end

local function mutate_template(template)
  -- Make a copy
  local ops = {}
  for i, v in ipairs(template) do
    ops[i] = v
  end

  -- Apply 1-3 mutations
  local num_mutations = 1 + rand(3)
  for _=1,num_mutations do
    local mutation_type = rand(6)

    if mutation_type == 1 and #ops > 2 then
      -- Replace a size/offset with extreme value
      for i=1,#ops do
        if ops[i]:match("%d+[blc]$") then
          if rand(2) == 1 then
            ops[i] = rand_extreme_size()
            break
          end
        end
      end

    elseif mutation_type == 2 then
      -- Replace location with extreme offset
      for i=1,#ops do
        if ops[i]:match("^[A-Z]") or ops[i]:match("^cursor") or ops[i]:match("^match") or ops[i]:match("^line") then
          if rand(2) == 1 then
            ops[i] = rand_extreme_offset()
            break
          end
        end
      end

    elseif mutation_type == 3 then
      -- Replace regex with extreme regex
      for i=1,#ops do
        if ops[i]:match("^/.*/$") then
          if rand(2) == 1 then
            ops[i] = rand_extreme_regex()
            break
          end
        end
      end

    elseif mutation_type == 4 and #ops > 3 then
      -- Remove random operation (1-2 tokens)
      local pos = rand(#ops - 1)
      table.remove(ops, pos)
      if rand(2) == 1 and #ops > pos then
        table.remove(ops, pos)
      end

    elseif mutation_type == 5 and #ops < 30 then
      -- Insert operation from another template
      local other = templates[rand(#templates)]
      local insert_pos = rand(#ops + 1)
      if #other > 0 then
        table.insert(ops, insert_pos, other[rand(#other)])
      end

    elseif mutation_type == 6 and #ops > 2 then
      -- Swap two adjacent operations
      local pos = rand(#ops - 1)
      ops[pos], ops[pos+1] = ops[pos+1], ops[pos]
    end
  end

  return ops
end

local function gen_from_template()
  local base = templates[rand(#templates)]

  -- 20% mutate, 80% use as-is (mutations often break syntax)
  if rand(10) <= 2 then
    return mutate_template(base)
  else
    -- Return copy
    local ops = {}
    for i, v in ipairs(base) do
      ops[i] = v
    end
    return ops
  end
end

-- ========= Input generator =========
local function gen_input_from_corpus()
  if #corpus == 0 then return nil end
  local base = corpus[rand(#corpus)]
  local mutation_count = 1 + rand(5)
  return apply_mutations(base, mutation_count)
end

local function gen_input_random()
  local total = 64 + rand(4096)
  local t = {}
  local ascii_lines = {
    "Starting text\n", "Middle line\n", "Ending line\n",
    "ERROR: something failed\n", "WARNING: disk almost full\n",
    "user=john id=12345\n", "[database]\nhost=localhost\nport=5432\n",
    "BEGIN data ----\n", "END data ----\n",
    "READY STATE\n", "STATE=READY\n",
  }
  local magics = { "\x89PNG\r\n\x1a\n", "\xFF\xD8\xFF", "PK\x03\x04" }
  local len = 0
  while len < total do
    local pick = rand(11)-1
    if pick <= 5 then
      local s = ascii_lines[rand(#ascii_lines)]
      t[#t+1] = s; len = len + #s
    elseif pick == 6 and (len + 8 < total) then
      local m = magics[rand(#magics)]
      t[#t+1] = m; len = len + #m
    else
      local b = string.char(rand(256)-1)
      t[#t+1] = b; len = len + 1
    end
  end
  return table.concat(t)
end

local function gen_input()
  -- Use corpus 80% of the time if available, pure generation 20%
  if cfg.use_corpus and #corpus > 0 and rand(10) <= 8 then
    local input = gen_input_from_corpus()
    if input then return input end
  end
  return gen_input_random()
end

-- ========= Signal handling =========
local interrupted = false

local function sigint_handler(signum)
  interrupted = true
end

local function install_signal_handlers()
  -- Create C callback for signal handler
  local handler = ffi.cast("sighandler_t", sigint_handler)
  ffi.C.signal(SIGINT, handler)
end

-- ========= Stats & Progress =========
local stats = { total=0, saved=0, timeouts=0, crashed=0, exits={}, start_time=os.time() }
local function bump_exit(code) stats.exits[code] = (stats.exits[code] or 0) + 1 end

local function format_time(seconds)
  if seconds < 60 then return string.format("%ds", seconds) end
  if seconds < 3600 then return string.format("%dm%ds", math.floor(seconds/60), seconds%60) end
  return string.format("%dh%dm", math.floor(seconds/3600), math.floor((seconds%3600)/60))
end

local function show_progress()
  local elapsed = os.time() - stats.start_time
  local rate = elapsed > 0 and math.floor(stats.total / elapsed) or 0
  local mode_str = cfg.cases and string.format("%d/%d", stats.total, cfg.cases) or string.format("%d", stats.total)
  io.write(string.format("\r[%s cases | %d exec/s | %d crashes | %d timeouts | %s]",
    mode_str, rate, stats.crashed, stats.timeouts, format_time(elapsed)))
  io.flush()
end

local function write_summary()
  local lines = {
    ("total=%d"):format(stats.total),
    ("saved=%d"):format(stats.saved),
    ("timeouts=%d"):format(stats.timeouts),
    ("crashed=%d"):format(stats.crashed),
  }
  for code,count in pairs(stats.exits) do
    lines[#lines+1] = ("exit[%s]=%d"):format(tostring(code), count)
  end
  write_all(("%s/run_summary.txt"):format(cfg.artifacts), table.concat(lines, "\n").."\n")
end

-- ========= Main fuzz loop (single worker) =========
local function run_worker(worker_id, num_workers)
  install_signal_handlers()

  -- Worker-specific seed
  math.randomseed(cfg.seed + worker_id)

  local start_time = os.time()
  local local_stats = { total=0, saved=0, timeouts=0, crashed=0, exits={} }

  -- Each worker processes: worker_id, worker_id + num_workers, worker_id + 2*num_workers, ...
  local case_iter = 0
  while true do
    if interrupted then break end

    local case_id = worker_id + case_iter * num_workers
    if cfg.cases and case_id >= cfg.cases then break end

    local raw, ops, input, tmp_path, res, interesting

    -- Generation mix: 50% templates, 30% grammar, 20% pure generation
    -- With retry for min/max ops constraints
    local max_retries = 50
    local retry_count = 0
    repeat
      local gen_type = rand(10)
      if gen_type <= 5 then
        -- Template-based (50%)
        ops = gen_from_template()
      elseif gen_type <= 8 then
        -- Grammar-based (30%)
        raw = {}
        gen_node(rules[start_rule], raw, 0, {max_depth=cfg.max_depth, max_repeat=cfg.max_repeat})
        ops = rewrite_tokens(raw)
      else
        -- Pure generation (20%)
        raw = {}
        gen_node(rules[start_rule], raw, 0, {max_depth=cfg.max_depth, max_repeat=cfg.max_repeat})
        ops = rewrite_tokens(raw)
      end

      retry_count = retry_count + 1

      -- Check operation count constraints
      local ops_ok = true
      if ops and #ops > 0 then
        if cfg.min_ops and #ops < cfg.min_ops then ops_ok = false end
        if cfg.max_ops and #ops > cfg.max_ops then ops_ok = false end
      else
        ops_ok = false
      end

      if ops_ok or retry_count >= max_retries then break end
    until false

    if ops and #ops > 0 then
        input = gen_input()
        tmp_path = string.format("%s/tmp_%d_%d.bin", cfg.artifacts, worker_id, case_id)
        write_all(tmp_path, input)

        res = run_fiskta(ops, tmp_path, nil, nil)

        local_stats.total = local_stats.total + 1
        local_stats.exits[res.exit or -999] = (local_stats.exits[res.exit or -999] or 0) + 1
        if res.timed_out then local_stats.timeouts = local_stats.timeouts + 1 end
        if res.crashed then local_stats.crashed = local_stats.crashed + 1 end

        interesting = res.timed_out or res.crashed or (res.exit == 11) or (res.exit == 10) or (res.exit == 2)

        if interesting then
          local base = string.format("%s/case_%d", cfg.artifacts, case_id)
          local out_stdout = base..".stdout.txt"
          local out_stderr = base..".stderr.txt"
          res = run_fiskta(ops, tmp_path, out_stdout, out_stderr)

          local final_ops = ops
          if cfg.minimize then
            final_ops = minimize_tokens(ops, tmp_path, res)
          end

          save_case(case_id, final_ops, input, res)
          local_stats.saved = local_stats.saved + 1
          os.remove(tmp_path)
        else
          os.remove(tmp_path)
        end
    end

    case_iter = case_iter + 1
  end

  -- Worker saves its stats
  local worker_summary = string.format("%s/worker_%d_summary.txt", cfg.artifacts, worker_id)
  local lines = {
    ("total=%d"):format(local_stats.total),
    ("saved=%d"):format(local_stats.saved),
    ("timeouts=%d"):format(local_stats.timeouts),
    ("crashed=%d"):format(local_stats.crashed),
  }
  for code,count in pairs(local_stats.exits) do
    lines[#lines+1] = ("exit[%s]=%d"):format(tostring(code), count)
  end
  write_all(worker_summary, table.concat(lines, "\n").."\n")

  os.exit(0)
end

-- ========= Coordinator (multi-worker) =========
local function run()
  ensure_dir(cfg.artifacts)

  -- Clean old artifacts
  os.execute(string.format("rm -f %s/case_*.{ops.txt,input.bin,meta.txt,stderr.txt,stdout.txt} 2>/dev/null", cfg.artifacts))
  os.execute(string.format("rm -f %s/tmp_*.bin 2>/dev/null", cfg.artifacts))
  os.execute(string.format("rm -f %s/worker_*_summary.txt 2>/dev/null", cfg.artifacts))

  io.write(string.format("fiskta fuzzer [%s | seed=%d | minimize=%s]\n",
    cfg.is_asan and "ASAN" or "release", cfg.seed, cfg.minimize and "on" or "off"))
  io.write(string.format("Binary: %s\n", cfg.fiskta_path))

  local num_workers = cfg.workers
  if not cfg.cases then
    -- Continuous mode: single worker only
    num_workers = 1
    io.write("Mode: continuous (Ctrl-C to stop, single worker)\n")
  else
    io.write(string.format("Mode: %d cases (%d workers)\n", cfg.cases, num_workers))
  end

  -- Load corpus before forking (shared read-only)
  load_corpus()
  if cfg.use_corpus and #corpus > 0 then
    io.write(string.format("Corpus: %d files (80%% mutations, 20%% generated)\n", #corpus))
  else
    io.write("Corpus: disabled (pure generation)\n")
  end
  io.write("\n")

  stats.start_time = os.time()

  -- Fork workers
  local workers = {}
  for i=0,num_workers-1 do
    local pid = ffi.C.fork()
    if pid == 0 then
      -- Child: run worker
      run_worker(i, num_workers)
      os.exit(0)  -- Should never reach here
    elseif pid > 0 then
      workers[#workers+1] = pid
    else
      io.stderr:write("Failed to fork worker\n")
      os.exit(1)
    end
  end

  -- Parent: wait for workers and show progress
  io.write(string.format("Spawned %d workers...\n\n", num_workers))

  while #workers > 0 do
    local status = ffi.new("int[1]", 0)
    local pid = ffi.C.waitpid(-1, status, 0)
    if pid > 0 then
      -- Worker finished
      for i=#workers,1,-1 do
        if workers[i] == pid then
          table.remove(workers, i)
          break
        end
      end
    end
    ffi.C.usleep(100000)  -- 100ms
  end

  -- Aggregate worker summaries
  local agg_stats = { total=0, saved=0, timeouts=0, crashed=0, exits={} }
  for i=0,num_workers-1 do
    local summary_path = string.format("%s/worker_%d_summary.txt", cfg.artifacts, i)
    local f = io.open(summary_path, "r")
    if f then
      for line in f:lines() do
        local key, val = line:match("([^=]+)=(.+)")
        if key == "total" then agg_stats.total = agg_stats.total + tonumber(val)
        elseif key == "saved" then agg_stats.saved = agg_stats.saved + tonumber(val)
        elseif key == "timeouts" then agg_stats.timeouts = agg_stats.timeouts + tonumber(val)
        elseif key == "crashed" then agg_stats.crashed = agg_stats.crashed + tonumber(val)
        elseif key:match("^exit%[") then
          local code = key:match("exit%[([^%]]+)%]")
          agg_stats.exits[code] = (agg_stats.exits[code] or 0) + tonumber(val)
        end
      end
      f:close()
    end
  end

  -- Write aggregated summary
  local lines = {
    ("total=%d"):format(agg_stats.total),
    ("saved=%d"):format(agg_stats.saved),
    ("timeouts=%d"):format(agg_stats.timeouts),
    ("crashed=%d"):format(agg_stats.crashed),
  }
  for code,count in pairs(agg_stats.exits) do
    lines[#lines+1] = ("exit[%s]=%d"):format(tostring(code), count)
  end
  write_all(("%s/run_summary.txt"):format(cfg.artifacts), table.concat(lines, "\n").."\n")

  -- Show results
  local elapsed = os.time() - stats.start_time
  local rate = elapsed > 0 and math.floor(agg_stats.total / elapsed) or 0
  io.write(string.format("[%d cases | %d exec/s | %d crashes | %d timeouts | %s]\n\n",
    agg_stats.total, rate, agg_stats.crashed, agg_stats.timeouts, format_time(elapsed)))

  if agg_stats.saved > 0 then
    io.write(string.format("Found %d interesting case(s) in artifacts/\n", agg_stats.saved))
    io.write("Reproduce: ./fuzz --repro-case N\n")
  else
    io.write("No crashes or interesting cases found\n")
  end
end

-- ========= Repro modes =========
local function repro_case(n)
  local base = string.format("%s/case_%d", cfg.artifacts, n)
  local input = base..".input.bin"
  local opsf  = base..".ops.txt"
  local ops = read_lines(opsf)
  local stdout_path = base..".repro.stdout.txt"
  local stderr_path = base..".repro.stderr.txt"
  local res = run_fiskta(ops, input, stdout_path, stderr_path)
  io.stderr:write(string.format("exit=%d signal=%s timed_out=%s\n", res.exit or -999, tostring(res.signal), tostring(res.timed_out)))
  io.stderr:write(string.format("stdout: %s\n", stdout_path))
  io.stderr:write(string.format("stderr: %s\n", stderr_path))
end

local function repro_paths(input, opsf)
  local ops = read_lines(opsf)
  local stdout_path = opsf..".repro.stdout.txt"
  local stderr_path = opsf..".repro.stderr.txt"
  local res = run_fiskta(ops, input, stdout_path, stderr_path)
  io.stderr:write(string.format("exit=%d signal=%s timed_out=%s\n", res.exit or -999, tostring(res.signal), tostring(res.timed_out)))
  io.stderr:write(string.format("stdout: %s\n", stdout_path))
  io.stderr:write(string.format("stderr: %s\n", stderr_path))
end

-- ========= Entry =========
if cfg.repro_case then
  repro_case(cfg.repro_case)
elseif cfg.repro_input and cfg.repro_ops then
  repro_paths(cfg.repro_input, cfg.repro_ops)
else
  run()
end
