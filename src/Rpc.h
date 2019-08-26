#ifndef HOMARPCBENCH_RPC_H
#define HOMARPCBENCH_RPC_H

#include <cstdint>
#include <iostream>
#include <map>

#include <Homa/Homa.h>

#include "WireFormat.h"

namespace HomaRpcBench {
namespace Rpc {

void
getServerList(Homa::Transport* transport, Homa::Driver::Address coordinatorAddr,
              std::map<uint64_t, Homa::Driver::Address>* serverMap)
{
    serverMap->clear();
    WireFormat::GetServerListRpc::Request request;
    request.common.opcode = WireFormat::GetServerListRpc::opcode;

    Homa::RemoteOp op(transport);
    op.request->append(&request, sizeof(request));
    op.send(coordinatorAddr);
    op.wait();

    WireFormat::GetServerListRpc::Response response;
    op.response->get(0, &response, sizeof(response));
    uint32_t offset = sizeof(response);

    for (uint32_t i = 0; i < response.num; ++i) {
        WireFormat::GetServerListRpc::ServerListEntry entry;
        op.response->get(offset, &entry, sizeof(entry));
        uint64_t serverId = entry.serverId;
        Homa::Driver::Address serverAddress =
            transport->driver->getAddress(&entry.address);
        serverMap->insert({serverId, serverAddress});
        offset += sizeof(entry);
    }
}

void
configServer(Homa::Transport* transport, Homa::Driver::Address server,
             bool forward,
             Homa::Driver::Address nextServer = Homa::Driver::Address(0))
{
    WireFormat::ConfigServerRpc::Request request;
    request.common.opcode = WireFormat::ConfigServerRpc::opcode;
    request.forward = forward;
    transport->driver->addressToWireFormat(nextServer, &request.nextAddress);

    Homa::RemoteOp op(transport);
    op.request->append(&request, sizeof(request));
    op.send(server);
    op.wait();
}

}  // namespace Rpc
}  // namespace HomaRpcBench

#endif  // HOMARPCBENCH_RPCWRAPPER_H
