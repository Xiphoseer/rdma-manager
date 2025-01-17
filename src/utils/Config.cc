

#include "Config.h"
#include "Logging.h"
#include <cmath>

using namespace rdma;

Config::Config(const std::string &exec_path) : Config(exec_path, true){}
Config::Config(const std::string &file_path, bool is_exec_path){
    Config::load(file_path, is_exec_path);
    auto num_cpu_cores = 0;
    auto num_numa_nodes = 0;
    
    NUMA_THREAD_CPUS = CpuNumaUtils::get_cpu_numa_map(num_cpu_cores, num_numa_nodes);

    setenv("MLX5_SINGLE_THREADED", to_string(Config::MLX5_SINGLE_THREADED).c_str(), true);
}

Config::~Config()
{
    Config::unload();
}


//TEST
int Config::HELLO_PORT = 4001;


//RDMA
size_t Config::RDMA_MEMSIZE = 1024ul * 1024 * 1024 * 8;  //1GB
uint32_t Config::RDMA_NUMAREGION = 1;
std::string Config::RDMA_DEVICE_FILE_PATH;
uint32_t Config::RDMA_IBPORT = 1;
std::string Config::RDMA_SERVER_ADDRESSES = "172.18.94.20"; // ip node02 RDMA_INTERFACEs
uint16_t Config::RDMA_PORT = 5200;
uint32_t Config::RDMA_MAX_WR = 4096;

uint32_t Config::RDMA_UD_MTU = 4096;

std::string Config::SEQUENCER_IP = "192.168.94.21"; //node02
uint16_t Config::SEQUENCER_PORT = 5600;
uint32_t Config::RDMA_GET_NODE_ID_RETRIES = 5; // 50

std::string Config::RDMA_INTERFACE = "ib1";

uint32_t Config::MLX5_SINGLE_THREADED = 1;

//THREADING
vector<vector<int>> Config::NUMA_THREAD_CPUS = {{0,1,2,3,4,5,6,7,8,9,10,11,12,13}, {14,15,16,17,18,19,20,21,22,23,24,25,26,27}}; //DM-cluster cpus

// GPUS (use 'nvidia-smi topo -m')
std::vector<std::vector<int>> Config::GPUS_TO_CPU_AFFINITY = {{0,1,2,3,4,5,6,7,8,9,10,11,12,13}, {14,15,16,17,18,19,20,21,22,23,24,25,26,27}}; // GPU index to CPU affinity

//LOGGING
int Config::LOGGING_LEVEL = 1;

// string& Config::getIPFromNodeId(NodeID& node_id){
//   return Config::DPI_NODES.at(node_id -1);
// }
// string& Config::getIPFromNodeId(const NodeID& node_id){
//   return Config::DPI_NODES.at(node_id -1);
// }


inline string trim(string str) {
  str.erase(0, str.find_first_not_of(' '));
  str.erase(str.find_last_not_of(' ') + 1);
  return str;
}

void Config::init_vector(vector<string>& values, string csv_list) {
  values.clear();
  char* csv_clist = new char[csv_list.length() + 1];
  strcpy(csv_clist, csv_list.c_str());
  char* token = strtok(csv_clist, ",");

  while (token) {
    values.push_back(token);
    token = strtok(nullptr, ",");
  }

  delete[] csv_clist;
}

void Config::init_vector(vector<int>& values, string csv_list) {
  values.clear();
  char* csv_clist = new char[csv_list.length() + 1];
  strcpy(csv_clist, csv_list.c_str());
  char* token = strtok(csv_clist, ",");

  while (token) {
    string value(token);
    values.push_back(stoi(value));
    token = strtok(nullptr, ",");
  }

  delete[] csv_clist;
}

void Config::unload() {
  google::protobuf::ShutdownProtobufLibrary();
}

void Config::load(const string &exec_path) {
  load(exec_path, true);
}
void Config::load(const string &file_path, bool is_exec_path) {
  string conf_file = file_path;
  if(is_exec_path){
    if (file_path.empty() || file_path.find("/") == string::npos) {
      conf_file = ".";
    } else {
      conf_file = file_path.substr(0, file_path.find_last_of("/"));
    }
    conf_file += "/conf/RDMA.conf";
  }

  ifstream file(conf_file.c_str());

  if (file.fail()) {
    Logging::error(__FILE__, __LINE__,
                    "Failed to load config file at " + conf_file + ". "
                    "The default values are used.");
  }

  string line;
  string key;
  string value;
  int posEqual;
  while (getline(file, line)) {

    if (line.length() == 0)
      continue;

    if (line[0] == '#')
      continue;
    if (line[0] == ';')
      continue;

    posEqual = line.find('=');
    key = line.substr(0, posEqual);
    value = line.substr(posEqual + 1);
    set(trim(key), trim(value));
  }
}

void Config::set(string key, string value) {
  //config
  if(key.compare("RDMA_SERVER_ADDRESSES") == 0){
    Config::RDMA_SERVER_ADDRESSES = value;
  } else if(key.compare("RDMA_PORT") == 0) {
    Config::RDMA_PORT = stoi(value);
  } else if (key.compare("RDMA_MEMSIZE") == 0) {
    Config::RDMA_MEMSIZE = strtoul(value.c_str(), nullptr, 0);
  } else if (key.compare("RDMA_NUMAREGION") == 0) {
    Config::RDMA_NUMAREGION = stoi(value);
  } else if (key.compare("RDMA_IBPORT") == 0) {
    Config::RDMA_IBPORT = stoi(value);
  } else if (key.compare("LOGGING_LEVEL") == 0) {
    Config::LOGGING_LEVEL = stoi(value);
  } else if (key.compare("MLX5_SINGLE_THREADED") == 0) {
    Config::MLX5_SINGLE_THREADED = stoi(value);
  } else if (key.compare("RDMA_INTERFACE") == 0) {
    Config::RDMA_INTERFACE = value;
  } else if (key.compare("NODE_SEQUENCER_IP") == 0) {
    Config::SEQUENCER_IP = value;
  } else if (key.compare("NODE_SEQUENCER_PORT") == 0) {
    Config::SEQUENCER_PORT = stoi(value);
  } else if (key.compare("RDMA_GET_NODE_ID_RETRIES") == 0) {
    Config::RDMA_GET_NODE_ID_RETRIES = stoi(value);
  } else {
    std::cerr << "Config: UNKNOWN key '" << key << "' = '" << value << "'" << std::endl;
  }
}


string Config::getIP(std::string &interface) {
  int fd;
  struct ifreq ifr;
  fd = socket(AF_INET, SOCK_DGRAM, 0);
  /* I want to get an IPv4 IP address */
  ifr.ifr_addr.sa_family = AF_INET;
  /* I want an IP address attached to interface */
  strncpy(ifr.ifr_name, interface.c_str(), IFNAMSIZ-1);

  int status = ioctl(fd, SIOCGIFADDR, &ifr);
  close(fd);

  if (status != 0) {
    Logging::error(__FILE__,__LINE__,"Failed to lookup IP address of interface '" + interface + "'");
    return "";
  }

  return inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr);
}
