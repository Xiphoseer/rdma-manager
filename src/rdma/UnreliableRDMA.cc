#include "UnreliableRDMA.h"

#ifndef HUGEPAGE
#define HUGEPAGE false
#endif

using namespace rdma;

UnreliableRDMA::UnreliableRDMA(size_t mem_size) : UnreliableRDMA(mem_size, HUGEPAGE){}
UnreliableRDMA::UnreliableRDMA(size_t mem_size, bool huge) : UnreliableRDMA(mem_size, huge, (int)Config::RDMA_NUMAREGION){}
UnreliableRDMA::UnreliableRDMA(size_t mem_size, int numaNode) : UnreliableRDMA(mem_size, HUGEPAGE, numaNode){}
UnreliableRDMA::UnreliableRDMA(size_t mem_size, bool huge, int numaNode) : UnreliableRDMA(mem_size, MEMORY_TYPE::MAIN, huge, numaNode){}
UnreliableRDMA::UnreliableRDMA(size_t mem_size, MEMORY_TYPE mem_type) : UnreliableRDMA(mem_size, (int)mem_type, HUGEPAGE, (int)Config::RDMA_NUMAREGION){}
UnreliableRDMA::UnreliableRDMA(size_t mem_size, MEMORY_TYPE mem_type, bool huge, int numaNode) : UnreliableRDMA(mem_size, (int)mem_type, huge, numaNode){}
UnreliableRDMA::UnreliableRDMA(size_t mem_size, int mem_type, bool huge, int numaNode) : BaseRDMA(mem_size, mem_type, huge, numaNode) {
  m_qpType = IBV_QPT_UD;
  m_lastMCastConnKey = 0;

  initQPWithSuppliedID(0);
}
UnreliableRDMA::UnreliableRDMA(BaseMemory *buffer) : BaseRDMA(buffer) {
  m_qpType = IBV_QPT_UD;
  m_lastMCastConnKey = 0;

  initQPWithSuppliedID(0);
}


UnreliableRDMA::~UnreliableRDMA() {
  rdmaConnID mcastID = 0;
  for(auto& mcastConn : m_udpMcastConns){
    (void)mcastConn;
    leaveMCastGroup(mcastID);
    mcastID++;
  }
  // destroy QPS
  destroyQPs();
  m_qps.clear();
}

void* UnreliableRDMA::localAlloc(const size_t& size) {
  rdma_mem_t memRes = m_buffer->internalAlloc(size + Config::RDMA_UD_OFFSET);
  if (!memRes.isnull) {
    return ((char*)m_buffer->pointer() + memRes.offset + Config::RDMA_UD_OFFSET);
  }
  throw runtime_error("UnreliableRDMA allocating local memory failed! Size: " + to_string(size));
}

void UnreliableRDMA::localFree(const size_t& offset) { m_buffer->free(offset); }

void UnreliableRDMA::localFree(const void* ptr) {
  char* begin = (char*)m_buffer->pointer();
  char* end = (char*)ptr;
  size_t offset = (end - begin) - Config::RDMA_UD_OFFSET;
  m_buffer->free(offset);
}

void UnreliableRDMA::initQPWithSuppliedID(const rdmaConnID rdmaConnID) {
  //TODO: Refactor such that the only QP does not need to be indexed with the rdmaConnID (Since there is only one!!)

  unique_lock<mutex> lck(m_cqCreateLock);
  // check if QP is already created
  if (m_udqp.qp != nullptr) {
    setQP(rdmaConnID, m_udqp);
    setLocalConnData(rdmaConnID, m_udqpConn);
    lck.unlock();
    return;
  }

  ib_qp_t* qp = &m_udqp;
  ib_conn_t* qpConn = &m_udqpConn;
  // create completion queues
  createCQ(qp->send_cq, qp->recv_cq);

  // create QP
  createQP(qp);

  // create local connection data
  union ibv_gid my_gid;
  memset(&my_gid, 0, sizeof my_gid);
  qpConn->buffer = (uintptr_t)m_buffer->pointer();
  qpConn->qp_num = m_udqp.qp->qp_num;
  qpConn->lid = m_buffer->ib_port_attributes().lid;
  memcpy(qpConn->gid, &my_gid, sizeof my_gid);
  qpConn->ud.psn = lrand48() & 0xffffff;
  qpConn->ud.ah = nullptr;

  // init queue pair
  modifyQPToInit(qp->qp);

  modifyQPToRTR(qp->qp);

  modifyQPToRTS(qp->qp, qpConn->ud.psn);

  // done
  setQP(rdmaConnID, *qp);
  setLocalConnData(rdmaConnID, *qpConn);
  lck.unlock();

  Logging::debug(__FILE__, __LINE__, "Created UD queue pair ");
}


