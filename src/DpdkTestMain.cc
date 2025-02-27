#include <iostream>
#include <vector>

#include <signal.h>

#include <Homa/Drivers/DPDK/DpdkDriver.h>
#include <PerfUtils/Cycles.h>
#include <PerfUtils/TimeTrace.h>
#include <docopt.h>

#include "Output.h"

static const char USAGE[] = R"(HomaRpcBench dpdk_test.

    Usage:
        dpdk_test [options] <port> (--server | <server_address>)

    Options:
        -h --help           Show this screen.
        --version           Show version.
        --timetrace         Enable TimeTrace output [default: false].
)";

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
                       true,                       // show help if requested
                       "HomaRpcBench dpdk_test");  // version string

    int port = args["<port>"].asLong();
    bool isServer = args["--server"].asBool();
    std::string server_address_string;
    if (!isServer) {
        server_address_string = args["<server_address>"].asString();
    }

    Homa::Drivers::DPDK::DpdkDriver driver(port);

    if (isServer) {
        std::cout << driver.addressToString(driver.getLocalAddress())
                  << std::endl;
        while (true) {
            if (INTERRUPT_FLAG) {
                break;
            }
            Homa::Driver::Packet* incoming[10];
            uint32_t receivedPackets;
            do {
                receivedPackets = driver.receivePackets(10, incoming);
            } while (receivedPackets == 0);
            Homa::Driver::Packet* pong = driver.allocPacket();
            pong->address = incoming[0]->address;
            pong->length = 100;
            driver.sendPacket(pong);
            driver.releasePackets(incoming, receivedPackets);
            driver.releasePackets(&pong, 1);
        }
    } else {
        Homa::Driver::Address server_address =
            driver.getAddress(&server_address_string);
        std::vector<Output::Latency> times;
        for (int i = 0; i < 100000; ++i) {
            uint64_t start = PerfUtils::Cycles::rdtsc();
            PerfUtils::TimeTrace::record(start, "START");
            Homa::Driver::Packet* ping = driver.allocPacket();
            PerfUtils::TimeTrace::record("allocPacket");
            ping->address = server_address;
            ping->length = 100;
            PerfUtils::TimeTrace::record("set ping args");
            driver.sendPacket(ping);
            PerfUtils::TimeTrace::record("sendPacket");
            driver.releasePackets(&ping, 1);
            PerfUtils::TimeTrace::record("releasePacket");
            Homa::Driver::Packet* incoming[10];
            uint32_t receivedPackets;
            do {
                receivedPackets = driver.receivePackets(10, incoming);
                PerfUtils::TimeTrace::record("receivePackets");
            } while (receivedPackets == 0);
            driver.releasePackets(incoming, receivedPackets);
            PerfUtils::TimeTrace::record("releasePacket");
            uint64_t stop = PerfUtils::Cycles::rdtsc();
            times.emplace_back(PerfUtils::Cycles::toSeconds(stop - start));
        }
        if (args["--timetrace"].asBool()) {
            PerfUtils::TimeTrace::print();
        }
        std::cout << Output::basicHeader() << std::endl;
        std::cout << Output::basic(times, "DpdkDriver Ping-Pong") << std::endl;
    }

    return 0;
}