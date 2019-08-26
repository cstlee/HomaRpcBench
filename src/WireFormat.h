#ifndef HOMARPCBENCH_WIREFORMAT_H
#define HOMARPCBENCH_WIREFORMAT_H

#include <stdint.h>

#include <Homa/Driver.h>

namespace HomaRpcBench {

/**
 * Defines the RPC structures on the
 */
namespace WireFormat {

enum Opcode {
    ENLIST_SERVER = 1,
    GET_SERVER_LIST,
    CONFIG_SERVER,
    ECHO,
    ECHO_MULTILEVEL,
    ILLEGAL_OPCODE,
};

struct Common {
    uint16_t opcode;
} __attribute__((packed));

/**
 * Used by Servers to make their existence known to the Coordinator.
 */
struct EnlistServerRpc {
    static const Opcode opcode = ENLIST_SERVER;

    struct Request {
        Common common;
        Homa::Driver::WireFormatAddress address;
    } __attribute__((packed));

    struct Response {
        Common common;
        uint64_t serverId;
    } __attribute__((packed));
};

/**
 * Used to get the current list of enlisted servers.
 */
struct GetServerListRpc {
    static const Opcode opcode = GET_SERVER_LIST;

    struct ServerListEntry {
        uint64_t serverId;
        Homa::Driver::WireFormatAddress address;
    } __attribute__((packed));

    struct Request {
        Common common;
    } __attribute__((packed));

    struct Response {
        Common common;
        uint32_t num;
        ServerListEntry servers[0];
    } __attribute__((packed));
};

/**
 * Used to request that a Server be ready for the benchmark to begin.
 */
struct ConfigServerRpc {
    static const Opcode opcode = CONFIG_SERVER;

    struct Request {
        Common common;
        /// True, if the target server should forward the RPC on to the next
        /// server in the chain.
        bool forward;
        /// If forward is false, the nextAddress will contain the address of the
        /// server to which the target server should chain the RPC.
        Homa::Driver::WireFormatAddress nextAddress;
    } __attribute__((packed));

    struct Response {
        Common common;
    } __attribute__((packed));
};

/**
 * The configurable benchmark RPC
 */
struct EchoRpc {
    static const Opcode opcode = ECHO;

    struct Request {
        Common common;
        uint32_t sentBytes;
        uint32_t responseBytes;
    } __attribute__((packed));

    struct Response {
        Common common;
        uint32_t hopCount;
        uint32_t responseBytes;
    } __attribute__((packed));
};

/**
 * The configurable benchmark RPC
 */
struct EchoMultiLevelRpc {
    static const Opcode opcode = ECHO_MULTILEVEL;

    struct Request {
        Common common;
        uint32_t sentBytes;
        uint32_t responseBytes;
    } __attribute__((packed));

    struct Response {
        Common common;
        uint32_t _pad_;
        uint32_t responseBytes;
    } __attribute__((packed));
};

}  // namespace WireFormat
}  // namespace HomaRpcBench

#endif  // HOMARPCBENCH_WIREFORMAT_H
