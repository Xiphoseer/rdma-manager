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
#include "RDMAClient.h"

#include <list>
#include <mutex>
#include <unordered_map>
#include <type_traits>

#ifndef HUGEPAGE
#define HUGEPAGE false
#endif

namespace rdma {

template <typename RDMA_API_T>
class RDMAServer : public ProtoServer, public RDMAClient<RDMA_API_T> {
 public:
  // backwards compatibility constructors
  RDMAServer() : RDMAServer("RDMAServer"){}
  RDMAServer(std::string name) : RDMAServer(name, Config::RDMA_PORT){}
  RDMAServer(std::string name, uint16_t port) : RDMAServer(name, port, Config::RDMA_MEMSIZE){}
  //RDMAServer(std::string name, int port, uint64_t mem_size) : RDMAServer(name, port, mem_size, (int)){}
  /*RDMAServer(string name, int port, uint64_t memsize, int numaNode) 
    : ProtoServer(name, port, Config::getIP(Config::RDMA_INTERFACE)), RDMAClient<RDMA_API_T>(memsize, name, Config::getIP(Config::RDMA_INTERFACE) + ":" + to_string(port), NodeType::Enum::SERVER, numaNode)
  {
    // if (!ProtoServer::isRunning())
    // {
    //   ProtoServer::startServer();
    // }
  }*/

  RDMAServer(uint64_t mem_size) : RDMAServer(Config::RDMA_PORT, mem_size){}

  // new constructors
  // no addr, sequencerIpPort, name
  RDMAServer(uint16_t port, uint64_t mem_size) : RDMAServer(port, mem_size, HUGEPAGE, (int)Config::RDMA_NUMAREGION){}
  RDMAServer(uint16_t port, uint64_t mem_size, bool huge) : RDMAServer(port, mem_size, huge, (int)Config::RDMA_NUMAREGION){}
  RDMAServer(uint16_t port, uint64_t mem_size, int numaNode) : RDMAServer(port, mem_size, HUGEPAGE, numaNode){}
  RDMAServer(uint16_t port, uint64_t mem_size, bool huge, int numaNode) : RDMAServer(port, mem_size, (int)MEMORY_TYPE::MAIN, huge, numaNode){}
  RDMAServer(uint16_t port, uint64_t mem_size, MEMORY_TYPE mem_type, bool huge, int numaNode) : RDMAServer(port, mem_size, (int)mem_type, huge, numaNode){}
  RDMAServer(uint16_t port, uint64_t mem_size, int mem_type, bool huge, int numaNode) : RDMAServer("RDMAServer", port, mem_size, mem_type, huge, numaNode){}

  // no addr, sequencerIpPort
  RDMAServer(std::string name, uint16_t port, uint64_t mem_size) : RDMAServer(name, port, mem_size, HUGEPAGE, (int)Config::RDMA_NUMAREGION){}
  RDMAServer(std::string name, uint16_t port, uint64_t mem_size, bool huge) : RDMAServer(name, port, mem_size, huge, (int)Config::RDMA_NUMAREGION){}
  RDMAServer(std::string name, uint16_t port, uint64_t mem_size, int numaNode) : RDMAServer(name, port, mem_size, HUGEPAGE, numaNode){}
  RDMAServer(std::string name, uint16_t port, uint64_t mem_size, bool huge, int numaNode) : RDMAServer(name, port, mem_size, (int)MEMORY_TYPE::MAIN, huge, numaNode){}
  RDMAServer(std::string name, uint16_t port, uint64_t mem_size, MEMORY_TYPE mem_type) : RDMAServer(name, port, mem_size, (int)mem_type){}
  RDMAServer(std::string name, uint16_t port, uint64_t mem_size, MEMORY_TYPE mem_type, bool huge, int numaNode) : RDMAServer(name, port, mem_size, (int)mem_type, huge, numaNode){}
  RDMAServer(std::string name, uint16_t port, uint64_t mem_size, int mem_type, bool huge, int numaNode) : RDMAServer(name, port, mem_size, mem_type, huge, numaNode, Config::SEQUENCER_IP+":"+to_string(Config::SEQUENCER_PORT)){}

