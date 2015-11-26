#include "error.h"

namespace jimdb
{
    namespace error
    {

        const char* ErrorCode::nameOf[] =
        {
			"parse_error_handshake",
			"invalid_json_request",
			"missing_type_or_data_request",
			"type_or_data_wrong_type_request"
        };

		static_assert(sizeof(ErrorCode::nameOf) / sizeof(char*) == ErrorCode::ERROR_CODES_SIZE, "sizes do not match");
    }
}