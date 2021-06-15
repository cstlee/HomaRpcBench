#include <Homa/Debug.h>
#include <Homa/Drivers/DPDK/DpdkDriver.h>
#include <Homa/Homa.h>
#include <PerfUtils/Cycles.h>
#include <PerfUtils/TimeTrace.h>
#include <Roo/Roo.h>
#include <docopt.h>
#include <signal.h>

#include <functional>
#include <iostream>
#include <map>

#include "Output.h"
#include "WireFormat.h"

static const char USAGE[] = R"(HomaRpcBench Server.

    Usage:
        server [options] [-v | -vv | -vvv | -vvvv] <port> <coordinator_address>

    Options:
        -h --help           Show this screen.
        --version           Show version.
        -v --verbose        Show verbose output.
        --timetrace=<dir>   Directory where a timetrace log should be output.
)";

namespace HomaRpcBench {

/**
 * Implements the server-side benchmark functionality.
 */
class Server {
  public:
    explicit Server(Homa::Transport* transport,
                    std::unique_ptr<Roo::Socket> socket);

    void poll();

  private:
    void dispatch(Roo::unique_ptr<Roo::ServerTask> task);
    void handleConfigServerRpc(Roo::unique_ptr<Roo::ServerTask> task);
    void handleEchoRpc(Roo::unique_ptr<Roo::ServerTask> task);
    void handleEchoMultiLevelRpc(Roo::unique_ptr<Roo::ServerTask> task);