  // no addr
  RDMAServer(std::string name, uint16_t port, uint64_t mem_size, std::string sequencerIpPort) : RDMAServer(name, port, mem_size, HUGEPAGE, (int)Config::RDMA_NUMAREGION, sequencerIpPort){}
  RDMAServer(std::string name, uint16_t port, uint64_t mem_size, bool huge, std::string sequencerIpPort) : RDMAServer(name, port, mem_size, huge, (int)Config::RDMA_NUMAREGION, sequencerIpPort){}
  RDMAServer(std::string name, uint16_t port, uint64_t mem_size, int numaNode, std::string sequencerIpPort) : RDMAServer(name, port, mem_size, HUGEPAGE, numaNode, sequencerIpPort){}
  RDMAServer(std::string name, uint16_t port, uint64_t mem_size, bool huge, int numaNode, std::string sequencerIpPort) : RDMAServer(name, port, mem_size, (int)MEMORY_TYPE::MAIN, huge, numaNode, sequencerIpPort){}
  RDMAServer(std::string name, uint16_t port, uint64_t mem_size, MEMORY_TYPE mem_type, std::string sequencerIpPort) : RDMAServer(name, port, mem_size, (int)mem_type, sequencerIpPort){}
  RDMAServer(std::string name, uint16_t port, uint64_t mem_size, MEMORY_TYPE mem_type, bool huge, int numaNode, std::string sequencerIpPort) : RDMAServer(name, port, mem_size, (int)mem_type, huge, numaNode, sequencerIpPort){}
  RDMAServer(std::string name, uint16_t port, uint64_t mem_size, int mem_type, bool huge, int numaNode, std::string sequencerIpPort) : RDMAServer(name, port, Config::getIP(Config::RDMA_INTERFACE), mem_size, mem_type, huge, numaNode, sequencerIpPort){}

  RDMAServer(std::string name, uint16_t port, std::string addr, uint64_t mem_size, std::string sequencerIpPort) : RDMAServer(name, port, addr, mem_size, HUGEPAGE, (int)Config::RDMA_NUMAREGION, sequencerIpPort){}
  RDMAServer(std::string name, uint16_t port, std::string addr, uint64_t mem_size, bool huge, std::string sequencerIpPort) : RDMAServer(name, port, addr, mem_size, huge, (int)Config::RDMA_NUMAREGION, sequencerIpPort){}
  RDMAServer(std::string name, uint16_t port, std::string addr, uint64_t mem_size, int numaNode, std::string sequencerIpPort) : RDMAServer(name, port, addr, mem_size, HUGEPAGE, numaNode, sequencerIpPort){}
  RDMAServer(std::string name, uint16_t port, std::string addr, uint64_t mem_size, bool huge, int numaNode, std::string sequencerIpPort) : RDMAServer(name, port, addr, mem_size, (int)MEMORY_TYPE::MAIN, huge, numaNode, sequencerIpPort){}
  RDMAServer(std::string name, uint16_t port, std::string addr, uint64_t mem_size, MEMORY_TYPE mem_type, std::string sequencerIpPort) : RDMAServer(name, port, addr, mem_size, (int)mem_type, sequencerIpPort){}
  RDMAServer(std::string name, uint16_t port, std::string addr, uint64_t mem_size, MEMORY_TYPE mem_type, bool huge, int numaNode, std::string sequencerIpPort) : RDMAServer(name, port, addr, mem_size, (int)mem_type, huge, numaNode, sequencerIpPort){}
  RDMAServer(std::string name, uint16_t port, std::string addr, uint64_t mem_size, int mem_type, bool huge, int numaNode, std::string sequencerIpPort) : ProtoServer(name, port, addr), RDMAClient<RDMA_API_T>(mem_size, mem_type, huge, numaNode, name, addr+":"+to_string(port), NodeType::Enum::SERVER, sequencerIpPort){
    /*if (!ProtoServer::isRunning()){
      ProtoServer::startServer();
    } */
  }

