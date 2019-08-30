#include <chrono>
#include <cstdarg>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <signal.h>

#include <Homa/Debug.h>
#include <Homa/Drivers/DPDK/DpdkDriver.h>
#include <Homa/Homa.h>
#include <PerfUtils/Cycles.h>
#include <PerfUtils/TimeTrace.h>
#include <docopt.h>

#include "Output.h"
#include "Rpc.h"
#include "WireFormat.h"

static const char USAGE[] = R"(HomaRpcBench Client.

    Usage:
        client [options] [-v | -vv | -vvv | -vvvv] <port> <coordinator_address> <bench>

    Options:
        -h --help           Show this screen.
        --version           Show version.
        -v --verbose        Show verbose output.
        --hops=<n>          Number of hops an op should make [default: 1].
        --sendBytes=<n>     Number of bytes in the request [default: 100].
        --receiveBytes=<n>  Number of bytes in the response [default: 100].
        --output=<type>     Format of the output [default: basic].
        --timetrace=<dir>   Enable TimeTrace output at provided location.
)";

using ServerMap = std::map<uint64_t, Homa::Driver::Address>;

struct Config {
    Homa::Transport* transport;
    int count;
    ServerMap serverMap;
    int hops;
    int sendBytes;
    int receiveBytes;
    bool timetrace;
};

struct TestCase {
    const char* name;       // Name of the performance test; this is what gets
                            // typed on the command line to run the test.
    void (*func)(Config&);  // Function that implements the test.
};

namespace Setup {

void
configServerChain(Config& config)
{
    if (config.hops > config.serverMap.size()) {
        std::cerr << config.hops << " requested but only "
                  << config.serverMap.size() << " servers." << std::endl;
        throw;
    }
    Homa::Driver* driver = config.transport->driver;

    int i = 0;
    auto entry = config.serverMap.begin();
    while ((i < config.hops - 1) &&
           std::next(entry) != config.serverMap.end()) {
        HomaRpcBench::Rpc::configServer(config.transport, entry->second, true,
                                        std::next(entry)->second);
        ++i;
        ++entry;
    }
    HomaRpcBench::Rpc::configServer(config.transport, entry->second, false);
}

}  // namespace Setup

