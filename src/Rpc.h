#ifndef HOMARPCBENCH_RPC_H
#define HOMARPCBENCH_RPC_H

#include <Homa/Homa.h>
#include <Roo/Roo.h>

#include <cstdint>
#include <iostream>
#include <map>

#include "WireFormat.h"

namespace HomaRpcBench {
namespace Rpc {

void
getServerList(Roo::Socket* socket, Homa::Driver* driver,
              Homa::Driver::Address coordinatorAddr,
              std::map<uint64_t, Homa::Driver::Address>* serverMap)
{
    serverMap->clear();
    WireFormat::GetServerListRpc::Request request;
    request.common.opcode = WireFormat::GetServerListRpc::opcode;

    Roo::unique_ptr<Roo::RooPC> rpc = socket->allocRooPC();
    Homa::unique_ptr<Homa::OutMessage> requestMsg = rpc->allocRequest();
    requestMsg->append(&request, sizeof(request));
    rpc->send(coordinatorAddr, std::move(requestMsg));
    rpc->wait();

    WireFormat::GetServerListRpc::Response response;
    Homa::unique_ptr<Homa::InMessage> responseMsg = rpc->receive();
    responseMsg->get(0, &response, sizeof(response));
    uint32_t offset = sizeof(response);

    for (uint32_t i = 0; i < response.num; ++i) {
        WireFormat::GetServerListRpc::ServerListEntry entry;
        responseMsg->get(offset, &entry, sizeof(entry));
        uint64_t serverId = entry.serverId;
        Homa::Driver::Address serverAddress =
            driver->getAddress(&entry.address);
        serverMap->insert({serverId, serverAddress});
        offset += sizeof(entry);
    }
}

void
configServer(Roo::Socket* socket, Homa::Driver* driver,
             Homa::Driver::Address server, bool forward,
             Homa::Driver::Address nextServer = Homa::Driver::Address(0))
{
    WireFormat::ConfigServerRpc::Request request;
    request.common.opcode = WireFormat::ConfigServerRpc::opcode;
    request.forward = forward;
    driver->addressToWireFormat(nextServer, &request.nextAddress);

    Roo::unique_ptr<Roo::RooPC> rpc = socket->allocRooPC();
    Homa::unique_ptr<Homa::OutMessage> requestMsg = rpc->allocRequest();
    requestMsg->append(&request, sizeof(request));
    rpc->send(server, std::move(requestMsg));
    rpc->wait();
}

void
dumpTimeTrace(Roo::Socket* socket, Homa::Driver* driver,
              Homa::Driver::Address server)
{
    WireFormat::DumpTimeTraceRpc::Request request;
    request.common.opcode = WireFormat::DumpTimeTraceRpc::opcode;

    Roo::unique_ptr<Roo::RooPC> rpc = socket->allocRooPC();
    Homa::unique_ptr<Homa::OutMessage> requestMsg = rpc->allocRequest();
    requestMsg->append(&request, sizeof(request));
    rpc->send(server, std::move(requestMsg));
    rpc->wait();
}

}  // namespace Rpc
}  // namespace HomaRpcBench

#endif  // HOMARPCBENCH_RPCWRAPPER_H
