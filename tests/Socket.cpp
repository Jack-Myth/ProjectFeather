#include "../src/Cli/Socket.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

void Check(bool Condition, const char* Message) {
    if (!Condition) throw std::runtime_error(Message);
}

} // namespace

int main() {
    try {
        using namespace Feather::Cli;
        auto Listener = TcpListener::Listen({"127.0.0.1", 0});
        auto Port = Listener.GetPort();
        std::exception_ptr ServerFailure;
        std::jthread Server([&] {
            try {
                auto Connection = Listener.Accept();
                auto First = Connection.ReceiveMessage(1024);
                auto Second = Connection.ReceiveMessage(1024);
                Check(First && *First == "first", "first framed message mismatch");
                Check(Second && *Second == "second\nmessage", "second framed message mismatch");
                Connection.SendMessage("reply");
            } catch (...) { ServerFailure = std::current_exception(); }
        });
        auto Client = TcpConnection::Connect({"127.0.0.1", Port});
        Client.SendMessage("first");
        Client.SendMessage("second\nmessage");
        auto Reply = Client.ReceiveMessage(1024);
        Check(Reply && *Reply == "reply", "reply framed message mismatch");
        Client.Close();
        Server.join();
        if (ServerFailure) std::rethrow_exception(ServerFailure);
        auto Parsed = ParseTcpEndpoint("127.0.0.1:4711");
        Check(Parsed.Host == "127.0.0.1" && Parsed.Port == 4711, "endpoint parse mismatch");
        std::cout << "socket transport tests passed\n";
        return 0;
    } catch (const std::exception& Failure) {
        std::cerr << "socket transport test failure: " << Failure.what() << '\n';
        return 1;
    }
}
