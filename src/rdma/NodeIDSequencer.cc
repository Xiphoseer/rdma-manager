#include "NodeIDSequencer.h"

using namespace rdma;

NodeIDSequencer::NodeIDSequencer(/* args */) : ProtoServer("NodeIDSequencer", Config::SEQUENCER_PORT)
{

}

NodeIDSequencer::~NodeIDSequencer()
{

}

NodeID NodeIDSequencer::getNextNodeID()
{
  return m_nextNodeID++; //indexing entries vector relies on nodeIDs being incremented by 1!
}

void NodeIDSequencer::handle(Any *anyReq, Any *anyResp)
{
  if (anyReq->Is<NodeIDRequest>()) {
    NodeIDResponse connResp;
    NodeIDRequest connReq;
    anyReq->UnpackTo(&connReq);
    std::string IP = connReq.ip();
    std::string name = connReq.name();
    NodeType::Enum nodeType = (NodeType::Enum)connReq.node_type_enum();
    NodeID newNodeID = getNextNodeID();
    
    NodeEntry_t entry{IP, name, newNodeID, nodeType};
    m_entries.emplace_back(entry);

    if (nodeType == NodeType::Enum::SERVER)
    {
      m_ipPortToNodeIDMapping[IP] = newNodeID;
    }
    connResp.set_nodeid(newNodeID);
    connResp.set_return_(MessageErrors::NO_ERROR);

    anyResp->PackFrom(connResp);
  } 
  else if (anyReq->Is<GetAllNodeIDsRequest>()) 
  {
    GetAllNodeIDsResponse connResp;
    GetAllNodeIDsRequest connReq;
    anyReq->UnpackTo(&connReq);
    
    for (auto &entry : m_entries)
    {
      auto nodeidEntry = connResp.add_nodeid_entries();
      nodeidEntry->set_name(entry.name);
      nodeidEntry->set_ip(entry.IP);
      nodeidEntry->set_node_id(entry.nodeID);
      nodeidEntry->set_node_type_enum(entry.nodeType);
    }
    connResp.set_return_(MessageErrors::NO_ERROR);

    anyResp->PackFrom(connResp);
  }
  else if (anyReq->Is<GetNodeIDForIpPortRequest>())
  {
    GetNodeIDForIpPortResponse connResp;
    GetNodeIDForIpPortRequest connReq;
    anyReq->UnpackTo(&connReq);

    std::string ipPort = connReq.ipport();

    if (m_ipPortToNodeIDMapping.find(ipPort) != m_ipPortToNodeIDMapping.end())
    {
      NodeID nodeId = m_ipPortToNodeIDMapping[ipPort];
      auto entry = m_entries[nodeId];
      connResp.set_ip(entry.IP);
      connResp.set_name(entry.name);
      connResp.set_node_id(entry.nodeID);
      connResp.set_node_type_enum(entry.nodeType);
      connResp.set_return_(MessageErrors::NO_ERROR);
    }
    else
    {
      connResp.set_return_(MessageErrors::NODEID_NOT_FOUND);
    }
    anyResp->PackFrom(connResp);
  }
}
