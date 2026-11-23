/***************************************************************************
 *   Copyright (C) 2020 PCSX-Redux authors                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#pragma once

#include <uv.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "json.hpp"
#include "support/eventbus.h"
#include "support/list.h"
#include "support/network.h"
#include "support/slice.h"

namespace PCSX {

struct UrlData {
    std::string schema;
    std::string host;
    std::string port;
    std::string path;
    std::string query;
    std::string fragment;
    std::string userInfo;
};

struct RequestData {
    UrlData urlData;
    enum Method {
        HTTP_DELETE,
        HTTP_HTTP_GET,
        HTTP_HEAD,
        HTTP_POST,
        HTTP_PUT,
        HTTP_CONNECT,
        HTTP_OPTIONS,
        HTTP_TRACE,
        HTTP_COPY,
        HTTP_LOCK,
        HTTP_MKCOL,
        HTTP_MOVE,
        HTTP_PROPFIND,
        HTTP_PROPPATH,
        HTTP_SEARCH,
        HTTP_UNLOCK,
        HTTP_BIND,
        HTTP_REBIND,
        HTTP_UNBIND,
        HTTP_ACL,
        HTTP_REPORT,
        HTTP_MKACTIVITY,
        HTTP_CHECKOUT,
        HTTP_MERGE,
        HTTP_MSEARCH,
        HTTP_NOTIFY,
        HTTP_SUBSCRIBE,
        HTTP_UNSUBSCRIBE,
        HTTP_PATCH,
        HTTP_PURGE,
        HTTP_MKCALENDAR,
        HTTP_LINK,
        HTTP_UNLINK,
        HTTP_SOURCE,
    } method;
    std::multimap<std::string, std::string> headers;
    std::multimap<std::string, std::string> form;
    Slice body;
    // For multipart/form-data, the request body exactly as received, framing included (body then holds only the
    // part data). Left empty for any other request, whose body already is the raw request body.
    Slice rawBody;
};

class WebClient;
class WebServer;

class WebExecutor : public Intrusive::List<WebExecutor>::Node {
  public:
    virtual bool match(WebClient* client, const UrlData&) = 0;
    virtual bool execute(WebClient* client, RequestData&) = 0;
    std::multimap<std::string, std::optional<std::string>> parseQuery(std::string_view);
    void write200(WebClient* client, const nlohmann::json& j);
};

class WebClient : public Intrusive::List<WebClient>::Node {
  public:
    WebClient(WebServer* server, IO<File> connection, uv_loop_t* loop);
    typedef Intrusive::List<WebClient> ListType;
    void close();
    void write(Slice&& slice);
    template <size_t L>
    void write(const char (&str)[L]) {
        static_assert((L - 1) <= std::numeric_limits<uint32_t>::max());
        Slice slice;
        slice.borrow(str, L - 1);
        write(std::move(slice));
    }
    void write(std::string&& str);
    void write(const std::string& str);

  private:
    struct WebClientImpl;
    std::unique_ptr<WebClientImpl> m_impl;
    friend WebServer;
};

class WebServer : public Network::Server {
  public:
    WebServer();
    ~WebServer() { m_executors.destroyAll(); }

  protected:
    void onConnection(IO<File> connection) override;
    void onStopped() override;
    void onFailed() override;
    int configuredPort() const override;

  private:
    WebClient::ListType m_clients;
    EventBus::Listener m_listener;
    Intrusive::List<WebExecutor> m_executors;

    friend struct WebClient::WebClientImpl;
};

}  // namespace PCSX