  RDMAServer(BaseMemory *memory) : RDMAServer("RDMAserver", memory){}
  RDMAServer(uint16_t port, BaseMemory *memory) : RDMAServer(port, memory, Config::SEQUENCER_IP+":"+to_string(Config::SEQUENCER_PORT)){}
  RDMAServer(uint16_t port, BaseMemory *memory, std::string sequencerIpPort) : RDMAServer("RDMAServer", port, memory, sequencerIpPort){}
  RDMAServer(uint16_t port, std::string addr, BaseMemory *memory) : RDMAServer(port, addr, memory, Config::SEQUENCER_IP+":"+to_string(Config::SEQUENCER_PORT)){}
  RDMAServer(uint16_t port, std::string addr, BaseMemory *memory, std::string sequencerIpPort) : RDMAServer("RDMAServer", port, addr, memory, sequencerIpPort){}
  RDMAServer(string name, BaseMemory *memory) : RDMAServer(name, Config::RDMA_PORT, memory){}
  RDMAServer(string name, BaseMemory *memory, std::string sequencerIpPort) : RDMAServer(name, Config::RDMA_PORT, memory, sequencerIpPort){}
  RDMAServer(string name, uint16_t port, BaseMemory *memory) : RDMAServer(name, port, memory, Config::SEQUENCER_IP+":"+to_string(Config::SEQUENCER_PORT)){}
  RDMAServer(string name, uint16_t port, BaseMemory *memory, std::string sequencerIpPort) : RDMAServer(name, port, Config::getIP(Config::RDMA_INTERFACE), memory, sequencerIpPort){}
  RDMAServer(string name, uint16_t port, std::string addr, BaseMemory *memory) : RDMAServer(name, port, addr, memory, Config::SEQUENCER_IP+":"+to_string(Config::SEQUENCER_PORT)){}
  RDMAServer(string name, uint16_t port, std::string addr, BaseMemory *memory, std::string sequencerIpPort) : ProtoServer(name, port, addr), RDMAClient<RDMA_API_T>(memory, name, addr+":"+to_string(port), NodeType::Enum::SERVER, sequencerIpPort){
    /*if (!ProtoServer::isRunning()){
      ProtoServer::startServer();
    } */
  }

  ~RDMAServer() = default;

  // server methods
  bool startServer() override{
    if (!ProtoClient::isConnected(m_sequencerIpPort)) {

      ProtoClient::setSendTimeout(Config::PROTO_SEND_TIMEOUT);
      ProtoClient::setRecvTimeout(Config::PROTO_RECV_TIMEOUT);

      try {
        // std::cout << "RDMAServer requesting nodeid!" << std::endl;
        RDMAClient<RDMA_API_T>::m_ownNodeID = RDMAClient<RDMA_API_T>::requestNodeID(RDMAClient<RDMA_API_T>::m_sequencerIpPort, RDMAClient<RDMA_API_T>::m_ownIpPort, RDMAClient<RDMA_API_T>::m_nodeType);
      } catch (std::runtime_error& e) {
        Logging::error(__FILE__, __LINE__, e.what());
        return false;
      }
    }
    if (ProtoServer::isRunning()) { 
      // std::cout << "RDMAServer is running!!!" << std::endl;
      return true;
    }
    // start data node server
    bool status;
    try {
      status = ProtoServer::startServer();
    } catch (std::runtime_error& e) {
      Logging::error(__FILE__, __LINE__, e.what());
      status = false;
    }
    if (!status) {
      Logging::error(__FILE__, __LINE__, "RDMAServer: could not be started");
      return false;
    }
    // std::cout << "RDMAServer started!!!" << std::endl;
    Logging::debug(__FILE__, __LINE__, "RDMAServer: started server!");
    return true;
  }

  void stopServer() override { ProtoServer::stopServer(); }


