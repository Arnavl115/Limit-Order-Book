#include "server.hpp"
#include "engine_host.hpp"
#include "core/order_book.hpp"
#include "core/fast_order_book.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

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
    auto run = [&](auto& book){
        gateway::Server srv;
        gateway::EngineHost<std::decay_t<decltype(book)>> host(book, srv);
        srv.setHandler([&](const std::string& j,int id){ return host.handleMessage(j,id); });
        if (!srv.start(port)) {
            std::cerr << "Failed to start server on " << port << "\n";
            return 1;
        }
        std::cout << "Listening on 127.0.0.1:" << srv.port() << " (TCP length-prefix + WS /ws + HTTP /)\n";
        std::cout << "Press Enter to stop...\n";
        std::string line;
        if (!std::getline(std::cin, line)) {
            // stdin closed (e.g., service) — keep running until killed
            std::cout << "stdin closed, running until terminated\n";
            while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        srv.stop();
        std::cout << "Stopped\n";
        return 0;
    };
    if (useFast) {
        lob::FastOrderBook book(1, 100000);
        book.reserveOrders(16384);
        return run(book);
    } else {
        lob::OrderBook book;
        return run(book);
    }
    return 0;
}
