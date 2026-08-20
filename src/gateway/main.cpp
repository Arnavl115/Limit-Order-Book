#include "server.hpp"
#include "engine_host.hpp"
#include "core/order_book.hpp"
#include "core/fast_order_book.hpp"

#include <iostream>
#include <string>

static void printUsage() {
    std::cout << "Usage: gateway.exe [--port 9000] [--book fast|canon]\n"
              << "  --port <n>   TCP/WebSocket port (default 9000, also serves HTTP static)\n"
              << "  --book <t>   fast (arena) or canon (std::map) (default fast)\n";
}

int main(int argc, char* argv[]) {
    uint16_t port = 9000;
    bool useFast = true;
    for (int i=1;i<argc;++i) {
        std::string a = argv[i];
        if (a=="--help" || a=="-h") { printUsage(); return 0; }
        if (a=="--port" && i+1<argc) { port = static_cast<uint16_t>(std::stoi(argv[++i])); }
        else if (a=="--book" && i+1<argc) { std::string v=argv[++i]; useFast = (v=="fast"); }
        else { std::cerr << "unknown arg: " << a << "\n"; printUsage(); return 1; }
    }

    std::cout << "Gateway starting port " << port << " book " << (useFast?"fast":"canon") << "\n";
    if (useFast) {
        lob::FastOrderBook book(1, 100000);
        book.reserveOrders(16384);
        gateway::Server srv;
        gateway::EngineHost<lob::FastOrderBook> host(book, srv);
        srv.setHandler([&](const std::string& j,int id){ return host.handleMessage(j,id); });
        srv.setConnectHandler([&](int id){ host.sendSnapshot(id); });
        if (!srv.start(port)) {
            std::cerr << "Failed to start server on " << port << "\n";
            return 1;
        }
        std::cout << "Listening on 127.0.0.1:" << srv.port() << " (TCP length-prefix + WS /ws + HTTP /)\n";
        std::cout << "Press Enter to stop...\n";
        std::string line;
        std::getline(std::cin, line);
        srv.stop();
        std::cout << "Stopped\n";
    } else {
        lob::OrderBook book;
        gateway::Server srv;
        gateway::EngineHost<lob::OrderBook> host(book, srv);
        srv.setHandler([&](const std::string& j,int id){ return host.handleMessage(j,id); });
        srv.setConnectHandler([&](int id){ host.sendSnapshot(id); });
        if (!srv.start(port)) {
            std::cerr << "Failed to start server on " << port << "\n";
            return 1;
        }
        std::cout << "Listening on 127.0.0.1:" << srv.port() << "\n";
        std::cout << "Press Enter to stop...\n";
        std::string line;
        std::getline(std::cin, line);
        srv.stop();
        std::cout << "Stopped\n";
    }
    return 0;
}
