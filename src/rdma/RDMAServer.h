/**
 * @file RDMAServer.h
 * @author cbinnig, tziegler
 * @date 2018-08-17
 */

#ifndef RDMAServer_H_
#define RDMAServer_H_

#include "../message/MessageErrors.h"
#include "../message/ProtoMessageFactory.h"
#include "../proto/ProtoServer.h"
#include "../rdma/BaseRDMA.h"
#include "../utils/Config.h"
#include "ReliableRDMA.h"
#include "UnreliableRDMA.h"
#include "NodeIDSequencer.h"

#include <list>
#include <mutex>
#include <unordered_map>
#include <type_traits>

namespace rdma {

template <typename RDMA_API_T>
class RDMAServer : public ProtoServer, public ProtoClient, public RDMA_API_T {
 public:
  RDMAServer() : RDMAServer("RDMAserver"){}
  RDMAServer(string name) : RDMAServer(name, Config::RDMA_PORT){}
  RDMAServer(string name, int port) : RDMAServer(name, port, Config::RDMA_MEMSIZE){}
  RDMAServer(string name, int port, uint64_t memsize) : ProtoServer(name, port), RDMA_API_T(memsize)
  {
    std::string sequencerIpPort = Config::SEQUENCER_IP + ":" + to_string(Config::SEQUENCER_PORT);
    std::string ownIpPort = Config::getIP(Config::RDMA_INTERFACE) + ":" + to_string(port);
    m_ownNodeID = requestNodeID(sequencerIpPort, ownIpPort);
  }

  ~RDMAServer() = default;

  // server methods
  bool startServer() {
    // start data node server
    if (!ProtoServer::startServer()) {
      Logging::error(__FILE__, __LINE__, "RDMAServer: could not be started");
      return false;
    }

    Logging::debug(__FILE__, __LINE__, "RDMAServer: started server!");
    return true;
  }

  void stopServer() { ProtoServer::stopServer(); }


  void *getBuffer(const size_t offset) {
    return ((char *)RDMA_API_T::getBuffer() + offset);
  }

  void activateSRQ(size_t srqID) {
    Logging::debug(__FILE__, __LINE__,
                   "setCurrentSRQ: assigned to " + to_string(srqID));
    m_currentSRQ = srqID;
  }

  void deactiveSRQ() { m_currentSRQ = SIZE_MAX; }

  size_t getCurrentSRQ() { return m_currentSRQ; }

 protected:

  void handle(Any *anyReq, Any *anyResp) override {
    if (anyReq->Is<RDMAConnRequest>()) {
      RDMAConnResponse connResp;
      RDMAConnRequest connReq;
      anyReq->UnpackTo(&connReq);
      connectQueue(&connReq, &connResp);
      Logging::debug(__FILE__, __LINE__,
                     "RDMAServer::handle: after connectQueue");
      anyResp->PackFrom(connResp);
    } else if (anyReq->Is<MemoryResourceRequest>()) {
      MemoryResourceResponse respMsg;
      MemoryResourceRequest reqMsg;
      anyReq->UnpackTo(&reqMsg);
      if (reqMsg.type() == MemoryResourceType::Enum::MEMORY_RESOURCE_RELEASE) {
        size_t offset = reqMsg.offset();
        respMsg.set_return_(releaseMemoryResource(offset));
        respMsg.set_offset(offset);
      } else if (reqMsg.type() == MemoryResourceType::Enum::MEMORY_RESOURCE_REQUEST) {
        size_t offset = 0;
        size_t size = reqMsg.size();
        respMsg.set_return_(requestMemoryResource(size, offset));
        respMsg.set_offset(offset);
      }
      anyResp->PackFrom(respMsg);
    } else {
      // Send response with bad return code;
      ErrorMessage errorResp;
      errorResp.set_return_(MessageErrors::INVALID_MESSAGE);
      anyResp->PackFrom(errorResp);
    }
  }


  // memory management
  MessageErrors requestMemoryResource(size_t size, size_t &offset) {
    unique_lock<mutex> lck(m_memLock);

    rdma_mem_t memRes = RDMA_API_T::remoteAlloc(size);
    offset = memRes.offset;

    if (!memRes.isnull) {
      lck.unlock();
      return MessageErrors::NO_ERROR;
    }

    lck.unlock();
    return MessageErrors::MEMORY_NOT_AVAILABLE;
  }

