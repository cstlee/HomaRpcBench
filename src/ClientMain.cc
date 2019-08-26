#include <algorithm>
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
#include <docopt.h>

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
)";

using Latency = std::chrono::duration<double>;

struct TimeDist {
    Latency min;   // Fastest time seen (seconds).
    Latency p50;   // Median time per operation (seconds).
    Latency p90;   // 90th percentile time/op (seconds).
    Latency p99;   // 99th percentile time/op (seconds).
    Latency p999;  // 99.9th percentile time/op (seconds).
};

namespace Output {

std::string
format(const std::string& format, ...)
{
    va_list args;
    va_start(args, format);
    size_t len = std::vsnprintf(NULL, 0, format.c_str(), args);
    va_end(args);
    std::vector<char> vec(len + 1);
    va_start(args, format);
    std::vsnprintf(&vec[0], len + 1, format.c_str(), args);
    va_end(args);
    return &vec[0];
}

std::string
formatTime(Latency seconds)
{
    if (seconds < std::chrono::duration<double, std::micro>(1)) {
        return format(
            "%5.1f ns",
            std::chrono::duration<double, std::nano>(seconds).count());
    } else if (seconds < std::chrono::duration<double, std::milli>(1)) {
        return format(
            "%5.1f us",
            std::chrono::duration<double, std::micro>(seconds).count());
    } else if (seconds < std::chrono::duration<double>(1)) {
        return format(
            "%5.2f ms",
            std::chrono::duration<double, std::milli>(seconds).count());
    } else {
        return format("%5.2f s ", seconds.count());
    }
}

std::string
basicHeader()
{
    return "median       min       p90       p99      p999     description";
}

std::string
basic(std::vector<Latency>& times, const std::string description)
{
    int count = times.size();
    std::sort(times.begin(), times.end());

    TimeDist dist;

    dist.min = times[0];
    int index = count / 2;
    if (index < count) {
        dist.p50 = times.at(index);
    } else {
        dist.p50 = dist.min;
    }
    index = count - (count + 5) / 10;
    if (index < count) {
        dist.p90 = times.at(index);
    } else {
        dist.p90 = dist.p50;
    }
    index = count - (count + 50) / 100;
    if (index < count) {
        dist.p99 = times.at(index);
    } else {
        dist.p99 = dist.p90;
    }
    index = count - (count + 500) / 1000;
    if (index < count) {
        dist.p999 = times.at(index);
    } else {
        dist.p999 = dist.p99;
    }

    std::string output = "";
    output += format("%9s", formatTime(dist.p50).c_str());
    output += format(" %9s", formatTime(dist.min).c_str());
    output += format(" %9s", formatTime(dist.p90).c_str());
    output += format(" %9s", formatTime(dist.p99).c_str());
    output += format(" %9s", formatTime(dist.p999).c_str());
    output += "  ";
    output += description;
    return output;
}

}  // namespace Output

using ServerMap = std::map<uint64_t, Homa::Driver::Address>;

struct Config {
    Homa::Transport* transport;
    int count;
    ServerMap serverMap;
    int hops;
    int sendBytes;
    int receiveBytes;
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

        Homa::RemoteOp op(config.transport);
        op.request->append(&request, sizeof(request));
        op.request->append(&buffer, request.sentBytes);

        op.send(server);
        op.wait();

        op.response->get(0, &response, sizeof(response));
        op.response->get(sizeof(response), &buffer, response.responseBytes);

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

        Homa::RemoteOp op(config.transport);
        op.request->append(&request, sizeof(request));
        op.request->append(&buffer, request.sentBytes);

        op.send(server);
        op.wait();

        op.response->get(0, &response, sizeof(response));
        op.response->get(sizeof(response), &buffer, response.responseBytes);

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

    Homa::Drivers::DPDK::DpdkDriver driver(port);
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