#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string_view>
#include <utility>

#include <xcb/xcb.h>
#include <xcb/xproto.h>

#include "utils.hpp"

namespace xcpp
{

std::size_t PairHash::operator()(std::pair<xcb_window_t, xcb_atom_t> p) const noexcept
{
    std::uint64_t n = p.second;
    n = (n << 32) | p.first;
    return std::hash<std::uint64_t>{}(n);
}

template <class... Args>
void Logger::operator()(Args&&... args) const
{
    std::string_view file_name = loc_.file_name();
    file_name.remove_prefix(file_name.rfind('/') + 1);
    std::clog << file_name << ':' << loc_.line() << ": ";
    (std::clog << ... << std::forward<Args>(args));
    std::clog << '\n';
}

void ErrorLogger::operator()(xcb_generic_error_t* err) const
{
    Logger::operator()(error_string(err->error_code), ": ", msg_);
}

std::string_view error_string(std::uint8_t error_code) noexcept
{
    switch (error_code)
    {
        case XCB_REQUEST:        return "BadRequest";
        case XCB_VALUE:          return "BadValue";
        case XCB_WINDOW:         return "BadWindow";
        case XCB_PIXMAP:         return "BadPixmap";
        case XCB_ATOM:           return "BadAtom";
        case XCB_CURSOR:         return "BadCursor";
        case XCB_FONT:           return "BadFont";
        case XCB_MATCH:          return "BadMatch";
        case XCB_DRAWABLE:       return "BadDrawable";
        case XCB_ACCESS:         return "BadAccess";
        case XCB_ALLOC:          return "BadAlloc";
        case XCB_COLORMAP:       return "BadColormap";
        case XCB_G_CONTEXT:      return "BadGContext";
        case XCB_ID_CHOICE:      return "BadIdChoice";
        case XCB_NAME:           return "BadName";
        case XCB_LENGTH:         return "BadLength";
        case XCB_IMPLEMENTATION: return "BadImplementation";
        default:                 return "<unknown error>";
    }
}

bool is_icccm_string(std::string_view data) noexcept
{
    return std::ranges::all_of(
        data, [](unsigned char c) { return (0x20 <= c && c <= 0x7E) || 0xA0 <= c || c == '\n' || c == '\t'; });
}

bool is_icccm_utf8_string(std::string_view data) noexcept
{
    std::size_t i = 0;
    std::uint32_t value = 0;
    while (i < data.size())
    {
        unsigned char c = data[i];
        std::size_t n = 0;
        if ((c & 0b1000'0000) == 0)
        {
            if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7F)
            {
                return false;
            }
            n = 0;
            value = c;
        }
        else if ((c & 0b0100'0000) == 0)
        {
            return false;
        }
        else if ((c & 0b0010'0000) == 0)
        {
            n = 1;
            value = c & 0b0001'1111;
        }
        else if ((c & 0b0001'0000) == 0)
        {
            n = 2;
            value = c & 0b0000'1111;
        }
        else if ((c & 0b0000'1000) == 0)
        {
            n = 3;
            value = c & 0b0000'0111;
        }
        else
        {
            return false;
        }
        std::size_t trail_len = n;
        for (++i; n != 0 && i < data.size(); --n, ++i)
        {
            if ((data[i] & 0b1100'0000) != 0b1000'0000)
            {
                return false;
            }
            value = (value << 6) | (data[i] & 0b0011'1111);
        }
        if (n != 0)
        {
            return false;
        }
        // check for overlong encoding
        std::uint32_t min_value[] = {0, 0x0080, 0x0800, 010000};
        if (value < min_value[trail_len])
        {
            return false;
        }
        // check for surrogate halves and non-existent symbols
        if ((0xD800 <= value && value <= 0xD8FF) || value > 0x10FFFF)
        {
            return false;
        }
    }
    return true;
}

static bool has_to_be_encoded(char c) noexcept
{
    return
        ('A' <= c && c <= 'Z') ||
        ('a' <= c && c <= 'z') ||
        ('0' <= c && c <= '9') ||
        c == '/' || c == '.' || c == '_' || c == '-' || c == '~';
}

static void encode(std::string_view file_path, char* buf) noexcept
{
    for (unsigned char c : file_path)
    {
        if (has_to_be_encoded(c))
        {
            *(buf++) = c;
        }
        else
        {
            unsigned char hi = c >> 4;
            unsigned char lo = c & 0xF;
            *(buf++) = '%';
            *(buf++) = hi > 9 ? 'A' + hi - 10 : '0' + hi;
            *(buf++) = lo > 9 ? 'A' + lo - 10 : '0' + lo;
        }
    }
}

static std::size_t uri_len(std::string_view file_path) noexcept
{
    std::size_t as_is_char_cnt = std::ranges::count_if(file_path, has_to_be_encoded);
    return as_is_char_cnt + 3 * (file_path.size() - as_is_char_cnt);
}

std::pair<std::unique_ptr<char[]>, std::size_t> to_uri(std::string_view file_path)
{
    constexpr std::string_view prefix = "file://";
    std::size_t size = prefix.size() + uri_len(file_path) + 2;
    char* buf = new char[size];
    std::memcpy(buf, prefix.data(), prefix.size());
    encode(file_path, buf + prefix.size());
    buf[size - 2] = '\r';
    buf[size - 1] = '\n';
    return {std::unique_ptr<char[]>{buf}, size};
}

std::pair<std::unique_ptr<char[]>, std::size_t> to_file_manager_clipboard_format(std::string_view file_path)
{
    constexpr std::string_view prefix = "copy\nfile://";
    std::size_t size = prefix.size() + uri_len(file_path);
    char* buf = new char[size];
    std::memcpy(buf, prefix.data(), prefix.size());
    encode(file_path, buf + prefix.size());
    return {std::unique_ptr<char[]>{buf}, size};
}

} // namespace xcpp