void UnreliableRDMA::initQPWithSuppliedID(ib_qp_t** qpp, ib_conn_t** localcon) {

    unique_lock<mutex> lck(m_cqCreateLock);
    // check if QP is already created
    if (m_udqp.qp != nullptr) {
        *qpp =  & m_udqp;
        *localcon = &m_udqpConn;
        lck.unlock();

        return;
    }

    ib_qp_t* qp = &m_udqp;
    ib_conn_t* qpConn = &m_udqpConn;
    // create completion queues
    createCQ(qp->send_cq, qp->recv_cq);

    // create QP
    createQP(qp);


    // create local connection data
    union ibv_gid my_gid;
    memset(&my_gid, 0, sizeof my_gid);
    qpConn->buffer = (uintptr_t)m_buffer->pointer();
    qpConn->qp_num = m_udqp.qp->qp_num;
    qpConn->lid = m_buffer->ib_port_attributes().lid;
    memcpy(qpConn->gid, &my_gid, sizeof my_gid);
    qpConn->ud.psn = lrand48() & 0xffffff;
    qpConn->ud.ah = nullptr;

    // init queue pair
    modifyQPToInit(qp->qp);

    modifyQPToRTR(qp->qp);

    modifyQPToRTS(qp->qp, qpConn->ud.psn);


    *qpp = &m_udqp;
    *localcon = &m_udqpConn;

    Logging::debug(__FILE__, __LINE__, "Created UD queue pair ");

    lck.unlock();
}

void UnreliableRDMA::initQP(rdmaConnID& retRdmaConnID) {
  // assign new QP number
  retRdmaConnID = nextConnKey();
  initQPWithSuppliedID(retRdmaConnID);
}

void UnreliableRDMA::connectQP(const rdmaConnID rdmaConnID) {
  std::unique_lock<std::mutex> lck(m_qpLock);
  // if QP is connected return
  if (m_connected.find(rdmaConnID) != m_connected.end() && !m_connected[rdmaConnID]) {
    return;
  }

  // create address handle
  struct ibv_ah_attr ah_attr;
  memset(&ah_attr, 0, sizeof ah_attr);
  ah_attr.is_global = 0;
  ah_attr.dlid = m_rconns[rdmaConnID].lid;
  ah_attr.sl = 0;
  ah_attr.src_path_bits = 0;
  ah_attr.port_num = m_buffer->getIBPort();
  struct ibv_ah* ah = ibv_create_ah(m_buffer->ib_pd(), &ah_attr);
  m_rconns[rdmaConnID].ud.ah = ah;

  m_connected[rdmaConnID] = true;
  Logging::debug(__FILE__, __LINE__, "Connected UD queue pair!");
}

void UnreliableRDMA::disconnectQP(const rdmaConnID rdmaConnID){
  std::unique_lock<std::mutex> lck(m_qpLock);

  if (m_connected.find(rdmaConnID) != m_connected.end() && m_connected[rdmaConnID])
  {
    ibv_destroy_ah(m_rconns[rdmaConnID].ud.ah);
    m_connected[rdmaConnID] = false;
  }
}

void UnreliableRDMA::destroyQPs() {
  std::unique_lock<std::mutex> lck(m_qpLock);

  if (m_udqp.qp != nullptr) {
    if (ibv_destroy_qp(m_udqp.qp) != 0) {
      throw runtime_error("Error, ibv_destroy_qp() failed while destroying QPs");
    }

    destroyCQ(m_udqp.send_cq, m_udqp.recv_cq);
    m_udqp.qp = nullptr;
  }

  if (m_udqpMgmt.qp != nullptr) {
    if (ibv_destroy_qp(m_udqpMgmt.qp) != 0) {
      throw runtime_error("Error, ibv_destroy_qp() failed while destroying QPs");
    }
    m_udqpMgmt.qp = nullptr;
  }
}