  void *getBuffer(const size_t offset=0) {
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
      ErrorMessage er;
      anyReq->UnpackTo(&connReq);
      if(connectQueue(&connReq, &connResp)){
          Logging::debug(__FILE__, __LINE__,
                         "RDMAServer::handle: after connectQueue");
          anyResp->PackFrom(connResp);
      }else{
          anyResp->PackFrom(er);
          Logging::debug(__FILE__, __LINE__,
                         "RDMAServer::handle: after connectQueue already connected");
      }

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

    } else if (anyReq->Is<RDMAConnDisconnect>()) {
      RDMAConnDisconnect disconnMsg;
      anyReq->UnpackTo(&disconnMsg);
      RDMA_API_T::disconnectQP(disconnMsg.nodeid());

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
    rdma_mem_t memRes = RDMA_API_T::internalAlloc(size);
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
      RDMA_API_T::internalFree(offset);
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

    unique_lock<mutex> lck(RDMAClient<RDMA_API_T>::m_connLock);
      NodeID nodeID = connRequest->nodeid();

      //check if client already called connect
      if (nodeID >= RDMAClient<RDMA_API_T>::m_NodeIDsQPs.size()) {
          RDMAClient<RDMA_API_T>::m_NodeIDsQPs.resize(nodeID + 1);
      }
      if(RDMAClient<RDMA_API_T>::m_NodeIDsQPs.at(nodeID) == false){
          RDMAClient<RDMA_API_T>::m_NodeIDsQPs[nodeID] = true;
      }else{
          if(nodeID ==RDMAClient<RDMA_API_T>::getOwnNodeID()) {
              //connecting to self is a problem at cleanup
              //cout << "connect to self" << endl;
              Logging::debug(
                      __FILE__, __LINE__,
                      "Connect to self. Will leak a QP right now " + to_string(nodeID));
          }

          if(nodeID < RDMAClient<RDMA_API_T>::getOwnNodeID() ){
              //back off if my nodeID is bigger then the calling
              lck.unlock();
              return false;
          }
      }
      //other server did not call connect yet

    // create local QP
    // Check if SRQ is active
    try
    {
      if (m_currentSRQ == SIZE_MAX) {
        Logging::debug(
            __FILE__, __LINE__,
            "RDMAServer: initializing queue pair - " + to_string(nodeID));
        RDMA_API_T::initQPWithSuppliedID(nodeID);
      } else {
        if (std::is_same<RDMA_API_T, ReliableRDMA>::value) {    
          Logging::debug(__FILE__, __LINE__,
                        "RDMAServer: initializing queue pair with srq id: " +
                            to_string(m_currentSRQ) + " - " + to_string(nodeID));
          reinterpret_cast<rdma::ReliableRDMA*>(this)->initQPForSRQWithSuppliedID(m_currentSRQ, nodeID);

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

  // NodeID requestNodeID(std::string sequencerIpPort, std::string ownIpPort) override
  // {    
  //   std::cout << "Server: requestNodeID" << std::endl;
  //   // check if client is connected to sequencer
  //   if (ProtoClient::isConnected(sequencerIpPort)) {
  //     return true;
  //   }
  //   ProtoClient::connectProto(sequencerIpPort);

  //   Any nodeIDRequest = ProtoMessageFactory::createNodeIDRequest(ownIpPort, ProtoServer::m_name, NodeType::Enum::SERVER);
  //   Any rcvAny;

  //   ProtoClient::exchangeProtoMsg(sequencerIpPort, &nodeIDRequest, &rcvAny);

  //   if (rcvAny.Is<NodeIDResponse>()) {
  //     NodeIDResponse connResponse;
  //     rcvAny.UnpackTo(&connResponse);
  //     return connResponse.nodeid();
  //   } else {
  //     Logging::error(__FILE__, __LINE__,
  //                    "RDMAServer could not request NodeID from NodeIDSequencer: received wrong response type");
  //     throw std::runtime_error("RDMAServer could not request NodeID from NodeIDSequencer: received wrong response type");
  //   }
  // }

  unordered_map<string, NodeID> m_mcastAddr;  // mcast_string to ibaddr

  // Locks for multiple clients accessing server

  mutex m_memLock;

  size_t m_currentSRQ = SIZE_MAX;

  std::string m_sequencerIpPort;
  // NodeID m_ownNodeID;

private:
  using ProtoClient::connectProto; //Make private
  using ProtoClient::exchangeProtoMsg; //Make private

};

}  // namespace rdma

#endif /* RDMAServer_H_ */
