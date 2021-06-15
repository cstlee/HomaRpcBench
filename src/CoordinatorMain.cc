#include <Homa/Debug.h>
#include <Homa/Drivers/DPDK/DpdkDriver.h>
#include <Homa/Homa.h>
#include <Roo/Roo.h>
#include <docopt.h>
#include <signal.h>

#include <functional>
#include <iostream>
#include <map>

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
        , socket(Roo::Socket::create(transport))
        , nextServerId(1)
        , serverMap()
    {}
    void poll();

  private:
    void dispatch(Roo::unique_ptr<Roo::ServerTask> task);
    void handleEnlistRpc(Roo::unique_ptr<Roo::ServerTask> task);
    void handleGetServerList(Roo::unique_ptr<Roo::ServerTask> task);
    Homa::Transport* transport;
    std::unique_ptr<Roo::Socket> socket;
    uint64_t nextServerId;
    std::map<uint64_t, Homa::Driver::Address> serverMap;
};

void
Coordinator::poll()
{
    Roo::unique_ptr<Roo::ServerTask> task = socket->receive();
    if (task) {
        dispatch(std::move(task));
    }
    socket->poll();
}

void
Coordinator::dispatch(Roo::unique_ptr<Roo::ServerTask> task)
{
    WireFormat::Common common;
    task->getRequest()->get(0, &common, sizeof(common));

    switch (common.opcode) {
        case WireFormat::EnlistServerRpc::opcode:
            handleEnlistRpc(std::move(task));
            break;
        case WireFormat::GetServerListRpc::opcode:
            handleGetServerList(std::move(task));
            break;
        default:
            std::cerr << "Unknown opcode" << std::endl;
    }
}

void
Coordinator::handleEnlistRpc(Roo::unique_ptr<Roo::ServerTask> task)
{
    WireFormat::EnlistServerRpc::Request request;
    WireFormat::EnlistServerRpc::Response response;
    task->getRequest()->get(0, &request, sizeof(request));
    uint64_t serverId = nextServerId++;
    Homa::Driver::Address serverAddress =
        transport->getDriver()->getAddress(&request.address);
    serverMap.insert({serverId, serverAddress});
    response.common.opcode = WireFormat::EnlistServerRpc::opcode;
    response.serverId = serverId;
    Homa::unique_ptr<Homa::OutMessage> responseMsg = task->allocOutMessage();
    responseMsg->append(&response, sizeof(response));
    task->reply(std::move(responseMsg));
    std::cout << "Enlisted Server " << serverId << " at "
              << transport->getDriver()->addressToString(serverAddress)
              << std::endl;
}

void
Coordinator::handleGetServerList(Roo::unique_ptr<Roo::ServerTask> task)
{
    WireFormat::GetServerListRpc::Response response;
    response.common.opcode = WireFormat::GetServerListRpc::opcode;
    response.num = serverMap.size();
    Homa::unique_ptr<Homa::OutMessage> responseMsg = task->allocOutMessage();
    responseMsg->append(&response, sizeof(response));
    for (auto server = serverMap.begin(); server != serverMap.end(); ++server) {
        WireFormat::GetServerListRpc::ServerListEntry entry;
        entry.serverId = server->first;
        transport->getDriver()->addressToWireFormat(server->second,
                                                    &entry.address);
        responseMsg->append(&entry, sizeof(entry));
    }
    task->reply(std::move(responseMsg));
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
    std::unique_ptr<Homa::Transport> transport(Homa::Transport::create(
        &driver, std::hash<std::string>{}(
                     driver.addressToString(driver.getLocalAddress()))));
    HomaRpcBench::Coordinator coordinator(transport.get());

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