#pragma once

#ifndef XCLIPP_UTILS_HPP
#define XCLIPP_UTILS_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

#include <xcb/xcb.h>
#include <xcb/xproto.h>

namespace xcpp
{

struct PairHash
{
    std::size_t operator()(std::pair<xcb_window_t, xcb_atom_t> p) const noexcept;
};

class Logger
{
public:
    explicit Logger(std::source_location loc = std::source_location::current()) noexcept : loc_{std::move(loc)}
    {
    }

    template <class... Args>
    void operator()(Args&&... args) const;

private:
    std::source_location loc_;
};

class ErrorLogger : public Logger
{
public:
    explicit ErrorLogger(std::string_view msg, std::source_location loc = std::source_location::current()) noexcept :
        Logger{loc},
        msg_{msg}
    {
    }

    void operator()(xcb_generic_error_t* err) const;

private:
    std::string_view msg_;
};

std::string_view error_string(std::uint8_t error_code) noexcept;

// non-control ISO Latin-1 characters or \n, \t
bool is_icccm_string(std::string_view data) noexcept;

// non-control UTF-8 characters or \n, \t
bool is_icccm_utf8_string(std::string_view data) noexcept;

std::pair<std::unique_ptr<char[]>, std::size_t> to_uri(std::string_view file_path);

std::pair<std::unique_ptr<char[]>, std::size_t> to_file_manager_clipboard_format(std::string_view file_path);

} // namespace xcpp

#endif // XCLIPP_UTILS_HPP

