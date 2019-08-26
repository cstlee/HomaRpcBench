#include <functional>
#include <iostream>
#include <map>

#include <signal.h>

#include <Homa/Debug.h>
#include <Homa/Drivers/DPDK/DpdkDriver.h>
#include <Homa/Homa.h>
#include <docopt.h>

#include "WireFormat.h"

static const char USAGE[] = R"(HomaRpcBench Server.

    Usage:
        server [options] [-v | -vv | -vvv | -vvvv] <port> <coordinator_address>

    Options:
        -h --help       Show this screen.
        --version       Show version.
        -v --verbose    Show verbose output.
)";

namespace HomaRpcBench {

/**
 * Implements the server-side benchmark functionality.
 */
class Server {
  public:
    explicit Server(Homa::Transport* transport);

    void poll();

  private:
    void dispatch(Homa::ServerOp* op);
    void handleConfigServerRpc(Homa::ServerOp* op);
    void handleEchoRpc(Homa::ServerOp* op);
    void handleEchoMultiLevelRpc(Homa::ServerOp* op);

    Homa::Transport* transport;
    bool proxy;
    Homa::Driver::Address delegate;
    char buffer[1024 * 1024];
};

Server::Server(Homa::Transport* transport)
    : transport(transport)
    , proxy(false)
    , delegate()
{}

void
Server::poll()
{
    Homa::ServerOp op = transport->receiveServerOp();
    if (op) {
        dispatch(&op);
    }
    transport->poll();
}

void
Server::dispatch(Homa::ServerOp* op)
{
    WireFormat::Common common;
    op->request->get(0, &common, sizeof(common));

    switch (common.opcode) {
        case WireFormat::ConfigServerRpc::opcode:
            handleConfigServerRpc(op);
            break;
        case WireFormat::EchoRpc::opcode:
            handleEchoRpc(op);
            break;
        case WireFormat::EchoMultiLevelRpc::opcode:
            handleEchoMultiLevelRpc(op);
            break;
        default:
            std::cerr << "Unknown opcode" << std::endl;
    }
}

void
Server::handleConfigServerRpc(Homa::ServerOp* op)
{
    WireFormat::ConfigServerRpc::Request request;
    WireFormat::ConfigServerRpc::Response response;
    op->request->get(0, &request, sizeof(request));
    proxy = request.forward;
    if (proxy) {
        delegate = transport->driver->getAddress(&request.nextAddress);
    }

    response.common.opcode = WireFormat::ConfigServerRpc::opcode;
    op->response->append(&response, sizeof(response));
    op->reply();
    if (proxy) {
        std::cout << "Server configured as proxy to "
                  << transport->driver->addressToString(delegate) << std::endl;
    } else {
        std::cout << "Server configured" << std::endl;
    }
}

void
Server::handleEchoRpc(Homa::ServerOp* op)
{
    WireFormat::EchoRpc::Request request;
    WireFormat::EchoRpc::Response response;
    op->request->get(0, &request, sizeof(request));
    op->request->get(sizeof(request), &buffer, request.sentBytes);

    response.common.opcode = WireFormat::EchoRpc::opcode;
    response.hopCount = 1;
    response.responseBytes = request.responseBytes;

    if (proxy) {
        Homa::RemoteOp proxyOp(transport);
        proxyOp.request->append(&request, sizeof(request));
        proxyOp.request->append(&buffer, request.sentBytes);

        proxyOp.send(delegate);
        proxyOp.wait();

        WireFormat::EchoRpc::Response proxyResponse;
        proxyOp.response->get(0, &proxyResponse, sizeof(proxyResponse));
        proxyOp.response->get(sizeof(proxyResponse), &buffer,
                              proxyResponse.responseBytes);
        if (proxyResponse.responseBytes != request.responseBytes) {
            std::cerr << "Expected " << request.responseBytes
                      << " bytes but only got " << proxyResponse.responseBytes
                      << " bytes." << std::endl;
        }
        response.hopCount += proxyResponse.hopCount;
    }

    op->response->append(&response, sizeof(response));
    op->response->append(&buffer, response.responseBytes);
    op->reply();
}

void
Server::handleEchoMultiLevelRpc(Homa::ServerOp* op)
{
    WireFormat::EchoMultiLevelRpc::Request request;
    op->request->get(0, &request, sizeof(request));
    op->request->get(sizeof(request), &buffer, request.sentBytes);

    if (proxy) {
        op->response->append(&request, sizeof(request));
        op->response->append(&buffer, request.sentBytes);
        op->delegate(delegate);
    } else {
        WireFormat::EchoMultiLevelRpc::Response response;
        response.common.opcode = WireFormat::EchoMultiLevelRpc::opcode;
        response.responseBytes = request.responseBytes;
        op->response->append(&response, sizeof(response));
        op->response->append(&buffer, response.responseBytes);
        op->reply();
    }
}

}  // namespace HomaRpcBench

volatile sig_atomic_t INTERRUPT_FLAG = 0;
void
sig_int_handler(int sig)
{
    INTERRUPT_FLAG = 1;
}

int
main(int argc, char* argv[])
{
    std::map<std::string, docopt::value> args =
        docopt::docopt(USAGE, {argv + 1, argv + argc},
                       true,                    // show help if requested
                       "HomaRpcBench Server");  // version string
    int port = args["<port>"].asLong();
    std::string coordinator_mac = args["<coordinator_address>"].asString();
    int verboseLevel = args["--verbose"].asLong();

    // Set log level
    Homa::Debug::setLogPolicy(Homa::Debug::logPolicyFromString("SILENT"));
    if (verboseLevel > 0) {
        Homa::Debug::setLogPolicy(Homa::Debug::logPolicyFromString("ERROR"));
    }
    if (verboseLevel > 1) {
        Homa::Debug::setLogPolicy(Homa::Debug::logPolicyFromString("WARNING"));
    }
    if (verboseLevel > 2) {
        Homa::Debug::setLogPolicy(Homa::Debug::logPolicyFromString("NOTICE"));
    }
    if (verboseLevel > 3) {
        Homa::Debug::setLogPolicy(Homa::Debug::logPolicyFromString("VERBOSE"));
    }

    Homa::Drivers::DPDK::DpdkDriver driver(port);
    Homa::Transport transport(
        &driver, std::hash<std::string>{}(
                     driver.addressToString(driver.getLocalAddress())));
    HomaRpcBench::Server server(&transport);

    // Register the signal handler
    signal(SIGINT, sig_int_handler);

    Homa::Driver::Address coordinatorAddr = driver.getAddress(&coordinator_mac);
    Homa::Driver::Address serverAddress = driver.getLocalAddress();

    // Register the Server
    Homa::RemoteOp enlistRpc(&transport);
    HomaRpcBench::WireFormat::EnlistServerRpc::Request request;
    HomaRpcBench::WireFormat::EnlistServerRpc::Response response;
    request.common.opcode = HomaRpcBench::WireFormat::EnlistServerRpc::opcode;
    driver.addressToWireFormat(serverAddress, &request.address);
    enlistRpc.request->append(&request, sizeof(request));
    enlistRpc.send(coordinatorAddr);
    while (!enlistRpc.isReady()) {
        if (INTERRUPT_FLAG) {
            break;
        }
        transport.poll();
    }
    enlistRpc.wait();
    enlistRpc.response->get(0, &response, sizeof(response));

    std::cout << "Registered as Server " << response.serverId << std::endl;

    // Run the server.
    while (true) {
        if (INTERRUPT_FLAG) {
            break;
        }
        server.poll();
    }

    return 0;
}
