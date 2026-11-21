-- SPDX-License-Identifier: GPL-3.0-or-later
-------------------------------------------------------------------------------------------------------------------
-- prelude_scan.lua: ashita.memory over real memory (src/ashita/scan_registry.h; docs/ashita-compat.md, "Native
-- stand-ins for memory signatures"). Appended to the prelude chunk after prelude_memory.lua; replaces the earlier
-- ashita.memory functions.
--
-- A scan finds the pattern in the user's own FFXiMain.dll, mapped as data at its own base, so the addresses an addon
-- reads out of the code are the game's. The game objects those point at are filled from the client every frame for the
-- signatures listed in `emulated`; the patterns in `safe` (an addon only reads or patches the image itself) are answered
-- too; any other pattern answers 0, as a failed scan does in Ashita, unless XI_ASHITA_SCAN=all. Without the image the
-- emulated signatures still answer, from a copy of their bytes whose wildcard operands point at emulated memory.
-- Reads and writes through ashita.memory outside the image and the emulated memory answer 0 and do nothing.
-- XI_ASHITA_SCAN=off leaves the earlier ashita.memory in place (for comparisons).
-------------------------------------------------------------------------------------------------------------------
if (os.getenv('XI_ASHITA_SCAN') ~= 'off') then
    local scan_valid, scan_find, scan_alloc, scan_stub, scan_image = xi.scan_valid, xi.scan_find, xi.scan_alloc, xi.scan_stub, xi.scan_image;
    local scan_all = (os.getenv('XI_ASHITA_SCAN') == 'all');

    local function ok(a, n)
        return type(a) == 'number' and a > 0 and scan_valid(a, n or 1);
    end
    local function rd(ct, a, n)
        if (not ok(a, n)) then return 0; end
        return ffi.cast(ct, a)[0];
    end
    local function wr(ct, a, n, v)
        if (not ok(a, n)) then return false; end
        ffi.cast(ct, a)[0] = v;
        return true;
    end
    local function u32(a) return tonumber(rd('uint32_t*', a, 4)); end
    local function w32(a, v) return wr('uint32_t*', a, 4, v); end
    local function w16(a, v) return wr('uint16_t*', a, 2, v); end
    local function w8(a, v) return wr('uint8_t*', a, 1, v); end
    local function wstr(a, s, n)
        if (not ok(a, n)) then return; end
        local p = ffi.cast('uint8_t*', a);
        for i = 0, n - 1 do p[i] = (i < #s) and s:byte(i + 1) or 0; end
    end

    local function norm(pattern)
        return (type(pattern) == 'string') and pattern:gsub('%s', ''):upper() or nil;
    end

    -- The address an instruction holds at base + off (a global, or a static object); in a signature copy the operand
    -- is 0, so it gets a block of emulated memory of `size` bytes.
    local function slot(base, off, size, key)
        local g = u32(base + off);
        if (not ok(g, size)) then
            g = scan_alloc(size, key);
            w32(base + off, g);
        end
        return g;
    end
    -- The object a global points at: emulated memory of `size` bytes when the global holds nothing usable.
    local function deref(g, size, key)
        local p = u32(g);
        if (not ok(p, size)) then
            p = scan_alloc(size, key);
            w32(g, p);
        end
        return p;
    end

    -- Earth seconds of the game's clock (LuAshitacast's reading: Vana'diel minutes x 2.4, less 92514960).
    local function vana_clock()
        local minutes = math.floor((os.time() - 1009810800) * 25 / 60) + 886 * 518400;
        return math.floor(minutes * 2.4 - 92514960) % 4294967296;
    end

    -- The game's 16-character layout names ("menu    map0    "): "menu" padded to 8, then the window's name.
    local function menu_name(s)
        if (type(s) ~= 'string' or s == '') then return ''; end
        local kind, rest = s:match('^(%S+)%s+(.*)$');
        if (kind ~= nil and #kind < 8) then s = kind .. (' '):rep(8 - #kind) .. rest; end
        return s:sub(1, 16);
    end

    -- Native settings (scan_registry.h NativeHooks): game memory an addon patches that stands for a client setting.
    -- A watch mirrors one setting at one address: the client's value is written there (so the addon reads it), and an
    -- addon's write is handed to the client. `seed` is the value when the client has none (the game's default).
    local native_get, native_set, native_supports = xi.native_get, xi.native_set, xi.native_supports;
    local function watch(list, name, addr, ct, n, seed)
        local w = { name = name, addr = addr, ct = ct, n = n };
        local v = native_get(name);
        if (v == nil) then v = seed; end
        if (v ~= nil) then wr(ct, addr, n, v); end
        w.last, w.client = tonumber(rd(ct, addr, n)), native_get(name);
        list[#list + 1] = w;
        return w;
    end
    -- A code patch: the setting is 1 while the `on` bytes are there (an addon wrote them), else 0; `off` are the
    -- game's own bytes (seeded into a copy).
    local function watch_patch(list, name, addr, on, off)
        local w = { name = name, addr = addr, on = on, patch = true };
        if (ok(addr, #off) and rd('uint8_t*', addr, 1) == 0) then
            for i = 1, #off do w8(addr + i - 1, off[i]); end
        end
        w.read = function ()
            if (not ok(addr, #on)) then return 0; end
            local p = ffi.cast('uint8_t*', addr);
            for i = 1, #on do if (p[i - 1] ~= on[i]) then return 0; end end
            return 1;
        end
        w.last = w.read();
        w.initial = w.last; -- the game's own bytes: what the setting returns to when the addon goes
        list[#list + 1] = w;
        return w;
    end
    -- A watch whose address is read from a code operand each frame (an addon may repoint the operand).
    local function watch_via(list, name, operand, ct, n, seed)
        local w = watch(list, name, u32(operand), ct, n, seed);
        w.operand = operand;
        return w;
    end
    local function sync(list)
        for _, w in ipairs(list) do
            if (w.operand ~= nil) then
                local a = u32(w.operand);
                if (a ~= w.addr and ok(a, w.n)) then w.addr = a; w.last = tonumber(rd(w.ct, a, w.n)); end
            end
            local cur = w.patch and w.read() or tonumber(rd(w.ct, w.addr, w.n));
            if (cur ~= w.last) then
                native_set(w.name, cur);
                w.last, w.client = cur, cur;
            elseif (not w.patch) then
                local c = native_get(w.name);
                if (c ~= nil and c ~= w.client) then
                    w.client = c;
                    wr(w.ct, w.addr, w.n, c);
                    if (w.mirror ~= nil) then wr(w.ct, w.mirror, w.n, c); end
                    w.last = tonumber(rd(w.ct, w.addr, w.n));
                end
            end
        end
    end
    local function watches(b, defs)
        local list = {};
        for _, d in ipairs(defs) do
            local g = slot(b, d[2], 16, 'native.' .. d[1]);
            if (d.via) then watch_via(list, d[1], b + d[2], d[3], d[4], d[5]); else watch(list, d[1], g, d[3], d[4], d[5]); end
        end
        return { watches = list };
    end
    -- A primitive window's visibility (hideparty): g -> primitive; +8 -> its object; +0x69 / +0x6A (u8) 1 = shown.
    local function window_watch(list, b, off, name)
        local prim = deref(slot(b, off, 8, 'window.' .. name .. '.g'), 0x10, 'window.' .. name .. '.prim');
        local o = deref(prim + 8, 0x80, 'window.' .. name .. '.obj');
        local w = watch(list, name, o + 0x69, 'uint8_t*', 1, 1);
        w.mirror = o + 0x6A;
        w8(w.mirror, rd('uint8_t*', w.addr, 1));
    end

    local emulated = {
        -- libs/scaling.lua: A1 [g] -> object { +0x10 window w, +0x12 h, +0x14 menu w, +0x16 menu h } (u16).
        ['A1????????85C05E74????80'] = {
            setup = function (b) local o = deref(slot(b, 1, 8, 'scaling.g'), 0x20, 'scaling.o'); return { o = o }; end,
            update = function (_, s)
                local w, h, mw, mh = xi.screen();
                w16(s.o + 0x10, w or 0); w16(s.o + 0x12, h or 0); w16(s.o + 0x14, mw or 0); w16(s.o + 0x16, mh or 0);
            end,
        },
        -- LuAshitacast: Vana'diel time. [+0x34] = g (the clock global, 0x104ae348 in 30260604); g + 0x0C = the clock.
        ['B0015EC390518B4C24088D4424005068'] = {
            size = 0x40,
            setup = function (b) return { g = slot(b, 0x34, 0x20, 'vanatime.g') }; end,
            update = function (_, s) w32(s.g + 0x0C, vana_clock()); end,
        },
        -- LuAshitacast: wardrobe access. A1 [g] -> object; +0xB4 (u8) flags: every wardrobe usable (INFERRED for LSB).
        ['A1????????8B88B4000000C1E907F6C101E9'] = {
            setup = function (b) return { o = deref(slot(b, 1, 8, 'wardrobe.g'), 0xC0, 'wardrobe.o') }; end,
            update = function (_, s) w8(s.o + 0xB4, 0xFF); end,
        },
        -- LuAshitacast / hgather: weather. 66 A1 [g]: g (u8) = the weather id.
        ['66A1????????663D????72'] = {
            setup = function (b) return { g = slot(b, 2, 8, 'weather.g') }; end,
            update = function (_, s) w8(s.g, M.state().weather or 0); end,
        },
        -- LuAshitacast / nomad: zone flags. [+9] = the flags' offset in the zone object (0x3C35C in 30260604),
        -- [+0x17] = g -> zone object; flags & 0x100 = in a Mog House.
        ['8B8C24040100008B90????????0BD18990????????8B15????????8B82'] = {
            size = 0x40,
            setup = function (b)
                local off = u32(b + 9);
                if (off == 0) then off = 0x10; w32(b + 9, off); w32(b + 0x11, off); end
                local g = slot(b, 0x17, 8, 'zoneflags.g');
                return { off = off, z = deref(g, off + 0x10, 'zoneflags.z') };
            end,
            update = function (_, s) w32(s.z + s.off, M.state().inMogHouse and 0x100 or 0); end,
        },
        -- HorizonXI libs/lin/ffxi.lua (HXUI, XIUI, tCrossBar, thotbar, skillchains, balloon, fancychat ...): the top menu.
        -- 3B 05 [g] at +0x0E: g -> menu object (0 = none); object + 4 -> header; header + 0x46 = name (16 chars).
        -- g is the interface object 0x105eed18's +0x54 in 30260604.
        ['8B480C85C974??8B510885D274??3B05'] = {
            size = 0x20,
            setup = function (b)
                local g = slot(b, 0x10, 8, 'menu.g');
                local m, h = scan_alloc(0x10, 'menu.object'), scan_alloc(0x60, 'menu.header');
                w32(m + 4, h);
                return { g = g, m = m, h = h };
            end,
            update = function (_, s)
                local name = menu_name(xi.client_ui().top_menu);
                wstr(s.h + 0x46, name, 16);
                w32(s.g, (name ~= '') and s.m or 0);
            end,
        },
        -- lin/ffxi.lua IsEventHappening: A0 [g] (u8) = 1 while an event runs.
        ['A0????????84C0741AA1????????85C0741166A1????????663B05????????0F94C0C3'] = {
            setup = function (b) return { g = slot(b, 1, 8, 'event.g') }; end,
            update = function (_, s) w8(s.g, xi.client_ui().event and 1 or 0); end,
        },
        -- lin/ffxi.lua IsInterfaceHidden: B9 [o] at +9 (the interface object, 0x105eed18): o + 0xB4 (u8) = 1 when hidden.
        ['8B4424046A016A0050B9????????E8????????F6D81BC040C3'] = {
            -- balloon also writes it (hiding the interface while its balloon shows): with the client's
            -- `interface.hidden` setting the write reaches the client, else the flag only follows ClientUi.
            setup = function (b)
                local s = { o = slot(b, 10, 0x100, 'interface.o') };
                if (native_supports('interface.hidden')) then
                    s.watches = {};
                    watch(s.watches, 'interface.hidden', s.o + 0xB4, 'uint8_t*', 1, xi.client_ui().hidden and 1 or 0);
                end
                return s;
            end,
            update = function (_, s) if (s.watches == nil) then w8(s.o + 0xB4, xi.client_ui().hidden and 1 or 0); end end,
        },
        -- lin/ffxi.lua IsChatExpanded: B9 [o] at +3 (the same interface object): o + 0xF1 (u8) = the log expanded.
        ['83EC??B9????????E8????????0FBF4C24??84C0'] = {
            setup = function (b) return { o = slot(b, 4, 0x100, 'interface.o') }; end,
            update = function (_, s) w8(s.o + 0xF1, xi.client_ui().chat_expanded and 1 or 0); end,
        },
        -- statustimers / ttimers / timers / thotbar / tCrossBar: the real-time stamp. 8B 0D [g]: g -> object;
        -- +0x0C = Unix seconds, +0x10 = milliseconds (the function returns +0x0C x 1000 + +0x10).
        ['8B0D????????8B410C8B49108D04808D04808D04808D04C1C3'] = {
            setup = function (b) return { o = deref(slot(b, 2, 8, 'realtime.g'), 0x20, 'realtime.o') }; end,
            update = function (_, s)
                local t = xi.clock and xi.clock() or os.clock();
                w32(s.o + 0x0C, os.time() % 4294967296);
                w32(s.o + 0x10, math.floor((t % 1) * 1000));
            end,
        },
        -- ttimers / thotbar / tCrossBar / skillchains: the ability recast table, B9 [o] at +0x18; 8-byte entries, +4
        -- (i16) the recast modifier. Left 0 (no modifier): IRecast answers the timers themselves.
        ['894124E9????????8B46??6A006A00508BCEE8'] = {
            size = 0x40,
            setup = function (b) slot(b, 0x19, 0x200, 'recast.abilities'); return {}; end,
        },
        -- config: get (8B 0D [this] ...): a stub to the client's config value; [+2] -> this (kept readable).
        ['8B0D????????85C974??8B44240450E8????????C383C8FFC3'] = {
            size = 0x40,
            setup = function (b)
                local g = slot(b, 2, 8, 'config.this.g');
                deref(g, 0x20, 'config.this');
                scan_stub(b, 'config_get', 4);
                return {};
            end,
        },
        -- config: set, the function 6 bytes before this pattern.
        ['85C974??8B4424088B5424045052E8????????C383C8FFC3'] = {
            size = 0x40, entry = 6,
            setup = function (b) scan_stub(b - 6, 'config_set'); return {}; end,
        },
        -- config: the configuration entry (an FsConfigSubject) for an id.
        ['8B490485C974108B4424048D14808D04508D0481C2040033C0C20400'] = {
            size = 0x40,
            setup = function (b) scan_stub(b, 'config_entry'); return {}; end,
        },
        -- config: SetAutoOffline.
        ['568B74240885F674??B889888888F7EE'] = {
            size = 0x40,
            setup = function (b) scan_stub(b, 'auto_offline'); return {}; end,
        },
        -- Ashita's named pointer 'inventory' (ashita.pointers.ini; offset 1): A1 [g] -> the inventory object. Here it
        -- holds the S2C 0x044 copies the game keeps at +0x40c44 (main job) / +0x40ce0 (support job): BLU set spells
        -- (thotbar, tCrossBar, skillchains, blusets, chains), PUP attachments (pupsets).
        ['A1????????8D9488????????8990????????E9'] = {
            setup = function (b)
                -- The offset as this client build has it (the BLU signature below), else 30260604's.
                local c = scan_find('C1E1032BC8B0018D????????????B9????????F3A55F5E5B', 0);
                local off = (c ~= 0) and u32(c + 10) or 0;
                if (off == 0 or off > 0x100000) then off = 0x40c44; end
                return { off = off, o = deref(slot(b, 1, 8, 'inventory.g'), off + 0x9C * 2 + 0x40, 'inventory.o') };
            end,
            update = function (_, s)
                for i = 0, 1 do
                    local bytes = xi.extended_job(i);
                    local at = s.o + s.off + 0x9C * i;
                    if (bytes ~= nil and ok(at, #bytes)) then
                        ffi.copy(ffi.cast('uint8_t*', at), bytes, #bytes);
                    end
                end
            end,
        },
        -- The 0x044 copies' offset in that object, read from an instruction (lea edi, [ebx + ecx * 4 + disp] at +7;
        -- disp at +10 = 0x40c44 in 30260604).
        ['C1E1032BC8B0018D????????????B9????????F3A55F5E5B'] = {
            size = 0x20,
            setup = function (b) if (u32(b + 10) == 0) then w32(b + 10, 0x40c44); end return {}; end,
        },
        -- ime: the input check it patches (+0x2F: 0xEB / 0x74). Nothing here reads it; a copy takes the writes.
        ['83EC08A1????????5333DB56'] = { size = 0x40, copy = true, setup = function () return {}; end },
        -- ime: 8B 0D [g] at +0: g -> the input object (+0xF0EC, +0xF10C: IME input on). ime reads [+2] through a
        -- `uint32_t**`, 8 bytes on x64, so the copy has 4 zero bytes after the operand.
        ['8B0D????????81EC0401000053568B'] = {
            size = 0x20, copy = true, zero = { 6 },
            setup = function (b) deref(slot(b, 2, 16, 'ime.input.g'), 0xF200, 'ime.input'); return {}; end,
        },
        -- ime: 8B 0D [g]: g -> the language object; +8 (u8) the client language (0 JP, 1 NA, 2/3 EU): English here.
        ['8B0D????????83EC0C8A51188B410884D256'] = {
            size = 0x20, copy = true, zero = { 6 },
            setup = function (b)
                local o = deref(slot(b, 2, 16, 'ime.lang.g'), 0x20, 'ime.lang');
                if (rd('uint8_t*', o + 8, 1) == 0) then w8(o + 8, 1); end
                return {};
            end,
        },

        -- Patches with a client setting behind them ("native" names; scan_registry.h NativeHooks). Each answers only
        -- when the client supports its settings (`needs`); otherwise 0, as a failed scan.

        -- drawdistance: 8B C1 48 74 08 D8 0D [world] ... [entity] at +0x0F: fmul factors (0x10456ab8 / 0x10456aa8,
        -- set at run time from the clipping-plane option).
        ['8BC1487408D80D'] = {
            size = 0x20, needs = { 'draw_distance.world', 'draw_distance.entity' },
            setup = function (b) return watches(b, { { 'draw_distance.world', 7, 'float*', 4, 1 }, { 'draw_distance.entity', 0x0F, 'float*', 4, 1 } }); end,
        },
        -- aspect: A1 [g] -> camera object; +0x2F0 (float) = height / (width x 0.75) (1 = the game's 4:3).
        ['A1????????85C074??D9442404D80D????????D80D'] = {
            needs = { 'aspect.factor' },
            setup = function (b)
                local o = deref(slot(b, 1, 8, 'aspect.g'), 0x300, 'aspect.o');
                local l = {};
                watch(l, 'aspect.factor', o + 0x2F0, 'float*', 4, 1);
                return { watches = l };
            end,
        },
        -- clearcolor: A1 [g] -> the Direct3D window object; +0x308 (D3DCOLOR) the clear colour.
        ['A1????????0508030000C3'] = {
            needs = { 'clear_colour' },
            setup = function (b)
                local o = deref(slot(b, 1, 8, 'clearcolor.g'), 0x310, 'clearcolor.o');
                local l = {};
                watch(l, 'clear_colour', o + 0x308, 'uint32_t*', 4, 0xFF000000);
                return { watches = l };
            end,
        },
        -- namecolors: B9 17 00 00 00 B8 80 80 80 80 BF [table]: 23 name colours (D3DCOLOR; 0x104e1b98 in 30260604).
        ['B917000000B880808080BF'] = {
            size = 0x10, needs = { 'name_colour.0' },
            setup = function (b)
                local t = slot(b, 0x0B, 23 * 4, 'namecolors.table');
                local l = {};
                for i = 0, 22 do watch(l, 'name_colour.' .. i, t + i * 4, 'uint32_t*', 4, 0x80808080); end
                return { watches = l };
            end,
        },
        -- hideparty: the party and target windows (+0x19, +0x23) and the alliance windows (the next pattern, +1 / +7).
        ['66C78182000000????C7818C000000????????C781900000'] = {
            size = 0x30, needs = { 'window.party.visible', 'window.target.visible' },
            setup = function (b)
                local l = {};
                window_watch(l, b, 0x19, 'window.party.visible');
                window_watch(l, b, 0x23, 'window.target.visible');
                return { watches = l };
            end,
        },
        ['A1????????8B0D????????89442424A1????????33DB89'] = {
            size = 0x20, needs = { 'window.alliance1.visible', 'window.alliance2.visible' },
            setup = function (b)
                local l = {};
                window_watch(l, b, 0x01, 'window.alliance1.visible');
                window_watch(l, b, 0x07, 'window.alliance2.visible');
                return { watches = l };
            end,
        },
        -- fps (and balloon): 8B 0D [g] at +0x0A: g -> the frame object; +0x30 (u32) the frame divisor (60 / n fps).
        ['81EC000100003BC174218B0D'] = {
            size = 0x20, needs = { 'fps.divisor' },
            setup = function (b)
                local o = deref(slot(b, 0x0C, 8, 'fps.g'), 0x40, 'fps.o');
                local l = {};
                watch(l, 'fps.divisor', o + 0x30, 'uint32_t*', 4, 2);
                return { watches = l };
            end,
        },
        -- instantchat: +0x1F `add eax, 0x14` (83 C0 14), the incoming chat delay; NOPs (90 90 90) = instant.
        ['8BF174??8B46340FBF4E5485C07D??894E34'] = {
            size = 0x30, needs = { 'chat.instant' },
            setup = function (b)
                local l = {};
                watch_patch(l, 'chat.instant', b + 0x1F, { 0x90, 0x90, 0x90 }, { 0x83, 0xC0, 0x14 });
                return { watches = l };
            end,
        },
        -- allmaps: +1 `call` (E8 rel32) of the key item test (0x101f7090 -> 0x10097db0) that sets the map window's
        -- "have this map" flag (+0xA46); `mov eax, 1` (B8 01 00 00 00) = every map. The first is the Map menu's map
        -- (0x101f9d09), the second the map shown for markers / waypoints (0x101f98e8).
        ['50??????????83C4048886????????0FBF8E????????8A'] = {
            size = 0x20, needs = { 'map.all' },
            setup = function (b)
                local l = {};
                watch_patch(l, 'map.all', b + 1, { 0xB8, 0x01, 0x00, 0x00, 0x00 }, { 0xE8, 0x00, 0x00, 0x00, 0x00 });
                return { watches = l };
            end,
        },
        ['50??????????83C4048886????????0FBF96????????8A'] = {
            size = 0x20, needs = { 'map.all.view' },
            setup = function (b)
                local l = {};
                watch_patch(l, 'map.all.view', b + 1, { 0xB8, 0x01, 0x00, 0x00, 0x00 }, { 0xE8, 0x00, 0x00, 0x00, 0x00 });
                return { watches = l };
            end,
        },
        -- filters: 8B 0D [g] at +8, add ecx, [offset] at +0x0E: g -> the chat filter object; + offset: three u32 words,
        -- the chat filter bits (as C2S 0x0DB sends them). filters reads +0x0A through a `uint32_t**` (8 bytes on x64),
        -- so this is always a copy whose operand is followed by zeros, with the offset 0x10000 (its low bytes 0).
        ['C3C74004000000008B0D????????81C1'] = {
            size = 0x20, copy = true, zero = { 0x0E }, needs = { 'chat.filter.0', 'chat.filter.1', 'chat.filter.2' },
            setup = function (b)
                w32(b + 0x10, 0x10000);
                local o = deref(slot(b, 0x0A, 16, 'filters.g'), 0x10010, 'filters.o');
                local l = {};
                for i = 0, 2 do watch(l, 'chat.filter.' .. i, o + 0x10000 + i * 4, 'uint32_t*', 4, 0); end
                return { watches = l };
            end,
        },
        -- xicamera: the follow camera's constants (dat-format §7.8b "Camera follow"): min 3 / max 6, battle 7.2 / 8.6,
        -- turn 0.0279 rad and rise 0.107 a tick.
        ['D8C9D9C0D8C1D9C2D80D????????D9C3DCC0D8EB'] = {
            size = 0x20, needs = { 'camera.distance.min' },
            setup = function (b) return watches(b, { { 'camera.distance.min', 0x0A, 'float*', 4, 3.0 } }); end,
        },
        ['D9442410D825????????51D80D'] = {
            size = 0x20, needs = { 'camera.distance.max' },
            setup = function (b) return watches(b, { { 'camera.distance.max', 0x06, 'float*', 4, 6.0 } }); end,
        },
        ['5152D8442424D905????????D8C1'] = {
            size = 0x20, needs = { 'camera.battle_distance.min' },
            setup = function (b) return watches(b, { { 'camera.battle_distance.min', 0x08, 'float*', 4, 7.2 } }); end,
        },
        ['D8C1D8CAD95C2450D805????????D8C9'] = {
            size = 0x20, needs = { 'camera.battle_distance.max' },
            setup = function (b) return watches(b, { { 'camera.battle_distance.max', 0x0A, 'float*', 4, 8.6 } }); end,
        },
        ['D84C24208B068BCED80D'] = {
            size = 0x20, needs = { 'camera.pan.horizontal' },
            setup = function (b) return watches(b, { { 'camera.pan.horizontal', 0x0A, 'float*', 4, 0.027924444 } }); end,
        },
        ['D84C24248B168BCED80D'] = {
            size = 0x20, needs = { 'camera.pan.vertical' },
            setup = function (b) return watches(b, { { 'camera.pan.vertical', 0x0A, 'float*', 4, 0.10666667 } }); end,
        },
        -- xicamera's battle camera range: an operand at +0x15 the addon repoints to its own float.
        ['D8C9D99C24DC000000DDD8D9442450D8442428D83D'] = {
            size = 0x30, needs = { 'camera.battle_range' },
            setup = function (b) return watches(b, { { 'camera.battle_range', 0x15, 'float*', 4, nil, via = true } }); end,
        },
        -- xicamera's other patches repoint code operands to its own constants (zoom set-up, walk animation, battle
        -- sound, jitter): no game code runs them here; they answer with the camera distance.
        ['85C0741AD9442404D80D????????D80D????????D87C'] = { size = 0x20, needs = { 'camera.distance.max' }, setup = function (b) slot(b, 0x10, 8, 'xicam.zoom'); return {}; end },
        ['0F85????????D80D????????D913D81D'] = { size = 0x20, needs = { 'camera.distance.max' }, setup = function () return {}; end },
        ['7514D9442410D80D????????D91B8B8E'] = { size = 0x20, needs = { 'camera.distance.max' }, setup = function () return {}; end },
        ['D95C2414741B487410D9442410D80D'] = { size = 0x20, needs = { 'camera.distance.max' }, setup = function () return {}; end },
        ['8D54242C8D44242CD8C9525550'] = { size = 0x30, needs = { 'camera.distance.max' }, setup = function (b) slot(b, 0x0F, 8, 'xicam.jitter'); return {}; end },
    };

    -- balloon / stepdialog: the event message. 8B 0D [g] at +2: g -> the TkEventMsg2 object while a message waits for
    -- a key (0 otherwise); TkEventMsg2::OnKeyDown (the other pattern) presses a key on it (5 = Enter).
    emulated['8B0D????????85C90F??????????8B410885C00F'] = {
        size = 0x20, needs = { 'event.message.waiting' },
        setup = function (b)
            local g = slot(b, 2, 8, 'eventmsg.g');
            return { g = g, o = scan_alloc(0x40, 'eventmsg.o') };
        end,
        update = function (_, s) w32(s.g, (native_get('event.message.waiting') or 0) ~= 0 and s.o or 0); end,
    };
    emulated['538B5C240856578B7C24148BF15753E8????????8B0D????????3BF174'] = {
        size = 0x40, needs = { 'event.message.key' },
        setup = function (b) scan_stub(b, 'event_message_key'); return {}; end,
    };
    -- balloon: 80 A0 B2 00 00 00 FB C6 05 [flag] 00: the "user controls the camera" flag (u8) it clears and sets.
    emulated['80A0B2000000FBC605????????00'] = {
        size = 0x20, needs = { 'camera.user_control' },
        setup = function (b) return watches(b, { { 'camera.user_control', 9, 'uint8_t*', 1, 1 } }); end,
    };
    -- chime: YmSePlayChime(id, power, force): a chime sound effect.
    emulated['A1????????5685C00F??????????6A006A046A40E8????????8BF083C40C8BCE'] = {
        size = 0x40, needs = { 'sound.chime' },
        setup = function (b) scan_stub(b, 'chime'); return {}; end,
    };
    -- The game's clock functions (the `ffxi.time` library: fishaid's restock countdown, ...), each answered by the
    -- game's own arithmetic (src/ashita/game_time.h); the game time is the last zone-in's 0x00A GameTime plus the
    -- seconds since, as ntGameTimeGet gives it.
    for pattern, name in pairs({
        ['E8????????8BC88B4424043BC876??2BC1488BC8B8FFFFFF7F'] = 'game_time_diff', -- ntGameTimeDiff 0x10103980
        ['E8????????0305????????C3'] = 'game_time',                             -- ntGameTimeGet 0x10103910
        ['8B44240433D2B9006C0000F7F1'] = 'game_weekday',                         -- ntGameDayWeekGet 0x10103a10
        ['8B44240433D2B900950100F7F1'] = 'game_day',                             -- ntGameDayGet 0x101039e0
        ['8B44240433D2B900FC1200F7F1'] = 'game_month',                           -- ntGameMonthGet 0x101039c0
        ['B84134F81AF7642404C1EA118BC2C3'] = 'game_year',                        -- ntGameYearGet 0x101039b0
        ['8B44240433D2B990000000F7F1'] = 'game_minutes',                         -- ntGameMinGet 0x10103a60
        ['8B44240433D2B9800D0000F7F1'] = 'game_hours',                           -- ntGameHourGet 0x10103a40
        ['8B4C2404B8DB4B682FF7E12BCA'] = 'game_moon',                            -- ntGameMoonGet 0x10103a90
        ['8B4C2404B8DB4B682FF7E18BC1'] = 'game_moon_percent',                    -- ntGameMoonPerGet 0x10103ad0
        ['518D44240050E8????????8B44240483C408C3'] = 'unix_time',                -- xiGetTime 0x100dbd40
        ['8B44240481EC8000000053555633F685C05774??50E8'] = 'time_string',        -- YkGetTimeString 0x101c8b80
        ['8B442404568BC833F63D8017E87F894C2408'] = 'local_time',                 -- sqLocalTime 0x101c88c0
    }) do
        emulated[pattern] = { size = 0x20, setup = function (b) scan_stub(b, name); return {}; end };
    end
    -- fancychat's view of the game's interface (lib/lifecycle.lua, lib/render.lua).
    -- The two log windows (the draw-message-window code 0x1012df6d: A1 [g1] ... 8B 0D [g2]; 0x105eee1c / 0x105eee20):
    -- g -> the log object (src/ui/log_object.h). Each frame the client's own shown height goes to +0x34 (lines x 16),
    -- the line limits to +0x4e / +0x50, the line height to +0x54 and the text rect's left / top to +0x40 / +0x42 (screen
    -- pixels); an addon's write of +0x34 since the last frame (fancychat clears it every frame to hide the log) goes
    -- to the client, which then draws that many lines while the addon keeps writing, and lets go when the addon
    -- unloads.
    local function i32(a) return tonumber(rd('int32_t*', a, 4)); end
    emulated['A1????????C64059018B0D????????C6415901C20800'] = {
        size = 0x30,
        setup = function (b)
            return {
                o = { deref(slot(b, 0x01, 4, 'msgwin.g1'), 0x80, 'msgwin.o1'), deref(slot(b, 0x0B, 4, 'msgwin.g2'), 0x80, 'msgwin.o2') },
                wrote = {}, held = {}, release = { 'log.window1.height', 'log.window2.height' },
            };
        end,
        update = function (_, s)
            local _, h = xi.screen();
            for i, o in ipairs(s.o) do
                local name = 'log.window' .. i;
                local cur = i32(o + 0x34);
                if (s.wrote[i] ~= nil and cur ~= s.wrote[i]) then
                    native_set(name .. '.height', cur);
                    s.held[i] = true;
                end
                w32(o + 0x34, native_get(name .. '.height') or 0);
                s.wrote[i] = i32(o + 0x34);
                w16(o + 0x4e, native_get(name .. '.max_lines') or 8);
                w16(o + 0x50, native_get(name .. '.min_lines') or 0);
                w16(o + 0x54, 16);
                local shown = (native_get(name .. '.shown') or 0) ~= 0;
                w16(o + 0x40, shown and 16 or 0);
                w16(o + 0x42, shown and math.max(0, (h or 0) - (native_get(name .. '.text_top') or 0)) or 0);
            end
        end,
    };
    -- The viewport (D3DVIEWPORT8 at 0x105eede4, copied to 0x105eedfc: X, Y, Width, Height, MinZ, MaxZ): `A1 [&Height]`
    -- (+1) and `BF [&copy]` (+1; Width is copy - 0x10, in the first), filled with the window's size each frame.
    emulated['A1????????3BF07E??8BF0'] = {
        size = 0x20,
        setup = function (b) return { g = slot(b, 0x01, 4, 'viewport.height') }; end,
        update = function (_, s) local _, h = xi.screen(); w32(s.g, h); end,
    };
    emulated['BF????????F3??0FBF4C24'] = {
        size = 0x20,
        setup = function (b)
            local p = u32(b + 1);
            if (not ok(p - 0x18, 0x30)) then
                p = scan_alloc(0x40, 'viewport') + 0x18;
                w32(b + 1, p);
            end
            return { p = p };
        end,
        update = function (_, s)
            local w, h = xi.screen();
            for _, v in ipairs({ s.p - 0x18, s.p }) do w32(v + 8, w); w32(v + 12, h); end
        end,
    };
    -- The event message window's "shown" byte (0x1047d3ec: set when one opens 0x100b274d, cleared 0x100ae2f1).
    emulated['A0????????53565784C08BF1'] = {
        size = 0x20,
        setup = function (b) return { g = slot(b, 0x01, 4, 'event.message.shown') }; end,
        update = function (_, s) w8(s.g, xi.client_ui().event_message and 1 or 0); end,
    };
    -- The zone object (`89 35 [&pGlobalNowZone]`, `81 C6 [offset]`): at + 0x4638 the network window's counters, +0
    -- bytes a second sent and +4 received over the last 3 s (0x100fca10), as the client's network window shows them.
    emulated['8935????????81C6????????56E8'] = {
        size = 0x20,
        setup = function (b)
            local off = u32(b + 8);
            if (off == 0 or off > 0x10000) then off = 0x4638; w32(b + 8, off); end
            return { o = deref(slot(b, 0x02, 4, 'zone.g'), off + 0x20, 'zone.o') + off };
        end,
        update = function (_, s)
            local ui = xi.client_ui();
            w32(s.o, ui.send_rate or 0);
            w32(s.o + 4, ui.recv_rate or 0);
        end,
    };
    -- The game's target helpers (fancychat's targets.lua: <bt>, <r>, <scan>), answered from the client's state.
    for pattern, name in pairs({
        ['66A1????????83EC186685C053565774??0FBFC08B0C85'] = 'battle_actor', -- SeekBattleActor 0x10077cb0
        ['53568B35????????33C085F6577E??8B'] = 'monster_name',              -- GetMonsterName 0x1008a810
        ['8B0D????????85C9750333C0C36A00'] = 'last_teller',                 -- ntSysGetLastTeller 0x100d9a30
        ['A1????????85C074??F7'] = 'tracking_pos',                          -- gcTrackingPosGet 0x100f9400
    }) do
        emulated[pattern] = { size = 0x20, setup = function (b) scan_stub(b, name); return {}; end };
    end
    -- blusets / pupsets: the game's C2S 0x102 sender (isSubJob, jobType, index, id). BLU's spell slots (jobType 0x1000)
    -- are sent as blusets' own "fast" mode builds the packet, from the current set; other jobs answer 0 (failed).
    emulated['8B0D????????81EC9C00000085C95356570F??????????8B'] = {
        size = 0x40,
        setup = function (b) scan_stub(b, 'equipex'); return {}; end,
        update = function ()
            for _, c in ipairs(xi.take_equipex()) do
                local sub, index, id = c[1], c[3], c[4];
                local cur = xi.extended_job((sub ~= 0) and 1 or 0) or ('\0'):rep(0x9C);
                local p = { 0x02, 0x53, 0, 0, id, 0, 0, 0, 0x10, (sub ~= 0) and 1 or 0, 0, 0 };
                for k = 0, 19 do
                    p[#p + 1] = (k == index) and id or cur:byte(4 + k + 1);
                end
                for _ = 1, 132 do p[#p + 1] = 0; end
                AshitaCore:GetPacketManager():AddOutgoingPacket(0x102, p);
            end
        end,
    };

    -- The macro palette (XIUI's libs/ffxi/macros.lua, macrofix): FsMacroContainer / FsMacroController and their
    -- functions, answered from the client's macro book (scan_registry.h MacroHooks).
    local function macros_given() return (xi.macros()); end
    local STOP = 'C781????????FFFFFFFFC3'; -- mov dword [ecx + running], -1; ret: FsMacroContainer::stopMacro
    local function stop_offset()
        local s = scan_find(STOP, 0);
        local off = (s ~= 0) and u32(s + 2) or 0;
        return (off ~= 0 and off < 0x10000) and off or 0x2000;
    end
    local function macro_container(g)
        local o = deref(g, math.max(4 + 380 * 20 + 16, stop_offset() + 8), 'macro.container');
        xi.macro_object(o);
        return o;
    end
    local function macro_stub(name)
        return { available = macros_given, size = 0x40, setup = function (b) scan_stub(b, name); return {}; end };
    end
    emulated['8B0D????????E8????????8B4424105EC38B15????????68'] = {
        available = macros_given, size = 0x20,
        setup = function (b) return { o = macro_container(slot(b, 2, 8, 'macro.g')) }; end,
    };
    emulated['A3????????EB??891D????????6A50E8'] = {
        available = macros_given, size = 0x20,
        setup = function (b) deref(slot(b, 1, 8, 'macro.controller.g'), 0x40, 'macro.controller'); return {}; end,
    };
    emulated['66A1????????5633F66685C074??0FBFC08B3485????????8B0D'] = macro_stub('macro_can_use');
    emulated['8B44240485C07C??83F8147D??8D1440C1E2052BD08D4C9104E8????????B001'] = macro_stub('macro_clear');
    emulated['8B44240485C07C??83F8147D??8D1440C1E2052BD08D4C9104E8????????85C0'] = macro_stub('macro_name');
    emulated['8B44240485C07C??83F8147D??8B542408528D1440C1E2052BD08D4C9104E8????????C2080033C0C20800'] = macro_stub('macro_line');
    emulated['8A44240453563C018BF175??8A5C24108B4E040FBEC350E8'] = macro_stub('macro_run');
    emulated['8B442404568BF18B8EC01D00003BC875??8BC15EC20400'] = macro_stub('macro_set_book');
    emulated['568BF1578B7C240C8B86C01D00008D04'] = macro_stub('macro_set_page');
    emulated['8B442408538B5C24085657508BCBE8????????85C07C??8B'] = macro_stub('macro_set');
    -- stopMacro: +2 the running field's offset (-1 = idle). Each frame it shows whether a client macro runs; an addon
    -- writing -1 while one runs stops it.
    emulated[STOP] = {
        available = macros_given, size = 0x10,
        setup = function (b)
            if (u32(b + 2) == 0) then w32(b + 2, stop_offset()); end
            local s = { off = u32(b + 2) };
            -- The container, through the macro signature (scanned now if the addon has not yet).
            s.o = u32(u32(ashita.memory.find(0, 0, '8B0D????????E8????????8B4424105EC38B15????????68', 2, 0)));
            if (not ok(s.o, s.off + 4)) then error('no macro container'); end
            s.last = 0xFFFFFFFF;
            w32(s.o + s.off, s.last);
            return s;
        end,
        update = function (_, s)
            local cur = u32(s.o + s.off);
            local _, running = xi.macros();
            if (cur == 0xFFFFFFFF and s.last ~= 0xFFFFFFFF and running) then xi.macro_stop(); running = false; end
            s.last = running and 0 or 0xFFFFFFFF;
            w32(s.o + s.off, s.last);
        end,
    };
    -- The palette's own code, which macrofix and XIUI patch: the hold delay (macrofix: NOPs at +5 of the first two)
    -- and drawing the game's palette (XIUI: F9 90 at +3 hides it, as XIUI draws its own bar).
    local function palette(macrofix, hide)
        return {
            size = 0x30,
            available = function () return native_supports('macro.instant') or native_supports('window.macro_palette.visible'); end,
            setup = function (b)
                local l = {};
                if (macrofix ~= nil and native_supports('macro.instant')) then watch_patch(l, 'macro.instant', b + 5, macrofix, {}); end
                if (hide and native_supports('window.macro_palette.visible')) then
                    local w = watch_patch(l, 'window.macro_palette.hidden', b + 3, { 0xF9, 0x90 }, { 0x3B, 0xC3 });
                    w.name = 'window.macro_palette.visible';
                    local read = w.read;
                    w.read = function () return 1 - read(); end
                    w.last = w.read();
                    w.initial = w.last;
                end
                return { watches = l };
            end,
        };
    end
    emulated['2B46103BC3????????????68????????B9'] = palette({ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }, true);
    emulated['2B46103BC3????68????????B9'] = palette({ 0x90, 0x90 }, true);
    emulated['83C41084C074??8BCEE8????????84C074??8A460CB9????????3AC3'] = palette(nil, false);
    emulated['83C41084C074??8BCEE8????????84C074??807E0C02'] = palette(nil, false);
    local e5 = palette(nil, false);
    e5.counts = 2;
    emulated['83C41084C0????8BCEE8????????84C0????8A460C84C0????8B461485C0'] = e5;

    -- Patterns answered from the image as they are: their addons only read the image or write their patches into it
    -- (no effect here), and treat what they read as data.
    local safe = {};

    local ready = {}; -- pattern -> { base, state } for this addon
    local copies = {};

    -- A copy of the pattern's bytes (wildcards 0) in emulated memory, `extra` bytes past its end and `entry` before it.
    local function copy_of(pattern, e, count)
        local key = pattern .. '#' .. (count or 0);
        if (copies[key] ~= nil) then return copies[key]; end
        local n, before = #pattern / 2, e.entry or 0;
        local mem = scan_alloc(before + n + (e.size or 0x20) + 0x20, 'sig.' .. key);
        local b = mem + before;
        local p = ffi.cast('uint8_t*', b);
        for i = 0, n - 1 do
            local h = pattern:sub(i * 2 + 1, i * 2 + 2);
            p[i] = (h == '??') and 0 or tonumber(h, 16);
        end
        for _, at in ipairs(e.zero or {}) do
            for i = at, at + 3 do p[i] = 0; end
        end
        copies[key] = b;
        return b;
    end

    local function refresh()
        for _, r in pairs(ready) do
            local pattern = r.pattern;
            local e = emulated[pattern];
            if (e.update ~= nil) then
                local okc, err = pcall(e.update, r.base, r.state);
                if (not okc) then xi.log(3, ('memory stand-in %s: %s'):format(pattern, tostring(err))); end
            end
            if (r.state.watches ~= nil) then
                local okc, err = pcall(sync, r.state.watches);
                if (not okc) then xi.log(3, ('memory stand-in %s: %s'):format(pattern, tostring(err))); end
            end
        end
    end

    local function find(module, _, pattern, offset, count)
        if (type(module) == 'string' and module:lower() ~= 'ffximain.dll') then
            warn_once('ashita.memory.find in ' .. module);
            return 0;
        end
        pattern = norm(pattern);
        if (pattern == nil or #pattern < 2 or #pattern % 2 ~= 0) then return 0; end
        count = tonumber(count) or 0;
        local e = emulated[pattern];
        if (e ~= nil and count < (e.counts or 1)) then
            local key = pattern .. '#' .. count;
            local r = ready[key];
            if (r == nil) then
                if (e.available ~= nil and not e.available()) then
                    warn_once(('ashita.memory.find %s (the client does not give it)'):format(pattern));
                    return 0;
                end
                for _, name in ipairs(e.needs or {}) do
                    if (not native_supports(name)) then
                        warn_once(('ashita.memory.find %s (the client has no %s)'):format(pattern, name));
                        return 0;
                    end
                end
                local b = (not e.copy) and scan_find(pattern, count) or 0;
                if (b == 0) then b = copy_of(pattern, e, count); end
                local okc, st = pcall(e.setup, b, count);
                if (not okc) then
                    xi.log(3, ('memory stand-in %s: %s'):format(pattern, tostring(st)));
                    return 0;
                end
                r = { base = b, state = st or {}, pattern = pattern };
                ready[key] = r;
                if (e.update ~= nil) then pcall(e.update, b, r.state); end
            end
            return r.base + (tonumber(offset) or 0);
        end
        if (not safe[pattern] and not scan_all) then
            warn_once('ashita.memory.find ' .. pattern);
            return 0;
        end
        local b = scan_find(pattern, count);
        if (b == 0) then return 0; end
        return b + (tonumber(offset) or 0);
    end

    ashita.memory.find = find;

    -- The memory functions, bounds-checked (Ashita's take an address, some an offset too).
    local types = {
        uint8 = { 'uint8_t*', 1 }, int8 = { 'int8_t*', 1 }, uint16 = { 'uint16_t*', 2 }, int16 = { 'int16_t*', 2 },
        uint32 = { 'uint32_t*', 4 }, int32 = { 'int32_t*', 4 }, uint64 = { 'uint64_t*', 8 }, int64 = { 'int64_t*', 8 },
        float = { 'float*', 4 }, double = { 'double*', 8 },
    };
    for name, t in pairs(types) do
        local ct, n = t[1], t[2];
        ashita.memory['read_' .. name] = function (addr, off)
            return tonumber(rd(ct, (tonumber(addr) or 0) + (tonumber(off) or 0), n));
        end
        ashita.memory['write_' .. name] = function (addr, v)
            return wr(ct, tonumber(addr) or 0, n, tonumber(v) or 0);
        end
    end
    ashita.memory.read_uint = ashita.memory.read_uint32;
    ashita.memory.read_int = ashita.memory.read_int32;
    ashita.memory.write_uint = ashita.memory.write_uint32;
    ashita.memory.write_int = ashita.memory.write_int32;
    ashita.memory.read_array = function (addr, n)
        local t = {};
        n = tonumber(n) or 0;
        if (ok(tonumber(addr) or 0, n)) then
            local p = ffi.cast('uint8_t*', addr);
            for i = 1, n do t[i] = p[i - 1]; end
        else
            for i = 1, n do t[i] = 0; end
        end
        return t;
    end
    ashita.memory.write_array = function (addr, t)
        if (type(t) ~= 'table' or not ok(tonumber(addr) or 0, #t)) then return false; end
        local p = ffi.cast('uint8_t*', addr);
        for i = 1, #t do p[i - 1] = t[i]; end
        return true;
    end
    ashita.memory.read_string = function (addr, n)
        n = tonumber(n) or 0;
        if (n <= 0 or not ok(tonumber(addr) or 0, n)) then return ''; end
        return ffi.string(ffi.cast('const char*', addr), n);
    end
    ashita.memory.read_literal = function (addr, n)
        local s = ashita.memory.read_string(addr, n);
        return (s:gsub('%z.*$', ''));
    end
    ashita.memory.write_string = function (addr, s, n)
        s = tostring(s or '');
        n = tonumber(n) or #s;
        if (not ok(tonumber(addr) or 0, n)) then return false; end
        wstr(addr, s, n);
        return true;
    end
    ashita.memory.get_base = function (module)
        if (type(module) == 'string' and module:lower() ~= 'ffximain.dll') then return 0; end
        local b = scan_image();
        return b;
    end
    ashita.memory.get_size = function (module)
        if (type(module) == 'string' and module:lower() ~= 'ffximain.dll') then return 0; end
        local _, n = scan_image();
        return n;
    end
    ashita.memory.alloc = function (n) return scan_alloc(tonumber(n) or 16, ''); end
    ashita.memory.dealloc = function () return true; end
    ashita.memory.protect = function () return true, 0x40; end
    ashita.memory.unprotect = function () return true, 0x40; end

    -- IPointerManager: Ashita's named pointers (config/ashita/ashita.pointers.ini, custom.pointers.ini) and the
    -- addons' own (Add). Get answers the scan; reads through ashita.memory past it cannot fault.
    local named, inis = {}, nil;
    local function load_inis()
        inis = {};
        for _, f in ipairs({ 'ashita.pointers.ini', 'custom.pointers.ini' }) do
            local h = io.open(('%s/config/ashita/%s'):format(root, f), 'r');
            if (h ~= nil) then
                local cur;
                for line in h:lines() do
                    line = line:gsub('[\r\n]', '');
                    local sec = line:match('^%s*%[([^%]]+)%]');
                    if (sec ~= nil) then
                        cur = { name = sec:lower():gsub('%s+$', '') };
                        inis[cur.name] = cur;
                    elseif (cur ~= nil and not line:match('^%s*;')) then
                        local k, v = line:match('^%s*(%w+)%s*=%s*(.-)%s*$');
                        if (k ~= nil) then cur[k:lower()] = v; end
                    end
                end
                h:close();
            end
        end
    end
    local function resolve(e)
        if (e.addr == nil) then
            local pattern = norm(e.pattern);
            local b = (pattern ~= nil) and (emulated[pattern] and find(e.module, 0, pattern, 0, e.count) or scan_find(pattern, tonumber(e.count) or 0)) or 0;
            e.addr = (b ~= 0) and (b + (tonumber(e.offset) or 0)) or 0;
        end
        return e.addr;
    end
    pointer_manager.Add = function (_, name, module, pattern, offset, count)
        if (type(name) ~= 'string') then return 0; end
        local e = { module = module, pattern = pattern, offset = offset, count = count };
        named[name:lower()] = e;
        return resolve(e);
    end
    pointer_manager.Get = function (_, name)
        if (type(name) ~= 'string') then return 0; end
        local e = named[name:lower()];
        if (e == nil) then
            if (inis == nil) then load_inis(); end
            e = inis[name:lower()];
        end
        return (e ~= nil) and resolve(e) or 0;
    end
    pointer_manager.Delete = function (_, name)
        if (type(name) == 'string') then named[name:lower()] = nil; end
    end

    -- When the addon goes (its state closes), its code patches go with it: each patch setting it changed returns to
    -- what the game's own bytes mean (allmaps' unload restores them too, but nothing reads them again).
    __xi_patch_gc = ffi.gc(ffi.new('uint8_t[1]'), function ()
        for _, r in pairs(ready) do
            -- A hold on a client value (the log objects' heights) goes with the addon: -1 lets the client's own show.
            for i, name in ipairs(r.state.release or {}) do
                if (r.state.held ~= nil and r.state.held[i]) then pcall(native_set, name, -1); end
            end
            for _, w in ipairs(r.state.watches or {}) do
                if (w.patch and w.initial ~= nil and w.last ~= w.initial) then pcall(native_set, w.name, w.initial); end
            end
        end
    end);

    -- Refresh the emulated memory before the frame's tasks and events.
    local frame_ = __xi_frame;
    function __xi_frame()
        if (next(ready) ~= nil) then refresh(); end
        frame_();
    end
end