void UnreliableRDMA::send(const rdmaConnID rdmaConnID, const void* memAddr,
                          size_t size, bool signaled) {

  //todo check signaled 
  checkSignaled(signaled, 0);

  struct ib_qp_t localQP = m_udqp; //m_qps[rdmaConnID]; 
  struct ib_conn_t remoteConn = m_rconns[rdmaConnID];

  struct ibv_send_wr sr;
  struct ibv_sge sge;
  memset(&sge, 0, sizeof(sge));
  sge.addr = (uintptr_t)memAddr;
  sge.lkey = m_buffer->ib_mr()->lkey;
  sge.length = size;
  memset(&sr, 0, sizeof(sr));
  sr.sg_list = &sge;
  sr.num_sge = 1;
  sr.opcode = IBV_WR_SEND;
  sr.next = NULL;

  sr.wr.ud.ah = remoteConn.ud.ah;
  sr.wr.ud.remote_qpn = remoteConn.qp_num;
  sr.wr.ud.remote_qkey = 0x11111111;  // remoteConn.ud.qkey;
  sr.send_flags = (signaled) ? IBV_SEND_SIGNALED : 0;

  struct ibv_send_wr* bad_wr = NULL;
  if ((errno = ibv_post_send(localQP.qp, &sr, &bad_wr)) != 0) {
    throw runtime_error("SEND not successful! errno: " + std::string(std::strerror(errno)));
  }

  int ne = 0;
  if (signaled) {
    struct ibv_wc wc;
    do {
      wc.status = IBV_WC_SUCCESS;
      ne = ibv_poll_cq(localQP.send_cq, 1, &wc);

      if (wc.status != IBV_WC_SUCCESS) {
        throw runtime_error("RDMA completion event in CQ with error! " + to_string(wc.status) + " errno: " + std::string(std::strerror(errno)));
      }

    } while (ne == 0);

    if (ne < 0) {
      throw runtime_error("RDMA polling from CQ failed!");
    }
  }
}

void UnreliableRDMA::receive(const rdmaConnID, const void* memAddr,
                             size_t size) {
  // struct ib_qp_t localQP = m_qps[rdmaConnID]; //m_udqp
  struct ib_qp_t localQP = m_udqp;

  struct ibv_sge sge;
  struct ibv_recv_wr wr;
  struct ibv_recv_wr* bad_wr;

  memset(&sge, 0, sizeof(sge));
  sge.addr = (uintptr_t)(((char*)memAddr) - Config::RDMA_UD_OFFSET);
  sge.length = size + Config::RDMA_UD_OFFSET;
  sge.lkey = m_buffer->ib_mr()->lkey;

  memset(&wr, 0, sizeof(wr));
  wr.wr_id = 0;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.next = nullptr;

  if ((errno = ibv_post_recv(localQP.qp, &wr, &bad_wr)) != 0) {
    throw runtime_error("RECV has not been posted successfully in receive()! errno: " + std::string(std::strerror(errno)));
  }
}

//Ignore rdmaConnID
int UnreliableRDMA::pollReceive(const rdmaConnID, bool doPoll,uint32_t* imm) {
  int ne;
  struct ibv_wc wc;

  struct ib_qp_t localQP = m_udqp;

  do {
    wc.status = IBV_WC_SUCCESS;
    ne = ibv_poll_cq(localQP.recv_cq, 1, &wc);

    if (wc.status != IBV_WC_SUCCESS) {
      throw runtime_error("RDMA completion event in CQ with error! " + to_string(wc.status));
    }
  } while (ne == 0 && doPoll);

  if (ne < 0) {
    throw runtime_error("RDMA polling from CQ failed!");
  }
  if(imm!= nullptr){
      * imm =wc.imm_data;
  }

  return ne;
}

void UnreliableRDMA::pollSend(const rdmaConnID, bool doPoll, uint32_t *imm) {
  int ne;
  struct ibv_wc wc;

  // struct ib_qp_t localQP = m_qps[rdmaConnID]; //m_udqp.qp
  struct ib_qp_t localQP = m_udqp;

  do {
    wc.status = IBV_WC_SUCCESS;
    ne = ibv_poll_cq(localQP.send_cq, 1, &wc);

    if (wc.status != IBV_WC_SUCCESS) {
      throw runtime_error("RDMA completion event in CQ with error! " + to_string(wc.status));
    }
  } while (ne == 0 && doPoll);

  if(imm != nullptr && ne > 0){
      *imm = wc.imm_data;
  }

  if (doPoll) {
    if (ne < 0) {
      throw runtime_error("RDMA polling from CQ failed!");
    }
    return;
  } else if (ne > 0) {
    return;
  }
  throw runtime_error("pollSend failed!");
}

