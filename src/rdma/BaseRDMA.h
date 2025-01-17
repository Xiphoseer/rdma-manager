/**
 * @file BaseRDMA.h
 * @author cbinnig, tziegler
 * @date 2018-08-17
 */

#ifndef BaseRDMA_H_
#define BaseRDMA_H_

#include "../memory/LocalBaseMemoryStub.h"
#include "../memory/BaseMemory.h"
#include "../proto/ProtoClient.h"
#include "../utils/Config.h"

#include <infiniband/verbs.h>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <sys/mman.h>


namespace rdma {

enum rdma_transport_t { rc, ud };

/*
struct ib_resource_t {
  // Memory region 
  void *buffer;  replaced with m_buffer
  struct ibv_pd *pd; // PD handle
  struct ibv_mr *mr; // MR handle for buf

  //Device attributes
  struct ibv_device_attr device_attr;
  struct ibv_port_attr port_attr; // IB port attributes
  struct ibv_context *ib_ctx;     // device handle
};
*/

struct ib_qp_t {
  struct ibv_qp *qp;      /* Queue pair */
  struct ibv_cq *send_cq; /* Completion Queue */
  struct ibv_cq *recv_cq;

  ib_qp_t() : qp(nullptr), send_cq(nullptr), recv_cq(nullptr) {}
};

struct ib_conn_t {
  uint64_t buffer; /*  Buffer address */
  uint64_t qp_num; /*  QP number */
  uint16_t lid;    /*  LID of the IB port */
  uint8_t gid[16]; /* GID */

  struct {
    uint32_t rkey; /*  Remote memory key */
  } rc;
  struct {
    uint32_t psn;      /* PSN*/
    struct ibv_ah *ah; /* Route to remote QP*/
  } ud;
};

/* Moved into BaseMemory.h
struct rdma_mem_t {
  size_t size; // size of memory region
  bool free;
  size_t offset;
  bool isnull;

  rdma_mem_t(size_t initSize, bool initFree, size_t initOffset)
      : size(initSize), free(initFree), offset(initOffset), isnull(false) {}

  rdma_mem_t() : size(0), free(false), offset(0), isnull(true) {}
};
*/

class BaseRDMA {
 protected:
  typedef size_t rdmaConnID;  // Indexs m_qps

 public:
  // constructors and destructor
  BaseRDMA(BaseMemory *buffer);
  BaseRDMA(BaseMemory *buffer, bool pass_buffer_ownership);

  BaseRDMA(size_t mem_size=Config::RDMA_MEMSIZE);
  BaseRDMA(size_t mem_size, bool huge);
  BaseRDMA(size_t mem_size, int numaNode);
  BaseRDMA(size_t mem_size, bool huge, int numaNode);
  BaseRDMA(size_t mem_size, MEMORY_TYPE mem_type);
  /**
   * Parameters huge and numaNode are only used if memory type is MAIN
   */
  BaseRDMA(size_t mem_size, MEMORY_TYPE mem_type, bool huge, int numaNode);
  /**
   * Parameters huge and numaNode are only used if memory type is MAIN
   */
  BaseRDMA(size_t mem_size, int mem_type, bool huge, int numaNode);

  virtual ~BaseRDMA();

  // unicast transfer methods

  /* Function: send
   * ----------------
   * Sends data of a given array to the remote side. 
   * The remote side has to first call receive and 
   * then handle the incoming data.
   * 
   * rdmaConnID:  id of the remote
   * memAddr:     address of the local array containing the data 
   *              that should be sent
   * size:        how many bytes should be transfered
   * signaled:    if true the function blocks until the send has fully been 
   *              completed. Multiple sends can be called. 
   *              At max Config::RDMA_MAX_WR fetches can be performed at once 
   *              without signaled=true.
   */
  virtual void send(const rdmaConnID rdmaConnID, const void *memAddr,
                    size_t size, bool signaled) = 0;
  
  /* Function receive
   * ----------------
   * Receives data that has been sent. 
   * Must be called before the actual send gets executed. 
   * Multiple calls up to Config::RDMA_MAX_WR are allowed 
   * without a send call inbetween.
   * To actually wait for the data and receive it call 
   * pollReceive() afterwards.
   * 
   * rdmaConnID:  id of the remote
   * memAddr:     address of the local array where the received 
   *              data should be written into
   * size:        how many bytes are expected to receive
   * 
   */
  virtual void receive(const rdmaConnID rdmaConnID, const void *memAddr,
                       size_t size) = 0;
  
  /* Function receiveWriteImm
   * ----------------
   * Must be called before remote calls writeImm().
   * To wait until the actual write happended and to 
   * receive the sent immediate value call pollReceive() afterwards.
   * 
   * rdmaConnID:  id of the remote
   * 
   */
  void receiveWriteImm(const rdmaConnID rdmaConnID){
    receive(rdmaConnID, nullptr, 0);
  }
  
  /* Function: pollReceive
   * ----------------
   * Checks if data from a receive has arrived.
   * 
   * rdmaConnID:  id of the remote
   * doPoll:      if true then function blocks until 
   *              data has arrived
   * imm:         Pointer to variable where received immediate value
   *              can be stored or nullptr (optional)
   * return:      how many receives arrived or zero
   */
  virtual int pollReceive(const rdmaConnID rdmaConnID, bool doPoll = true, uint32_t* imm = nullptr) = 0;
  // virtual void pollReceive(const rdmaConnID rdmaConnID, uint32_t &ret_qp_num)
  // = 0;

