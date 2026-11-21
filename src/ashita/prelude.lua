-- SPDX-License-Identifier: GPL-3.0-or-later
-- ffxi-linux: the Ashita v4 addon environment, built over the client's native module `__xi` (src/ashita/lua_native.cpp).
-- Runs once in every addon's Lua state before the addon's own file. docs/ashita-compat.md lists what is covered.
-- Ashita's interfaces are Lua tables here; a method this file does not implement answers through `stub` (it logs the
-- name once and returns 0), so an addon touching a rare corner keeps running.

local xi = __xi;
local ffi = require('ffi');
local bit = require('bit');

local unpack = unpack or table.unpack;

-- Ashita's LuaJIT is built with LUAJIT_ENABLE_LUA52COMPAT: table.pack / table.unpack, math.log with a base,
-- string.rep with a separator.
if (table.pack == nil) then
    table.pack = function (...) return { n = select('#', ...), ... }; end
end
if (table.unpack == nil) then
    table.unpack = unpack;
end
do
    local log = math.log;
    if (select(2, pcall(log, 8, 2)) ~= 3) then
        math.log = function (x, base)
            if (base == nil) then return log(x); end
            return log(x) / log(base);
        end
    end
    local rep = string.rep;
    string.rep = function (s, n, sep)
        if (sep == nil or n <= 1) then return rep(s, n); end
        return rep(s .. sep, n - 1) .. s;
    end
end
local traceback = debug.traceback; -- kept: an addon (parse) replaces the global `debug`

-------------------------------------------------------------------------------------------------------------------
-- Paths: Ashita addons build Windows paths ('%s\\config\\addons\\'); every file function takes them here.
-------------------------------------------------------------------------------------------------------------------
local function norm(p)
    if (type(p) ~= 'string') then
        return p;
    end
    return __xi.resolve(p); -- '\\' -> '/', components matched without regard to case when the exact path is missing
end
do
    local open, lines, remove, rename, dofile_, loadfile_ = io.open, io.lines, os.remove, os.rename, dofile, loadfile;
    io.open = function (p, m) return open(norm(p), m); end
    io.lines = function (p, ...) if (p == nil) then return lines(); end return lines(norm(p), ...); end
    os.remove = function (p) return remove(norm(p)); end
    os.rename = function (a, b) return rename(norm(a), norm(b)); end
    dofile = function (p) return dofile_(norm(p)); end
    loadfile = function (p, ...) return loadfile_(norm(p), ...); end
    local input, output = io.input, io.output;
    io.input = function (f) if (type(f) == 'string') then f = norm(f); end return input(f); end
    io.output = function (f) if (type(f) == 'string') then f = norm(f); end return output(f); end
end

local root = xi.root;
local addon_dir = xi.addon_dir;
package.path = ('%s/?.lua;%s/libs/?.lua;%s/libs/?/init.lua;%s/?.lua'):format(addon_dir, root .. '/addons', root .. '/addons', root .. '/addons');
package.cpath = '';
package.preload['struct'] = function () return struct; end
table.insert(package.loaders, 2, function (name)
    if (not name:find('\\', 1, true)) then
        return nil;
    end
    local file = package.searchpath(name:gsub('\\', '.'), package.path);
    if (file == nil) then
        return ('\n\tno file for %s'):format(name);
    end
    return assert(loadfile(file));
end);
-- Last: the package.path templates again with each path matched without regard to case (require 'imguiWrap' for
-- imguiwrap.lua, as Windows finds it).
package.loaders[#package.loaders + 1] = function (name)
    local rel = name:gsub('\\', '/'):gsub('%.', '/');
    for template in package.path:gmatch('[^;]+') do
        local file = __xi.resolve((template:gsub('%?', rel)));
        local f = io.open(file, 'r');
        if (f ~= nil) then
            f:close();
            return assert(loadfile(file));
        end
    end
    return ('\n\tno file for %s (any case)'):format(name);
end

-- 1: ffi.cdef as Ashita's LuaJIT takes it: integer constants with C suffixes (0x10UL, 1LL) are read without them.
do
    local cdef = ffi.cdef;
    ffi.cdef = function (s, ...)
        if (type(s) ~= 'string') then
            return cdef(s, ...);
        end
        s = s:gsub('(%f[%w_]0[xX]%x+)[uUlL]+%f[^%w_]', '%1'):gsub('(%f[%w_]%d+)[uUlL]+%f[^%w_]', '%1');
        local ok, err = pcall(cdef, s, ...);
        if (ok) then
            return;
        end
        if (not tostring(err):find('redefine', 1, true)) then
            error(err, 2);
        end
        -- Ashita's addons declare shared types again (libs/d3d8.lua then an addon's own copy): declare piece by
        -- piece, skipping the pieces already there.
        local depth, start = 0, 1;
        for i = 1, #s do
            local c = s:sub(i, i);
            if (c == '{') then depth = depth + 1;
            elseif (c == '}') then depth = depth - 1;
            elseif (c == ';' and depth == 0) then
                local piece = s:sub(start, i);
                start = i + 1;
                local pok, perr = pcall(cdef, piece);
                if (not pok and not tostring(perr):find('redefine', 1, true)) then
                    error(perr, 2);
                end
            end
        end
    end
end

