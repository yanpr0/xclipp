#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

#include <xcb/bigreq.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

#include "clipper.hpp"
#include "utils.hpp"

namespace xcpp
{

Clipper::Clipper(std::string_view data, bool is_file) :
    data_{data},
    connection_{nullptr, xcb_disconnect}
{
    int screen_id = 0;
    connection_.reset(xcb_connect(nullptr, &screen_id));

    if (int err = xcb_connection_has_error(connection_.get()))
    {
        throw std::runtime_error("Failed to connect to X server, XCB_CONN_* error code " + std::to_string(err));
    }

    xcb_prefetch_extension_data(connection_.get(), &xcb_big_requests_id);
    xcb_prefetch_maximum_request_length(connection_.get());

    xcb_screen_t *screen = nullptr;
    for (auto it = xcb_setup_roots_iterator(xcb_get_setup(connection_.get())); it.rem > 0; xcb_screen_next(&it))
    {
        if (screen_id == 0)
        {
            screen = it.data;
            break;
        }
        --screen_id;
    }
    if (screen == nullptr)
    {
        throw std::runtime_error("Failed to get default screen");
    }

    // create window
    owner_ = xcb_generate_id(connection_.get());
    xcb_event_mask_t event_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    auto window_cookie = xcb_create_window_checked(
        connection_.get(),
        0,
        owner_,
        screen->root,
        0, 0, 1, 1, 0,
        XCB_WINDOW_CLASS_INPUT_ONLY,
        XCB_COPY_FROM_PARENT,
        XCB_CW_EVENT_MASK, &event_mask);

    // get required and supported atoms
    std::unordered_map<std::string_view, xcb_intern_atom_cookie_t> target_cookies;
    for (auto t : required_targets)
    {
        target_cookies[t] = xcb_intern_atom(connection_.get(), 0, t.size(), t.data());
    }
    for (auto t : text_targets)
    {
        if (auto f = text_validators.find(t); f == text_validators.end() || f->second(data))
        {
            target_cookies[t] = xcb_intern_atom(connection_.get(), 0, t.size(), t.data());
        }
    }
    if (is_file)
    {
        for (auto t : file_targets)
        {
            target_cookies[t] = xcb_intern_atom(connection_.get(), 0, t.size(), t.data());
        }
    }
    xcb_intern_atom_cookie_t clipboard_cookie = xcb_intern_atom(connection_.get(), 0, 9, "CLIPBOARD");
    xcb_intern_atom_cookie_t atom_pair_cookie = xcb_intern_atom(connection_.get(), 0, 9, "ATOM_PAIR");
    xcb_intern_atom_cookie_t incr_cookie = xcb_intern_atom(connection_.get(), 0, 4, "INCR");

    // trigger event to get timestamp for selection acquiring
    auto time_cookie = xcb_change_property_checked(
        connection_.get(), XCB_PROP_MODE_REPLACE, owner_, XCB_ATOM_PRIMARY, XCB_ATOM_PRIMARY, 8, 0, nullptr);

    xcb_flush(connection_.get());

    Await(window_cookie, "Failed to create window");

    std::unordered_map<std::string_view, xcb_atom_t> targets;
    for (auto t : required_targets)
    {
        targets[t] = Await(
            target_cookies[t], xcb_intern_atom_reply, &xcb_intern_atom_reply_t::atom,
            std::format("Failed to get {} atom", t));
    }
    for (auto& [t, cookie] : target_cookies)
    {
        if (targets.contains(t))
        {
            continue;
        }
        auto atom = Await(
            cookie, xcb_intern_atom_reply, &xcb_intern_atom_reply_t::atom,
            ErrorLogger{std::format("Failed to get {} atom", t)});
        if (atom)
        {
            targets[t] = *atom;
        }
    }
    clipboard_atom_ = Await(
        clipboard_cookie, xcb_intern_atom_reply, &xcb_intern_atom_reply_t::atom, "Failed to get CLIPBOARD atom");
    atom_pair_atom_ = Await(
        atom_pair_cookie, xcb_intern_atom_reply, &xcb_intern_atom_reply_t::atom, "Failed to get ATOM_PAIR atom");
    incr_atom_ = Await(incr_cookie, xcb_intern_atom_reply, &xcb_intern_atom_reply_t::atom, "Failed to get INCR atom");

    Await(time_cookie, "Failed to get server timestamp by dummy property change");
    xcb_generic_event_t* time_event = nullptr;
    while ((time_event = xcb_wait_for_event(connection_.get())))
    {
        if (time_event->response_type == XCB_PROPERTY_NOTIFY &&
            reinterpret_cast<xcb_property_notify_event_t*>(time_event)->window == owner_)
        {
            break;
        }
        std::free(time_event);
    }
    if (time_event == nullptr)
    {
        throw std::runtime_error("Failed to get server timestamp by dummy property change, I/O error");
    }
    ownership_timestamp_ = reinterpret_cast<xcb_property_notify_event_t*>(time_event)->time;
    std::free(time_event);

    auto set_owner_cookie =
        xcb_set_selection_owner_checked(connection_.get(), owner_, clipboard_atom_, ownership_timestamp_);
    Await(set_owner_cookie, "Failed to acquire CLIPBOARD selection");

    // taking half of max request size (it's in 4-byte words)
    max_transfer_size_ = 2 * xcb_get_maximum_request_length(connection_.get());

    RegisterHandlers(targets);
}

void Clipper::Run()
{
    auto get_owner_cookie = xcb_get_selection_owner(connection_.get(), clipboard_atom_);
    auto curr_owner = Await(
        get_owner_cookie, xcb_get_selection_owner_reply, &xcb_get_selection_owner_reply_t::owner,
        "Failed to get owner of CLIPBOARD selection");
    if (owner_ != curr_owner)
    {
        return; // outraced by another client or lost ownership in a standard way
    }

    xcb_generic_event_t* event = nullptr;
    bool own = true;
    while ((own || !req_queues_.empty()) && (event = xcb_wait_for_event(connection_.get())))
    {
        switch (event->response_type & ~0x80)
        {
            // conversion request
            case XCB_SELECTION_REQUEST:
            {
                auto req = reinterpret_cast<xcb_selection_request_event_t*>(event);
                req_queues_[req->requestor].emplace_back(req, true);
                break;
            }
            // another client now owns the clipboard
            case XCB_SELECTION_CLEAR:
            {
                own = false;
                std::free(event);
                break;
            }
            // next transfer is available
            case XCB_PROPERTY_NOTIFY:
            {
                auto notify = reinterpret_cast<xcb_property_notify_event_t*>(event);
                auto req = req_queues_.find(notify->window);
                if (notify->state == XCB_PROPERTY_DELETE &&
                    req != req_queues_.end() &&
                    !req->second.empty() &&
                    req->second.front().req->property == notify->atom)
                {
                    req->second.front().is_ready = true;
                }
                std::free(event);
                break;
            }
            // connection broken
            case 0:
            {
                std::free(event);
                return;
            }
            default:
            {
                std::free(event);
                break;
            }
        }

        for (auto& [w, q] : req_queues_)
        {
            if (q.front().is_ready)
            {
                StartRequestProcessing(q.front().req);
            }
        }

        std::erase_if(req_queues_, [](auto& q) { return q.second.empty(); });
    }
}

template <class Reply, class Cookie, std::invocable<Reply*> Callback>
auto Clipper::Await(
    Cookie cookie,
    Reply*(*reply_getter)(xcb_connection_t*, Cookie, xcb_generic_error_t**),
    Callback&& callback,
    std::string_view err_msg,
    std::source_location loc) -> std::remove_reference_t<std::invoke_result_t<Callback, Reply*>>
{
    xcb_generic_error_t* err = nullptr;
    Reply* reply = reply_getter(connection_.get(), cookie, &err);
    if (err != nullptr)
    {
        using namespace std::literals;
        auto err_code = err->error_code;
        std::free(err);
        std::string_view file_name = loc.file_name();
        file_name.remove_prefix(file_name.rfind('/') + 1);
        std::string what = std::format("{}:{}: {}: {}", file_name, loc.line(), error_string(err_code), err_msg);
        throw std::runtime_error(std::move(what));
    }
    if constexpr (std::is_same_v<void, std::invoke_result_t<Callback, Reply*>>)
    {
        std::invoke(std::forward<Callback>(callback), reply);
        std::free(reply);
    }
    else
    {
        auto res = std::invoke(std::forward<Callback>(callback), reply);
        std::free(reply);
        return res;
    }
}

template <class Reply, class Cookie, std::invocable<Reply*> Callback, std::invocable<xcb_generic_error_t*> Handler>
auto Clipper::Await(
    Cookie cookie,
    Reply*(*reply_getter)(xcb_connection_t*, Cookie, xcb_generic_error_t**),
    Callback&& callback,
    Handler&& handler) ->
        std::conditional_t<
            std::is_same_v<void, std::invoke_result_t<Callback, Reply*>>,
            void,
            std::optional<std::remove_reference_t<std::invoke_result_t<Callback, Reply*>>>
        >
{
    xcb_generic_error_t* err = nullptr;
    Reply* reply = reply_getter(connection_.get(), cookie, &err);
    if (err != nullptr)
    {
        std::invoke(std::forward<Handler>(handler), err);
        std::free(err);
        if constexpr (std::is_same_v<void, std::invoke_result_t<Callback, Reply*>>) {
            return;
        }
        else
        {
            return {};
        }
    }
    if constexpr (std::is_same_v<void, std::invoke_result_t<Callback, Reply*>>)
    {
        std::invoke(std::forward<Callback>(callback), reply);
        std::free(reply);
    }
    else
    {
        auto res = std::invoke(std::forward<Callback>(callback), reply);
        std::free(reply);
        return res;
    }
}

void Clipper::Await(xcb_void_cookie_t cookie, std::string_view err_msg, std::source_location loc)
{
    if (xcb_generic_error_t* err = xcb_request_check(connection_.get(), cookie))
    {
        using namespace std::literals;
        auto err_code = err->error_code;
        std::free(err);
        std::string_view file_name = loc.file_name();
        file_name.remove_prefix(file_name.rfind('/') + 1);
        std::string what = std::format("{}:{}: {}: {}", file_name, loc.line(), error_string(err_code), err_msg);
        throw std::runtime_error(std::move(what));
    }
}

template <std::invocable<xcb_generic_error_t*> Handler>
bool Clipper::Await(xcb_void_cookie_t cookie, Handler&& handler)
{
    if (xcb_generic_error_t* err = xcb_request_check(connection_.get(), cookie))
    {
        std::invoke(std::forward<Handler>(handler), err);
        std::free(err);
        return false;
    }
    return true;
}

bool Clipper::SendFinishNotification(xcb_selection_request_event_t* req)
{
    xcb_selection_notify_event_t resp = {};
    resp.response_type = XCB_SELECTION_NOTIFY;
    resp.requestor = req->requestor;
    resp.selection = req->selection;
    resp.target = req->target;
    resp.time = req->time;
    resp.property = req->property;

    auto send_cookie = xcb_send_event_checked(
        connection_.get(), 1, req->requestor, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<char*>(&resp));

    return Await(send_cookie, ErrorLogger{"Failed to send finish notification"});
}

void Clipper::FinishRequestProcessing(xcb_selection_request_event_t* req, bool send_notification)
{
    auto requestor = req->requestor;
    if (auto& on_finish = req_queues_[requestor].front().on_finish)
    {
        (*on_finish)(req);
    }
    else if (send_notification)
    {
        SendFinishNotification(req);
    }
    req_queues_[requestor].pop_front();
}

void Clipper::StartRequestProcessing(xcb_selection_request_event_t* req)
{
    if (req->owner != owner_ ||
        (req->time < ownership_timestamp_ && req->time != XCB_CURRENT_TIME) ||
        req->selection != clipboard_atom_ ||
        !handlers_.contains(req->target))
    {
        req->property = XCB_ATOM_NONE;
        FinishRequestProcessing(req);
    }
    else
    {
        handlers_[req->target](req);
    }
}

std::optional<bool> Clipper::Transfer(xcb_selection_request_event_t* req)
{
    auto& transfer = transfers_[{req->requestor, req->property}];
    ConvertedDataView view = transfer.GetData();
    std::size_t transferred = transfer.tranferred;
    auto [type, format, data, size] = view;

    // transfer has not been started yet
    if (transferred == TransferState::TRANSFER_PREINIT)
    {
        // can transfer in one shot
        if (size <= max_transfer_size_)
        {
            auto change_prop_cookie = xcb_change_property_checked(
                connection_.get(),
                XCB_PROP_MODE_REPLACE,
                req->requestor,
                req->property,
                type, format, 8 * size / format, data);
            if (!Await(change_prop_cookie, ErrorLogger{"Failed to change property"}))
            {
                return {};
            }
            transfer.tranferred = size;
            return true;
        }

        // subscribe for notifications about requestor's properties
        xcb_event_mask_t event_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
        auto subscribe_for_prop_cookie =
            xcb_change_window_attributes(connection_.get(), req->requestor, XCB_CW_EVENT_MASK, &event_mask);

        std::uint32_t size_hint = std::min(size, static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));
        // initiate multistage transfer with INCR
        auto change_prop_cookie = xcb_change_property_checked(
            connection_.get(), XCB_PROP_MODE_REPLACE, req->requestor, req->property, incr_atom_, 32, 1, &size_hint);
        if (!Await(subscribe_for_prop_cookie, ErrorLogger{"Failed to subscribe for property changes"}) ||
            !Await(change_prop_cookie, ErrorLogger{"Failed to change property"}))
        {
            return {};
        }
        if (!SendFinishNotification(req))
        {
            return {};
        }
        transfer.tranferred = 0;
        return false;
    }

