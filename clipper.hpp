#pragma once

#ifndef XCLIPP_CLIPPER_HPP
#define XCLIPP_CLIPPER_HPP

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>
#include <source_location>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

#include <xcb/xcb.h>
#include <xcb/xproto.h>

#include "utils.hpp"

namespace xcpp
{

class Clipper
{
public:
    explicit Clipper(std::string_view data, bool is_file = false);

    void Run();

private:
    template <class Reply, class Cookie, std::invocable<Reply*> Callback>
    auto Await(
        Cookie cookie,
        Reply*(*reply_getter)(xcb_connection_t*, Cookie, xcb_generic_error_t**),
        Callback&& callback,
        std::string_view msg = {},
        std::source_location loc = std::source_location::current()) -> std::remove_reference_t<std::invoke_result_t<Callback, Reply*>>;

    template <class Reply, class Cookie, std::invocable<Reply*> Callback, std::invocable<xcb_generic_error_t*> Handler>
    auto Await(
        Cookie cookie,
        Reply*(*reply_getter)(xcb_connection_t*, Cookie, xcb_generic_error_t**),
        Callback&& callback,
        Handler&& handler) ->
            std::conditional_t<
                std::is_same_v<void, std::invoke_result_t<Callback, Reply*>>,
                void,
                std::optional<std::remove_reference_t<std::invoke_result_t<Callback, Reply*>>>
            >;

    void Await(xcb_void_cookie_t cookie, std::string_view msg, std::source_location loc = std::source_location::current());

    template <std::invocable<xcb_generic_error_t*> Handler>
    bool Await(xcb_void_cookie_t cookie, Handler&& handler);

    bool SendFinishNotification(xcb_selection_request_event_t* req);

    void FinishRequestProcessing(xcb_selection_request_event_t* req, bool send_notification = true);

    void StartRequestProcessing(xcb_selection_request_event_t* req);

    std::optional<bool> Transfer(xcb_selection_request_event_t* req);

    void RegisterHandlers(std::unordered_map<std::string_view, xcb_atom_t>& targets);

    using ConvertedData = std::tuple<xcb_atom_t, std::uint8_t, std::unique_ptr<char[]>, std::size_t>;
    using ConvertedDataView = std::tuple<xcb_atom_t, std::uint8_t, const char*, std::size_t>;

    template <class Convert>
    requires
        std::is_invocable_r_v<std::optional<ConvertedData>, Convert, xcb_selection_request_event_t*> ||
        std::is_invocable_r_v<ConvertedDataView, Convert, xcb_selection_request_event_t*>
    void ProceedRequest(xcb_selection_request_event_t* req, Convert&& convert);

    template <class Convert>
    requires std::is_invocable_r_v<ConvertedData, Convert, xcb_selection_request_event_t*>
    auto Cached(Convert&& convert) noexcept;

    struct TransferState
    {
        ConvertedDataView GetData() const noexcept
        {
            if (data.index() == 0)
            {
                auto& [type, format, ptr, size] = std::get<ConvertedData>(data);
                return ConvertedDataView{type, format, ptr.get(), size};
            }
            return std::get<ConvertedDataView>(data);
        }

        std::variant<ConvertedData, ConvertedDataView> data;
        std::size_t tranferred;

        enum : std::size_t { TRANSFER_PREINIT = std::numeric_limits<std::size_t>::max() };
    };

    struct Request
    {
        ~Request()
        {
            std::free(req);
        }

        xcb_selection_request_event_t* req;
        bool is_ready;
        std::optional<std::function<void(xcb_selection_request_event_t*)>> on_finish;
    };

    inline static constexpr std::string_view required_targets[] =
    {
        "TIMESTAMP",
        "TARGETS",
        "MULTIPLE"
    };

    inline static constexpr std::string_view text_targets[] =
    {
        "TEXT",
        "STRING",
        "UTF8_STRING",
        "C_STRING",
    };

    inline static constexpr std::string_view file_targets[] =
    {
        "FILE_NAME",
        "text/uri-list",
        "x-special/gnome-copied-files",
        "x-special/KDE-copied-files",
        "x-special/mate-copied-files",
        "x-special/nautilus-clipboard"
    };

    inline static const std::unordered_map<std::string_view, bool(*)(std::string_view)> text_validators =
    {
        {"STRING", is_icccm_string},
        {"UTF8_STRING", is_icccm_utf8_string},
    };

    std::string_view data_;
    std::unique_ptr<xcb_connection_t, void(*)(xcb_connection_t*)> connection_;
    xcb_window_t owner_;
    xcb_timestamp_t ownership_timestamp_;
    xcb_atom_t clipboard_atom_;
    xcb_atom_t atom_pair_atom_;
    xcb_atom_t incr_atom_;
    std::size_t max_transfer_size_;
    std::unordered_map<xcb_window_t, std::deque<Request>> req_queues_;
    std::unordered_map<std::pair<xcb_window_t, xcb_atom_t>, TransferState, PairHash> transfers_;
    std::unordered_map<xcb_atom_t, std::function<void(xcb_selection_request_event_t*)>> handlers_;
    std::unordered_map<xcb_atom_t, ConvertedData> cache_;
};

} // namespace xcpp

#endif // XCLIPP_CLIPPER_HPP

