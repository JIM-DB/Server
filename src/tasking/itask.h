#pragma once
#define ASIO_STANDALONE
#define ASIO_HAS_STD_CHRONO
#define ASIO_HAS_STD_ARRAY
#define ASIO_HAS_STD_TYPE_TRAITS
#define ASIO_HAS_CSTDINT
#define ASIO_HAS_STD_SHARED_PTR
#define ASIO_HAS_STD_ADDRESSOF
#include <asio.hpp>
#include "../network/asiohandle.h"

namespace jimdb
{
    namespace tasking
    {
        class ITask
        {
        public:
            explicit ITask(std::shared_ptr<network::AsioHandle> sock) : m_socket(sock) {};
            virtual ~ITask() { };

            virtual bool continuous()
            {
                return false;
            };

            virtual void operator()() = 0;
        protected:
            std::shared_ptr<network::AsioHandle> m_socket;
        };
    }
}