    // transfer the next chunk of data
    std::size_t chunk_size = std::min(max_transfer_size_, size - transferred);
    chunk_size -= (8 * chunk_size) % format;
    auto change_prop_cookie = xcb_change_property_checked(
        connection_.get(),
        XCB_PROP_MODE_REPLACE,
        req->requestor,
        req->property,
        type, format, 8 * chunk_size / format, data + transferred);
    if (!Await(change_prop_cookie, ErrorLogger{"Failed to change property"}))
    {
        return {};
    }
    transfer.tranferred += chunk_size;

    // more data yet to transfer (at least final 0-size transfer)
    if (transferred < size)
    {
        return false;
    }

    // unsubscribe from notifications about requestor's properties, transfer finished, don't need them any more
    xcb_event_mask_t event_mask = XCB_EVENT_MASK_NO_EVENT;
    auto unsubscribe_from_prop_cookie =
        xcb_change_window_attributes(connection_.get(), req->requestor, XCB_CW_EVENT_MASK, &event_mask);
    Await(unsubscribe_from_prop_cookie, ErrorLogger{"Failed to unsubscribe from property changes"});
    return true;
}

template <class Convert>
requires
    std::is_invocable_r_v<std::optional<Clipper::ConvertedData>, Convert, xcb_selection_request_event_t*> ||
    std::is_invocable_r_v<Clipper::ConvertedDataView, Convert, xcb_selection_request_event_t*>
