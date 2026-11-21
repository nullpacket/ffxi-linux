-- SPDX-License-Identifier: GPL-3.0-or-later
-------------------------------------------------------------------------------------------------------------------
-- ffxi-linux: Direct3D 8, D3DX 8 and gdifonttexture.dll for Ashita addons, over the native objects of
-- src/ashita/d3d8_emu.cpp (__xi_d3d). Appended to prelude.lua / prelude_memory.lua in one chunk (uses `ffi`).
-- docs/ashita-compat.md "Direct3D 8 and gdifonts" lists what is covered.
-------------------------------------------------------------------------------------------------------------------
do
    local D = __xi_d3d;

    -- Ashita's LuaJIT is 32-bit Windows, where long is 32 bits: libs/win32types.lua makes DWORD / LONG `unsigned long`
    -- / `long`, which would be 64 bits here and shift every D3D structure (RECT, D3DSURFACE_DESC, D3DVIEWPORT8 ...).
    local cdef = ffi.cdef;
    local function llp64(s)
        s = s:gsub('%f[%w_]unsigned%s+long%s+long%f[^%w_]', '@@XIULL@@'):gsub('%f[%w_]long%s+long%f[^%w_]', '@@XILL@@');
        s = s:gsub('%f[%w_]unsigned%s+long%s+int%f[^%w_]', 'uint32_t'):gsub('%f[%w_]unsigned%s+long%f[^%w_]', 'uint32_t');
        s = s:gsub('%f[%w_]signed%s+long%f[^%w_]', 'int32_t'):gsub('%f[%w_]long%s+int%f[^%w_]', 'int32_t'):gsub('%f[%w_]long%f[^%w_]', 'int32_t');
        return (s:gsub('@@XIULL@@', 'unsigned long long'):gsub('@@XILL@@', 'long long'));
    end
    ffi.cdef = function (s, ...)
        if (type(s) == 'string') then
            s = llp64(s);
        end
        return cdef(s, ...);
    end

    -- The device AshitaCore:GetDirect3DDevice() hands out (libs/d3d8.lua casts it to IDirect3DDevice8*).
    __xi_d3d_device = D.device;

    local fn = {
        create_sprite = ffi.cast('int32_t (*)(void*, void*)', D.fn.create_sprite),
        from_file = ffi.cast('int32_t (*)(void*, const char*, void*)', D.fn.texture_from_file),
        from_file_ex = ffi.cast('int32_t (*)(void*, const char*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, void*, void*, void*)', D.fn.texture_from_file_ex),
        from_mem = ffi.cast('int32_t (*)(void*, const void*, uint32_t, void*)', D.fn.texture_from_memory),
        from_mem_ex = ffi.cast('int32_t (*)(void*, const void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, void*, void*, void*)', D.fn.texture_from_memory_ex),
        gdi_create_manager = ffi.cast('void* (*)(void*)', D.fn.gdi_create_manager),
        gdi_destroy_manager = ffi.cast('void (*)(void*)', D.fn.gdi_destroy_manager),
        gdi_create_texture = ffi.cast('void* (*)(void*, const void*, int32_t*)', D.fn.gdi_create_texture),
        gdi_create_texture_color = ffi.cast('void* (*)(void*, const void*, int32_t*)', D.fn.gdi_create_texture_color),
        gdi_set_font_folder = ffi.cast('void (*)(const char*)', D.fn.gdi_set_font_folder),
        gdi_create_rect = ffi.cast('void* (*)(void*, const void*, int32_t*)', D.fn.gdi_create_rect),
        gdi_font_available = ffi.cast('int32_t (*)(const char*)', D.fn.gdi_font_available),
    };

    -- Image data an addon passes: a Lua string (resource icons' Bitmap) or a pointer; a size of -1 / 0 with a string
    -- means the whole string.
    local function mem(data, size)
        if (type(data) == 'string') then
            local n = tonumber(size) or 0;
            if (n <= 0 or n > #data) then n = #data; end
            return ffi.cast('const char*', data), n;
        end
        return data, size;
    end
    local function u32(v) return tonumber(ffi.cast('uint32_t', v or 0)); end

    register_ffi_c('D3DXCreateSprite', function (dev, out) return fn.create_sprite(dev, out); end);
    register_ffi_c('D3DXCreateTextureFromFileA', function (dev, path, out) return fn.from_file(dev, path, out); end);
    register_ffi_c('D3DXCreateTextureFromFileExA', function (dev, path, w, h, mips, usage, fmt, pool, filter, mipfilter, key, info, pal, out)
        return fn.from_file_ex(dev, path, u32(w), u32(h), u32(mips), u32(usage), u32(fmt), u32(pool), u32(filter), u32(mipfilter), u32(key), info, pal, out);
    end);
    register_ffi_c('D3DXCreateTextureFromFileInMemory', function (dev, data, size, out)
        local p, n = mem(data, size);
        return fn.from_mem(dev, p, u32(n), out);
    end);
    register_ffi_c('D3DXCreateTextureFromFileInMemoryEx', function (dev, data, size, w, h, mips, usage, fmt, pool, filter, mipfilter, key, info, pal, out)
        local p, n = mem(data, size);
        return fn.from_mem_ex(dev, p, u32(n), u32(w), u32(h), u32(mips), u32(usage), u32(fmt), u32(pool), u32(filter), u32(mipfilter), u32(key), info, pal, out);
    end);

    -- gdifonts/gdifonttexture.dll (ffi.load of any copy an addon bundles): text and rounded rectangles as textures.
    __xi_ffi_load['gdifonttexture.dll'] = function ()
        local wh = ffi.new('int32_t[2]');
        local function result(p)
            local tex = (p ~= nil) and ffi.cast('IDirect3DTexture8*', p) or nil;
            return ffi.new('GdiFontReturn_t', wh[0], wh[1], tex);
        end
        return {
            CreateFontManager = function (dev) return ffi.cast('uint32_t*', fn.gdi_create_manager(dev)); end,
            DestroyFontManager = function (m) fn.gdi_destroy_manager(m); end,
            CreateTexture = function (m, data) return result(fn.gdi_create_texture(m, data, wh)); end,
            -- fancychat's build: colour marks in the text, and its own font folder.
            CreateTextureColor = function (m, data) return result(fn.gdi_create_texture_color(m, data, wh)); end,
            SetFontFolder = function (folder) fn.gdi_set_font_folder(tostring(folder)); end,
            CreateRectTexture = function (m, data) return result(fn.gdi_create_rect(m, data, wh)); end,
            GetFontAvailable = function (name) return fn.gdi_font_available(name) ~= 0; end,
            EnableTextureDump = function () end,
            DisableTextureDump = function () end,
        };
    end

    -- ImGui with D3D textures: addons pass a texture as tonumber(ffi.cast('uint32_t', tex)) or as the pointer itself.
    local gui = __xi_imgui;
    local function texid(t)
        if (type(t) == 'number') then return t; end
        if (type(t) == 'cdata') then return tonumber(ffi.cast('uintptr_t', t)); end
        return 0;
    end
    local function v2(t, dx, dy)
        if (type(t) ~= 'table' and type(t) ~= 'cdata') then return dx, dy; end
        return tonumber(t.x or t[1]) or dx, tonumber(t.y or t[2]) or dy;
    end
    local function v4(t, a, b, c, d)
        if (type(t) ~= 'table' and type(t) ~= 'cdata') then return a, b, c, d; end
        return tonumber(t.x or t[1]) or a, tonumber(t.y or t[2]) or b, tonumber(t.z or t[3]) or c, tonumber(t.w or t[4]) or d;
    end
    local function col(c)
        if (type(c) == 'table') then return gui.ColorConvertFloat4ToU32(c); end
        return tonumber(c) or 0xFFFFFFFF;
    end
    gui.Image = function (tex, size, uv0, uv1)
        local w, h = v2(size, 0, 0);
        local u0, v0 = v2(uv0, 0, 0);
        local u1, v1 = v2(uv1, 1, 1);
        D.image(texid(tex), w, h, u0, v0, u1, v1);
    end
    gui.ImageWithBg = function (tex, size, uv0, uv1, bg, tint)
        local w, h = v2(size, 0, 0);
        local u0, v0 = v2(uv0, 0, 0);
        local u1, v1 = v2(uv1, 1, 1);
        local br, bg_, bb, ba = v4(bg, 0, 0, 0, 0);
        local tr, tg, tb, ta = v4(tint, 1, 1, 1, 1);
        D.image_bg(texid(tex), w, h, u0, v0, u1, v1, br, bg_, bb, ba, tr, tg, tb, ta);
    end
    -- ImageButton(str_id, tex, size, uv0, uv1, bg, tint) (Ashita 4.3 / ImGui 1.92), or the older
    -- ImageButton(tex, size, uv0, uv1, frame_padding, bg, tint).
    gui.ImageButton = function (a, ...)
        local sid, tex, size, uv0, uv1, bg, tint;
        if (type(a) == 'string') then
            sid = a;
            tex, size, uv0, uv1, bg, tint = ...;
        else
            local fp;
            tex = a;
            size, uv0, uv1, fp, bg, tint = ...;
            sid = '##xi_imagebutton' .. tostring(texid(tex));
        end
        local w, h = v2(size, 0, 0);
        local u0, v0 = v2(uv0, 0, 0);
        local u1, v1 = v2(uv1, 1, 1);
        local br, bg_, bb, ba = v4(bg, 0, 0, 0, 0);
        local tr, tg, tb, ta = v4(tint, 1, 1, 1, 1);
        return D.image_button(sid, texid(tex), w, h, u0, v0, u1, v1, br, bg_, bb, ba, tr, tg, tb, ta);
    end

    local function add_image(self, tex, pmin, pmax, uvmin, uvmax, c)
        local x0, y0 = v2(pmin, 0, 0);
        local x1, y1 = v2(pmax, 0, 0);
        local u0, v0 = v2(uvmin, 0, 0);
        local u1, v1 = v2(uvmax, 1, 1);
        D.dl_image(self.__list, texid(tex), x0, y0, x1, y1, u0, v0, u1, v1, col(c));
    end
    local function add_image_rounded(self, tex, pmin, pmax, uvmin, uvmax, c, rounding, flags)
        local x0, y0 = v2(pmin, 0, 0);
        local x1, y1 = v2(pmax, 0, 0);
        local u0, v0 = v2(uvmin, 0, 0);
        local u1, v1 = v2(uvmax, 1, 1);
        D.dl_image_rounded(self.__list, texid(tex), x0, y0, x1, y1, u0, v0, u1, v1, col(c), tonumber(rounding) or 0, tonumber(flags) or 0);
    end
    local function add_image_quad(self, tex, p1, p2, p3, p4, uv1, uv2, uv3, uv4, c)
        local ax, ay = v2(p1, 0, 0);
        local bx, by = v2(p2, 0, 0);
        local cx, cy = v2(p3, 0, 0);
        local dx, dy = v2(p4, 0, 0);
        local au, av = v2(uv1, 0, 0);
        local bu, bv = v2(uv2, 1, 0);
        local cu, cv = v2(uv3, 1, 1);
        local du, dv = v2(uv4, 0, 1);
        D.dl_image_quad(self.__list, texid(tex), ax, ay, bx, by, cx, cy, dx, dy, au, av, bu, bv, cu, cv, du, dv, col(c));
    end
    for _, name in ipairs({ 'GetWindowDrawList', 'GetForegroundDrawList', 'GetBackgroundDrawList' }) do
        local get = rawget(gui, name);
        if (get ~= nil) then
            gui[name] = function (...)
                local dl = get(...);
                if (type(dl) == 'table') then
                    dl.AddImage = add_image;
                    dl.AddImageRounded = add_image_rounded;
                    dl.AddImageQuad = add_image_quad;
                end
                return dl;
            end
        end
    end

    -- user32 / kernel32 through ffi.load (fancychat ...): the clipboard is the client's, the game window always counts
    -- as focused (it is this process's), and GlobalAlloc hands out plain buffers.
    local pid = 0x1000;
    local globals = {};
    local user32 = {
        OpenClipboard = function () return 1; end,
        EmptyClipboard = function () return 1; end,
        CloseClipboard = function () return 1; end,
        SetClipboardData = function (fmt, h)
            local g = globals[tostring(ffi.cast('uintptr_t', h))];
            if (g ~= nil) then xi.set_clipboard(ffi.string(g)); end
            return h;
        end,
        GetClipboardData = function () return nil; end,
        IsClipboardFormatAvailable = function () return 0; end,
        GetForegroundWindow = function () return ffi.cast('void*', 1); end,
        GetWindowThreadProcessId = function (hwnd, out) if (out ~= nil) then out[0] = pid; end return 1; end,
        GetKeyState = function () return 0; end,
        GetAsyncKeyState = function () return 0; end,
    };
    local kernel32 = {
        GetCurrentProcessId = function () return pid; end,
        GlobalAlloc = function (flags, n)
            local b = ffi.new('char[?]', math.max(1, tonumber(n) or 1));
            local h = ffi.cast('void*', b);
            globals[tostring(ffi.cast('uintptr_t', h))] = b;
            return h;
        end,
        GlobalLock = function (h) return h; end,
        GlobalUnlock = function () return 1; end,
        GlobalFree = function (h) globals[tostring(ffi.cast('uintptr_t', h))] = nil; return nil; end,
    };
    for _, lib in ipairs({ { 'user32', user32 }, { 'kernel32', kernel32 } }) do
        local t = setmetatable(lib[2], { __index = function (_, k) error(('%s.%s is not available in ffxi-linux'):format(lib[1], k), 2); end });
        __xi_ffi_load[lib[1]] = function () return t; end
        __xi_ffi_load[lib[1] .. '.dll'] = __xi_ffi_load[lib[1]];
    end
    register_ffi_c('GetKeyState', user32.GetKeyState);
    register_ffi_c('GetAsyncKeyState', user32.GetAsyncKeyState);
    register_ffi_c('GetForegroundWindow', user32.GetForegroundWindow);
    register_ffi_c('GetWindowThreadProcessId', user32.GetWindowThreadProcessId);
    register_ffi_c('GetCurrentProcessId', kernel32.GetCurrentProcessId);

    -- Released textures leave the GPU once ImGui's backend is done with them.
    local draw = __xi_draw;
    function __xi_draw()
        D.sweep();
        if (draw ~= nil) then
            draw();
        end
    end
end