void UnreliableRDMA::joinMCastGroup(string mCastAddress,
                                    rdmaConnID& retRdmaConnID) {
  retRdmaConnID = nextMCastConnKey();

  rdma_mcast_conn_t mCastConn;
  mCastConn.mcast_addr = const_cast<char*>(mCastAddress.c_str());

  // create event channel
  mCastConn.channel = rdma_create_event_channel();
  if (!mCastConn.channel) {
    throw runtime_error("Could not create event channel for multicast!");
  }

  // create connection
  if (rdma_create_id(mCastConn.channel, &mCastConn.id, NULL, RDMA_PS_UDP) !=
      0) {
    throw runtime_error("Could not create connection for multicast!");
  }

  // resolve multicast address
  rdma_addrinfo* mcast_rai = nullptr;
  rdma_addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_port_space = RDMA_PS_UDP;
  hints.ai_flags = 0;
  if (rdma_getaddrinfo(mCastConn.mcast_addr, nullptr, &hints, &mcast_rai) != 0) {
    throw runtime_error("Could not resolve info for multicast address (1)!");
  }

  if (rdma_resolve_addr(mCastConn.id, nullptr, mcast_rai->ai_dst_addr, 2000) !=
      0) {
    throw runtime_error("Could not resolve info for multicast address (2)!");
  }

  getCmEvent(mCastConn.channel, RDMA_CM_EVENT_ADDR_RESOLVED, nullptr);
  memcpy(&mCastConn.mcast_sockaddr, mcast_rai->ai_dst_addr, sizeof(struct sockaddr));
  rdma_freeaddrinfo(mcast_rai);


    // create protection domain
  mCastConn.pd = ibv_alloc_pd(mCastConn.id->verbs);
  if (!mCastConn.pd) {
    throw runtime_error("Could not create multicast protection domain!");
  }

  mCastConn.mr =
      ibv_reg_mr(mCastConn.pd, m_buffer->pointer(), m_buffer->getSize(), IBV_ACCESS_LOCAL_WRITE);
  if (!mCastConn.mr) {
    throw runtime_error("Could not assign memory region to multicast protection domain!");
  }

  // create multicast queues
  ibv_qp_init_attr attr;
  memset(&attr, 0, sizeof(attr));

  mCastConn.scq = ibv_create_cq(mCastConn.id->verbs, Config::RDMA_MAX_WR + 1,
                                nullptr, nullptr, 0);
  mCastConn.rcq = ibv_create_cq(mCastConn.id->verbs, Config::RDMA_MAX_WR + 1,
                                nullptr, nullptr, 0);
  if (!mCastConn.scq || !mCastConn.rcq) {
    throw runtime_error("Could not create multicast completion queues!");
  }

  attr.qp_type = IBV_QPT_UD;
  attr.send_cq = mCastConn.scq;
  attr.recv_cq = mCastConn.rcq;
  attr.cap.max_send_wr = Config::RDMA_MAX_WR;
  attr.cap.max_recv_wr = Config::RDMA_MAX_WR;
  attr.cap.max_send_sge = Config::RDMA_MAX_SGE;
  attr.cap.max_recv_sge = Config::RDMA_MAX_SGE;
  attr.cap.max_inline_data = Config::MAX_UD_INLINE_SEND;

  if (rdma_create_qp(mCastConn.id, mCastConn.pd, &attr) != 0) {
    throw runtime_error("Could not create multicast queue pairs!");
  }

  // join multicast group
  if (rdma_join_multicast(mCastConn.id, &mCastConn.mcast_sockaddr, nullptr) !=
      0) {
    throw runtime_error("Could not join multicast group (1)!");
  }

  // verify that we successfully joined the multicast group
  rdma_cm_event* event;
  getCmEvent(mCastConn.channel, RDMA_CM_EVENT_MULTICAST_JOIN, &event);

  mCastConn.remote_qpn = event->param.ud.qp_num;
  mCastConn.remote_qkey = event->param.ud.qkey;
  mCastConn.ah = ibv_create_ah(m_buffer->ib_pd(), &event->param.ud.ah_attr);
  if (!mCastConn.ah) {
    throw runtime_error("Could not join multicast address handle!");
  }
  rdma_ack_cm_event(event);

  mCastConn.active = true;

  // done
  setMCastConn(retRdmaConnID, mCastConn);

}