void Clipper::ProceedRequest(xcb_selection_request_event_t* req, Convert&& convert)
{
    std::pair key = {req->requestor, req->property};
    auto transfer = transfers_.find(key);
    if (transfer == transfers_.end())
    {
        if constexpr (std::is_same_v<ConvertedDataView, std::invoke_result_t<Convert, xcb_selection_request_event_t*>>)
        {
            transfer = transfers_.emplace(
                key, TransferState{std::forward<Convert>(convert)(req), TransferState::TRANSFER_PREINIT}).first;
        }
        else
        {
            auto res = std::forward<Convert>(convert)(req);
            if (res)
            {
                transfer =
                    transfers_.emplace(key, TransferState{std::move(*res), TransferState::TRANSFER_PREINIT}).first;
            }
            else
            {
                req->property = XCB_ATOM_NONE;
                FinishRequestProcessing(req);
                return;
            }
        }
    }
    // in case MULTIPLE has put subrequests before itself
    if (req_queues_[req->requestor].front().req == req)
    {
        auto transfer_res = Transfer(req);
        if (!transfer_res) // fatal transfer error, discard request
        {
            req->property = XCB_ATOM_NONE;
            transfers_.erase(transfer);
            FinishRequestProcessing(req, false);
        }
        else if (*transfer_res) // transfer finished
        {
            bool send_notification = transfer->second.tranferred <= max_transfer_size_;
            transfers_.erase(transfer);
            FinishRequestProcessing(req, send_notification);
        }
        else // transfer partly finished, have to wait for notification to continue
        {
            req_queues_[req->requestor].front().is_ready = false;
        }
    }
}

