#pragma once
#include <memory>
#include <string>
namespace bilibili {
class Server;
std::shared_ptr<Server> startServer();
std::wstring panelUrl(const Server&);
void stopServer(Server&);
}
