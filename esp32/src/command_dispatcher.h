#pragma once
#include <stddef.h>
#include <stdint.h>
namespace brucelink {
constexpr uint8_t kVersion = 1;
constexpr size_t kMaxPayload = 512;
enum class Command : uint8_t {
    Ping = 0x01, Pong = 0x02, StatusRequest = 0x03,
    StatusResponse = 0x04, WifiScanRequest = 0x10,
    WifiScanResult = 0x11, Error = 0x7F
};
struct Request { Command command; const uint8_t* payload; size_t length; };
struct Response { Command command; const char* message; };
Response dispatch(const Request& request);
}
