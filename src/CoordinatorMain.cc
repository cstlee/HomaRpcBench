#include <functional>
#include <iostream>
#include <map>

#include <signal.h>

#include <Homa/Debug.h>
#include <Homa/Drivers/DPDK/DpdkDriver.h>
#include <Homa/Homa.h>
#include <docopt.h>

#include "WireFormat.h"

static const char USAGE[] = R"(HomaRpcBench Coordinator.

    Usage:
        coordinator [options] [-v | -vv | -vvv | -vvvv] <port>

    Options:
        -h --help       Show this screen.
        --version       Show version.
        -v --verbose    Show verbose output.
)";

namespace HomaRpcBench {

/**
 * Contains the functionality of the Coordinator which keeps track of a list
 * of Servers that can targeted by client benchmarks.
 */
class Coordinator {
  public:
    explicit Coordinator(Homa::Transport* transport)
        : transport(transport)
        , nextServerId(1)
        , serverMap()
    {}
    void poll();

  private:
    void dispatch(Homa::ServerOp* op);
    void handleEnlistRpc(Homa::ServerOp* op);
    void handleGetServerList(Homa::ServerOp* op);
    Homa::Transport* transport;
    uint64_t nextServerId;
    std::map<uint64_t, Homa::Driver::Address> serverMap;
};

void
Coordinator::poll()
{
    Homa::ServerOp op = transport->receiveServerOp();
    if (op) {
        dispatch(&op);
    }
    transport->poll();
}

void
Coordinator::dispatch(Homa::ServerOp* op)
{
    WireFormat::Common common;
    op->request->get(0, &common, sizeof(common));

    switch (common.opcode) {
        case WireFormat::EnlistServerRpc::opcode:
            handleEnlistRpc(op);
            break;
        case WireFormat::GetServerListRpc::opcode:
            handleGetServerList(op);
            break;
        default:
            std::cerr << "Unknown opcode" << std::endl;
    }
}

void
Coordinator::handleEnlistRpc(Homa::ServerOp* op)
{
    WireFormat::EnlistServerRpc::Request request;
    WireFormat::EnlistServerRpc::Response response;
    op->request->get(0, &request, sizeof(request));
    uint64_t serverId = nextServerId++;
    Homa::Driver::Address serverAddress =
        transport->driver->getAddress(&request.address);
    serverMap.insert({serverId, serverAddress});
    response.common.opcode = WireFormat::EnlistServerRpc::opcode;
    response.serverId = serverId;
    op->response->append(&response, sizeof(response));
    op->reply();
    std::cout << "Enlisted Server " << serverId << " at "
              << transport->driver->addressToString(serverAddress) << std::endl;
}

void
Coordinator::handleGetServerList(Homa::ServerOp* op)
{
    WireFormat::GetServerListRpc::Response response;
    response.common.opcode = WireFormat::GetServerListRpc::opcode;
    response.num = serverMap.size();
    op->response->append(&response, sizeof(response));
    for (auto server = serverMap.begin(); server != serverMap.end(); ++server) {
        WireFormat::GetServerListRpc::ServerListEntry entry;
        entry.serverId = server->first;
        transport->driver->addressToWireFormat(server->second, &entry.address);
        op->response->append(&entry, sizeof(entry));
    }
    op->reply();
    std::cout << "Replied to getServerList with " << response.num << " entries."
              << std::endl;
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
                       true,                         // show help if requested
                       "HomaRpcBench Coordinator");  // version string
    int port = args["<port>"].asLong();
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

    Homa::Drivers::DPDK::DpdkDriver::Config driverConfig;
    driverConfig.HIGHEST_PACKET_PRIORITY_OVERRIDE = 0;
    Homa::Drivers::DPDK::DpdkDriver driver(port, &driverConfig);
    Homa::Transport transport(
        &driver, std::hash<std::string>{}(
                     driver.addressToString(driver.getLocalAddress())));
    HomaRpcBench::Coordinator coordinator(&transport);

    // Register the signal handler
    signal(SIGINT, sig_int_handler);

    while (true) {
        if (INTERRUPT_FLAG) {
            break;
        }
        coordinator.poll();
    }

    return 0;
}