void UnreliableRDMA::leaveMCastGroup(const rdmaConnID rdmaConnID) {
  if(m_udpMcastConns.empty()){
    return;
  }

  rdma_mcast_conn_t &mCastConn = m_udpMcastConns[rdmaConnID];

  if (!mCastConn.active)
    return;

  // leave group
  if (rdma_leave_multicast(mCastConn.id, &mCastConn.mcast_sockaddr) != 0) {
    throw runtime_error("Did not leave rdma multicast successfully. rdmaConnID: " + to_string(rdmaConnID));
  }

  // destroy resources
  if (mCastConn.ah) ibv_destroy_ah(mCastConn.ah);
  if (mCastConn.id && mCastConn.id->qp) rdma_destroy_qp(mCastConn.id);
  if (mCastConn.scq) ibv_destroy_cq(mCastConn.scq);
  if (mCastConn.rcq) ibv_destroy_cq(mCastConn.rcq);
  if (mCastConn.mr) rdma_dereg_mr(mCastConn.mr);
  if (mCastConn.pd) ibv_dealloc_pd(mCastConn.pd);
  if (mCastConn.id) rdma_destroy_id(mCastConn.id);
  if (mCastConn.channel) rdma_destroy_event_channel(mCastConn.channel);
  
  mCastConn.active = false;
}

void UnreliableRDMA::sendMCast(const rdmaConnID rdmaConnID, const void* memAddr,
                               size_t size, bool signaled) {
  rdma_mcast_conn_t mCastConn = m_udpMcastConns[rdmaConnID];
  checkSignaledMCast(signaled, rdmaConnID);

  struct ibv_send_wr wr, *bad_wr;
  struct ibv_sge sge;
  sge.length = size;
  sge.lkey = mCastConn.mr->lkey;
  sge.addr = (uintptr_t)memAddr;

  wr.next = nullptr;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.opcode = IBV_WR_SEND_WITH_IMM;
  wr.send_flags = (signaled) ? IBV_SEND_SIGNALED : 0 | (size < Config::MAX_UD_INLINE_SEND ? IBV_SEND_INLINE : 0);
  wr.wr_id = 0;
  wr.imm_data = htonl(mCastConn.id->qp->qp_num);
  wr.wr.ud.ah = mCastConn.ah;
  wr.wr.ud.remote_qpn = mCastConn.remote_qpn;
  wr.wr.ud.remote_qkey = mCastConn.remote_qkey;
  size_t ret = ibv_post_send(mCastConn.id->qp, &wr, &bad_wr);
  if (ret != 0) {
    throw runtime_error("Sending multicast data failed (error: " + to_string(ret) + ")");
  }

  int ne = 0;
  if (signaled) {
    struct ibv_wc wc;
    do {
      wc.status = IBV_WC_SUCCESS;
      ne = ibv_poll_cq(mCastConn.scq, 1, &wc);

      if (wc.status != IBV_WC_SUCCESS) {
        throw runtime_error("RDMA completion event in multicast CQ with error! " +
                           to_string(wc.status));
      }
    } while (ne == 0);

    if (ne < 0) {
      throw runtime_error("RDMA polling from multicast CQ failed!");
    }
  }
}

void UnreliableRDMA::receiveMCast(const rdmaConnID rdmaConnID,
                                  const void* memAddr, size_t size) {
  rdma_mcast_conn_t mCastConn = m_udpMcastConns[rdmaConnID];

  void* buffer = (void*)(((char*)memAddr) - Config::RDMA_UD_OFFSET);
  
  assert(buffer > m_buffer->pointer() || (char *)buffer + size < (char *)m_buffer->pointer() + m_buffer->ib_mr()->length);
  
  if (rdma_post_recv(mCastConn.id, nullptr, buffer, size + Config::RDMA_UD_OFFSET, mCastConn.mr) != 0) {
    throw runtime_error("Receiving multicast data failed");
  }
}

int UnreliableRDMA::pollReceiveMCast(const rdmaConnID rdmaConnID, bool doPoll) {
  rdma_mcast_conn_t mCastConn = m_udpMcastConns[rdmaConnID];
  int ne = 0;
  struct ibv_wc wc;
  do {
    wc.status = IBV_WC_SUCCESS;
    ne = ibv_poll_cq(mCastConn.rcq, 1, &wc);

    if (wc.status != IBV_WC_SUCCESS) {
      throw runtime_error("RDMA completion event in multicast CQ with error! " +
                         to_string(wc.status));
    }

  } while (ne == 0 && doPoll);

  if (ne < 0) {
    throw runtime_error("RDMA polling from multicast CQ failed!");
  }

  return ne;
}