-- Win32 text conversion LuAshitacast's encoding.lua calls through ffi.C (MultiByteToWideChar / WideCharToMultiByte),
-- done with iconv: code page 932 is Shift_JIS, 65001 UTF-8; wchar_t is UTF-32 here.
do
    pcall(ffi.cdef, [[
        typedef void* xi_iconv_t;
        xi_iconv_t iconv_open(const char* tocode, const char* fromcode);
        size_t iconv(xi_iconv_t cd, char** inbuf, size_t* inbytesleft, char** outbuf, size_t* outbytesleft);
        int iconv_close(xi_iconv_t cd);
    ]]);
    local C = ffi.C;
    local function cp_name(cp) if (cp == 932) then return 'SHIFT_JIS'; elseif (cp == 65001) then return 'UTF-8'; end return 'WINDOWS-1252'; end
    local function convert(to, from, src, srclen, dst, dstbytes)
        local cd = C.iconv_open(to, from);
        if (cd == ffi.cast('xi_iconv_t', -1)) then return -1; end
        local inp = ffi.new('char*[1]', ffi.cast('char*', src));
        local inl = ffi.new('size_t[1]', srclen);
        local outp = ffi.new('char*[1]', ffi.cast('char*', dst));
        local outl = ffi.new('size_t[1]', dstbytes);
        C.iconv(cd, inp, inl, outp, outl);
        C.iconv_close(cd);
        return tonumber(dstbytes) - tonumber(outl[0]);
    end
    local shims = {
        MultiByteToWideChar = function (cp, flags, src, cb, dst, cch)
            local n = (cb == -1) and (ffi.C.strlen(src) + 1) or cb;
            if (dst == nil or cch == 0) then return n; end
            local bytes = convert('UTF-32LE', cp_name(cp), src, n, dst, cch * ffi.sizeof('wchar_t'));
            bytes = tonumber(bytes);
            return bytes < 0 and 0 or math.floor(bytes / ffi.sizeof('wchar_t'));
        end,
        WideCharToMultiByte = function (cp, flags, src, cch, dst, cb)
            local n = cch;
            if (cch == -1) then
                local p = ffi.cast('uint32_t*', src);
                n = 0;
                while (p[n] ~= 0) do n = n + 1; end
                n = n + 1;
            end
            if (dst == nil or cb == 0) then return n * 4; end
            local bytes = convert(cp_name(cp), 'UTF-32LE', src, n * ffi.sizeof('wchar_t'), dst, cb);
            bytes = tonumber(bytes);
            return bytes < 0 and 0 or bytes;
        end,
    };
    pcall(ffi.cdef, 'size_t strlen(const char*);');
    -- Other parts of the prelude (and later chunks) add Win32 / D3DX stand-ins here by name.
    __xi_ffi_c = shims;
    function register_ffi_c(name, fn) shims[name] = fn; end
    -- Win32 calls addons make that mean nothing here: they succeed. A module handle points at a zeroed block, so an
    -- addon that reads through it reads zeros instead of faulting.
    local fake_module = ffi.new('uint8_t[?]', 0x10000);
    shims.GetModuleHandleA = function () return ffi.cast('void*', fake_module); end
    shims.GetModuleHandleW = shims.GetModuleHandleA;
    shims.SetProcessWorkingSetSize = function () return 1; end
    shims.GetCurrentProcess = function () return ffi.cast('void*', -1); end
    shims.Sleep = function () end
    -- The game window as a handle (GetFinalFantasyHwnd answers the same number); focus follows the client window.
    shims.GetFocus = function () return ffi.cast('void*', xi.focused() and 0x1000 or 0); end
    shims.GetForegroundWindow = shims.GetFocus;
    shims.GetActiveWindow = shims.GetFocus;
    shims.GetTickCount = function () return math.floor(xi.clock() * 1000) % 4294967296; end
    local realC = ffi.C;
    ffi.C = setmetatable({}, { __index = function (t, k)
        local f = shims[k];
        if (f ~= nil) then return f; end
        return realC[k];
    end });
    -- ffi.load of a Windows DLL an addon bundles (gdifonts/gdifonttexture.dll ...): a stand-in registered in
    -- __xi_ffi_load[basename, lower case] answers it; any other .dll fails as a missing library would.
    __xi_ffi_load = {};
    -- winmm (XIUI and others): timers and PlaySound. Sounds are not played (logged once); the calls succeed.
    local winmm = {
        timeGetTime = function () return math.floor(xi.clock() * 1000) % 4294967296; end,
        timeBeginPeriod = function () return 0; end,
        timeEndPeriod = function () return 0; end,
        PlaySoundA = function () warn_once('winmm PlaySound'); return 1; end,
        sndPlaySoundA = function () warn_once('winmm sndPlaySound'); return 1; end,
    };
    winmm.PlaySoundW, winmm.PlaySound, winmm.sndPlaySoundW = winmm.PlaySoundA, winmm.PlaySoundA, winmm.sndPlaySoundA;
    local winmm_lib = setmetatable({}, { __index = function (t, k)
        local f = winmm[k];
        if (f == nil) then warn_once('winmm.' .. tostring(k)); f = function () return 0; end; end
        return f;
    end });
    __xi_ffi_load['winmm'] = function () return winmm_lib; end
    local load = ffi.load;
    ffi.load = function (name, ...)
        local base = tostring(name):gsub('\\', '/'):match('([^/]+)$'):lower();
        local f = __xi_ffi_load[base] or __xi_ffi_load[base:gsub('%.dll$', '')];
        if (f ~= nil) then return f(name, ...); end
        return load(name, ...);
    end
end

-------------------------------------------------------------------------------------------------------------------
-- Logging and stubs
-------------------------------------------------------------------------------------------------------------------
local warned = {};
local function warn_once(what)
    if (not warned[what]) then
        warned[what] = true;
        xi.log(3, ('not available in ffxi-linux: %s'):format(what));
    end
end
local function stub_for(owner)
    return function (t, k)
        if (type(k) ~= 'string' or not k:match('^%u')) then
            return nil; -- a field Ashita does not have reads nil (HXUI tests player.isZoning)
        end
        local f;
        if (k:match('^Get.*Manager$')) then
            local m = setmetatable({}, { __index = stub_for(k:sub(4)) });
            f = function () warn_once(owner .. ':' .. k); return m; end
        else
            f = function () warn_once(owner .. ':' .. k); return 0; end
        end
        rawset(t, k, f);
        return f;
    end
end
local function interface(name, t)
    return setmetatable(t, { __index = stub_for(name) });
end

