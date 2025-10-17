#!/usr/bin/env luajit
-- Grammar-guided fuzzer for fiskta (LuaJIT only).
-- No external libraries; uses LuaJIT's built-in ffi/bit and POSIX syscalls.
-- POSIX targets (Linux/macOS). For Windows, run under WSL.

local ffi = require("ffi")
local bit = require("bit")

-- ========= POSIX FFI =========
ffi.cdef[[
typedef int pid_t;
int     kill(pid_t pid, int sig);
pid_t   fork(void);
int     execvp(const char *file, char *const argv[]);
pid_t   waitpid(pid_t pid, int *status, int options);
int     usleep(unsigned int usec);
int     open(const char *pathname, int flags, ...);
int     dup2(int oldfd, int newfd);
int     close(int fd);
extern int errno;
const char *strerror(int errnum);
]]

local SIGKILL = 9
local WNOHANG = 1
-- open flags + modes
local O_WRONLY, O_CREAT, O_TRUNC = 1, 64, 512
local S_IRUSR, S_IWUSR, S_IRGRP, S_IROTH = 256,128,32,4 -- 0644

-- ========= Args =========
local function parse_args()
  local cfg = {
    fiskta_path = nil,
    artifacts = "artifacts",
    grammar = nil,
    readme = nil,
    cases = 1000,
    seed = os.time(),
    timeout_ms = 1500,
    max_depth = 12,
    max_repeat = 3,
    minimize = false,
    repro_case = nil,
    repro_input = nil,
    repro_ops = nil,
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
    elseif a == "--repro-case" then i=i+1; cfg.repro_case = tonumber(arg[i])
    elseif a == "--repro" then i=i+1; cfg.repro_input = arg[i]; i=i+1; cfg.repro_ops = arg[i]
    elseif a == "-h" or a == "--help" then
      io.stderr:write([[
Usage:
  luajit tests/fiskta_fuzz.lua --fiskta-path ./fiskta [--grammar grammar.txt | --readme README.md]
                               [--cases N] [--seed N] [--timeout-ms N]
                               [--max-depth N] [--max-repeat N]
                               [--artifacts DIR] [--minimize]
  Reproduce:
    --repro-case N
    --repro <input.bin> <ops.txt>
]])
      os.exit(1)
    end
    i = i + 1
  end
  if not cfg.fiskta_path then io.stderr:write("--fiskta-path is required\n"); os.exit(2) end
  if not (cfg.repro_case or cfg.repro_input) then
    if not cfg.grammar and not cfg.readme then
      io.stderr:write("Provide --grammar or --readme\n"); os.exit(2)
    end
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
  local f, e = io.open(path, "rb"); if not f then return nil, e end
  local s = f:read("*a"); f:close(); return s
end

local function write_all(path, data)
  ensure_dir(cfg.artifacts)
  local f, e = io.open(path, "wb"); if not f then error(e) end
  f:write(data); f:close()
end

local function read_lines(path)
  local t = {}
  for line in io.lines(path) do t[#t+1] = line end
  return t
end

-- ========= Grammar extract (last fenced block) =========
local function grammar_from_readme(readme_path)
  local s, e = read_all(readme_path); if not s then error(e) end
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
-- Tokens: Ident, Str (quoted), symbols: = . | : ( ) [ ] { }
local function tokenize(src)
  local tokens = {}
  local i, n = 1, #src
  local function isalpha(c) return c:match("%a") end
  local function isalnum_(c) return c:match("[%w_%-%/]") end -- allow - _ /
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
      table.insert(tokens, {kind=c, lex=c}); i = i + 1
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
  return t[rand(#t)]           -- 1..#t inclusive
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
local function rand_signed_number() -- FIX: SignedNumber is sign+digits only
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
  local s = { string.char(string.byte('A') + rand(26)-1) } -- first must be A-Z
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
    local sub = rules[nm]; if not sub then return end
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
    -- Merge Location + Offset
    if a and is_location_base(a) and b and looks_like_offset(b) then
      table.insert(out, a .. b); i = i + 2
    -- Merge AtBase + Offset
    elseif a and is_at_base(a) and b and looks_like_offset(b) then
      table.insert(out, a .. b); i = i + 2
    -- take Number Unit -> take NU
    elseif a == "take" and b and is_num(b) and c and is_unit(c) then
      table.insert(out, a); table.insert(out, b..c); i = i + 3
    -- take SignedNumber Unit -> take SNU
    elseif a == "take" and b and looks_like_signednum(b) and c and is_unit(c) then
      table.insert(out, a); table.insert(out, b..c); i = i + 3
    -- general Number Unit adjacency (outside "take")
    elseif b and is_num(a) and is_unit(b) then
      table.insert(out, a..b); i = i + 2
    else
      table.insert(out, a or ""); i = i + 1
    end
  end
  -- strip EBNF punctuation (if any leaked)
  local pruned = {}
  local drop = {["="]=1, ["."]=1, ["|"]=1, [":"]=1, ["("]=1, [")"]=1, ["["]=1, ["]"]=1, ["{"]=1, ["}"]=1, [""]=1}
  for _,x in ipairs(out) do if not drop[x] then table.insert(pruned, x) end end
  return pruned
end

-- ========= fiskta exec =========
-- Quiet by default (child stdout/stderr -> /dev/null). If stdout_path/stderr_path provided, capture to files.
local function run_fiskta(ops_tokens, input_path, stdout_path, stderr_path)
  -- argv: fiskta --input <path> -- <ops...>
  local argc = 3 + 1 + #ops_tokens + 1
  local argv = ffi.new("char*[?]", argc)
  local idx = 0
  local function set(s) argv[idx] = ffi.cast("char*", ffi.new("char[?]", #s+1, s)); idx = idx + 1 end
  set(cfg.fiskta_path); set("--input"); set(input_path); set("--")
  for _,t in ipairs(ops_tokens) do set(t) end
  argv[argc-1] = nil

  local pid = ffi.C.fork()
  if pid == 0 then
    -- child: setup stdout/stderr
    local mode = bit.bor(S_IRUSR,S_IWUSR,S_IRGRP,S_IROTH) -- 0644
    -- stdout
    do
      local fd
      if stdout_path and #stdout_path > 0 then
        fd = ffi.C.open(stdout_path, bit.bor(O_WRONLY,O_CREAT,O_TRUNC), mode)
      else
        fd = ffi.C.open("/dev/null", O_WRONLY)
      end
      if fd >= 0 then ffi.C.dup2(fd, 1); ffi.C.close(fd) end
    end
    -- stderr
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
    os.exit(127) -- exec failed
  end

  -- parent: wait with timeout
  local waited, status = 0, ffi.new("int[1]", 0)
  local deadline_us = cfg.timeout_ms * 1000
  while true do
    local r = ffi.C.waitpid(pid, status, WNOHANG)
    if r == pid then
      local st = status[0]
      if bit.band(st, 0x7f) == 0 then
        return {crashed=false, timed_out=false, exit=bit.rshift(bit.band(st, 0xff00), 8), signal=nil}
      else
        -- signaled
        return {crashed=true, timed_out=false, exit=128 + bit.band(st, 0x7f), signal=bit.band(st, 0x7f)}
      end
    elseif r == 0 then
      if waited >= deadline_us then
        ffi.C.kill(pid, SIGKILL)
        ffi.C.waitpid(pid, status, 0)
        return {crashed=false, timed_out=true, exit=-2, signal=nil}
      end
      ffi.C.usleep(1000) -- 1ms
      waited = waited + 1000
    else
      -- waitpid error
      return {crashed=true, timed_out=false, exit=-3, signal=nil}
    end
  end
end

local function save_case(id, ops_tokens, input_data, res)
  local base = string.format("%s/case_%d", cfg.artifacts, id)
  ensure_dir(cfg.artifacts)
  write_all(base..".ops.txt", table.concat(ops_tokens, "\n"))  -- one token per line
  write_all(base..".input.bin", input_data)
  local meta = string.format("exit=%d\nsignal=%s\ntimed_out=%s\n",
    res.exit or -999, tostring(res.signal), tostring(res.timed_out))
  write_all(base..".meta.txt", meta)
end

-- ========= Minimizer (greedy token deletion) =========
local function same_failure(a, b)
  if a.timed_out and b.timed_out then return true end
  if a.crashed and b.crashed then return true end
  return (a.exit == b.exit) -- allow matching specific exits like 10/11/2
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

-- ========= Input generator =========
local function gen_input()
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

-- ========= Stats =========
local stats = { total=0, saved=0, timeouts=0, crashed=0, exits={} }
local function bump_exit(code) stats.exits[code] = (stats.exits[code] or 0) + 1 end
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

-- ========= Main fuzz loop =========
local function run()
  ensure_dir(cfg.artifacts)
  for case_id = 0, cfg.cases-1 do
    -- 1) Generate ops
    local raw = {}
    gen_node(rules[start_rule], raw, 0, {max_depth=cfg.max_depth, max_repeat=cfg.max_repeat})
    if #raw == 0 then goto cleanup end
    local ops = rewrite_tokens(raw)

    -- 2) Input
    local input = gen_input()
    local tmp_path = string.format("%s/tmp_%d.bin", cfg.artifacts, case_id)
    write_all(tmp_path, input)

    -- 3) Quiet probe run
    local res = run_fiskta(ops, tmp_path, nil, nil)

    -- 4) Stats
    stats.total = stats.total + 1
    bump_exit(res.exit or -999)
    if res.timed_out then stats.timeouts = stats.timeouts + 1 end
    if res.crashed then stats.crashed = stats.crashed + 1 end

    -- 5) Decide interesting
    local interesting = res.timed_out or res.crashed or (res.exit == 11) or (res.exit == 10) or (res.exit == 2)

    if interesting then
      -- Capture evidence (rerun to files)
      local base = string.format("%s/case_%d", cfg.artifacts, case_id)
      local out_stdout = base..".stdout.txt"
      local out_stderr = base..".stderr.txt"
      res = run_fiskta(ops, tmp_path, out_stdout, out_stderr)

      -- Minimize ops if requested
      local final_ops = ops
      if cfg.minimize then
        final_ops = minimize_tokens(ops, tmp_path, res)
      end

      -- Save case
      save_case(case_id, final_ops, input, res)
      stats.saved = stats.saved + 1
      os.remove(tmp_path) -- remove tmp after saving
    else
      -- Not interesting; delete tmp
      os.remove(tmp_path)
    end

    ::cleanup::
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
end

local function repro_paths(input, opsf)
  local ops = read_lines(opsf)
  local stdout_path = opsf..".repro.stdout.txt"
  local stderr_path = opsf..".repro.stderr.txt"
  local res = run_fiskta(ops, input, stdout_path, stderr_path)
  io.stderr:write(string.format("exit=%d signal=%s timed_out=%s\n", res.exit or -999, tostring(res.signal), tostring(res.timed_out)))
end

-- ========= Entry =========
if cfg.repro_case then
  repro_case(cfg.repro_case)
elseif cfg.repro_input and cfg.repro_ops then
  repro_paths(cfg.repro_input, cfg.repro_ops)
else
  run()
  write_summary()
end