/********** private methods **********/
void UnreliableRDMA::createQP(struct ib_qp_t* qp) {
  // initialize QP attributes
  struct ibv_qp_init_attr qp_init_attr;
  memset(&qp_init_attr, 0, sizeof(qp_init_attr));

  qp_init_attr.send_cq = qp->send_cq;
  qp_init_attr.recv_cq = qp->recv_cq;
  qp_init_attr.sq_sig_all =
      0;  // In every WR, it must be decided whether to generate a WC or not
  qp_init_attr.srq = NULL;
  qp_init_attr.qp_type = m_qpType;

  qp_init_attr.cap.max_send_wr = Config::RDMA_MAX_WR;
  qp_init_attr.cap.max_recv_wr = Config::RDMA_MAX_WR;
  qp_init_attr.cap.max_send_sge = Config::RDMA_MAX_SGE;
  qp_init_attr.cap.max_recv_sge = Config::RDMA_MAX_SGE;

  // create queue pair
  if (!(qp->qp = ibv_create_qp(m_buffer->ib_pd(), &qp_init_attr))) {
    throw runtime_error("Cannot create queue pair!");
  }
}

void UnreliableRDMA::modifyQPToInit(struct ibv_qp* qp) {
  int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY;
  struct ibv_qp_attr attr;

  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_INIT;
  attr.port_num = m_buffer->getIBPort();
  attr.pkey_index = 0;
  attr.qkey = 0x11111111;

  if ((errno = ibv_modify_qp(qp, &attr, flags)) > 0) {
    throw runtime_error("Failed modifyQPToInit!");
  }
}

void UnreliableRDMA::modifyQPToRTR(struct ibv_qp* qp) {
  struct ibv_qp_attr attr;
  int flags = IBV_QP_STATE;
  memset(&attr, 0, sizeof(attr));

  attr.qp_state = IBV_QPS_RTR;

  if ((errno = ibv_modify_qp(qp, &attr, flags)) > 0) {
    throw runtime_error("Failed modifyQPToRTR!");
  }
}

void UnreliableRDMA::modifyQPToRTS(struct ibv_qp* qp, const uint32_t psn) {
  struct ibv_qp_attr attr;
  int flags = IBV_QP_STATE | IBV_QP_SQ_PSN;
  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_RTS;
  attr.sq_psn = psn;

  if ((errno = ibv_modify_qp(qp, &attr, flags)) > 0) {
    throw runtime_error("Failed modifyQPToRTS!");
  }
}

void UnreliableRDMA::getCmEvent(struct rdma_event_channel* channel,
                                enum rdma_cm_event_type type,
                                struct rdma_cm_event** out_ev) {
  struct rdma_cm_event* event = NULL;
  if (rdma_get_cm_event(channel, &event) != 0) {
    throw runtime_error("rdma_get_cm_event failed!");
  }
  /* Verify the event is the expected type */
  if (event->event != type) {
    throw runtime_error("rdma_get_cm_event returned event did not match type! received: " + to_string(event->event) + " expected: " + to_string(type));
  }
  /* Pass the event back to the user if requested */
  if (!out_ev) {
    rdma_ack_cm_event(event);
  } else {
    *out_ev = event;
  }
}

void UnreliableRDMA::setMCastConn(const rdmaConnID rdmaConnID,
                                  rdma_mcast_conn_t& conn) {
  if (m_udpMcastConns.size() < rdmaConnID + 1) {
    m_udpMcastConns.resize(rdmaConnID + 1);
    m_sendMCastCount.resize(rdmaConnID + 1);
  }
  m_udpMcastConns[rdmaConnID] = conn;
}



  inline void __attribute__((always_inline))
  UnreliableRDMA::checkSignaledMCast(bool &signaled, rdmaConnID rdmaConnID) {
    if (signaled) 
    {
      m_sendMCastCount[rdmaConnID] = 0;
      return;
    }
    ++m_sendMCastCount[rdmaConnID];
    if (m_sendMCastCount[rdmaConnID] == Config::RDMA_MAX_WR) {
      signaled = true;
      m_sendMCastCount[rdmaConnID] = 0;
    }
  }