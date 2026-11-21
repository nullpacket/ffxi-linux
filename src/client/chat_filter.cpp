// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/chat_filter.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace xi::client
{

namespace
{
char lower(char c)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string lowered(std::string_view s)
{
    std::string out(s);
    std::ranges::transform(out, out.begin(), lower);
    return out;
}

std::string readCapped(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        return {};
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() > 0x23f0)
    {
        text.resize(0x23f0); // the game's read cap
    }
    if (text.starts_with("\xEF\xBB\xBF"))
    {
        text.erase(0, 3);
    }
    return text;
}
} // namespace

bool ChatFilter::filtered(uint8_t kind)
{
    return kind == 1 || kind == 26 || kind == 34 || kind == 35; // shout, yell, assist J / E
}

std::vector<ChatFilter::Entry> ChatFilter::parse(std::string_view text)
{
    std::vector<Entry> out;
    std::size_t        pos = 0;
    while (pos < text.size())
    {
        auto end = text.find_first_of("\r\n", pos);
        if (end == std::string_view::npos)
        {
            end = text.size();
        }
        std::string_view line = text.substr(pos, end - pos);
        pos                   = end + 1;
        Entry e;
        // Leading "(flags)": i 4, s 2, n 8 (CODE 0x101bd575..0x101bd5e8); "\(" starts a literal parenthesis.
        if (line.starts_with('('))
        {
            const auto close = line.find(')');
            if (close != std::string_view::npos && line.substr(1, close - 1).find_first_not_of("isn") == std::string_view::npos)
            {
                for (const char c : line.substr(1, close - 1))
                {
                    e.flags |= c == 'i' ? 4u : c == 's' ? 2u : 8u;
                }
                line.remove_prefix(close + 1);
            }
        }
        if (line.empty())
        {
            continue;
        }
        if (line.size() > 31)
        {
            line = line.substr(0, 31);
        }
        e.pattern = lowered(line);
        out.push_back(std::move(e));
    }
    return out;
}

bool ChatFilter::matchAt(std::string_view p, std::string_view s, std::size_t at, std::size_t* end)
{
    std::size_t i = 0;
    while (i < p.size())
    {
        if (at >= s.size())
        {
            return false;
        }
        const char c = s[at];
        if (p[i] == '\\' && i + 1 < p.size())
        {
            if (p[i + 1] != c)
            {
                return false;
            }
            i += 2;
        }
        else if (p[i] == '.')
        {
            ++i;
        }
        else if (p[i] == '[')
        {
            std::size_t j      = i + 1;
            const bool  negate = j < p.size() && p[j] == '^';
            j += negate ? 1 : 0;
            bool hit = false;
            while (j < p.size() && p[j] != ']')
            {
                char a = p[j];
                if (a == '\\' && j + 1 < p.size())
                {
                    a = p[++j];
                }
                if (j + 2 < p.size() && p[j + 1] == '-' && p[j + 2] != ']')
                {
                    hit = hit || (c >= a && c <= p[j + 2]);
                    j += 3;
                }
                else
                {
                    hit = hit || c == a;
                    ++j;
                }
            }
            if (hit == negate)
            {
                return false;
            }
            i = j + 1;
        }
        else
        {
            if (p[i] != c)
            {
                return false;
            }
            ++i;
        }
        ++at;
    }
    if (end)
    {
        *end = at;
    }
    return true;
}

bool ChatFilter::contains(const Entry& e, std::string_view s)
{
    for (std::size_t at = 0; at < s.size(); ++at)
    {
        if (matchAt(e.pattern, s, at, nullptr))
        {
            return true;
        }
    }
    return false;
}

std::size_t ChatFilter::load(const std::filesystem::path& dir)
{
    set(readCapped(dir / "prohibited.txt"), readCapped(dir / "allowed.txt"));
    setProfanity(readCapped(dir / "profanity.txt"));
    return size();
}

void ChatFilter::setProfanity(std::string_view words)
{
    auto w = parse(words);
    std::lock_guard lock(mutex_);
    profanity_ = std::move(w);
}

void ChatFilter::setProfanityOn(bool on)
{
    std::lock_guard lock(mutex_);
    profanityOn_ = on;
}

std::string ChatFilter::censor(std::string_view text) const
{
    std::lock_guard lock(mutex_);
    std::string     out(text);
    if (!profanityOn_ || profanity_.empty())
    {
        return out;
    }
    const std::string t = lowered(text);
    for (std::size_t at = 0; at < t.size(); ++at)
    {
        for (const auto& e : profanity_)
        {
            std::size_t end = 0;
            if (matchAt(e.pattern, t, at, &end) && end > at)
            {
                std::fill(out.begin() + static_cast<std::ptrdiff_t>(at), out.begin() + static_cast<std::ptrdiff_t>(end), '*');
                at = end - 1;
                break;
            }
        }
    }
    return out;
}

void ChatFilter::set(std::string_view prohibited, std::string_view allowed)
{
    auto p = parse(prohibited);
    auto a = parse(allowed);
    std::lock_guard lock(mutex_);
    prohibited_ = std::move(p);
    allowed_    = std::move(a);
}

std::size_t ChatFilter::size() const
{
    std::lock_guard lock(mutex_);
    return prohibited_.size();
}

void ChatFilter::setMode(int mode)
{
    std::lock_guard lock(mutex_);
    mode_ = mode;
}

int ChatFilter::mode() const
{
    std::lock_guard lock(mutex_);
    return mode_;
}

ChatFilter::Verdict ChatFilter::check(uint8_t kind, std::string_view from, std::string_view text) const
{
    std::lock_guard lock(mutex_);
    if ((mode_ != 1 && mode_ != 2) || prohibited_.empty() || !filtered(kind))
    {
        return Verdict::Show;
    }
    const std::string t = lowered(text);
    const std::string n = lowered(from);
    // Both modes run the same match (CODE 0x100da510 -> 0x101bca60(sender, normalised text) -> 0x101bcb30): `(n)`
    // entries against the sender's name (as a whole name, INFERRED), the others against the text; the mode only
    // picks what a match does.
    const auto matches = [&](const Entry& e) {
        if (e.flags & 8)
        {
            std::size_t end = 0;
            return matchAt(e.pattern, n, 0, &end) && end == n.size();
        }
        return contains(e, t);
    };
    if (std::ranges::any_of(allowed_, matches))
    {
        return Verdict::Show;
    }
    if (!std::ranges::any_of(prohibited_, matches))
    {
        return Verdict::Show;
    }
    return mode_ == 1 ? Verdict::Drop : Verdict::Deleted;
}

} // namespace xi::client
