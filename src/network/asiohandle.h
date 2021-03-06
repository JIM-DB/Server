﻿// /**
// ############################################################################
// # GPL License                                                              #
// #                                                                          #
// # This file is part of the JIM-DB.                                         #
// # Copyright (c) 2015, Benjamin Meyer, <benjamin.meyer@tu-clausthal.de>     #
// # This program is free software: you can redistribute it and/or modify     #
// # it under the terms of the GNU General Public License as                  #
// # published by the Free Software Foundation, either version 3 of the       #
// # License, or (at your option) any later version.                          #
// #                                                                          #
// # This program is distributed in the hope that it will be useful,          #
// # but WITHOUT ANY WARRANTY; without even the implied warranty of           #
// # MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the            #
// # GNU General Public License for more details.                             #
// #                                                                          #
// # You should have received a copy of the GNU General Public License        #
// # along with this program. If not, see <http://www.gnu.org/licenses/>.     #
// ############################################################################
// **/
#pragma once
#define ASIO_STANDALONE
#define ASIO_HAS_STD_CHRONO
#define ASIO_HAS_STD_ARRAY
#define ASIO_HAS_STD_TYPE_TRAITS
#define ASIO_HAS_CSTDINT
#define ASIO_HAS_STD_SHARED_PTR
#define ASIO_HAS_STD_ADDRESSOF
#include <asio.hpp>
namespace jimdb
{
    namespace network
    {
        class AsioHandle : public asio::ip::tcp::socket
        {
        public:
            explicit AsioHandle(asio::io_service& io_service);
            AsioHandle(asio::io_service& io_service, const protocol_type& protocol);
            AsioHandle(asio::io_service& io_service, const endpoint_type& endpoint);
            AsioHandle(asio::io_service& io_service, const protocol_type& protocol, const native_handle_type& native_socket);

            void operator<<(std::shared_ptr<std::string> s);
            uint64_t ID() const;
        private:
            uint64_t m_id;
            static uint64_t s_counter;
        };
    }
}