-- print goes to the chat log, as in Ashita.
print = function (...)
    local parts = {};
    for i = 1, select('#', ...) do
        parts[#parts + 1] = tostring((select(i, ...)));
    end
    xi.print(table.concat(parts, ' '));
end

-------------------------------------------------------------------------------------------------------------------
-- Constants (Ashita's own enums)
-------------------------------------------------------------------------------------------------------------------
ChatInputOpenStatus = { Closed = 0x00, Opened = 0x01, OpenedChat = 0x11, OpenedOther = 0x21 };
CommandMode = { AshitaForceHandle = -3, AshitaScript = -2, AshitaParse = -1, Menu = 0, Typed = 1, Macro = 2,
    SubTargetST = 3, SubTargetSTPC = 4, SubTargetSTNPC = 5, SubTargetSTPT = 6, SubTargetSTAL = 7 };
FontBorderFlags = { None = 0x00, Top = 0x01, Bottom = 0x02, Left = 0x04, Right = 0x08, All = 0x0F };
FontCreateFlags = { None = 0x00, Bold = 0x01, Italic = 0x02, StrikeThrough = 0x04, Underlined = 0x08, CustomFile = 0x10, ClearType = 0x20 };
FontDrawFlags = { None = 0x00, Filtered = 0x01, CenterX = 0x02, CenterY = 0x04, RightJustified = 0x08, Outlined = 0x10, ManualRender = 0x20 };
FrameAnchor = { TopLeft = 0, TopRight = 1, BottomLeft = 2, BottomRight = 3, Right = 1, Bottom = 2 };
LogLevel = { None = 0, Critical = 1, Error = 2, Warn = 3, Info = 4, Debug = 5 };
KeyboardEvent = { Down = 0, Up = 1 };
MouseEvent = { ClickLeft = 0, ClickRight = 1, ClickMiddle = 2, ClickX1 = 3, ClickX2 = 4, WheelUp = 5, WheelDown = 6, Move = 7 };
PluginFlags = { None = 0, UseCommands = 1, UseText = 2, UsePackets = 4, UseDirect3D = 8, UsePluginEvents = 0x10, Legacy = 7, LegacyDirect3D = 0x15, All = 0x1F };
PrimitiveDrawFlags = { None = 0, ManualRender = 1 };

RECT = { __type = {} };
RECT.new = function () return setmetatable({ left = 0, top = 0, right = 0, bottom = 0 }, RECT); end
RECT.__type.is = function (v) return getmetatable(v) == RECT; end
SIZE = { __type = {} };
SIZE.new = function () return setmetatable({ cx = 0, cy = 0, x = 0, y = 0 }, SIZE); end
SIZE.__type.is = function (v) return getmetatable(v) == SIZE; end

-------------------------------------------------------------------------------------------------------------------
-- addon and ashita tables
-------------------------------------------------------------------------------------------------------------------
addon = {
    name = xi.addon, author = '', version = '0', desc = '', link = '', path = addon_dir,
    instance = { current_frame = 0, state = 0, get_memory_usage = function () return collectgarbage('count') * 1024; end },
};
_addon = addon; -- Ashita v3's name for it, still used by some addons (yield)

-- Lua 5.3's utf8 library, which Ashita's LuaJIT carries (fancychat and others use utf8.char).
if (utf8 == nil) then
    utf8 = { charpattern = '[\0-\x7F\xC2-\xF4][\x80-\xBF]*' };
    local function encode(c)
        if (c < 0x80) then return string.char(c); end
        if (c < 0x800) then return string.char(0xC0 + bit.rshift(c, 6), 0x80 + bit.band(c, 0x3F)); end
        if (c < 0x10000) then return string.char(0xE0 + bit.rshift(c, 12), 0x80 + bit.band(bit.rshift(c, 6), 0x3F), 0x80 + bit.band(c, 0x3F)); end
        return string.char(0xF0 + bit.rshift(c, 18), 0x80 + bit.band(bit.rshift(c, 12), 0x3F), 0x80 + bit.band(bit.rshift(c, 6), 0x3F), 0x80 + bit.band(c, 0x3F));
    end
    function utf8.char(...)
        local t = {};
        for i = 1, select('#', ...) do t[i] = encode((select(i, ...))); end
        return table.concat(t);
    end
    -- The code point at byte i and the byte after it, or nil when s has no valid sequence there.
    local function decode(s, i)
        local c = s:byte(i);
        if (c == nil) then return nil; end
        if (c < 0x80) then return c, i + 1; end
        local n = c >= 0xF0 and 3 or c >= 0xE0 and 2 or c >= 0xC0 and 1 or nil;
        if (n == nil) then return nil; end
        local v = bit.band(c, bit.rshift(0x3F, n));
        for k = 1, n do
            local cc = s:byte(i + k);
            if (cc == nil or bit.band(cc, 0xC0) ~= 0x80) then return nil; end
            v = v * 64 + bit.band(cc, 0x3F);
        end
        return v, i + n + 1;
    end
    function utf8.codepoint(s, i, j)
        i = i or 1;
        if (i < 0) then i = #s + i + 1; end
        j = j or i;
        if (j < 0) then j = #s + j + 1; end
        local out, p = {}, i;
        while (p <= j) do
            local c, nxt = decode(s, p);
            if (c == nil) then error('invalid UTF-8 code', 2); end
            out[#out + 1] = c;
            p = nxt;
        end
        return unpack(out);
    end
    function utf8.len(s, i, j)
        i = i or 1;
        j = j or -1;
        if (j < 0) then j = #s + j + 1; end
        local n, p = 0, i;
        while (p <= j) do
            local c, nxt = decode(s, p);
            if (c == nil) then return nil, p; end
            n, p = n + 1, nxt;
        end
        return n;
    end
    function utf8.codes(s)
        local p = 1;
        return function ()
            if (p > #s) then return nil; end
            local start = p;
            local c, nxt = decode(s, p);
            if (c == nil) then error('invalid UTF-8 code', 2); end
            p = nxt;
            return start, c;
        end
    end
    function utf8.offset(s, n, i)
        local function cont(p) local b = s:byte(p); return b ~= nil and bit.band(b, 0xC0) == 0x80; end
        i = i or ((n >= 0) and 1 or #s + 1);
        if (n == 0) then
            while (i > 1 and cont(i)) do i = i - 1; end
            return i;
        end
        if (cont(i)) then error('initial position is a continuation byte', 2); end
        if (n > 0) then
            n = n - 1;
            while (n > 0 and i <= #s) do
                i = i + 1;
                while (cont(i)) do i = i + 1; end
                n = n - 1;
            end
            return (n == 0) and i or nil;
        end
        while (n < 0 and i > 1) do
            i = i - 1;
            while (i > 1 and cont(i)) do i = i - 1; end
            n = n + 1;
        end
        return (n == 0) and i or nil;
    end
end

ashita = {
    addons_version = 4.20,
    interface_version = 4.30,
    events = {}, fs = {}, time = {}, tasks = {}, memory = { assembler = {} }, misc = {}, bits = {}, regex = {},
};

-- Events: callbacks by event name, in registration order.
local events = {};
function ashita.events.register(name, alias, cb)
    if (type(name) ~= 'string' or type(alias) ~= 'string' or type(cb) ~= 'function') then
        return false;
    end
    name = name:lower();
    local list = events[name];
    if (list == nil) then
        list = {};
        events[name] = list;
    end
    for _, v in ipairs(list) do
        if (v.alias == alias) then
            v.cb = cb;
            return true;
        end
    end
    list[#list + 1] = { alias = alias, cb = cb };
    return true;
end
function ashita.events.unregister(name, alias)
    local list = events[(name or ''):lower()];
    if (list == nil) then
        return false;
    end
    for i, v in ipairs(list) do
        if (v.alias == alias) then
            table.remove(list, i);
            return true;
        end
    end
    return false;
end

local function report(err)
    local msg = tostring(err);
    xi.log(2, msg);
    xi.chat(-99, ('\30\68[%s]\30\01 \30\76%s\30\01'):format(addon.name, msg)); -- not passed to text_in
end
-- Event callbacks run as coroutines, as in Ashita: one may coroutine.sleep (LuAshitacast does in packet_in); the rest
-- of it then runs later as a task.
local resume_later;
local function call(cb, ...)
    local args, n = { ... }, select('#', ...);
    local co = coroutine.create(function () return cb(unpack(args, 1, n)); end);
    local ok, r = coroutine.resume(co);
    if (not ok) then
        report(traceback(co, r));
    elseif (coroutine.status(co) ~= 'dead') then
        resume_later(co, r);
    end
end

-- Called by the manager: an event to every callback registered for it.
function __xi_event(name, e)
    local list = events[name];
    if (list == nil) then
        return e;
    end
    for _, v in ipairs({ unpack(list) }) do
        call(v.cb, e);
    end
    return e;
end
function __xi_has_event(name)
    local list = events[name];
    return list ~= nil and #list > 0;
end

-- Packets: data / data_modified as strings, data_raw / data_modified_raw as pointers to the bytes, chunk_* likewise.
function __xi_packet(name, id, size, data, modified, injected, blocked, chunk)
    local list = events[name];
    if (list == nil or #list == 0) then
        return modified, blocked;
    end
    -- Buffers as big as the largest sub-packet (0x1FF x 4 bytes): an addon writing a field past the end of a short
    -- packet stays inside them, as it does in the game's own buffers.
    local raw = ffi.new('uint8_t[?]', math.max(#data + 1, 2048));
    ffi.copy(raw, data, #data);
    local mraw = ffi.new('uint8_t[?]', math.max(#modified + 1, 2048));
    ffi.copy(mraw, modified, #modified);
    local craw = ffi.new('uint8_t[?]', math.max(#chunk + 1, 2048));
    ffi.copy(craw, chunk, #chunk);
    local e = {
        id = id, size = size, data = data, data_raw = raw, data_modified = modified, data_modified_raw = mraw,
        chunk_size = #chunk, chunk_data = chunk, chunk_data_raw = craw, injected = injected, blocked = blocked,
    };
    for _, v in ipairs({ unpack(list) }) do
        local before = e.data_modified;
        local snapshot = ffi.string(mraw, #before);
        call(v.cb, e);
        if (e.data_modified ~= before) then
            -- A new string: it replaces the bytes (and the raw view follows it).
            mraw = ffi.new('uint8_t[?]', math.max(#e.data_modified + 1, 2048));
            ffi.copy(mraw, e.data_modified, #e.data_modified);
            e.data_modified_raw = mraw;
        else
            local now = ffi.string(mraw, #before);
            if (now ~= snapshot) then
                e.data_modified = now; -- written through data_modified_raw
            end
        end
    end
    return e.data_modified, e.blocked;
end

-------------------------------------------------------------------------------------------------------------------
-- ashita.fs / time / misc / regex / bits
-------------------------------------------------------------------------------------------------------------------
ashita.fs.exists = function (p) return xi.fs_exists(p); end
ashita.fs.create_directory = function (p) return xi.fs_mkdir(p); end
ashita.fs.create_dir = ashita.fs.create_directory;
ashita.fs.remove = function (p) return xi.fs_remove(p); end
ashita.fs.rename = function (a, b) return xi.fs_rename(a, b); end
ashita.fs.size = function (p) return xi.fs_size(p); end
ashita.fs.get_directory = function (p, mask, sub) return xi.fs_list(p, mask, sub); end
ashita.fs.get_dir = ashita.fs.get_directory;
ashita.fs.normalize = function (p) return norm(p); end
ashita.fs.equivalent = function (a, b) return norm(a) == norm(b); end
ashita.fs.current_directory = function () return root; end
ashita.fs.get_install_directory = function () return root; end
ashita.fs.space = function () return 0, 0, 0; end
ashita.fs.current_dir = ashita.fs.current_directory;
ashita.fs.equals = ashita.fs.equivalent;
ashita.fs.get_install_dir = ashita.fs.get_install_directory;
ashita.fs.status = function (p) return { exists = xi.fs_exists(p), is_directory = xi.fs_isdir(p), size = xi.fs_size(p) }; end

ashita.time.clock = function () local s = xi.clock(); return { s = s, ms = s * 1000 }; end
ashita.time.get_tick = function () return math.floor(xi.clock() * 1000); end
ashita.time.query_performance_counter = function () return math.floor(xi.clock() * 1e6); end
ashita.time.query_performance_frequency = function () return 1e6; end
local function datetime(t)
    return { year = t.year, month = t.month, day = t.day, hour = t.hour, minute = t.min, second = t.sec, milliseconds = 0, day_of_week = t.wday - 1 };
end
ashita.time.get_localtime = function () return datetime(os.date('*t')); end
ashita.time.get_systemtime = function () return datetime(os.date('!*t')); end
ashita.time.qpc = ashita.time.query_performance_counter;
ashita.time.qpf = ashita.time.query_performance_frequency;
ashita.time.glt = ashita.time.get_localtime;
ashita.time.gst = ashita.time.get_systemtime;
ashita.time.tick = ashita.time.get_tick;

ashita.misc.get_clipboard = function () return xi.get_clipboard(); end
ashita.misc.set_clipboard = function (s) return xi.set_clipboard(s); end
ashita.misc.hide_console = function () end
ashita.misc.show_console = function () end
ashita.misc.execute = function () warn_once('ashita.misc.execute'); return false; end
ashita.misc.open_url = function () warn_once('ashita.misc.open_url'); return false; end
ashita.misc.play_sound = function () warn_once('ashita.misc.play_sound'); return false; end

ashita.regex.match = function (m, p) return xi.re_match(m, p); end
ashita.regex.search = function (m, p) return xi.re_search(m, p); end
ashita.regex.replace = function (m, p, r) return xi.re_replace(m, p, r); end
ashita.regex.split = function (m, p) return xi.re_split(m, p); end

-- Bits: (data, byte_offset, bit_offset, len) or (data, bit_offset, len). data is a pointer, a string or a 1-based byte
-- table. "be" reads the game's bit packing (the action packet's), least significant bit first (INFERRED from the
-- packets addons read with it); "le" reads most significant bit first.
local function byte_at(data, i)
    local t = type(data);
    if (t == 'cdata' or t == 'userdata' or t == 'number') then
        if (t == 'number') then
            return 0;
        end
        return ffi.cast('uint8_t*', data)[i];
    elseif (t == 'string') then
        return data:byte(i + 1) or 0;
    end
    return data[i + 1] or 0;
end
local function set_byte(data, i, v)
    local t = type(data);
    if (t == 'cdata' or t == 'userdata') then
        ffi.cast('uint8_t*', data)[i] = v;
    elseif (t == 'table') then
        data[i + 1] = v;
    end
end
local function bit_args(...)
    local n = select('#', ...);
    if (n >= 4) then
        local data, byte_off, bit_off, len = ...;
        return data, byte_off * 8 + bit_off, len;
    end
    local data, off, len = ...;
    return data, off, len;
end
ashita.bits.unpack_be = function (...)
    local data, pos, len = bit_args(...);
    local v = 0;
    for i = 0, len - 1 do
        local p = pos + i;
        if (bit.band(byte_at(data, math.floor(p / 8)), bit.lshift(1, p % 8)) ~= 0) then
            v = v + 2 ^ i;
        end
    end
    return v;
end
ashita.bits.unpack_le = function (...)
    local data, pos, len = bit_args(...);
    local v = 0;
    for i = 0, len - 1 do
        local p = pos + i;
        local b = bit.band(byte_at(data, math.floor(p / 8)), bit.lshift(1, 7 - p % 8)) ~= 0 and 1 or 0;
        v = v * 2 + b;
    end
    return v;
end
local function pack_args(...)
    local n = select('#', ...);
    if (n >= 5) then
        local data, value, byte_off, bit_off, len = ...;
        return data, value, byte_off * 8 + bit_off, len;
    end
    local data, value, off, len = ...;
    return data, value, off, len;
end
ashita.bits.pack_be = function (...)
    local data, value, pos, len = pack_args(...);
    for i = 0, len - 1 do
        local p = pos + i;
        local idx = math.floor(p / 8);
        local mask = bit.lshift(1, p % 8);
        local b = byte_at(data, idx);
        if (math.floor(value / 2 ^ i) % 2 == 1) then b = bit.bor(b, mask); else b = bit.band(b, bit.bnot(mask)); end
        set_byte(data, idx, bit.band(b, 0xFF));
    end
    return pos + len;
end
ashita.bits.pack_le = function (...)
    local data, value, pos, len = pack_args(...);
    for i = 0, len - 1 do
        local p = pos + i;
        local idx = math.floor(p / 8);
        local mask = bit.lshift(1, 7 - p % 8);
        local b = byte_at(data, idx);
        if (math.floor(value / 2 ^ (len - 1 - i)) % 2 == 1) then b = bit.bor(b, mask); else b = bit.band(b, bit.bnot(mask)); end
        set_byte(data, idx, bit.band(b, 0xFF));
    end
    return pos + len;
end

-------------------------------------------------------------------------------------------------------------------
-- Tasks and coroutine.sleep
-------------------------------------------------------------------------------------------------------------------
local tasks = {};
local frame_no = 0;
ashita.tasks.list = tasks;
resume_later = function (co, r)
    local t = { co = co };
    if (type(r) == 'table' and r.sleepf) then t.at_frame = frame_no + r.sleepf; else t.at = xi.clock() + ((type(r) == 'table' and r.sleep) or 0); end
    tasks[#tasks + 1] = t;
end
local function add_task(t)
    t.co = coroutine.create(function () t.fn(); end);
    tasks[#tasks + 1] = t;
end
ashita.tasks.once = function (delay, fn)
    if (fn == nil) then fn, delay = delay, 0; end
    add_task({ at = xi.clock() + (delay or 0), fn = fn });
end
ashita.tasks.oncef = function (delay, fn)
    add_task({ at_frame = frame_no + (delay or 0), fn = fn });
end
local function repeating(frames, delay, repeats, repeat_delay, fn)
    local t = { fn = function () end };
    t.fn = function ()
        local n = 0;
        while (repeats == 0 or repeats == nil or n < repeats) do
            fn();
            n = n + 1;
            if (frames) then coroutine.sleepf(repeat_delay or 1); else coroutine.sleep(repeat_delay or 0); end
        end
    end;
    if (frames) then t.at_frame = frame_no + (delay or 0); else t.at = xi.clock() + (delay or 0); end
    add_task(t);
end
ashita.tasks.repeating = function (delay, repeats, repeat_delay, fn) repeating(false, delay, repeats, repeat_delay, fn); end
ashita.tasks.repeatingf = function (delay, repeats, repeat_delay, fn) repeating(true, delay, repeats, repeat_delay, fn); end

coroutine.sleep = function (s)
    if (not coroutine.running()) then
        error('coroutine.sleep called outside a coroutine');
    end
    coroutine.yield({ sleep = s or 0 });
end
coroutine.sleepf = function (f)
    if (not coroutine.running()) then
        error('coroutine.sleepf called outside a coroutine');
    end
    coroutine.yield({ sleepf = f or 1 });
end
coroutine.kill = function () end

local function run_tasks()
    local now = xi.clock();
    local keep = {};
    local list = { unpack(tasks) };
    for k in pairs(tasks) do tasks[k] = nil; end
    for _, t in ipairs(list) do
        local due = (t.at ~= nil and now >= t.at) or (t.at_frame ~= nil and frame_no >= t.at_frame);
        if (due) then
            local ok, r = coroutine.resume(t.co);
            if (not ok) then
                report(traceback(t.co, r));
            elseif (coroutine.status(t.co) ~= 'dead') then
                t.at, t.at_frame = nil, nil;
                if (type(r) == 'table' and r.sleepf) then t.at_frame = frame_no + r.sleepf; else t.at = now + ((type(r) == 'table' and r.sleep) or 0); end
                keep[#keep + 1] = t;
            end
        else
            keep[#keep + 1] = t;
        end
    end
    for _, t in ipairs(keep) do tasks[#tasks + 1] = t; end
end

-- The memory / resource interfaces live in prelude_memory.lua (appended after this file in the same chunk, so it sees
-- this file's locals); it fills M.state, M.entity, M.memory_manager and M.resource_manager.
local M = {};
-------------------------------------------------------------------------------------------------------------------
-- Chat, packets, configuration, plugins, pointers
-------------------------------------------------------------------------------------------------------------------
local chat_manager = interface('IChatManager', {
    QueueCommand = function (self, mode, cmd) xi.queue(mode or 1, tostring(cmd)); end,
    Write = function (self, mode, indent, msg) xi.chat(mode or 1, tostring(msg or '')); end,
    AddChatMessage = function (self, mode, indent, msg) xi.chat(mode or 1, tostring(msg or '')); end,
    ParseAutoTranslate = function (self, msg) return msg; end, -- the log already shows auto-translate phrases as text
    GetInputTextRaw = function () return ''; end,
    GetInputTextParsed = function () return ''; end,
    GetInputTextDisplay = function () return ''; end,
    GetInputTextRawLength = function () return 0; end,
    GetInputTextParsedLength = function () return 0; end,
    GetInputTextParsedLengthMax = function () return 150; end,
    GetInputTextRawCaretPosition = function () return 0; end,
    SetInputText = function () end,
    SetInputTextRaw = function () end,
    SetInputTextParsed = function () end,
    SetInputTextDisplay = function () end,
    IsInputOpen = function () return 0; end,
    GetSilentAliases = function () return false; end,
    SetSilentAliases = function () end,
    ExecuteScript = function (self, file) warn_once('ExecuteScript'); end,
    ExecuteScriptString = function (self, str) for line in tostring(str):gmatch('[^\r\n]+') do xi.queue(-1, line); end end,
});

local function to_bytes(t)
    if (type(t) == 'string') then return t; end
    local parts = {};
    for i = 1, #t do parts[i] = string.char(bit.band(tonumber(t[i]) or 0, 0xFF)); end
    return table.concat(parts);
end
local packet_manager = interface('IPacketManager', {
    AddOutgoingPacket = function (self, id, data) xi.send(id, to_bytes(data)); end,
    AddIncomingPacket = function (self, id, data) xi.inject(id, to_bytes(data)); end,
    QueuePacket = function () warn_once('IPacketManager:QueuePacket'); return false; end,
});

-- Configuration: ini files under config/ by alias (Load / Save), plus Ashita's own boot settings answered with
-- their defaults (English client).
local ini = {};
local function ini_parse(path)
    local t, sec = {}, nil;
    local f = io.open(path, 'r');
    if (f == nil) then return nil; end
    for line in f:lines() do
        line = line:gsub('\r', '');
        local s = line:match('^%s*%[(.-)%]%s*$');
        if (s) then sec = s; t[sec] = t[sec] or {};
        elseif (sec and not line:match('^%s*[;#]')) then
            local k, v = line:match('^%s*(.-)%s*=%s*(.-)%s*$');
            if (k and k ~= '') then t[sec][k] = v; end
        end
    end
    f:close();
    return t;
end
-- The boot file's [ffxi.registry] answers the client's real sizes (aspect reads 0001 / 0002 to work out the window's
-- shape): 0001 / 0002 window, 0003 / 0004 background (the same here), 0037 / 0038 menu.
local function boot_registry(key)
    local w, h, mw, mh = xi.screen();
    local v = ({ ['0001'] = w, ['0002'] = h, ['0003'] = w, ['0004'] = h, ['0037'] = mw, ['0038'] = mh })[key];
    return v and tostring(v) or nil;
end
local function ini_get(alias, sec, key)
    local t = ini[alias];
    local v = t and t.data[sec] and t.data[sec][key];
    if (v == nil and alias == 'boot' and sec == 'ffxi.registry') then
        v = boot_registry(key);
    end
    return v;
end
local config_manager = interface('IConfigurationManager', {
    Load = function (self, alias, file)
        local path = ('%s/config/%s'):format(root, norm(file));
        ini[alias] = { path = path, data = ini_parse(path) or {} };
        return true;
    end,
    Save = function (self, alias, file)
        local t = ini[alias];
        if (t == nil) then return false; end
        local path = file and ('%s/config/%s'):format(root, norm(file)) or t.path;
        local f = io.open(path, 'w');
        if (f == nil) then return false; end
        for s, kv in pairs(t.data) do
            f:write(('[%s]\n'):format(s));
            for k, v in pairs(kv) do f:write(('%s = %s\n'):format(k, tostring(v))); end
        end
        f:close();
        return true;
    end,
    Delete = function (self, alias) ini[alias] = nil; end,
    GetSections = function (self, alias)
        local r = {};
        for s in pairs((ini[alias] or { data = {} }).data) do r[#r + 1] = s; end
        return r;
    end,
    GetSectionKeys = function (self, alias, sec)
        local r = {};
        for k in pairs(((ini[alias] or { data = {} }).data[sec]) or {}) do r[#r + 1] = k; end
        return r;
    end,
    GetString = function (self, alias, sec, key) return ini_get(alias, sec, key); end,
    SetValue = function (self, alias, sec, key, v)
        ini[alias] = ini[alias] or { path = ('%s/config/%s.ini'):format(root, alias), data = {} };
        ini[alias].data[sec] = ini[alias].data[sec] or {};
        ini[alias].data[sec][key] = tostring(v);
    end,
    GetBool = function (self, alias, sec, key, def)
        local v = ini_get(alias, sec, key);
        if (v == nil) then return def; end
        return v == '1' or v:lower() == 'true';
    end,
});
for _, n in ipairs({ 'GetInt8', 'GetUInt8', 'GetInt16', 'GetUInt16', 'GetInt32', 'GetUInt32', 'GetInt64', 'GetUInt64', 'GetFloat', 'GetDouble', 'GetInt' }) do
    config_manager[n] = function (self, alias, sec, key, def)
        return tonumber(ini_get(alias, sec, key)) or def;
    end
end

-- The one plugin there always is: Ashita's own addon loader, "Addons".
local addons_plugin = interface('IPlugin', {
    GetName = function () return 'Addons'; end,
    GetAuthor = function () return 'Ashita Development Team'; end,
    GetDescription = function () return 'Lua addon support for Ashita.'; end,
    GetLink = function () return 'https://www.ashitaxi.com/'; end,
    GetVersion = function () return ashita.addons_version; end,
    GetInterfaceVersion = function () return ashita.interface_version; end,
    GetPriority = function () return 0; end,
    GetFlags = function () return PluginFlags.All; end,
});
local plugin_manager = interface('IPluginManager', {
    IsLoaded = function (self, name) return tostring(name):lower() == 'addons'; end,
    Get = function (self, name)
        if (name == 0 or tostring(name):lower() == 'addons') then return addons_plugin; end
        return nil;
    end,
    Count = function () return 1; end,
    RaiseEvent = function () return nil; end, -- (name, data) and (name, data, size): no plugins here
    GetSilentPlugins = function () return false; end,
    SetSilentPlugins = function () end,
});
local polplugin_manager = interface('IPolPluginManager', {
    IsLoaded = function () return false; end,
    Get = function () return nil; end,
    Count = function () return 0; end,
    RaiseEvent = function () return nil; end,
});
local pointer_manager = interface('IPointerManager', { Get = function () return 0; end, Add = function () return 0; end, Delete = function () end });
local offset_manager = interface('IOffsetManager', { Get = function () return 0; end, Add = function () end, Delete = function () end });
local input_manager = interface('IInputManager', {
    GetKeyboard = function () return interface('IKeyboard', { IsKeyDown = function () return false; end }); end,
    GetMouse = function () return interface('IMouse', {}); end,
    GetController = function () return interface('IController', {}); end,
    GetXInput = function () return interface('IXInput', {}); end,
});
LogManager = interface('ILogManager', {
    Log = function (self, level, source, msg) xi.log(level or 4, ('%s: %s'):format(tostring(source), tostring(msg))); end,
    GetLogLevel = function () return 4; end,
    SetLogLevel = function () end,
});
local properties = interface('IProperties', {
    GetFinalFantasyHwnd = function () return 0x1000; end,
    GetFinalFantasyRect = function ()
        local w, h = xi.screen();
        local r = RECT.new(); r.right, r.bottom = w, h;
        return r;
    end,
});

-------------------------------------------------------------------------------------------------------------------
-- Font objects and primitives, drawn after d3d_present (positions in window pixels; a negative x / y is measured from
-- the right / bottom edge, as in Ashita).
-------------------------------------------------------------------------------------------------------------------
local drawables = {};
local function prop_methods(mt, props)
    for name, default in pairs(props) do
        mt['Get' .. name] = function (self) local v = rawget(self, '_' .. name); if (v == nil) then return default; end return v; end
        mt['Set' .. name] = function (self, v) rawset(self, '_' .. name, v); end
    end
end

local PrimitiveMT = {};
PrimitiveMT.__index = PrimitiveMT;
prop_methods(PrimitiveMT, {
    Alias = '', TextureOffsetX = 0, TextureOffsetY = 0, BorderVisible = false, BorderColor = 0, BorderFlags = 0, Visible = false,
    PositionX = 0, PositionY = 0, CanFocus = true, Locked = false, LockedZ = false, ScaleX = 1, ScaleY = 1, Width = 0, Height = 0,
    DrawFlags = 0, Color = 0xFFFFFFFF,
});
PrimitiveMT.GetBorderSizes = function (self) return rawget(self, '_BorderSizes') or { left = 0, top = 0, right = 0, bottom = 0 }; end
PrimitiveMT.SetBorderSizes = function (self, r) rawset(self, '_BorderSizes', r); end
PrimitiveMT.SetTextureFromFile = function (self, path) rawset(self, '_Texture', path); return true; end
PrimitiveMT.SetTextureFromMemory = function () return true; end
PrimitiveMT.SetTextureFromResource = function () return true; end
PrimitiveMT.SetTextureFromResourceCache = function () return true; end
PrimitiveMT.SetTextureFromTexture = function () return true; end
PrimitiveMT.Render = function () end
PrimitiveMT.HitTest = function (self, x, y)
    local sw, sh = xi.screen();
    local px, py = self:GetPositionX(), self:GetPositionY();
    if (px < 0) then px = sw + px; end
    if (py < 0) then py = sh + py; end
    return x >= px and y >= py and x < px + self:GetWidth() * self:GetScaleX() and y < py + self:GetHeight() * self:GetScaleY();
end

local function new_primitive(alias)
    return setmetatable({ _Alias = alias }, PrimitiveMT);
end

local FontMT = {};
FontMT.__index = FontMT;
prop_methods(FontMT, {
    Alias = '', Visible = true, CanFocus = true, Locked = false, LockedZ = false, IsDirty = false, WindowWidth = 0, WindowHeight = 0,
    FontFile = '', FontFamily = 'Arial', FontHeight = 10, CreateFlags = 0, DrawFlags = 0, Bold = false, Italic = false,
    RightJustified = false, StrikeThrough = false, Underlined = false, Color = 0xFFFFFFFF, ColorOutline = 0, Padding = 0,
    PositionX = 0, PositionY = 0, AutoResize = true, Anchor = 0, AnchorParent = 0, Text = '',
});
FontMT.GetBackground = function (self) return rawget(self, '_bg'); end
FontMT.GetParent = function (self) return rawget(self, '_parent'); end
FontMT.SetParent = function (self, p) rawset(self, '_parent', p); end
FontMT.Render = function () end
-- Font text takes Ashita's colour tags: |cAARRGGBB| sets the colour, |r goes back to the font's own.
local function strip_tags(s)
    return (tostring(s):gsub('|c%x%x%x%x%x%x%x%x|', ''):gsub('|r', ''));
end
local function text_size(self)
    local w, h = xi.text(0, 0, strip_tags(self:GetText()), self:GetFontFamily(), self:GetFontHeight(), 0, self:GetBold(), 0, true);
    return w, h;
end
local function draw_line(o, x, y, line, outline)
    local colour, lh = o:GetColor(), 0;
    local pos = 1;
    while (pos <= #line) do
        local a, b, hex = line:find('|c(%x%x%x%x%x%x%x%x)|', pos);
        local r1, r2 = line:find('|r', pos, true);
        local nexttag = math.min(a or math.huge, r1 or math.huge);
        local piece = line:sub(pos, nexttag == math.huge and #line or nexttag - 1);
        if (#piece > 0) then
            local w, h = xi.text(x, y, piece, o:GetFontFamily(), o:GetFontHeight(), colour, o:GetBold(), outline, false);
            x, lh = x + w, math.max(lh, h);
        end
        if (nexttag == math.huge) then break; end
        if (a ~= nil and a == nexttag) then
            colour = tonumber(hex, 16);
            pos = b + 1;
        else
            colour = o:GetColor();
            pos = r2 + 1;
        end
    end
    if (lh == 0) then
        local _, h = xi.text(0, 0, ' ', o:GetFontFamily(), o:GetFontHeight(), 0, o:GetBold(), 0, true);
        lh = h;
    end
    return lh;
end
FontMT.GetTextSize = function (self, size)
    local w, h = text_size(self);
    if (size ~= nil) then size.cx, size.cy, size.x, size.y = w, h, w, h; end
    return w, h;
end
local function font_pos(self)
    local sw, sh = xi.screen();
    local x, y = self:GetPositionX(), self:GetPositionY();
    local parent = self:GetParent();
    if (parent ~= nil and parent.GetRealPositionX ~= nil) then
        x, y = x + parent:GetRealPositionX(), y + parent:GetRealPositionY();
    else
        if (x < 0) then x = sw + x; end
        if (y < 0) then y = sh + y; end
    end
    return x, y;
end
FontMT.GetRealPositionX = function (self) local x = font_pos(self); return x; end
FontMT.GetRealPositionY = function (self) local _, y = font_pos(self); return y; end
-- The font's box on screen as draw_all() draws it: the text, right-justified to the left of its position, plus the
-- background's padding when that is shown.
local function font_rect(o)
    local x, y = font_pos(o);
    local w, h = text_size(o);
    local pad = math.floor((o:GetPadding() or 0) * o:GetFontHeight() * 4 / 3);
    if (o:GetRightJustified() or bit.band(o:GetDrawFlags(), FontDrawFlags.RightJustified) ~= 0) then x = x - w; end
    local bg = o:GetBackground();
    if (bg ~= nil and bg:GetVisible()) then
        local bw = o:GetAutoResize() and (w + pad * 2) or bg:GetWidth();
        local bh = o:GetAutoResize() and (h + pad * 2) or bg:GetHeight();
        return x, y, w, h, x - pad, y - pad, bw, bh;
    end
    return x, y, w, h, x, y, w, h;
end
FontMT.HitTest = function (self, mx, my)
    local _, _, _, _, bx, by, bw, bh = font_rect(self);
    return mx >= bx and my >= by and mx < bx + bw and my < by + bh;
end

local function new_font(alias)
    local f = setmetatable({ _Alias = alias, _bg = new_primitive(alias .. '_bg') }, FontMT);
    return f;
end

local function draw_all()
    for _, d in ipairs(drawables) do
        local o = d.obj;
        if (d.kind == 'font') then
            local manual = bit.band(o:GetDrawFlags(), FontDrawFlags.ManualRender) ~= 0;
            local text = tostring(o:GetText() or '');
            if (o:GetVisible() and not manual and text ~= '') then
                local x, y, _, _, bx, by, bw, bh = font_rect(o);
                local bg = o:GetBackground();
                if (bg:GetVisible()) then
                    xi.rect(bx, by, bw, bh, bg:GetColor(), bg:GetBorderVisible() and bg:GetBorderColor() or 0, bg:GetBorderVisible() and 1 or 0);
                end
                local outline = bit.band(o:GetDrawFlags(), FontDrawFlags.Outlined) ~= 0 and o:GetColorOutline() or 0;
                -- Several lines: one draw per line.
                local ly = y;
                for line in (text .. '\n'):gmatch('(.-)\n') do
                    ly = ly + draw_line(o, x, ly, line, outline);
                end
            end
        elseif (o:GetVisible() and bit.band(o:GetDrawFlags(), PrimitiveDrawFlags.ManualRender) == 0) then
            local sw, sh = xi.screen();
            local x, y = o:GetPositionX(), o:GetPositionY();
            if (x < 0) then x = sw + x; end
            if (y < 0) then y = sh + y; end
            xi.rect(x, y, o:GetWidth() * o:GetScaleX(), o:GetHeight() * o:GetScaleY(), o:GetColor(), o:GetBorderVisible() and o:GetBorderColor() or 0, o:GetBorderVisible() and 1 or 0);
        end
    end
end

-- Ashita's font and primitive managers move an object that is not locked when it is dragged with Shift and the left
-- button (the convention the addons document: "Hold Shift+Click and drag to move display"; the core's own code is not
-- available, INFERRED). The topmost drawn object under the mouse is taken; a child font moves its root parent, as the
-- children are placed relative to it. The new position is absolute (an anchor to the right / bottom edge is dropped),
-- and the addon saves it the way it saves any position (font.position_x read back by its settings).
local drag = nil;
local function drag_root(o)
    local root = o;
    while (root.GetParent ~= nil and root:GetParent() ~= nil) do root = root:GetParent(); end
    return root;
end
local function real_pos(o)
    if (o.GetRealPositionX ~= nil) then return o:GetRealPositionX(), o:GetRealPositionY(); end
    local sw, sh = xi.screen();
    local x, y = o:GetPositionX(), o:GetPositionY();
    if (x < 0) then x = sw + x; end
    if (y < 0) then y = sh + y; end
    return x, y;
end
local function drag_target(mx, my)
    for i = #drawables, 1, -1 do
        local d = drawables[i];
        local o = d.obj;
        local shown;
        if (d.kind == 'font') then
            shown = o:GetVisible() and bit.band(o:GetDrawFlags(), FontDrawFlags.ManualRender) == 0 and tostring(o:GetText() or '') ~= '';
        else
            shown = o:GetVisible() and bit.band(o:GetDrawFlags(), PrimitiveDrawFlags.ManualRender) == 0;
        end
        if (shown and not o:GetLocked() and o:HitTest(mx, my)) then
            local root = drag_root(o);
            if (not root:GetLocked()) then return root; end
        end
    end
    return nil;
end
local WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP = 0x200, 0x201, 0x202;
local function native_drag(e, shift)
    if (drag ~= nil) then
        if (e.message == WM_MOUSEMOVE) then
            drag.obj:SetPositionX(e.x - drag.dx);
            drag.obj:SetPositionY(e.y - drag.dy);
            e.blocked = true;
        elseif (e.message == WM_LBUTTONUP) then
            drag = nil;
            e.blocked = true;
        end
        return;
    end
    if (e.message == WM_LBUTTONDOWN and shift and not e.blocked) then
        local o = drag_target(e.x, e.y);
        if (o ~= nil) then
            local x, y = real_pos(o);
            drag = { obj = o, dx = e.x - x, dy = e.y - y };
            e.blocked = true;
        end
    end
end
-- Called by the manager for each mouse message: the managers' own drag first, then the addon's mouse events.
function __xi_mouse(e, shift)
    native_drag(e, shift);
    return __xi_event('mouse', e);
end

local function manager_of(kind, make)
    local by_alias = {};
    return {
        Create = function (self, alias)
            if (by_alias[alias] ~= nil) then return nil; end
            local o = make(alias);
            by_alias[alias] = o;
            drawables[#drawables + 1] = { kind = kind, obj = o };
            return o;
        end,
        Get = function (self, alias) return by_alias[alias]; end,
        Delete = function (self, alias)
            local o = by_alias[alias];
            by_alias[alias] = nil;
            if (drag ~= nil and drag.obj == o) then drag = nil; end
            for i, d in ipairs(drawables) do if (d.obj == o) then table.remove(drawables, i); break; end end
        end,
        GetFocusedObject = function () return nil; end,
        SetFocusedObject = function () end,
        GetVisible = function () return true; end,
        SetVisible = function () end,
    };
end
local font_manager = interface('IFontManager', manager_of('font', new_font));
local primitive_manager = interface('IPrimitiveManager', manager_of('primitive', new_primitive));

-- The ImGui manager: __xi_imgui's functions, plus GetVisible / SetVisible.
local gui_manager = __xi_imgui;
gui_manager.GetVisible = function () return true; end
gui_manager.SetVisible = function () end
setmetatable(gui_manager, { __index = function (t, k)
    if (type(k) ~= 'string' or not k:match('^%u')) then return nil; end
    local f = function () warn_once('imgui.' .. k); return nil; end
    rawset(t, k, f);
    return f;
end });

-------------------------------------------------------------------------------------------------------------------
-- ashita.memory: no game process to read. The signatures well-known libraries and addons scan for (libs/scaling.lua's
-- window object, LuAshitacast's Vana'diel time / weather / zone flags / wardrobe flags) resolve to a small emulated
-- memory whose values come from the client; any other scan returns 0, as a failed scan does in Ashita.
-------------------------------------------------------------------------------------------------------------------
local vm = { cells = {}, next = 0x10000000 };
local function vm_alloc(n) local a = vm.next; vm.next = vm.next + math.max(n, 16) + 16; return a; end
local function vm_cell(addr, size, fn) vm.cells[addr] = { size = size, fn = fn }; end
local function vm_read(addr, size)
    local c = vm.cells[addr];
    if (c ~= nil) then
        local v = c.fn();
        if (size < c.size) then v = v % (2 ^ (8 * size)); end
        return v;
    end
    return 0;
end
local function vanadiel_raw()
    -- Earth seconds since the Vana'diel epoch, as the game's clock word plus 92514960 (LuAshitacast's reading): the
    -- Vana'diel minute count of LSB's clock (vanadiel_clock.h) x 2.4.
    local minutes = math.floor((os.time() - 1009810800) * 25 / 60) + 886 * 518400;
    return minutes * 2.4;
end
local signatures = {
    -- libs/scaling.lua: A1 [ptr] -> ptr -> { +0x10 window w, +0x12 h, +0x14 menu w, +0x16 menu h } (u16)
    ['A1????????85C05E74????80'] = function ()
        local code, p1, p2 = vm_alloc(8), vm_alloc(4), vm_alloc(0x20);
        vm_cell(code + 1, 4, function () return p1; end);
        vm_cell(p1, 4, function () return p2; end);
        vm_cell(p2 + 0x10, 2, function () local w = xi.screen(); return w; end);
        vm_cell(p2 + 0x12, 2, function () local _, h = xi.screen(); return h; end);
        vm_cell(p2 + 0x14, 2, function () local _, _, mw = xi.screen(); return mw; end);
        vm_cell(p2 + 0x16, 2, function () local _, _, _, mh = xi.screen(); return mh; end);
        return code;
    end,
    -- LuAshitacast: Vana'diel time. [+0x34] -> clock object; +0x0C = raw - 92514960.
    ['B0015EC390518B4C24088D4424005068'] = function ()
        local code, obj = vm_alloc(0x40), vm_alloc(0x10);
        vm_cell(code + 0x34, 4, function () return obj; end);
        vm_cell(obj + 0x0C, 4, function () return (vanadiel_raw() - 92514960) % 4294967296; end);
        return code;
    end,
    -- LuAshitacast: wardrobe access flags. [ptr] -> [ptr] -> +0xB4 (u8): every wardrobe usable (INFERRED for LSB).
    ['A1????????8B88B4000000C1E907F6C101E9'] = function ()
        local code, p1, p2 = vm_alloc(8), vm_alloc(4), vm_alloc(0xC0);
        vm_cell(code, 4, function () return p1; end);
        vm_cell(p1, 4, function () return p2; end);
        vm_cell(p2 + 0xB4, 1, function () return 0xFF; end);
        return code;
    end,
    -- LuAshitacast: weather. [+2] -> u8 weather id.
    ['66A1????????663D????72'] = function ()
        local code, w = vm_alloc(8), vm_alloc(4);
        vm_cell(code + 2, 4, function () return w; end);
        vm_cell(w, 1, function () return M.state().weather or 0; end);
        return code;
    end,
    -- LuAshitacast: zone flags. [+9] = offset, [+0x17] -> zone object pointer; object + offset: 0x100 in a Mog House.
    ['8B8C24040100008B90????????0BD18990????????8B15????????8B82'] = function ()
        local code, holder, zone = vm_alloc(0x20), vm_alloc(4), vm_alloc(0x20);
        vm_cell(code + 9, 4, function () return 0x10; end);
        vm_cell(code + 0x17, 4, function () return holder; end);
        vm_cell(holder, 4, function () return zone; end);
        vm_cell(zone + 0x10, 4, function () return M.state().inMogHouse and 0x100 or 0; end);
        return code;
    end,
};
local found = {};
ashita.memory.find = function (a, b, pattern, offset, usage)
    if (type(pattern) ~= 'string') then return 0; end
    pattern = pattern:upper();
    local make = signatures[pattern];
    if (make == nil) then
        warn_once('ashita.memory.find ' .. pattern);
        return 0;
    end
    found[pattern] = found[pattern] or make();
    return found[pattern] + (offset or 0);
end
for name, size in pairs({ uint8 = 1, int8 = 1, uint16 = 2, int16 = 2, uint32 = 4, int32 = 4, uint64 = 8, int64 = 8 }) do
    ashita.memory['read_' .. name] = function (addr, off)
        local v = vm_read((addr or 0) + (off or 0), size);
        if (name:sub(1, 1) == 'i' and v >= 2 ^ (8 * size - 1)) then v = v - 2 ^ (8 * size); end
        return v;
    end
    ashita.memory['write_' .. name] = function () warn_once('ashita.memory.write'); end
end
ashita.memory.read_float = function (addr, off) return vm_read((addr or 0) + (off or 0), 4); end
ashita.memory.read_double = function (addr, off) return vm_read((addr or 0) + (off or 0), 8); end
ashita.memory.read_array = function (addr, n) local t = {}; for i = 1, n or 0 do t[i] = 0; end return t; end
ashita.memory.read_string = function () return ''; end
ashita.memory.read_literal = function () return ''; end
ashita.memory.write_float = function () end
ashita.memory.write_double = function () end
ashita.memory.write_array = function () end
ashita.memory.write_string = function () end
ashita.memory.get_base = function () return 0; end
ashita.memory.get_size = function () return 0; end
ashita.memory.alloc = function (n) return vm_alloc(n or 16); end
ashita.memory.dealloc = function () end
ashita.memory.protect = function () return 0; end
ashita.memory.unprotect = function () return 0; end
ashita.memory.assembler = interface('ashita.memory.assembler', {});

-------------------------------------------------------------------------------------------------------------------
-- AshitaCore
-------------------------------------------------------------------------------------------------------------------
AshitaCore = interface('IAshitaCore', {
    GetHandle = function () return 0; end,
    -- The D3D8 device the emulation (prelude_d3d.lua) registers in __xi_d3d_device; nil without it.
    GetDirect3DDevice = function () return __xi_d3d_device; end,
    GetInstallPath = function () return root .. '/'; end, -- with the separator, as Ashita returns it
    GetProperties = function () return properties; end,
    GetChatManager = function () return chat_manager; end,
    GetConfigurationManager = function () return config_manager; end,
    GetFontManager = function () return font_manager; end,
    GetGuiManager = function () return gui_manager; end,
    GetInputManager = function () return input_manager; end,
    GetMemoryManager = function () return M.memory_manager; end,
    GetOffsetManager = function () return offset_manager; end,
    GetPacketManager = function () return packet_manager; end,
    GetPluginManager = function () return plugin_manager; end,
    GetPolPluginManager = function () return polplugin_manager; end,
    GetPointerManager = function () return pointer_manager; end,
    GetPrimitiveManager = function () return primitive_manager; end,
    GetResourceManager = function () return M.resource_manager; end,
    GetLogManager = function () return LogManager; end,
    GetSystemMetrics = function () return 0; end,
    SetCursorPos = function () return false; end,
});

-------------------------------------------------------------------------------------------------------------------
-- Called by the manager each frame and when drawing.
-------------------------------------------------------------------------------------------------------------------
function __xi_frame()
    frame_no = frame_no + 1;
    addon.instance.current_frame = frame_no;
    run_tasks();
end
function __xi_draw()
    if (#drawables > 0) then
        draw_all();
    end
end