  MessageErrors releaseMemoryResource(size_t &offset) {
    unique_lock<mutex> lck(m_memLock);
    try 
    {
      RDMA_API_T::remoteFree(offset);
    }
    catch (runtime_error& e)
    {
      lck.unlock();
      return MessageErrors::MEMORY_RELEASE_FAILED;
    }
    lck.unlock();
    return MessageErrors::NO_ERROR;
  }

  bool connectQueue(RDMAConnRequest *connRequest,
                    RDMAConnResponse *connResponse) {
    unique_lock<mutex> lck(m_connLock);

    // create local QP
    NodeID nodeID = connRequest->nodeid();

    // Check if SRQ is active
    try
    {
      if (m_currentSRQ == SIZE_MAX) {
        Logging::debug(
            __FILE__, __LINE__,
            "RDMAServer: initializing queue pair - " + to_string(nodeID));
        RDMA_API_T::initQPWithSuppliedID(nodeID);
      } else {
        if constexpr (std::is_same_v<RDMA_API_T, ReliableRDMA>) {
          
        Logging::debug(__FILE__, __LINE__,
                      "RDMAServer: initializing queue pair with srq id: " +
                          to_string(m_currentSRQ) + " - " + to_string(nodeID));
        RDMA_API_T::initQPForSRQWithSuppliedID(m_currentSRQ, nodeID);

        }
      }
    }
    catch(const std::runtime_error& e)
    {
      std::cerr << e.what() << '\n';
      lck.unlock();
      return false;
    }

    // set remote connection data
    struct ib_conn_t remoteConn;
    remoteConn.buffer = connRequest->buffer();
    remoteConn.rc.rkey = connRequest->rkey();
    remoteConn.qp_num = connRequest->qp_num();
    remoteConn.lid = connRequest->lid();
    for (int i = 0; i < 16; ++i) {
      remoteConn.gid[i] = connRequest->gid(i);
    }
    remoteConn.ud.psn = connRequest->psn();
    RDMA_API_T::setRemoteConnData(nodeID, remoteConn);

    try
    {
      RDMA_API_T::connectQP(nodeID);
    }
    catch(const std::runtime_error& e)
    {
      std::cerr << e.what() << '\n';
      lck.unlock();
      return false;
    }

    // create response
    ib_conn_t localConn = RDMA_API_T::getLocalConnData(nodeID);
    connResponse->set_buffer(localConn.buffer);
    connResponse->set_rkey(localConn.rc.rkey);
    connResponse->set_qp_num(localConn.qp_num);
    connResponse->set_lid(localConn.lid);
    connResponse->set_psn(localConn.ud.psn);
    for (int i = 0; i < 16; ++i) {
      connResponse->add_gid(localConn.gid[i]);
    }

    Logging::debug(__FILE__, __LINE__,
                   "RDMAServer: connected to client!" + to_string(nodeID));

    lck.unlock();
    return true;
  }

  NodeID requestNodeID(std::string sequencerIpPort, std::string ownIpPort)
  {    
    // check if client is connected to sequencer
    if (ProtoClient::isConnected(sequencerIpPort)) {
      return true;
    }
    ProtoClient::connectProto(sequencerIpPort);

    Any nodeIDRequest = ProtoMessageFactory::createNodeIDRequest(ownIpPort, ProtoServer::m_name, NodeType::Enum::SERVER);
    Any rcvAny;

    ProtoClient::exchangeProtoMsg(sequencerIpPort, &nodeIDRequest, &rcvAny);

    if (rcvAny.Is<NodeIDResponse>()) {
      NodeIDResponse connResponse;
      rcvAny.UnpackTo(&connResponse);
      return connResponse.nodeid();
    } else {
      Logging::error(__FILE__, __LINE__,
                     "RDMAServer could not request NodeID from NodeIDSequencer: received wrong response type");
      throw std::runtime_error("RDMAServer could not request NodeID from NodeIDSequencer: received wrong response type");
    }
  }

  unordered_map<string, NodeID> m_mcastAddr;  // mcast_string to ibaddr

  // Locks for multiple clients accessing server
  mutex m_connLock;
  mutex m_memLock;

  size_t m_currentSRQ = SIZE_MAX;

  std::string m_sequencerIpPort = Config::SEQUENCER_IP + ":" + to_string(Config::SEQUENCER_PORT);
  NodeID m_ownNodeID;

private:
  using ProtoClient::connectProto; //Make private
  using ProtoClient::exchangeProtoMsg; //Make private

};

}  // namespace rdma

#endif /* RDMAServer_H_ */