  virtual void pollSend(const rdmaConnID rdmaConnID, bool doPoll = true, uint32_t *imm = nullptr) = 0;

  // unicast connection management
  virtual void initQPWithSuppliedID(const rdmaConnID suppliedID) = 0;
  virtual void initQPWithSuppliedID( struct ib_qp_t** qp ,struct ib_conn_t ** localConn) = 0;
  virtual void initQP(rdmaConnID &retRdmaConnID) = 0;

  virtual void connectQP(const rdmaConnID rdmaConnID) = 0;
  virtual void disconnectQP(const rdmaConnID rdmaConnID) = 0;

  uint64_t getQPNum(const rdmaConnID rdmaConnID) {
    return m_qps[rdmaConnID].qp->qp_num;
  }

  ib_conn_t getLocalConnData(const rdmaConnID rdmaConnID) {
    return m_lconns[rdmaConnID];
  }

  ib_conn_t getRemoteConnData(const rdmaConnID rdmaConnID) {
    return m_rconns[rdmaConnID];
  }

  void setRemoteConnData(const rdmaConnID rdmaConnID, ib_conn_t &conn);

  // memory management

  /* Function: localAlloc
   * ----------------
   * Allocates a memory part from the local buffer.
   * Typ of memory same as buffer type.
   * Use localMalloc() to abstract memory type
   * 
   * size:    how big the memory part should be in bytes
   * return:  pointer of memory part.
   *          Must be released with localFree().
   *          Memory type depends on buffer type
   */
  virtual void *localAlloc(const size_t &size) = 0;

  /* Function: localFree
   * ----------------
   * Releases an allocated memory part again.
   * 
   * ptr: pointer of the memory part
   * 
   */
  virtual void localFree(const void *ptr) = 0;

  /* Function: localFree
   * ----------------
   * Releases an allocated memory part again 
   * based of its offset in the buffer.
   * 
   * offset:  offset in the buffer to 
   *          the memory part
   * 
   */
  virtual void localFree(const size_t &offset) = 0;

  /* Function: localMalloc
   * ----------------
   * Allocates a memory part from the local buffer
   * 
   * size:    how big the memory part should be in bytes
   * return:  memory part handler object. 
   *          Delete it to release memory part again
   */
  LocalBaseMemoryStub *localMalloc(const size_t &size){
    return m_buffer->malloc(size);
  }

  /* Function: getBuffer
   * ----------------
   * Returns pointer of the local buffer.
   * Memory type of buffer must be known 
   * from context. 
   * Use getBufferObj() to abstract memory type
   * 
   * return:  pointer of local buffer
   */
  void *getBuffer() { return m_buffer->pointer(); }

  /* Function: getBufferObj
   * ----------------
   * Returns local buffer
   * 
   * return:  local buffer
   */
  BaseMemory *getBufferObj(){ return m_buffer; }

  const list<rdma_mem_t> getFreeMemList() const { return m_buffer->getFreeMemList(); }

  void *convertOffsetToPointer(size_t offset) {
    // check if already allocated
    return (void *)((char *)m_buffer->pointer() + offset);
  }

  size_t convertPointerToOffset(void* ptr) {
    // check if already allocated
    return (size_t)((char *)ptr - (char*) m_buffer->pointer());
  }

  size_t getBufferSize() { return m_buffer->getSize(); }

  void printBuffer();

  std::vector<size_t> getConnectedConnIDs() {
    std::vector<size_t> connIDs;
    for (auto iter = m_connected.begin(); iter != m_connected.end(); iter++)
    {
      if (iter->second)
        connIDs.push_back(iter->first);
    }
    return connIDs;
  }

 protected:
  virtual void destroyQPs() = 0;

  rdma_mem_t internalAlloc(const size_t &size);

  void internalFree(const size_t &offset);

  uint64_t nextConnKey() { return m_qps.size(); }

  void setQP(const rdmaConnID rdmaConnID, ib_qp_t &qp);

  void setLocalConnData(const rdmaConnID rdmaConnID, ib_conn_t &conn);

  void createCQ(ibv_cq *&send_cq, ibv_cq *&rcv_cq);
  void destroyCQ(ibv_cq *&send_cq, ibv_cq *&rcv_cq);
  virtual void createQP(struct ib_qp_t *qp) = 0;

  inline void __attribute__((always_inline))
  checkSignaled(bool &signaled, rdmaConnID rdmaConnID) {
    if (signaled) 
    {
      m_countWR[rdmaConnID] = 0;
      return;
    }
    ++m_countWR[rdmaConnID];
    if (m_countWR[rdmaConnID] == Config::RDMA_MAX_WR) {
      signaled = true;
      m_countWR[rdmaConnID] = 0;
    }
  }

  vector<size_t> m_countWR;

  ibv_qp_type m_qpType;
  BaseMemory *m_buffer;
  bool m_buffer_owner = false;
  int m_gidIdx = -1;

  vector<ib_qp_t> m_qps;  // rdmaConnID is the index of the vector
  vector<ib_conn_t> m_rconns;
  vector<ib_conn_t> m_lconns;

  unordered_map<uint64_t, bool> m_connected;
  unordered_map<uint64_t, rdmaConnID> m_qpNum2connID;

};

}  // namespace rdma

#endif /* BaseRDMA_H_ */
