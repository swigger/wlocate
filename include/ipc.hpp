#pragma once
#include "index.hpp"

namespace wl {
inline constexpr wchar_t pipe_name[] = L"\\\\.\\pipe\\WLocate.Search.v1";
Reply request_service(const Request& request, HANDLE cancel = nullptr);
void serve(Engine& engine, HANDLE stop_event, const std::function<void()>& rebuild);
}
