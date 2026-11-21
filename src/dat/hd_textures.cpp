// SPDX-License-Identifier: GPL-3.0-or-later
#include "dat/hd_textures.h"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wduplicated-branches"
#pragma GCC diagnostic ignored "-Wnull-dereference"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>
#pragma GCC diagnostic pop

namespace xi::dat::hd
{
namespace fs = std::filesystem;

std::string key(const Texture& tex)
{
    uint64_t h   = 1469598103934665603ULL;
    auto     mix = [&](uint8_t b) {
        h ^= b;
        h *= 1099511628211ULL;
    };
    for (const uint32_t v : { tex.width, tex.height, static_cast<uint32_t>(tex.format) })
    {
        for (int s = 0; s < 32; s += 8)
        {
            mix(static_cast<uint8_t>(v >> s));
        }
    }
    for (const auto b : tex.data)
    {
        mix(static_cast<uint8_t>(b));
    }
    std::string name;
    for (const char c : tex.name)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        name += ok ? c : '_';
    }
    while (!name.empty() && name.back() == '_')
    {
        name.pop_back();
    }
    return fmt::format("{}-{:016x}", name.empty() ? std::string("tex") : name, h);
}

fs::path defaultDir()
{
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x)
    {
        return fs::path(x) / "ffxi-linux" / "hd-textures";
    }
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / ".cache" / "ffxi-linux" / "hd-textures";
}

fs::path entryPath(const fs::path& dir, const std::string& k, const char* suffix)
{
    // Spread over 256 folders by the hash's last byte.
    return dir / "tex" / k.substr(k.size() - 2) / (k + suffix + ".png");
}

std::optional<Image> readPng(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }
    const std::vector<char> bytes{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    int                     w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &w, &h, &n, 4);
    if (!px)
    {
        return std::nullopt;
    }
    Image img{ static_cast<uint32_t>(w), static_cast<uint32_t>(h), {} };
    img.rgba.assign(px, px + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    stbi_image_free(px);
    return img;
}

std::optional<Image> load(const fs::path& dir, const std::string& k, const char* suffix)
{
    return readPng(entryPath(dir, k, suffix));
}

namespace
{
std::optional<fs::path> g_dir;
bool                    g_dump = false;
} // namespace

void setRuntimeDir(std::optional<fs::path> dir, bool dump)
{
    g_dir  = std::move(dir);
    g_dump = dump && g_dir.has_value();
}
const std::optional<fs::path>& runtimeDir()
{
    return g_dir;
}
bool dumpMissing()
{
    return g_dump;
}

bool isCutoutAlpha(const Image& img)
{
    int a = -1, b = -1;
    for (std::size_t i = 3; i < img.rgba.size(); i += 4)
    {
        const int v = img.rgba[i];
        if (v == a || v == b)
        {
            continue;
        }
        if (a < 0)
        {
            a = v;
        }
        else if (b < 0)
        {
            b = v;
        }
        else
        {
            return false;
        }
    }
    return true;
}