namespace Benchmark {

void
noop(Config&)
{
    std::cout << "Nothing was done." << std::endl;
}

void
serverList(Config& config)
{
    Homa::Driver* driver = config.transport->driver;

    std::cout << "Server List has " << config.serverMap.size() << " entries."
              << std::endl;
    for (auto entry : config.serverMap) {
        std::cout << "Server " << entry.first << " at "
                  << driver->addressToString(entry.second) << std::endl;
    }
}

void
nestedRpc(Config& config)
{
    Setup::configServerChain(config);
    std::string description = Output::format(
        "send %dB message, receive %dB message, nested with %d hops",
        config.sendBytes, config.receiveBytes, config.hops);
    std::vector<std::chrono::duration<double>> times;
    char buffer[1024 * 1024];

    Homa::Driver::Address server = config.serverMap.begin()->second;

    HomaRpcBench::WireFormat::EchoRpc::Request request;
    HomaRpcBench::WireFormat::EchoRpc::Response response;
    request.common.opcode = HomaRpcBench::WireFormat::EchoRpc::opcode;
    request.sentBytes = config.sendBytes;
    request.responseBytes = config.receiveBytes;

    for (int i = 0; i < config.count; ++i) {
        uint64_t start = PerfUtils::Cycles::rdtsc();
        PerfUtils::TimeTrace::record(start, "Benchmark: +++ START +++");

        Homa::RemoteOp op(config.transport);
        PerfUtils::TimeTrace::record("Benchmark: RemoteOp constructed");
        op.request->append(&request, sizeof(request));
        op.request->append(&buffer, request.sentBytes);
        PerfUtils::TimeTrace::record("Benchmark: Request serialized");
        op.send(server);
        PerfUtils::TimeTrace::record("Benchmark: Request sent");

        op.wait();
        PerfUtils::TimeTrace::record("Benchmark: Response received");
        op.response->get(0, &response, sizeof(response));
        op.response->get(sizeof(response), &buffer, response.responseBytes);
        PerfUtils::TimeTrace::record("Benchmark: Response deserialized");
        uint64_t stop = PerfUtils::Cycles::rdtsc();
        times.emplace_back(PerfUtils::Cycles::toSeconds(stop - start));
        if (response.responseBytes != request.responseBytes) {
            std::cerr << "Expected " << request.responseBytes
                      << " bytes but got " << response.responseBytes
                      << " bytes." << std::endl;
        }
        if (response.hopCount != config.hops) {
            std::cerr << "Expected " << config.hops << " hops but got "
                      << response.hopCount << " hops." << std::endl;
        }
    }
    std::cout << Output::basicHeader() << std::endl;
    std::cout << Output::basic(times, description) << std::endl;
    if (config.timetrace) {
        PerfUtils::TimeTrace::print();
        for (auto server : config.serverMap) {
            HomaRpcBench::Rpc::dumpTimeTrace(config.transport, server.second);
        }
    }
}

void
ringRpc(Config& config)
{
    Setup::configServerChain(config);
    std::string description = Output::format(
        "send %dB message, receive %dB message, ring with %d hops",
        config.sendBytes, config.receiveBytes, config.hops);
    std::vector<std::chrono::duration<double>> times;
    char buffer[1024 * 1024];

    Homa::Driver::Address server = config.serverMap.begin()->second;

    HomaRpcBench::WireFormat::EchoMultiLevelRpc::Request request;
    HomaRpcBench::WireFormat::EchoMultiLevelRpc::Response response;
    request.common.opcode = HomaRpcBench::WireFormat::EchoMultiLevelRpc::opcode;
    request.sentBytes = config.sendBytes;
    request.responseBytes = config.receiveBytes;

    for (int i = 0; i < config.count; ++i) {
        uint64_t start = PerfUtils::Cycles::rdtsc();
        PerfUtils::TimeTrace::record(start, "Benchmark: +++ START +++");

        Homa::RemoteOp op(config.transport);
        PerfUtils::TimeTrace::record("Benchmark: RemoteOp constructed");
        op.request->append(&request, sizeof(request));
        op.request->append(&buffer, request.sentBytes);
        PerfUtils::TimeTrace::record("Benchmark: Request serialized");
        op.send(server);
        PerfUtils::TimeTrace::record("Benchmark: Request sent");

        op.wait();
        PerfUtils::TimeTrace::record("Benchmark: Response received");
        op.response->get(0, &response, sizeof(response));
        op.response->get(sizeof(response), &buffer, response.responseBytes);
        PerfUtils::TimeTrace::record("Benchmark: Response deserialized");

        uint64_t stop = PerfUtils::Cycles::rdtsc();
        times.emplace_back(PerfUtils::Cycles::toSeconds(stop - start));
        if (response.responseBytes != request.responseBytes) {
            std::cerr << "Expected " << request.responseBytes
                      << " bytes but got " << response.responseBytes
                      << " bytes." << std::endl;
        }
    }
    std::cout << Output::basicHeader() << std::endl;
    std::cout << Output::basic(times, description) << std::endl;
    if (config.timetrace) {
        PerfUtils::TimeTrace::print();
        for (auto server : config.serverMap) {
            HomaRpcBench::Rpc::dumpTimeTrace(config.transport, server.second);
        }
    }
}

}  // namespace Benchmark

TestCase tests[] = {
    {"noop", Benchmark::noop},
    {"serverList", Benchmark::serverList},
    {"nestedRpc", Benchmark::nestedRpc},
    {"ringRpc", Benchmark::ringRpc},
};

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
                       "HomaRpcBench Client");  // version string

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

    Config config;
    config.count = 100000;
    config.hops = args["--hops"].asLong();
    config.sendBytes = args["--sendBytes"].asLong();
    config.receiveBytes = args["--receiveBytes"].asLong();
    config.timetrace = args["--timetrace"].isString();
    if (config.timetrace) {
        std::string timetrace_log_path = args["--timetrace"].asString();
        timetrace_log_path += "/client-timetrace.log";
        PerfUtils::TimeTrace::setOutputFileName(timetrace_log_path.c_str());
    }

    Homa::Drivers::DPDK::DpdkDriver::Config driverConfig;
    driverConfig.HIGHEST_PACKET_PRIORITY_OVERRIDE = 0;
    Homa::Drivers::DPDK::DpdkDriver driver(port, &driverConfig);
    Homa::Transport transport(
        &driver, std::hash<std::string>{}(
                     driver.addressToString(driver.getLocalAddress())));
    config.transport = &transport;

    Homa::Driver::Address coordinatorAddr = driver.getAddress(&coordinator_mac);
    HomaRpcBench::Rpc::getServerList(&transport, coordinatorAddr,
                                     &config.serverMap);

    // Register the signal handler
    signal(SIGINT, sig_int_handler);

    bool foundTest = false;
    const std::string testName = args["<bench>"].asString();
    for (TestCase& test : tests) {
        if (std::strstr(test.name, testName.c_str()) != NULL) {
            foundTest = true;
            test.func(config);
            break;
        }
    }
    if (!foundTest) {
        std::cout << "No test found matching the given arguments" << std::endl;
    }

    return 0;
}