template <class Convert>
requires std::is_invocable_r_v<Clipper::ConvertedData, Convert, xcb_selection_request_event_t*>
auto Clipper::Cached(Convert&& convert) noexcept
{
    return [this, convert = std::move(convert)](xcb_selection_request_event_t* req)
    {
        if (!cache_.contains(req->target))
        {
            cache_[req->target] = convert(req);
        }
        auto& [type, format, data, size] = cache_[req->target];

        return ConvertedDataView{type, format, data.get(), size};
    };
}

void Clipper::RegisterHandlers(std::unordered_map<std::string_view, xcb_atom_t>& targets)
{
    handlers_[targets["TIMESTAMP"]] = [this](xcb_selection_request_event_t* req)
    {
        req->property = req->property == XCB_ATOM_NONE ? req->target : req->property; // support obsolete clients
        auto convert = [this](xcb_selection_request_event_t*)
        {
            return ConvertedDataView{
                XCB_ATOM_INTEGER, 32, reinterpret_cast<char*>(&ownership_timestamp_), sizeof(ownership_timestamp_)};
        };
        ProceedRequest(req, convert);
    };

    handlers_[targets["TARGETS"]] = [this](xcb_selection_request_event_t* req)
    {
        req->property = req->property == XCB_ATOM_NONE ? req->target : req->property; // support obsolete clients
        auto convert = [this](xcb_selection_request_event_t*)
        {
            xcb_atom_t* targets = new xcb_atom_t[handlers_.size()];
            std::ranges::transform(handlers_, targets, [](auto& p) { return p.first; });
            std::sort(targets, targets + handlers_.size());
            return ConvertedData{
                XCB_ATOM_ATOM,
                32,
                std::unique_ptr<char[]>{reinterpret_cast<char*>(targets)},
                sizeof(xcb_atom_t) * handlers_.size()};
        };
        ProceedRequest(req, Cached(convert));
    };

    handlers_[targets["MULTIPLE"]] = [this](xcb_selection_request_event_t* req)
    {
        if (req->property == XCB_ATOM_NONE)
        {
            FinishRequestProcessing(req);
            return;
        }

        auto convert = [this](xcb_selection_request_event_t* req) -> std::optional<ConvertedData>
        {
            xcb_get_property_cookie_t prop_cookie =
                xcb_get_property(connection_.get(), 0, req->requestor, req->property, XCB_ATOM_ANY, 0, 0);

            auto subreqs_info = Await(
                prop_cookie,
                xcb_get_property_reply,
                [](xcb_get_property_reply_t* r) { return std::tuple{r->bytes_after, r->format, r->type}; },
                ErrorLogger{"Failed to get property value"});

            if (!subreqs_info)
            {
                return {};
            }

            auto [prop_size, format, type] = *subreqs_info;

            // subrequests must be in specific format
            if (format != 32 || type != atom_pair_atom_ || prop_size % (2 * sizeof(xcb_atom_t)) != 0)
            {
                return {};
            }

            prop_cookie = xcb_get_property(connection_.get(), 0, req->requestor, req->property, type, 0, prop_size / 4);

            auto cb = [this, req, n = prop_size / sizeof(xcb_atom_t)](xcb_get_property_reply_t* r)
            {
                // 0 subrequests
                if (n == 0)
                {
                    return std::pair{std::unique_ptr<char[]>{}, n};
                }
                auto data = std::unique_ptr<char[]>{reinterpret_cast<char*>(new xcb_atom_t[n])};
                std::memcpy(data.get(), xcb_get_property_value(r), n * sizeof(xcb_atom_t));
                xcb_atom_t* subreqs = reinterpret_cast<xcb_atom_t*>(data.get());
                auto init_subreq = [this, subreqs, req](int i, xcb_atom_t next_target, xcb_atom_t next_prop)
                {
                    if (subreqs[i + 1] == XCB_ATOM_NONE || // subrequest's property can't be None
                        (subreqs[i] == req->target &&
                            transfers_.contains({req->requestor, subreqs[i + 1]}))) // loop detection
                    {
                        subreqs[i + 1] = XCB_ATOM_NONE;
                    }
                    else
                    {
                        auto on_finish = [this, subreqs, i, next_target, next_prop](xcb_selection_request_event_t* req)
                        {
                            // put subrequest result into buffer
                            if (req->property == XCB_ATOM_NONE)
                            {
                                subreqs[i + 1] = XCB_ATOM_NONE;
                            }
                            // restore target and property for the next request in queue
                            req->target = next_target;
                            req->property = next_prop;
                        };
                        req_queues_[req->requestor].emplace_front(req, true, on_finish);
                    }
                };

                // put subrequests to the front of queue in reverse order
                init_subreq(n - 2, req->target, req->property);
                for (int i = static_cast<int>(n) - 4; i >= 0; i -= 2)
                {
                    init_subreq(i, subreqs[i + 2], subreqs[i + 3]);
                }
                req->target = subreqs[0];
                req->property = subreqs[1];
                return std::pair{std::move(data), n * sizeof(xcb_atom_t)};
            };

            auto res = Await(prop_cookie, xcb_get_property_reply, cb, ErrorLogger{"Failed to get property value"});
            if (!res)
            {
                return {};
            }
            auto [data, size] = std::move(*res);

            return ConvertedData{atom_pair_atom_, 32, std::move(data), size};
        };
        ProceedRequest(req, convert);
    };

    auto as_is_convert = [this](xcb_selection_request_event_t* req)
    {
        req->property = req->property == XCB_ATOM_NONE ? req->target : req->property; // support obsolete clients
        auto convert = [this](xcb_selection_request_event_t* req)
        {
            return ConvertedDataView{req->target, 8, data_.data(), data_.size()};
        };
        ProceedRequest(req, convert);
    };

    if (targets.contains("C_STRING"))
    {
        handlers_[targets["C_STRING"]] = as_is_convert;
    }
    if (targets.contains("STRING"))
    {
        handlers_[targets["STRING"]] = as_is_convert;
    }
    if (targets.contains("UTF8_STRING"))
    {
        handlers_[targets["UTF8_STRING"]] = as_is_convert;
    }

    xcb_atom_t text_mapping_atom = XCB_ATOM_NONE;
    if (targets.contains("UTF8_STRING"))
    {
        text_mapping_atom = targets["UTF8_STRING"];
    }
    else if (targets.contains("STRING"))
    {
        text_mapping_atom = targets["STRING"];
    }
    else if (targets.contains("C_STRING"))
    {
        text_mapping_atom = targets["C_STRING"];
    }
    if (text_mapping_atom != XCB_ATOM_NONE && targets.contains("TEXT"))
    {
        // TEXT type should be replaced with actual encoding
        handlers_[targets["TEXT"]] = [this, type = text_mapping_atom](xcb_selection_request_event_t* req)
        {
            req->property = req->property == XCB_ATOM_NONE ? req->target : req->property; // support obsolete clients
            auto convert = [this, type](xcb_selection_request_event_t*)
            {
                return ConvertedDataView{type, 8, data_.data(), data_.size()};
            };
            ProceedRequest(req, convert);
        };
    }

    if (targets.contains("FILE_NAME") && targets.contains("C_STRING"))
    {
        // file names are null-terminated strings
        handlers_[targets["FILE_NAME"]] = [this, type = targets["C_STRING"]](xcb_selection_request_event_t* req)
        {
            req->property = req->property == XCB_ATOM_NONE ? req->target : req->property; // support obsolete clients
            auto convert = [this, type](xcb_selection_request_event_t*)
            {
                return ConvertedDataView{type, 8, data_.data(), data_.size()};
            };
            ProceedRequest(req, convert);
        };
    }

    if (targets.contains("text/uri-list"))
    {
        handlers_[targets["text/uri-list"]] = [this](xcb_selection_request_event_t* req)
        {
            req->property = req->property == XCB_ATOM_NONE ? req->target : req->property; // support obsolete clients
            auto convert = [this](xcb_selection_request_event_t* req)
            {
                auto [data, size] = to_uri(data_);
                return ConvertedData{req->target, 8, std::move(data), size};
            };
            ProceedRequest(req, Cached(convert));
        };
    }

    auto file_convert = [this](xcb_selection_request_event_t* req)
    {
        req->property = req->property == XCB_ATOM_NONE ? req->target : req->property; // support obsolete clients
        auto convert = [this](xcb_selection_request_event_t *req)
        {
            auto [data, size] = to_file_manager_clipboard_format(data_);
            return ConvertedData{req->target, 8, std::move(data), size};
        };
        ProceedRequest(req, Cached(convert));
    };
    if (targets.contains("x-special/gnome-copied-files"))
    {
        handlers_[targets["x-special/gnome-copied-files"]] = file_convert;
    }
    if (targets.contains("x-special/KDE-copied-files"))
    {
        handlers_[targets["x-special/KDE-copied-files"]] = file_convert;
    }
    if (targets.contains("x-special/mate-copied-files"))
    {
        handlers_[targets["x-special/mate-copied-files"]] = file_convert;
    }
    if (targets.contains("x-special/nautilus-clipboard"))
    {
        handlers_[targets["x-special/nautilus-clipboard"]] = file_convert;
    }
}

} // namespace xcpp

