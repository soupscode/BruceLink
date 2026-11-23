#include "command_dispatcher.h"
namespace brucelink {
Response dispatch(const Request& request) {
    if(request.length > kMaxPayload) return {Command::Error, "payload too large"};
    if(request.command == Command::Ping && request.length == 0)
        return {Command::Pong, "ok"};
    return {Command::Error, "unsupported command"};
}
}
