

#include "ProtoServer.h"
#include "../utils/Network.h"

using namespace rdma;

ProtoServer::ProtoServer(string name, int port, std::string ip)
    : m_name(name), m_port(port), m_ip(ip), m_running(false), m_pSocket(nullptr) {
      
}

ProtoServer::~ProtoServer() {
  if (isRunning())
  {
    stopServer();
  }
  if (m_pSocket != nullptr) {
    delete m_pSocket;
    m_pSocket = nullptr;
  }
}

bool ProtoServer::startServer() {
  if (isRunning())
  {
    return true;
  }
  m_pSocket = new ProtoSocket(m_ip, m_port, ZMQ_REP);
  start();

  stringstream ss;
  while (!m_running) {
    if (killed()) {
      ss << m_name << " starting failed";
      Logging::error(__FILE__, __LINE__, ss.str());
      return false;
    }
    ss << m_name << " starting done. ip: " + m_ip + " port: " + to_string(m_port) + " \n";
    Logging::debug(__FILE__, __LINE__, ss.str());
    usleep(Config::RDMA_SLEEP_INTERVAL);
  }

  return true;
}

void ProtoServer::run() {
  if (!m_pSocket->bind()) {
    stringstream ss;
    ss << "Could not bind port " << m_pSocket;
    Logging::error(__FILE__, __LINE__, ss.str());
    stop();
    return;
  }
  m_running = true;

  while (!killed()) {


    Any rcvMsg;
    Any respMsg;

    unique_lock<mutex> lck(m_handleLock);
    if (!m_pSocket->receive(&rcvMsg)) {
      break;
    } else {
      handle(&rcvMsg, &respMsg);
      m_pSocket->send(&respMsg);
    }
    m_handleLock.unlock();
  }
    m_running = false;
}

bool ProtoServer::isRunning() { return m_running; }

void ProtoServer::stopServer() {

  stringstream ss;

  if (m_running) {

    // m_pSocket->close();
    stop();
    m_pSocket->closeContext();
    join();
  }
  ss << m_name << " stopping done \n";
  Logging::debug(__FILE__, __LINE__, ss.str());
}
