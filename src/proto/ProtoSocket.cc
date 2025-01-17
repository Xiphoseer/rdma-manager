

#include "ProtoSocket.h"

#include <cerrno>

using namespace rdma;

ProtoSocket::ProtoSocket(string ip, int port, int sockType)
    : m_sockType(sockType), m_isOpen(false) {
  m_pCtx = new zmq::context_t(1, Config::PROTO_MAX_SOCKETS);

  m_pSock = new zmq::socket_t(*m_pCtx, m_sockType);
  int hwm = 0;
  int linger = 0; // after close how long unsent messages should be kept in memory
  m_pSock->setsockopt(ZMQ_SNDHWM, &hwm, sizeof(hwm));
  m_pSock->setsockopt(ZMQ_RCVHWM, &hwm, sizeof(hwm));
  m_pSock->setsockopt(ZMQ_LINGER, &linger, sizeof(linger));

  if (m_sockType == ZMQ_SUB) m_pSock->setsockopt(ZMQ_SUBSCRIBE, NULL, 0);

  m_conn = "tcp://" + ip + ":" + to_string(port);
}

ProtoSocket::~ProtoSocket() {
  if (m_isOpen) {
    this->close();
    this->closeContext();
  }
}

bool ProtoSocket::bind() {
  try {
    m_pSock->bind(m_conn.c_str());
    m_isOpen = true;
  } catch (zmq::error_t& e) {
    Logging::error(__FILE__, __LINE__, e.what());
    m_isOpen = false;
  }

  return m_isOpen;
}

bool ProtoSocket::connect() {
  try {
    m_pSock->connect(m_conn.c_str());
    m_isOpen = m_pSock->connected();
  } catch (zmq::error_t& e) {
    Logging::error(__FILE__, __LINE__, e.what());
    m_isOpen = false;
  }

  return m_isOpen;
}

bool ProtoSocket::isOpen() { return m_isOpen; }

bool ProtoSocket::sendMore(Any* msg) {
  if (msg == nullptr) return false;

  string* data = new string();
  if (!msg->SerializeToString(data)) {
    delete data;
    data = nullptr;
    return false;
  }

  zmq::message_t zmsg(data->size());
  memcpy(zmsg.data(), data->data(), data->size());
  bool send = m_pSock->send(&zmsg, ZMQ_SNDMORE);

  delete data;
  data = nullptr;
  return send > 0;
}

bool ProtoSocket::send(Any* msg) {
  if (msg == nullptr) return false;

  string* data = new string();
  if (!msg->SerializeToString(data)) {
    delete data;
    data = nullptr;
    return false;
  }

  zmq::message_t zmsg(data->size());
  memcpy(zmsg.data(), data->data(), data->size());
  bool send = false;
  try {
    send = m_pSock->send(zmsg);
  } catch (zmq::error_t& e) {
    Logging::error(__FILE__, __LINE__, e.what());
  }

  delete data;
  data = nullptr;
  return send;
}

bool ProtoSocket::receive(Any* msg) {
  if (msg == nullptr) return false;

  zmq::message_t zmsg;
  bool recv = false;
  try {
    recv = m_pSock->recv(&zmsg);
  } catch (zmq::error_t& e) {
    // recv() throws ETERM when the zmq context is destroyed
    // http://stackoverflow.com/questions/18811146/zmq-recv-is-blocking-even-after-the-context-was-terminated
    if (e.num() != ETERM) {
      Logging::fatal(__FILE__, __LINE__, e.what());
    } else if (m_isOpen) {
      // Logging::info("ZMQ Context has been closed, closing socket...");
      this->close();
    }
  }

  if (recv) {
    char* data = (char*)zmsg.data();
    recv = msg->ParseFromArray(data, zmsg.size());
  }

  return recv;
}

bool ProtoSocket::close() {
  // Calls the zmq_close() function, as described in zmq_close(3)
  try {
    delete m_pSock;
    m_pSock = nullptr;
    m_isOpen = false;
    return true;
  } catch (zmq::error_t& e) {
    Logging::error(__FILE__, __LINE__, e.what());
  }

  return false;
}

bool ProtoSocket::closeContext() {
  try {
    delete m_pCtx;
    m_pCtx = nullptr;
    return true;
  } catch (zmq::error_t& e) {
    Logging::error(__FILE__, __LINE__, e.what());
  }

  return false;
}

bool ProtoSocket::setOption(int option_name, const void *option_value, size_t option_len){
  int status = zmq_setsockopt(m_pSock, option_name, option_value, option_len);
 return status == 0;
}

int64_t ProtoSocket::getSendTimeout(){
  return this->m_send_timeout;
}

bool ProtoSocket::setSendTimeout(int64_t milliseconds){
  if(setOption(ZMQ_SNDTIMEO, &milliseconds, sizeof(milliseconds))){
    this->m_send_timeout = milliseconds;
    return true;
  } return false;
}

int64_t ProtoSocket::getRecvTimeout(){
  return this->m_recv_timeout;
}

bool ProtoSocket::setRecvTimeout(int64_t milliseconds){
  if(setOption(ZMQ_RCVTIMEO, &milliseconds, sizeof(milliseconds))){
    this->m_recv_timeout = milliseconds;
    return true;
  } return false;
}

bool ProtoSocket::hasConnection(){
  // if connection is closed this operation fails
  return setSendTimeout(this->m_send_timeout);
}
