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
#include "itask.h"
#include "../network/iclient.h"

namespace jimdb
{
    namespace tasking
    {
        class DeleteTask : public ITask
        {
        public:
            /**
            \brief task to delete an object

            This task delets an object. If the oid is invalid like it is no int or
			even missing, it does disconnect the client.
            @author Benjamin Meyer
            @date 15.12.2015 09:26
            */
            DeleteTask(const std::shared_ptr<network::AsioHandle>& sock, const std::shared_ptr<network::Message>& message);

            void operator()() override;

        private:
            std::shared_ptr<network::Message> m_msg;
        };
    }
}