void resizeChannel(const Image& src, Image& dst, int channel, int threshold)
{
    // Two values for a cut-out: the snap targets.
    int lo = 255, hi = 0;
    if (threshold >= 0)
    {
        for (std::size_t i = static_cast<std::size_t>(channel); i < src.rgba.size(); i += 4)
        {
            lo = std::min<int>(lo, src.rgba[i]);
            hi = std::max<int>(hi, src.rgba[i]);
        }
    }
    const auto at = [&](uint32_t x, uint32_t y) {
        return static_cast<float>(src.rgba[(static_cast<std::size_t>(y) * src.width + x) * 4 + static_cast<std::size_t>(channel)]);
    };
    for (uint32_t y = 0; y < dst.height; ++y)
    {
        // Pixel centres, wrapping (the textures tile).
        const float sy = (static_cast<float>(y) + 0.5f) * static_cast<float>(src.height) / static_cast<float>(dst.height) - 0.5f;
        const float fy = std::floor(sy);
        const float ty = sy - fy;
        const auto  y0 = static_cast<uint32_t>((static_cast<int>(fy) % static_cast<int>(src.height) + static_cast<int>(src.height)) % static_cast<int>(src.height));
        const auto  y1 = (y0 + 1) % src.height;
        for (uint32_t x = 0; x < dst.width; ++x)
        {
            const float sx = (static_cast<float>(x) + 0.5f) * static_cast<float>(src.width) / static_cast<float>(dst.width) - 0.5f;
            const float fx = std::floor(sx);
            const float tx = sx - fx;
            const auto  x0 = static_cast<uint32_t>((static_cast<int>(fx) % static_cast<int>(src.width) + static_cast<int>(src.width)) % static_cast<int>(src.width));
            const auto  x1 = (x0 + 1) % src.width;
            float       v  = (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) + (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
            if (threshold >= 0)
            {
                v = v >= static_cast<float>(lo + hi) * 0.5f ? static_cast<float>(hi) : static_cast<float>(lo);
            }
            dst.rgba[(static_cast<std::size_t>(y) * dst.width + x) * 4 + static_cast<std::size_t>(channel)] = static_cast<uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
        }
    }
}

void matchColour(const Image& original, Image& up)
{
    for (std::size_t c = 0; c < 3; ++c)
    {
        auto stats = [c](const Image& img) {
            double s = 0, s2 = 0;
            const auto n = static_cast<double>(img.rgba.size() / 4);
            for (std::size_t i = c; i < img.rgba.size(); i += 4)
            {
                s += img.rgba[i];
                s2 += static_cast<double>(img.rgba[i]) * img.rgba[i];
            }
            const double m = s / n;
            return std::pair{ m, std::sqrt(std::max(0.0, s2 / n - m * m)) };
        };
        const auto [mo, so] = stats(original);
        const auto [mu, su] = stats(up);
        const double k      = su > 0.5 ? so / su : 1.0;
        for (std::size_t i = c; i < up.rgba.size(); i += 4)
        {
            up.rgba[i] = static_cast<uint8_t>(std::clamp((up.rgba[i] - mu) * k + mo + 0.5, 0.0, 255.0));
        }
    }
}

Image downscale(const Image& src, uint32_t f)
{
    if (f <= 1)
    {
        return src;
    }
    Image dst{ std::max(1u, src.width / f), std::max(1u, src.height / f), {} };
    dst.rgba.resize(static_cast<std::size_t>(dst.width) * dst.height * 4);
    for (uint32_t y = 0; y < dst.height; ++y)
    {
        for (uint32_t x = 0; x < dst.width; ++x)
        {
            for (std::size_t c = 0; c < 4; ++c)
            {
                uint32_t sum = 0, n = 0;
                for (uint32_t yy = y * f; yy < std::min(src.height, (y + 1) * f); ++yy)
                {
                    for (uint32_t xx = x * f; xx < std::min(src.width, (x + 1) * f); ++xx)
                    {
                        sum += src.rgba[(static_cast<std::size_t>(yy) * src.width + xx) * 4 + c];
                        ++n;
                    }
                }
                dst.rgba[(static_cast<std::size_t>(y) * dst.width + x) * 4 + c] = static_cast<uint8_t>(n ? sum / n : 0);
            }
        }
    }
    return dst;
}

namespace
{
float luma(const Image& img, int x, int y)
{
    const auto w = static_cast<int>(img.width), h = static_cast<int>(img.height);
    x            = (x % w + w) % w;
    y            = (y % h + h) % h;
    const auto* p = &img.rgba[(static_cast<std::size_t>(y) * img.width + static_cast<std::size_t>(x)) * 4];
    return (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / 255.0f;
}
} // namespace

Image normalMap(const Image& c, float strength)
{
    Image n{ c.width, c.height, std::vector<uint8_t>(c.rgba.size()) };
    for (int y = 0; y < static_cast<int>(c.height); ++y)
    {
        for (int x = 0; x < static_cast<int>(c.width); ++x)
        {
            const float dx = (luma(c, x + 1, y - 1) + 2 * luma(c, x + 1, y) + luma(c, x + 1, y + 1)) - (luma(c, x - 1, y - 1) + 2 * luma(c, x - 1, y) + luma(c, x - 1, y + 1));
            const float dy = (luma(c, x - 1, y + 1) + 2 * luma(c, x, y + 1) + luma(c, x + 1, y + 1)) - (luma(c, x - 1, y - 1) + 2 * luma(c, x, y - 1) + luma(c, x + 1, y - 1));
            float       vx = -dx * strength, vy = -dy * strength, vz = 1.0f;
            const float l  = std::sqrt(vx * vx + vy * vy + vz * vz);
            vx /= l;
            vy /= l;
            vz /= l;
            auto* p = &n.rgba[(static_cast<std::size_t>(y) * c.width + static_cast<std::size_t>(x)) * 4];
            p[0]    = static_cast<uint8_t>((vx * 0.5f + 0.5f) * 255.0f + 0.5f);
            p[1]    = static_cast<uint8_t>((vy * 0.5f + 0.5f) * 255.0f + 0.5f);
            p[2]    = static_cast<uint8_t>((vz * 0.5f + 0.5f) * 255.0f + 0.5f);
            p[3]    = 255;
        }
    }
    return n;
}

Image roughnessMap(const Image& c)
{
    // INFERRED heuristic: busy (high local contrast) texels read as rough, smooth bright areas a little glossier.
    Image r{ c.width, c.height, std::vector<uint8_t>(c.rgba.size()) };
    for (int y = 0; y < static_cast<int>(c.height); ++y)
    {
        for (int x = 0; x < static_cast<int>(c.width); ++x)
        {
            const float m    = luma(c, x, y);
            float       var  = 0;
            for (int j = -1; j <= 1; ++j)
            {
                for (int i = -1; i <= 1; ++i)
                {
                    const float d = luma(c, x + i, y + j) - m;
                    var += d * d;
                }
            }
            const float rough = std::clamp(0.85f + std::sqrt(var / 9.0f) * 2.0f - m * 0.3f, 0.3f, 1.0f);
            auto*       p     = &r.rgba[(static_cast<std::size_t>(y) * c.width + static_cast<std::size_t>(x)) * 4];
            p[0] = p[1] = p[2] = static_cast<uint8_t>(rough * 255.0f + 0.5f);
            p[3]               = 255;
        }
    }
    return r;
}

namespace
{
// Texels of wrapped border around each texture fed to the model (no more than the texture itself).
uint32_t padFor(const Image& img)
{
    return std::min({ 16u, img.width, img.height });
}

// Runs the upscaler over a folder, its console output dropped; true on exit status 0.
bool runUpscaler(const BuildOptions& opt, const fs::path& in, const fs::path& out)
{
    const std::string exe = opt.upscaler.string(), i = in.string(), o = out.string(), m = (opt.upscaler.parent_path() / "models").string();
    std::vector<std::string> args{ exe, "-i", i, "-o", o, "-n", opt.model, "-s", "4", "-f", "png", "-m", m };
    std::vector<char*>       argv;
    for (auto& a : args)
    {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid == 0)
    {
        if (const int devnull = open("/dev/null", O_WRONLY); devnull >= 0)
        {
            dup2(devnull, 1);
            dup2(devnull, 2);
        }
        execv(exe.c_str(), argv.data());
        _exit(127);
    }
    if (pid < 0)
    {
        return false;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
} // namespace

BuildStats build(const std::vector<std::pair<std::string, Image>>& textures, const BuildOptions& opt)
{
    BuildStats         st;
    const fs::path     work = opt.dir / "work";
    constexpr std::size_t kBatch = 256;
    std::vector<const std::pair<std::string, Image>*> todo;
    for (const auto& t : textures)
    {
        if (t.second.width < 8 || t.second.height < 8)
        {
            ++st.tiny; // flat fills and gradients: nothing to gain
        }
        else if (!opt.force && fs::exists(entryPath(opt.dir, t.first)))
        {
            ++st.skipped;
        }
        else
        {
            todo.push_back(&t);
        }
    }
    for (std::size_t b = 0; b < todo.size(); b += kBatch)
    {
        std::error_code ec;
        fs::remove_all(work, ec);
        fs::create_directories(work / "in");
        fs::create_directories(work / "out");
        std::vector<const std::pair<std::string, Image>*> batch(todo.begin() + static_cast<std::ptrdiff_t>(b),
                                                                 todo.begin() + static_cast<std::ptrdiff_t>(std::min(todo.size(), b + kBatch)));
        for (const auto* t : batch)
        {
            // Colour only (the alpha is resized here, not by the model), wrapped by kPad texels on every side: the
            // textures tile, and a model that sees an edge makes a seam there.
            const auto&    o = t->second;
            const uint32_t p = padFor(o);
            Image          rgb{ o.width + 2 * p, o.height + 2 * p, {} };
            rgb.rgba.resize(static_cast<std::size_t>(rgb.width) * rgb.height * 4);
            for (uint32_t y = 0; y < rgb.height; ++y)
            {
                for (uint32_t x = 0; x < rgb.width; ++x)
                {
                    const uint32_t sx = (x + o.width - p % o.width) % o.width, sy = (y + o.height - p % o.height) % o.height;
                    const auto*    s  = &o.rgba[(static_cast<std::size_t>(sy) * o.width + sx) * 4];
                    auto*          d  = &rgb.rgba[(static_cast<std::size_t>(y) * rgb.width + x) * 4];
                    d[0] = s[0];
                    d[1] = s[1];
                    d[2] = s[2];
                    d[3] = 255;
                }
            }
            writePng((work / "in" / (t->first + ".png")).string(), rgb.width, rgb.height, rgb.rgba);
        }
        if (!runUpscaler(opt, work / "in", work / "out"))
        {
            spdlog::error("hdtex: upscaler failed on batch {}", b / kBatch);
            st.failed += batch.size();
            continue;
        }
        for (const auto* t : batch)
        {
            const auto padded = readPng(work / "out" / (t->first + ".png"));
            const uint32_t p  = padFor(t->second);
            if (!padded || padded->width != (t->second.width + 2 * p) * 4 || padded->height != (t->second.height + 2 * p) * 4)
            {
                ++st.failed;
                continue;
            }
            auto up = std::optional<Image>(Image{ t->second.width * 4, t->second.height * 4, {} });
            up->rgba.resize(static_cast<std::size_t>(up->width) * up->height * 4);
            for (uint32_t y = 0; y < up->height; ++y)
            {
                std::copy_n(&padded->rgba[(static_cast<std::size_t>(y + 4 * p) * padded->width + 4 * p) * 4], static_cast<std::size_t>(up->width) * 4,
                            &up->rgba[static_cast<std::size_t>(y) * up->width * 4]);
            }
            matchColour(t->second, *up);
            const int cut = isCutoutAlpha(t->second) ? 1 : -1;
            resizeChannel(t->second, *up, 3, cut);
            Image out = downscale(*up, 4 / std::clamp(opt.scale, 1u, 4u));
            while (std::max(out.width, out.height) > opt.maxSize && std::max(out.width, out.height) / 2 >= std::max(t->second.width, t->second.height))
            {
                out = downscale(out, 2);
            }
            const auto path = entryPath(opt.dir, t->first);
            fs::create_directories(path.parent_path());
            writePng(path.string(), out.width, out.height, out.rgba);
            st.bytes += fs::file_size(path);
            if (opt.maps)
            {
                const auto n = normalMap(out, 2.0f), r = roughnessMap(out);
                writePng(entryPath(opt.dir, t->first, "_n").string(), n.width, n.height, n.rgba);
                writePng(entryPath(opt.dir, t->first, "_r").string(), r.width, r.height, r.rgba);
                st.bytes += fs::file_size(entryPath(opt.dir, t->first, "_n")) + fs::file_size(entryPath(opt.dir, t->first, "_r"));
            }
            ++st.built;
        }
        spdlog::info("hdtex: {} / {} built", std::min(todo.size(), b + kBatch), todo.size());
    }
    std::error_code ec;
    fs::remove_all(work, ec);
    return st;
}

} // namespace xi::dat::hd
