/*
    Copyright (C) 2014-2016 Leosac

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

#pragma once

#include "api/CRUDResourceHandler.hpp"
#include "core/auth/AuthFwd.hpp"

namespace Leosac
{
namespace Module
{
namespace WebSockAPI
{
/**
 * CRUD Handler for AccessPoint.
 *
 *
 * @note Module specific access-point attributes are not handled
 * by this class. It cannot work similarly to the credential CRUD handler:
 * Credential CRUD handler is compile-time aware of all credential type,
 * while this module cannot be aware of all type of access-point because
 * they can be plugged in as modules.
 */
class AccessPointCRUD : public CRUDResourceHandler
{
  private:
    AccessPointCRUD(RequestContext ctx);

  public:
    static CRUDResourceHandlerUPtr instanciate(RequestContext);

  private:
    virtual std::vector<ActionActionParam>
    required_permission(Verb verb, const json &req) const override;

    virtual boost::optional<json> create_impl(const json &req) override;

    virtual boost::optional<json> read_impl(const json &req) override;

    virtual boost::optional<json> update_impl(const json &req) override;

    virtual boost::optional<json> delete_impl(const json &req) override;
};
}
}
}
