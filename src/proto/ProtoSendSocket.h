/**
 * @file ProtoClient.h
 * @author cbinnig, tziegler
 * @date 2018-08-17
 */

#ifndef NET_PROTOCLIENT_H
#define NET_PROTOCLIENT_H

#include "../message/ProtoMessageFactory.h"
#include "../utils/Config.h"
#include "../utils/Logging.h"
#include "ProtoSocket.h"

using google::protobuf::Any;
namespace rdma {

class ProtoSendSocket {
 public:
  ProtoSendSocket(string address, int port);
  virtual ~ProtoSendSocket();
  void connect();
  void send(Any* sendMsg);
  void send(Any* sendMsg, Any* recMsg);

  int getPort() { return m_port; }

  bool setOption(int option_name, const void *option_value, size_t option_len = sizeof(int));

  int64_t getSendTimeout();

  bool setSendTimeout(int64_t milliseconds = -1);

  int64_t getRecvTimeout();

  bool setRecvTimeout(int64_t milliseconds = -1);

  bool hasConnection();
  

 private:
  string m_address;
  int m_port;
  ProtoSocket* m_pSocket;
  bool m_isConnected;
};

}  // end namespace rdma

#endif /* CLIENT_H_ */
