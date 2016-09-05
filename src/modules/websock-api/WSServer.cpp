/*
    Copyright (C) 2014-2016 Islog

    This file is part of Leosac.

    Leosac is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Leosac is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "WSServer.hpp"
#include "Exceptions.hpp"
#include "WSAPICall_odb.h"
#include "api/GroupGet.hpp"
#include "api/LogGet.hpp"
#include "api/MembershipGet.hpp"
#include "api/UserGet.hpp"
#include "core/audit/WSAPICall.hpp"
#include "exception/ExceptionsTools.hpp"
#include "tools/db/DBService.hpp"
#include "tools/db/MultiplexedTransaction.hpp"
#include "tools/log.hpp"
#include <json.hpp>
#include <odb/mysql/exceptions.hxx>
#include <odb/session.hxx>
#include <odb/sqlite/exceptions.hxx>

using namespace Leosac;
using namespace Leosac::Module;
using namespace Leosac::Module::WebSockAPI;

using json = nlohmann::json;

WSServer::WSServer(WebSockAPIModule &module, DBPtr database)
    : auth_(*this)
    , db_(database)
    , module_(module)
{
    ASSERT_LOG(db_, "No database object passed into WSServer.");
    using websocketpp::lib::placeholders::_1;
    using websocketpp::lib::placeholders::_2;
    srv_.init_asio();

    srv_.set_open_handler(std::bind(&WSServer::on_open, this, _1));
    srv_.set_close_handler(std::bind(&WSServer::on_close, this, _1));
    srv_.set_message_handler(std::bind(&WSServer::on_message, this, _1, _2));
    srv_.set_reuse_addr(true);
    // clear all logs.
    // srv_.clear_access_channels(websocketpp::log::alevel::all);

    handlers_["get_leosac_version"]      = &APISession::get_leosac_version;
    handlers_["create_auth_token"]       = &APISession::create_auth_token;
    handlers_["authenticate_with_token"] = &APISession::authenticate_with_token;
    handlers_["logout"]                  = &APISession::logout;
    handlers_["system_overview"]         = &APISession::system_overview;

    handlers2_["user_get"]       = &UserGet::create;
    handlers2_["get_logs"]       = &LogGet::create;
    handlers2_["group_get"]      = &GroupGet::create;
    handlers2_["membership_get"] = &MembershipGet::create;
}

void WSServer::on_open(websocketpp::connection_hdl hdl)
{
    INFO("New WebSocket connection !");
    connection_api_.insert(std::make_pair(hdl, std::make_shared<APISession>(*this)));
}

void WSServer::on_close(websocketpp::connection_hdl hdl)
{
    INFO("WebSocket connection closed.");
    connection_api_.erase(hdl);
}

void WSServer::on_message(websocketpp::connection_hdl hdl, Server::message_ptr msg)
{
    json req = json::parse(msg->get_payload());

    assert(connection_api_.find(hdl) != connection_api_.end());
    auto api_handle = connection_api_.find(hdl)->second;

    INFO("Incoming payload: \n" << req.dump(4));

    Audit::WSAPICall audit;
    ServerMessage response = handle_request(api_handle, req);

    audit.author_           = api_handle->current_user();
    audit.uuid_             = response.uuid;
    audit.api_method_       = response.type;
    audit.status_code_      = response.status_code;
    audit.status_string_    = response.status_string;
    audit.response_content_ = response.content.dump(4);

    {
        db::MultiplexedTransaction t;
        db_->persist(audit);
        t.commit();
    }

    send_message(hdl, response);
}

void WSServer::run(uint16_t port)
{
    srv_.listen(port);
    srv_.start_accept();
    srv_.run();
}

void WSServer::start_shutdown()
{
    srv_.stop_listening();
    for (auto con_api : connection_api_)
    {
        srv_.close(con_api.first, 0, "bye");
    }
}

APIAuth &WSServer::auth()
{
    return auth_;
}

json WSServer::dispatch_request(APIPtr api_handle, const ClientMessage &in)
{
    // A request is a "Unit-of-Work" for the application.
    // We create a default database session for the request.
    odb::session database_session;
    auto handler_factory = handlers2_.find(in.type);

    if (handler_factory != handlers2_.end())
    {
        RequestContext ctx{.session = api_handle,
                           .dbsrv   = std::make_shared<DBService>(db_),
                           .server  = *this};

        MethodHandlerUPtr method_handler = handler_factory->second(ctx);
        return method_handler->process(in);
    }

    auto handler_method = handlers_.find(in.type);

    if (handler_method == handlers_.end())
    {
        throw InvalidCall();
    }
    else
    {
        if (api_handle->allowed(in.type))
        {
            api_handle->hook_before_request();
            auto method_ptr = handler_method->second;
            return ((*api_handle).*method_ptr)(in.content);
        }
        else
        {
            throw PermissionDenied();
        }
    }
}

DBPtr WSServer::db()
{
    return db_;
}

CoreUtilsPtr WSServer::core_utils()
{
    return module_.core_utils();
}

void WSServer::send_message(websocketpp::connection_hdl hdl,
                            const ServerMessage &msg)
{
    json json_message;

    json_message["uuid"]          = msg.uuid;
    json_message["type"]          = msg.type;
    json_message["status_code"]   = static_cast<int64_t>(msg.status_code);
    json_message["status_string"] = msg.status_string;
    json_message["content"]       = msg.content;

    srv_.send(hdl, json_message.dump(4), websocketpp::frame::opcode::text);
}

ClientMessage WSServer::parse_request(const json &req)
{
    ClientMessage msg;

    try
    {
        // Extract general message argument.
        msg.uuid    = req.at("uuid");
        msg.type    = req.at("type");
        msg.content = req.at("content");
    }
    catch (const std::out_of_range &e)
    {
        throw MalformedMessage();
    }
    catch (const std::domain_error &e)
    {
        throw MalformedMessage();
    }
    return msg;
}

ServerMessage WSServer::handle_request(APIPtr api_handle, const json &req)
{
    ServerMessage response;
    response.status_code = APIStatusCode::SUCCESS;
    response.content     = {};
    try
    {
        ClientMessage input = parse_request(req);
        response.uuid       = input.uuid;
        response.type       = input.type;
        response.content    = dispatch_request(api_handle, input);
    }
    catch (const InvalidCall &e)
    {
        response.status_code   = APIStatusCode::INVALID_CALL;
        response.status_string = e.what();
    }
    catch (const PermissionDenied &e)
    {
        response.status_code   = APIStatusCode::PERMISSION_DENIED;
        response.status_string = e.what();
    }
    catch (const MalformedMessage &e)
    {
        response.status_code   = APIStatusCode::MALFORMED;
        response.status_string = e.what();
    }
    catch (const SessionAborted &e)
    {
        response.status_code   = APIStatusCode::SESSION_ABORTED;
        response.status_string = e.what();
    }
    catch (const EntityNotFound &e)
    {
        response.status_code            = APIStatusCode::ENTITY_NOT_FOUND;
        response.status_string          = e.what();
        response.content["entity_id"]   = e.entity_id();
        response.content["entity_type"] = e.entity_type();
    }
    catch (const LEOSACException &e)
    {
        WARN("Leosac specific exception has been caught: " << e.what() << std::endl
                                                           << e.trace().str());
        response.status_code   = APIStatusCode::GENERAL_FAILURE;
        response.status_string = e.what(); // todo Maybe remove in production.
    }
    catch (const odb::exception &e)
    {
        ERROR("Database Error: " << e.what());
        response.status_code   = APIStatusCode::GENERAL_FAILURE;
        response.status_string = "Database Error: " + std::string(e.what());
    }
    catch (const std::exception &e)
    {
        WARN("Exception when processing request: " << e.what());
        response.status_code   = APIStatusCode::GENERAL_FAILURE;
        response.status_string = e.what();
    }
    return response;
}