    Homa::Transport* transport;
    std::unique_ptr<Roo::Socket> socket;
    bool proxy;
    Homa::Driver::Address delegate;
    char buffer[1024 * 1024];
};

Server::Server(Homa::Transport* transport, std::unique_ptr<Roo::Socket> socket)
    : transport(transport)
    , socket(std::move(socket))
    , proxy(false)
    , delegate()
{}

void
Server::poll()
{
    uint64_t poll_start = PerfUtils::Cycles::rdtsc();
    Roo::unique_ptr<Roo::ServerTask> task = socket->receive();
    if (task) {
        PerfUtils::TimeTrace::record(poll_start,
                                     "Benchmark: Server::poll : START");
        PerfUtils::TimeTrace::record(
            "Benchmark: Server::poll : ServerTask Constructed/Received");
        dispatch(std::move(task));
    }
    socket->poll();
}

void
Server::dispatch(Roo::unique_ptr<Roo::ServerTask> task)
{
    WireFormat::Common common;
    task->getRequest()->get(0, &common, sizeof(common));

    switch (common.opcode) {
        case WireFormat::ConfigServerRpc::opcode:
            handleConfigServerRpc(std::move(task));
            break;
        case WireFormat::DumpTimeTraceRpc::opcode:
            PerfUtils::TimeTrace::print();
            break;
        case WireFormat::EchoRpc::opcode:
            handleEchoRpc(std::move(task));
            break;
        case WireFormat::EchoMultiLevelRpc::opcode:
            handleEchoMultiLevelRpc(std::move(task));
            break;
        default:
            std::cerr << "Unknown opcode" << std::endl;
    }
}

void
Server::handleConfigServerRpc(Roo::unique_ptr<Roo::ServerTask> task)
{
    WireFormat::ConfigServerRpc::Request request;
    WireFormat::ConfigServerRpc::Response response;
    task->getRequest()->get(0, &request, sizeof(request));
    proxy = request.forward;
    if (proxy) {
        delegate = transport->getDriver()->getAddress(&request.nextAddress);
    }

    response.common.opcode = WireFormat::ConfigServerRpc::opcode;
    Homa::unique_ptr<Homa::OutMessage> responseMsg = task->allocOutMessage();
    responseMsg->append(&response, sizeof(response));
    task->reply(std::move(responseMsg));
    if (proxy) {
        std::cout << "Server configured as proxy to "
                  << transport->getDriver()->addressToString(delegate)
                  << std::endl;
    } else {
        std::cout << "Server configured" << std::endl;
    }
}

void
Server::handleEchoRpc(Roo::unique_ptr<Roo::ServerTask> task)
{
    PerfUtils::TimeTrace::record("Benchmark: Server::handleEchoRpc : START");
    WireFormat::EchoRpc::Request request;
    WireFormat::EchoRpc::Response response;
    task->getRequest()->get(0, &request, sizeof(request));
    task->getRequest()->get(sizeof(request), &buffer, request.sentBytes);
    PerfUtils::TimeTrace::record(
        "Benchmark: Server::handleEchoRpc : Request deserialized");

    response.common.opcode = WireFormat::EchoRpc::opcode;
    response.hopCount = 1;
    response.responseBytes = request.responseBytes;

    if (proxy) {
        PerfUtils::TimeTrace::record(
            "Benchmark: Server::handleEchoRpc : Nested : START");
        Roo::unique_ptr<Roo::RooPC> proxyRpc = socket->allocRooPC();
        PerfUtils::TimeTrace::record(
            "Benchmark: Server::handleEchoRpc : Nested : RooPC constructed");
        Homa::unique_ptr<Homa::OutMessage> requestMsg =
            proxyRpc->allocRequest();
        requestMsg->append(&request, sizeof(request));
        requestMsg->append(&buffer, request.sentBytes);
        PerfUtils::TimeTrace::record(
            "Benchmark: Server::handleEchoRpc : Nested : Request serialized");

        proxyRpc->send(delegate, std::move(requestMsg));
        PerfUtils::TimeTrace::record(
            "Benchmark: Server::handleEchoRpc : Nested : Request sent");
        proxyRpc->wait();
        PerfUtils::TimeTrace::record(
            "Benchmark: Server::handleEchoRpc : Nested : Response received");

        WireFormat::EchoRpc::Response proxyResponse;
        Homa::unique_ptr<Homa::InMessage> responseMsg = proxyRpc->receive();
        responseMsg->get(0, &proxyResponse, sizeof(proxyResponse));
        responseMsg->get(sizeof(proxyResponse), &buffer,
                         proxyResponse.responseBytes);
        if (proxyResponse.responseBytes != request.responseBytes) {
            std::cerr << "Expected " << request.responseBytes
                      << " bytes but only got " << proxyResponse.responseBytes
                      << " bytes." << std::endl;
        }
        response.hopCount += proxyResponse.hopCount;
        PerfUtils::TimeTrace::record(
            "Benchmark: Server::handleEchoRpc : Nested : "
            "Response deserialized");
    }

    Homa::unique_ptr<Homa::OutMessage> responseMsg = task->allocOutMessage();
    responseMsg->append(&response, sizeof(response));
    responseMsg->append(&buffer, response.responseBytes);
    PerfUtils::TimeTrace::record(
        "Benchmark: Server::handleEchoRpc : Response serialized");
    task->reply(std::move(responseMsg));
    PerfUtils::TimeTrace::record(
        "Benchmark: Server::handleEchoRpc : Response sent (reply)");
}

void
Server::handleEchoMultiLevelRpc(Roo::unique_ptr<Roo::ServerTask> task)
{
    WireFormat::EchoMultiLevelRpc::Request request;
    task->getRequest()->get(0, &request, sizeof(request));
    task->getRequest()->get(sizeof(request), &buffer, request.sentBytes);

    if (proxy) {
        Homa::unique_ptr<Homa::OutMessage> delegateMsg =
            task->allocOutMessage();
        delegateMsg->append(&request, sizeof(request));
        delegateMsg->append(&buffer, request.sentBytes);
        task->delegate(delegate, std::move(delegateMsg));
    } else {
        WireFormat::EchoMultiLevelRpc::Response response;
        response.common.opcode = WireFormat::EchoMultiLevelRpc::opcode;
        response.responseBytes = request.responseBytes;
        Homa::unique_ptr<Homa::OutMessage> responseMsg =
            task->allocOutMessage();
        responseMsg->append(&response, sizeof(response));
        responseMsg->append(&buffer, response.responseBytes);
        task->reply(std::move(responseMsg));
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

    Homa::Drivers::DPDK::DpdkDriver::Config driverConfig;
    driverConfig.HIGHEST_PACKET_PRIORITY_OVERRIDE = 0;
    Homa::Drivers::DPDK::DpdkDriver driver(port, &driverConfig);
    std::unique_ptr<Homa::Transport> transport(Homa::Transport::create(
        &driver, std::hash<std::string>{}(
                     driver.addressToString(driver.getLocalAddress()))));
    std::unique_ptr<Roo::Socket> socket = Roo::Socket::create(transport.get());

    // Register the signal handler
    signal(SIGINT, sig_int_handler);

    Homa::Driver::Address coordinatorAddr = driver.getAddress(&coordinator_mac);
    Homa::Driver::Address serverAddress = driver.getLocalAddress();

    // Register the Server
    Roo::unique_ptr<Roo::RooPC> enlistRpc = socket->allocRooPC();
    HomaRpcBench::WireFormat::EnlistServerRpc::Request request;
    HomaRpcBench::WireFormat::EnlistServerRpc::Response response;
    request.common.opcode = HomaRpcBench::WireFormat::EnlistServerRpc::opcode;
    driver.addressToWireFormat(serverAddress, &request.address);
    Homa::unique_ptr<Homa::OutMessage> requestMsg = enlistRpc->allocRequest();
    requestMsg->append(&request, sizeof(request));
    enlistRpc->send(coordinatorAddr, std::move(requestMsg));
    while (enlistRpc->checkStatus() == Roo::RooPC::Status::IN_PROGRESS) {
        if (INTERRUPT_FLAG) {
            break;
        }
        socket->poll();
    }
    enlistRpc->wait();
    Homa::unique_ptr<Homa::InMessage> responseMsg = enlistRpc->receive();
    responseMsg->get(0, &response, sizeof(response));

    std::cout << "Registered as Server " << response.serverId << std::endl;

    if (args["--timetrace"].isString()) {
        std::string timetrace_log_path = args["--timetrace"].asString();
        timetrace_log_path +=
            Output::format("/server-%u-timetrace.log", response.serverId);
        PerfUtils::TimeTrace::setOutputFileName(timetrace_log_path.c_str());
    }

    HomaRpcBench::Server server(transport.get(), std::move(socket));
    // Run the server.
    while (true) {
        if (INTERRUPT_FLAG) {
            break;
        }
        server.poll();
    }

    return 0;
}
