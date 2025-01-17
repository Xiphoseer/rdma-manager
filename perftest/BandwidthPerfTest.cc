#include "BandwidthPerfTest.h"

#include "../src/memory/BaseMemory.h"
#include "../src/memory/MainMemory.h"
#include "../src/memory/CudaMemory.h"
#include "../src/utils/Config.h"

#include <limits>
#include <algorithm>


/*	LT: Local Thread, RT: Remote Thread, B: BufferSlot, C: Connection, R: Receive Packet Size, S: Send Packet Size
 *
 *	Client Memory:	LT1{ B1[ C1(R, S), C2(R, S), ... ], B2[ C1(R, S), C2(R, S), ... ] }, T2{ B1[ C1(R, S), C2(R, S), ... ], LT2[ C1(R, S), C2(R, S), ... ] }, ...
 *	Server Memory:	LT1{ B1[ S ], B2[ S ], ... }, LT2{ B1[ S ], B2[ S ], ... }, ..., RT1{ B1[ R ], B2[ R ], ... }, RT2{ B1[ R ], B2[ R ], ... }
 */


mutex rdma::BandwidthPerfTest::waitLock;
condition_variable rdma::BandwidthPerfTest::waitCv;
bool rdma::BandwidthPerfTest::signaled;
rdma::TestOperation rdma::BandwidthPerfTest::testOperation;
size_t rdma::BandwidthPerfTest::client_count;
size_t rdma::BandwidthPerfTest::thread_count;

rdma::BandwidthPerfClientThread::BandwidthPerfClientThread(BaseMemory *memory, std::vector<std::string>& rdma_addresses, std::string ownIpPort, std::string sequencerIpPort, size_t packet_size, int buffer_slots, size_t iterations_per_thread, size_t max_rdma_wr_per_thread, WriteMode write_mode) {
	this->m_client = new RDMAClient<ReliableRDMA>(memory, "BandwidthPerfTestClient", ownIpPort, sequencerIpPort);
	this->m_rdma_addresses = rdma_addresses;
	this->m_packet_size = packet_size;
	this->m_buffer_slots = buffer_slots;
	this->m_remote_memory_size_per_thread = packet_size * buffer_slots; // remote memory size per thread
	this->m_memory_size_per_thread = m_remote_memory_size_per_thread * rdma_addresses.size(); // local memory size per thread
	this->m_iterations_per_thread = iterations_per_thread;
	this->m_max_rdma_wr_per_thread = max_rdma_wr_per_thread;
	this->m_write_mode = write_mode;
	m_remOffsets = new size_t[m_rdma_addresses.size()];

	for (size_t i = 0; i < m_rdma_addresses.size(); ++i) {
	    NodeID  nodeId = 0;
		//ib_addr_t ibAddr;
		string conn = m_rdma_addresses[i];
		//std::cout << "Thread trying to connect to '" << conn << "' . . ." << std::endl; // TODO REMOVE
		if(!m_client->connect(conn, nodeId)) {
			std::cerr << "BandwidthPerfThread::BandwidthPerfThread(): Could not connect to '" << conn << "'" << std::endl;
			throw invalid_argument("BandwidthPerfThread connection failed");
		}
		//std::cout << "Thread connected to '" << conn << "'" << std::endl; // TODO REMOVE
		m_addr.push_back(nodeId);
		m_client->remoteAlloc(conn, m_remote_memory_size_per_thread, m_remOffsets[i]);
	}

	m_local_memory = m_client->localMalloc(m_memory_size_per_thread);
	m_local_memory->openContext();
	if(m_local_memory->isMainMemory()) m_local_memory->setRandom(); // use thread specific workload to prevent compression 
	
	// send nodeID to tell remote thread how to respond
	for(size_t connIdx=0; connIdx < m_rdma_addresses.size(); connIdx++){
		m_local_memory->set((uint32_t)m_client->getOwnNodeID(), 0);
		if(m_client->getBufferObj()->isGPUMemory()){ // TODO REMOVE
			m_client->write(m_addr[connIdx], m_remOffsets[connIdx], m_local_memory->pointer(), rdma::Config::GPUDIRECT_MINIMUM_MSG_SIZE, true); // TODO REMOVE
		} else { // TODO REMOVE

			m_client->write(m_addr[connIdx], m_remOffsets[connIdx], m_local_memory->pointer(), sizeof(uint32_t), true);

		} // TODO REMOVE
	}
}


rdma::BandwidthPerfClientThread::~BandwidthPerfClientThread() {
	for (size_t i = 0; i < m_rdma_addresses.size(); ++i) {
		string addr = m_rdma_addresses[i];
		m_client->remoteFree(addr, m_remote_memory_size_per_thread, m_remOffsets[i]);
	}
    delete m_remOffsets;
	delete m_local_memory; // implicitly deletes local allocs in RDMAClient and also closes context
	delete m_client;
}


void rdma::BandwidthPerfClientThread::run() {
	rdma::PerfTest::global_barrier_client(m_client, m_addr); // global barrier
	unique_lock<mutex> lck(BandwidthPerfTest::waitLock); // local barrier
	if (!BandwidthPerfTest::signaled) {
		m_ready = true;
		BandwidthPerfTest::waitCv.wait(lck);
	}
	lck.unlock();
	m_ready = false;
	
	size_t offset, sendOffset, receiveOffset, remoteOffset;
	size_t ackMsgPackgin = m_max_rdma_wr_per_thread/2; if(ackMsgPackgin<1) ackMsgPackgin=1; // how many ACK should be packet into a single ACK message
	size_t workingConnections = m_rdma_addresses.size();
	std::vector<size_t> ackMsgCounters(workingConnections, 0);
	std::vector<size_t> totalBudgets(workingConnections, m_iterations_per_thread);
	std::vector<size_t> pendingMessages(workingConnections, 0); // each connection counds how many receive it expects
	uint32_t imm;

	auto start = rdma::PerfTest::startTimer();
	switch(BandwidthPerfTest::testOperation){
		case WRITE_OPERATION: // Write
			switch(m_write_mode){
				case WRITE_MODE_NORMAL:
					// one sided - server does nothing
					for(size_t i = 0; i < m_iterations_per_thread; i++){
						bool signaled = ( (i+1)==m_iterations_per_thread );
						size_t offset = (i % m_buffer_slots) * m_packet_size;
						for(size_t connIdx=0; connIdx < m_rdma_addresses.size(); connIdx++){
							size_t sendOffset = connIdx * m_packet_size * m_buffer_slots + offset;
							size_t remoteOffset = m_remOffsets[connIdx] + offset;
							m_client->write(m_addr[connIdx], remoteOffset, m_local_memory->pointer(sendOffset), m_packet_size, signaled);
						}
					}
					break;
				case WRITE_MODE_IMMEDIATE:
					do {
						for(size_t connIdx=0; connIdx < m_rdma_addresses.size(); connIdx++){
							size_t totalBudget = totalBudgets[connIdx];
							size_t pendingMsgs = pendingMessages[connIdx];
							size_t ackMsgCounter = ackMsgCounters[connIdx];
							
							if(totalBudget == 0 && pendingMsgs == 0) continue; // skip this connection as it has already finished

							// send as many messages as possible
							size_t connOffset = connIdx * m_packet_size * m_buffer_slots;
							size_t sendBudget = m_max_rdma_wr_per_thread - pendingMsgs; // calculate how many receives can be posted
							if(sendBudget > totalBudget) sendBudget = totalBudget; // check if possible receives exeeds actual left totalBudget
							for(size_t i=0; i<sendBudget; i++){
								if(ackMsgCounter == 0) m_client->receiveWriteImm(m_addr[connIdx]); // just for ACK
								ackMsgCounters[connIdx] = (ackMsgCounter+1 < ackMsgPackgin ? ackMsgCounter+1 : 0);

								offset = ((totalBudget-i) % m_buffer_slots) * m_packet_size;
								remoteOffset = m_remOffsets[connIdx] + offset;
								sendOffset = connOffset + offset;
								m_client->writeImm(m_addr[connIdx], remoteOffset, m_local_memory->pointer(sendOffset), m_packet_size, (uint32_t)0, (i+1)==sendBudget);
							}
							totalBudgets[connIdx] -= sendBudget; // decrement totalBudget by amount of sent messages
							pendingMessages[connIdx] += sendBudget; // increment pending receives by amount of sent messages

							// poll ACKs
							do {
								if(m_client->pollReceive(m_addr[connIdx], pendingMessages[connIdx]==m_max_rdma_wr_per_thread, &imm) <= 0) break; // break because no more ACKs
								pendingMessages[connIdx] -= imm; // decrement pending receives by received amount
							} while(true);

							if(totalBudgets[connIdx] == 0 && pendingMessages[connIdx] == 0){ // check if connection has finished
								workingConnections--;
							}
						}
					} while(workingConnections > 0);
					break;
				default: throw invalid_argument("BandwidthPerfClientThread unknown write mode");
			}
			if(BandwidthPerfTest::client_count > 1) rdma::PerfTest::global_barrier_client(m_client, m_addr, false); // global end barrier if multiple nodes
			m_elapsedWriteMs = rdma::PerfTest::stopTimer(start);
			break;

		case READ_OPERATION: // Read
			// one sided - server does nothing
			for(size_t i = 0; i < m_iterations_per_thread; i++){
				bool signaled = ( (i+1)==m_iterations_per_thread );
				offset = (i % m_buffer_slots) * m_packet_size;
				for(size_t connIdx=0; connIdx < m_rdma_addresses.size(); connIdx++){
					receiveOffset = connIdx * m_packet_size * m_buffer_slots + offset;
					remoteOffset = m_remOffsets[connIdx] + offset;
					m_client->read(m_addr[connIdx], remoteOffset, m_local_memory->pointer(receiveOffset), m_packet_size, signaled);
				}
			}
			if(BandwidthPerfTest::client_count > 1) rdma::PerfTest::global_barrier_client(m_client, m_addr, false); // global end barrier if multiple nodes
			m_elapsedReadMs = rdma::PerfTest::stopTimer(start);
			break;

		case SEND_RECEIVE_OPERATION: // Send & Receive
			do {
				for(size_t connIdx=0; connIdx < m_rdma_addresses.size(); connIdx++){
					size_t totalBudget = totalBudgets[connIdx];
					size_t pendingMsgs = pendingMessages[connIdx];
					size_t ackMsgCounter = ackMsgCounters[connIdx];

					if(totalBudget == 0 && pendingMsgs == 0) continue; // skip this connection as it has already finished

					// send as many messages as possible
					size_t connOffset = connIdx * m_packet_size * m_buffer_slots;
					size_t sendBudget = m_max_rdma_wr_per_thread - pendingMsgs; // calculate how many receives can be posted
					if(sendBudget > totalBudget) sendBudget = totalBudget; // check if possible receives exeeds actual left totalBudget
					for(size_t i=0; i<sendBudget; i++){
						if(ackMsgCounter == 0) m_client->receiveWriteImm(m_addr[connIdx]); // just for ACK
						ackMsgCounters[connIdx] = (ackMsgCounter+1 < ackMsgPackgin ? ackMsgCounter+1 : 0);

						sendOffset = connOffset + ((totalBudget-i) % m_buffer_slots) * m_packet_size;
						m_client->send(m_addr[connIdx], m_local_memory->pointer(sendOffset), m_packet_size, (i+1)==sendBudget);
					}
					totalBudgets[connIdx] -= sendBudget; // decrement totalBudget by amount of sent messages
					pendingMessages[connIdx] += sendBudget; // increment pending receives by amount of sent messages

					// poll ACKs
					do {
						if(m_client->pollReceive(m_addr[connIdx], pendingMessages[connIdx]==m_max_rdma_wr_per_thread, &imm) <= 0) break; // break because no more ACKs
						pendingMessages[connIdx] -= imm; // decrement pending receives by received amount
					} while(true);

					if(totalBudgets[connIdx] == 0 && pendingMessages[connIdx] == 0){ // check if connection has finished
						workingConnections--;
					}
				}
			} while(workingConnections > 0);
			if(BandwidthPerfTest::client_count > 1) rdma::PerfTest::global_barrier_client(m_client, m_addr, false); // global end barrier if multiple nodes
			m_elapsedSendMs = rdma::PerfTest::stopTimer(start); // stop timer
			break;
		
		default: throw invalid_argument("BandwidthPerfClientThread unknown test mode");
	}
}



rdma::BandwidthPerfServerThread::BandwidthPerfServerThread(RDMAServer<ReliableRDMA> *server, size_t packet_size, int buffer_slots, size_t iterations_per_thread, size_t max_rdma_wr_per_thread, WriteMode write_mode) {
	this->m_server = server;
	this->m_packet_size = packet_size;
	this->m_buffer_slots = buffer_slots;
	this->m_memory_size_per_thread = packet_size * buffer_slots;
	this->m_iterations_per_thread = iterations_per_thread;
	this->m_max_rdma_wr_per_thread = max_rdma_wr_per_thread;
	this->m_write_mode = write_mode;
	this->m_local_memory = server->localMalloc(this->m_memory_size_per_thread);
	this->m_local_memory->openContext();
	if(m_local_memory->isMainMemory())  m_local_memory->setRandom(); // use thread specific workload to prevent compression 
}

rdma::BandwidthPerfServerThread::~BandwidthPerfServerThread() {
	delete m_local_memory;  // implicitly deletes local allocs in RDMAServer
}

void rdma::BandwidthPerfServerThread::run() {
	unique_lock<mutex> lck(BandwidthPerfTest::waitLock);
	if (!BandwidthPerfTest::signaled) {
		m_ready = true;
		BandwidthPerfTest::waitCv.wait(lck);
	}
	lck.unlock();
	m_ready = false;

	const size_t receiveBaseOffset = rdma::BandwidthPerfTest::thread_count * m_memory_size_per_thread;

	// receive nodeID to respond to
	if(m_respond_conn_id < 0){
		m_respond_conn_id = m_local_memory->getInt32(receiveBaseOffset);
		m_local_memory->set((int32_t)0, receiveBaseOffset);
	}

	size_t ackMsgPackgin = m_max_rdma_wr_per_thread/2; if(ackMsgPackgin<1) ackMsgPackgin=1; // how many ACK should be packet into a single ACK message
	size_t ackMsgCounter = 0, receiveOffset;
	size_t totalBudget = m_iterations_per_thread, budgetR = totalBudget;
	switch(BandwidthPerfTest::testOperation){
		case WRITE_OPERATION:
			if(m_write_mode == WRITE_MODE_IMMEDIATE){
				totalBudget = m_iterations_per_thread;
				budgetR = (totalBudget > m_max_rdma_wr_per_thread ? m_max_rdma_wr_per_thread : totalBudget); // how many receives have been posted
				for(size_t i=0; i<budgetR; i++){ // intially post enough receives
					m_server->receiveWriteImm(m_respond_conn_id);
				}
				totalBudget -= budgetR;

				do {
					if(m_server->pollReceive(m_respond_conn_id, true) <= 0) break;   // wait for one message to receive
					budgetR--;
					if(totalBudget > 0){
						m_server->receiveWriteImm(m_respond_conn_id);
						budgetR++;
						totalBudget--;
					}
					ackMsgCounter++;
					bool lastMsg = (totalBudget==0 && budgetR==0);
					if(ackMsgCounter==ackMsgPackgin || lastMsg){ // if packing budget full or last message then ACK

						if(lastMsg && BandwidthPerfTest::client_count > 1) // only if last message and multiple client instances
							PerfTest::global_barrier_server_prepare(m_server, m_respond_conn_id); // prepare final barrier if multiple nodes

						m_server->writeImm(m_respond_conn_id, 0, nullptr, 0, (uint32_t)ackMsgCounter, true); // ACK each packet individuall
						ackMsgCounter = 0;
					}
				} while(totalBudget > 0 || budgetR > 0);

			} else if(BandwidthPerfTest::client_count > 1) 
					PerfTest::global_barrier_server_prepare(m_server, m_respond_conn_id); // global end barrier if multiple nodes
			break;

		case READ_OPERATION:
			if(BandwidthPerfTest::client_count > 1) 
				PerfTest::global_barrier_server_prepare(m_server, m_respond_conn_id); // prepare final barrier if multiple nodes
			break;

		case SEND_RECEIVE_OPERATION:
			totalBudget = m_iterations_per_thread;
			budgetR = (totalBudget > m_max_rdma_wr_per_thread ? m_max_rdma_wr_per_thread : totalBudget); // how many receives have been posted
			for(size_t i=0; i<budgetR; i++){ // intially post enough receives
				receiveOffset = receiveBaseOffset + ((totalBudget-i) % m_buffer_slots) * m_packet_size;
				m_server->receive(m_respond_conn_id, m_local_memory->pointer(receiveOffset), m_packet_size);
			}
			totalBudget -= budgetR;

			do {
				if(m_server->pollReceive(m_respond_conn_id, true) <= 0) break;   // wait for one message to receive
				budgetR--;
				if(totalBudget > 0){
					receiveOffset = receiveBaseOffset + (totalBudget % m_buffer_slots) * m_packet_size;
					m_server->receive(m_respond_conn_id, m_local_memory->pointer(receiveOffset), m_packet_size);
					budgetR++;
					totalBudget--;
				}
				ackMsgCounter++;
				bool lastMsg = (totalBudget==0 && budgetR==0);
				if(ackMsgCounter==ackMsgPackgin || lastMsg){ // if packing budget full or last message then ACK

					if(lastMsg && BandwidthPerfTest::client_count > 1) // only if last message and multiple client instances
							PerfTest::global_barrier_server_prepare(m_server, m_respond_conn_id); // prepare final barrier if multiple nodes

					m_server->writeImm(m_respond_conn_id, 0, nullptr, 0, (uint32_t)ackMsgCounter, true); // ACK each packet individuall
					ackMsgCounter = 0;
				}
			} while(totalBudget > 0 || budgetR > 0);
			break;

		default: throw invalid_argument("BandwidthPerfServerThread unknown test mode");
	}
	//m_elapsedReceive = rdma::PerfTest::stopTimer(start);
}



rdma::BandwidthPerfTest::BandwidthPerfTest(int testOperations, bool is_server, std::vector<std::string> rdma_addresses, int rdma_port, std::string ownIpPort, std::string sequencerIpPort, int local_gpu_index, int remote_gpu_index, int client_count, int thread_count, uint64_t packet_size, int buffer_slots, uint64_t iterations_per_thread, WriteMode write_mode) : PerfTest(testOperations){
	if(is_server) thread_count *= client_count;

	this->m_is_server = is_server;
	this->m_rdma_port = rdma_port;
	this->m_ownIpPort = ownIpPort;
	this->m_sequencerIpPort = sequencerIpPort;
	this->m_local_gpu_index = local_gpu_index;
	this->m_actual_gpu_index = -1;
	this->m_remote_gpu_index = remote_gpu_index;
	this->client_count = client_count;
	this->thread_count = thread_count;
	this->m_packet_size = packet_size;
	this->m_buffer_slots = buffer_slots;
	this->m_memory_size = thread_count * packet_size * buffer_slots * rdma_addresses.size() * 2; // 2x because server needs for send/recv separat
	this->m_iterations_per_thread = iterations_per_thread;
	this->m_write_mode = (write_mode!=WRITE_MODE_AUTO ? write_mode : rdma::BandwidthPerfTest::DEFAULT_WRITE_MODE);
	this->m_rdma_addresses = rdma_addresses;
}
rdma::BandwidthPerfTest::~BandwidthPerfTest(){
	for (size_t i = 0; i < m_client_threads.size(); i++) {
		delete m_client_threads[i];
	}
	m_client_threads.clear();
	for (size_t i = 0; i < m_server_threads.size(); i++) {
		delete m_server_threads[i];
	}
	m_server_threads.clear();
	if(m_is_server)
		delete m_server;
	delete m_memory;
}

std::string rdma::BandwidthPerfTest::getTestParameters(bool forCSV){
	std::ostringstream oss;
	oss << (m_is_server ? "Server" : "Client") << ", threads=" << thread_count << ", bufferslots=" << m_buffer_slots;
	if(!forCSV){ 
		oss << ", packetsize=" << m_packet_size;
		oss << ", memory=" << m_memory_size << " (2x " << thread_count << "x " << m_buffer_slots << "x ";
		if(!m_is_server){ oss << m_rdma_addresses.size() << "x "; } oss << m_packet_size << ")";
	}
	oss << ", memory_type=" << getMemoryName(m_local_gpu_index, m_actual_gpu_index) << (m_remote_gpu_index!=-404 ? "->"+getMemoryName(m_remote_gpu_index) : "");
	oss << ", iterations=" << (m_iterations_per_thread*thread_count) << ", writemode=" << (m_write_mode==WRITE_MODE_NORMAL ? "Normal" : "Immediate");
	if(!forCSV){ oss << ", clients=" << client_count << ", servers=" << m_rdma_addresses.size(); }
	return oss.str();
}
std::string rdma::BandwidthPerfTest::getTestParameters(){
	return getTestParameters(false);
}

void rdma::BandwidthPerfTest::makeThreadsReady(TestOperation testOperation){
	BandwidthPerfTest::testOperation = testOperation;
	BandwidthPerfTest::signaled = false;
	if(m_is_server){
		// Server
		for(BandwidthPerfServerThread* perfThread : m_server_threads){ perfThread->start(); }
		for(BandwidthPerfServerThread* perfThread : m_server_threads){ while(!perfThread->ready()) usleep(Config::RDMA_SLEEP_INTERVAL); }
		rdma::PerfTest::global_barrier_server(m_server, (size_t)thread_count);

	} else {
		// Client
		for(BandwidthPerfClientThread* perfThread : m_client_threads){ perfThread->start(); }
		for(BandwidthPerfClientThread* perfThread : m_client_threads){ while(!perfThread->ready()) usleep(Config::RDMA_SLEEP_INTERVAL); }
	}
}

void rdma::BandwidthPerfTest::runThreads(){
	BandwidthPerfTest::signaled = false;
	unique_lock<mutex> lck(BandwidthPerfTest::waitLock);
	BandwidthPerfTest::waitCv.notify_all();
	BandwidthPerfTest::signaled = true;
	lck.unlock();

	for (size_t i = 0; i < m_server_threads.size(); i++) {
		m_server_threads[i]->join();
	}

	if(m_is_server && BandwidthPerfTest::client_count > 1)  // if is server and multiple client instances then sync with global end barrier
		PerfTest::global_barrier_server(m_server, BandwidthPerfTest::thread_count, false); // finish global end barrier

	for (size_t i = 0; i < m_client_threads.size(); i++) {
		m_client_threads[i]->join();
	}
}

void rdma::BandwidthPerfTest::setupTest(){
	m_elapsedWriteMs = -1;
	m_elapsedReadMs = -1;
	m_elapsedSendMs = -1;
	m_actual_gpu_index = -1;
	#ifdef CUDA_ENABLED /* defined in CMakeLists.txt to globally enable/disable CUDA support */
		if(m_local_gpu_index <= -3){
			m_memory = new rdma::MainMemory(m_memory_size);
		} else {
			rdma::CudaMemory *mem = new rdma::CudaMemory(m_memory_size, m_local_gpu_index);
			m_memory = mem;
			m_actual_gpu_index = mem->getDeviceIndex();
		}
	#else
		m_memory = (rdma::BaseMemory*)new MainMemory(m_memory_size);
	#endif

	const size_t max_rdma_wr_per_thread = rdma::Config::RDMA_MAX_WR;

	if(m_is_server){
		// Server
		m_server = new RDMAServer<ReliableRDMA>("BandwidthTestRDMAServer", m_rdma_port, Network::getAddressOfConnection(m_ownIpPort), m_memory, m_sequencerIpPort);
		for (size_t thread_id = 0; thread_id < thread_count; thread_id++) {
			BandwidthPerfServerThread* perfThread = new BandwidthPerfServerThread(m_server, m_packet_size, m_buffer_slots, m_iterations_per_thread, max_rdma_wr_per_thread, m_write_mode);
			m_server_threads.push_back(perfThread);
		}

	} else {
		// Client
		for (size_t thread_id = 0; thread_id < thread_count; thread_id++) {
			BandwidthPerfClientThread* perfThread = new BandwidthPerfClientThread(m_memory, m_rdma_addresses, m_ownIpPort, m_sequencerIpPort, m_packet_size, m_buffer_slots, m_iterations_per_thread, max_rdma_wr_per_thread, m_write_mode);
			m_client_threads.push_back(perfThread);
		}
	}
}

void rdma::BandwidthPerfTest::runTest(){
	if(m_is_server){
		// Server
		std::cout << "Starting server on '" << rdma::Config::getIP(rdma::Config::RDMA_INTERFACE) << ":" << m_rdma_port << "' . . ." << std::endl;
		if(!m_server->startServer()){
			std::cerr << "BandwidthPerfTest::runTest(): Could not start server" << std::endl;
			throw invalid_argument("BandwidthPerfTest server startup failed");
		} else {
			std::cout << "Server running on '" << rdma::Config::getIP(rdma::Config::RDMA_INTERFACE) << ":" << m_rdma_port << "'" << std::endl; // TODO REMOVE
		}
		
		if(hasTestOperation(WRITE_OPERATION)){
			makeThreadsReady(WRITE_OPERATION); // receive
			//auto startReceive = rdma::PerfTest::startTimer();
			runThreads();
			//m_elapsedReceiveMs = rdma::PerfTest::stopTimer(startReceive);
		}
		
		if(hasTestOperation(READ_OPERATION)){
			makeThreadsReady(READ_OPERATION); // receive
			//auto startReceive = rdma::PerfTest::startTimer();
			runThreads();
			//m_elapsedReceiveMs = rdma::PerfTest::stopTimer(startReceive);
		}

		if(hasTestOperation(SEND_RECEIVE_OPERATION)){
			makeThreadsReady(SEND_RECEIVE_OPERATION); // receive
			//auto startReceive = rdma::PerfTest::startTimer();
			runThreads();
			//m_elapsedReceiveMs = rdma::PerfTest::stopTimer(startReceive);
		}

		// wait until server is done
		while (m_server->isRunning() && m_server->getConnectedConnIDs().size() > 0) usleep(Config::RDMA_SLEEP_INTERVAL);
		std::cout << "Server stopped" << std::endl;

	} else {
		// Client

        // Measure bandwidth for writing
		if(hasTestOperation(WRITE_OPERATION)){
			makeThreadsReady(WRITE_OPERATION); // write
			usleep(Config::PERFORMANCE_TEST_SERVER_TIME_ADVANTAGE); // let server first post its receives
			auto startWrite = rdma::PerfTest::startTimer();
			runThreads();
			m_elapsedWriteMs = rdma::PerfTest::stopTimer(startWrite);
		}

		// Measure bandwidth for reading
		if(hasTestOperation(READ_OPERATION)){
			makeThreadsReady(READ_OPERATION); // read
			auto startRead = rdma::PerfTest::startTimer();
			runThreads();
			m_elapsedReadMs = rdma::PerfTest::stopTimer(startRead);
		}

		if(hasTestOperation(SEND_RECEIVE_OPERATION)){
			// Measure bandwidth for sending
			makeThreadsReady(SEND_RECEIVE_OPERATION); // send
			usleep(Config::PERFORMANCE_TEST_SERVER_TIME_ADVANTAGE); // let server first post its receives
			auto startSend = rdma::PerfTest::startTimer();
			runThreads();
			m_elapsedSendMs = rdma::PerfTest::stopTimer(startSend);
		}

	}
}


std::string rdma::BandwidthPerfTest::getTestResults(std::string csvFileName, bool csvAddHeader){
	if(m_is_server){
		return "only client";
	} else {
		
		/*	There are  n  threads
			Each thread computes  iterations_per_thread = total_iterations / n
			Each thread takes  n  times more time compared to a single thread
			 Bandwidth 	= transferedBytes / elapsedTime
						= (n * iterations_per_thread * packetSize) / (elapsedTime_per_thread)
		*/

		const long double tu = (long double)NANO_SEC; // 1sec (nano to seconds as time unit)

		uint64_t transferedBytes = thread_count * m_iterations_per_thread * m_packet_size * m_rdma_addresses.size();
		int64_t maxWriteMs=-1, minWriteMs=std::numeric_limits<int64_t>::max();
		int64_t maxReadMs=-1, minReadMs=std::numeric_limits<int64_t>::max();
		int64_t maxSendMs=-1, minSendMs=std::numeric_limits<int64_t>::max();
		int64_t arrWriteMs[thread_count];
		int64_t arrReadMs[thread_count];
		int64_t arrSendMs[thread_count];
		long double avgWriteMs=0, medianWriteMs, avgReadMs=0, medianReadMs, avgSendMs=0, medianSendMs;
		const long double divAvg = m_client_threads.size();
		for(size_t i=0; i<m_client_threads.size(); i++){
			BandwidthPerfClientThread *thr = m_client_threads[i];
			if(thr->m_elapsedWriteMs < minWriteMs) minWriteMs = thr->m_elapsedWriteMs;
			if(thr->m_elapsedWriteMs > maxWriteMs) maxWriteMs = thr->m_elapsedWriteMs;
			avgWriteMs += (long double) thr->m_elapsedWriteMs / divAvg;
			arrWriteMs[i] = thr->m_elapsedWriteMs;
			if(thr->m_elapsedReadMs < minReadMs) minReadMs = thr->m_elapsedReadMs;
			if(thr->m_elapsedReadMs > maxReadMs) maxReadMs = thr->m_elapsedReadMs;
			avgReadMs += (long double) thr->m_elapsedReadMs / divAvg;
			arrReadMs[i] = thr->m_elapsedReadMs;
			if(thr->m_elapsedSendMs < minSendMs) minSendMs = thr->m_elapsedSendMs;
			if(thr->m_elapsedSendMs > maxSendMs) maxSendMs = thr->m_elapsedSendMs;
			avgSendMs += (long double) thr->m_elapsedSendMs / divAvg;
			arrSendMs[i] = thr->m_elapsedSendMs;
		}

		std::sort(arrWriteMs, arrWriteMs + thread_count);
		std::sort(arrReadMs, arrReadMs + thread_count);
		std::sort(arrSendMs, arrSendMs + thread_count);

		medianWriteMs = arrWriteMs[(int)(thread_count/2)];
		medianReadMs = arrReadMs[(int)(thread_count/2)];
		medianSendMs = arrSendMs[(int)(thread_count/2)];

		// write results into CSV file
		if(!csvFileName.empty()){
			const long double su = 1024*1024; // size unit for MebiBytes
			std::ofstream ofs;
			ofs.open(csvFileName, std::ofstream::out | std::ofstream::app);
			ofs << rdma::CSV_PRINT_NOTATION << rdma::CSV_PRINT_PRECISION;
			if(csvAddHeader){
				ofs << std::endl << "BANDWIDTH, " << getTestParameters(true) << std::endl;
				ofs << "PacketSize [Bytes], Transfered [Bytes]";
				if(hasTestOperation(WRITE_OPERATION)){
					ofs << ", Write [MB/s], Min Write [MB/s],Max Write [MB/s], Avg Write [MB/s], Median Write [MB/s], ";
					ofs << "Write [Sec], Min Write [Sec], Max Write [Sec], Avg Write [Sec], Median Write [Sec]";
				}
				if(hasTestOperation(READ_OPERATION)){
					ofs << ", Read [MB/s], Min Read [MB/s], Max Read [MB/s], Avg Read [MB/s], Median Read [MB/s], ";
					ofs << "Read [Sec], Min Read [Sec], Max Read [Sec], Avg Read [Sec], Median Read [Sec]";
				}
				if(hasTestOperation(SEND_RECEIVE_OPERATION)){
					ofs << ", Send/Recv [MB/s], Min Send/Recv [MB/s], Max Send/Recv [MB/s], Avg Send/Recv [MB/s], Median Send/Recv [MB/s], ";
					ofs << "Send/Recv [Sec], Min Send/Recv [Sec], Max Send/Recv [Sec], Avg Send/Recv [Sec], Median Send/Recv [Sec]";
				}
				ofs << std::endl;
			}
			ofs << m_packet_size << ", " << transferedBytes; // packet size Bytes
			if(hasTestOperation(WRITE_OPERATION)){
				ofs << ", " << (round(transferedBytes*tu/su/m_elapsedWriteMs * 100000)/100000.0) << ", "; // write MB/s
				ofs << (round(transferedBytes*tu/su/maxWriteMs * 100000)/100000.0) << ", "; // min write MB/s
				ofs << (round(transferedBytes*tu/su/minWriteMs * 100000)/100000.0) << ", "; // max write MB/s
				ofs << (round(transferedBytes*tu/su/avgWriteMs * 100000)/100000.0) << ", "; // avg write MB/s
				ofs << (round(transferedBytes*tu/su/medianWriteMs * 100000)/100000.0) << ", "; // median write MB/s
				ofs << (round(m_elapsedWriteMs/tu * 100000)/100000.0) << ", " ; // write Sec
				ofs << (round(minWriteMs/tu * 100000)/100000.0) << ", "; // min write Sec
				ofs << (round(maxWriteMs/tu * 100000)/100000.0) << ", "; // max write Sec
				ofs << (round(avgWriteMs/tu * 100000)/100000.0) << ", "; // avg write Sec
				ofs << (round(medianWriteMs/tu * 100000)/100000.0); // median write Sec
			}
			if(hasTestOperation(READ_OPERATION)){
				ofs << ", " << (round(transferedBytes*tu/su/m_elapsedReadMs * 100000)/100000.0) << ", "; // read MB/s
				ofs << (round(transferedBytes*tu/su/maxReadMs * 100000)/100000.0) << ", "; // min read MB/s
				ofs << (round(transferedBytes*tu/su/minReadMs * 100000)/100000.0) << ", "; // max read MB/s
				ofs << (round(transferedBytes*tu/su/avgReadMs * 100000)/100000.0) << ", "; // avg read MB/s
				ofs << (round(transferedBytes*tu/su/medianReadMs * 100000)/100000.0) << ", "; // median read MB/s
				ofs << (round(m_elapsedReadMs/tu * 100000)/100000.0) << ", "; // read Sec
				ofs << (round(minReadMs/tu * 100000)/100000.0) << ", "; // min read Sec
				ofs << (round(maxReadMs/tu * 100000)/100000.0) << ", "; // max read Sec
				ofs << (round(avgReadMs/tu * 100000)/100000.0) << ", "; // avg read Sec
				ofs << (round(medianReadMs/tu * 100000)/100000.0); // median read Sec
			}
			if(hasTestOperation(SEND_RECEIVE_OPERATION)){
				ofs << ", " << (round(transferedBytes*tu/su/m_elapsedSendMs * 100000)/100000.0) << ", "; // send/recv MB/s
				ofs << (round(transferedBytes*tu/su/maxSendMs * 100000)/100000.0) << ", "; // min send/recv MB/s
				ofs << (round(transferedBytes*tu/su/minSendMs * 100000)/100000.0) << ", "; // max send/recv MB/s
				ofs << (round(transferedBytes*tu/su/avgSendMs * 100000)/100000.0) << ", "; // avg send/recv MB/s
				ofs << (round(transferedBytes*tu/su/medianSendMs * 100000)/100000.0) << ", "; // median send/recv MB/s
				ofs << (round(m_elapsedSendMs/tu * 100000)/100000.0) << ", "; // send Sec
				ofs << (round(minSendMs/tu * 100000)/100000.0) << ", "; // min send Sec
				ofs << (round(maxSendMs/tu * 100000)/100000.0) << ", "; // max send Sec
				ofs << (round(avgSendMs/tu * 100000)/100000.0) << ", "; // avg send Sec
				ofs << (round(medianSendMs/tu * 100000)/100000.0); // median send Sec
			}

			ofs << std::endl; ofs.close();
		}

		// generate result string
		std::ostringstream oss;
		oss << rdma::CONSOLE_PRINT_NOTATION << rdma::CONSOLE_PRINT_PRECISION;
		oss << "transfered = " << rdma::PerfTest::convertByteSize(transferedBytes) << std::endl;
		if(hasTestOperation(WRITE_OPERATION)){
			oss << " - Write:         bandwidth = " << rdma::PerfTest::convertBandwidth(transferedBytes*tu/m_elapsedWriteMs); 
			oss << "  (range = " << rdma::PerfTest::convertBandwidth(transferedBytes*tu/maxWriteMs) << " - " << rdma::PerfTest::convertBandwidth(transferedBytes*tu/minWriteMs);
			oss << " ; avg=" << rdma::PerfTest::convertBandwidth(transferedBytes*tu/avgWriteMs) << " ; median=";
			oss << rdma::PerfTest::convertBandwidth(transferedBytes*tu/minWriteMs) << ")";
			oss << "   &   time = " << rdma::PerfTest::convertTime(m_elapsedWriteMs) << "  (range=";
			oss << rdma::PerfTest::convertTime(minWriteMs) << "-" << rdma::PerfTest::convertTime(maxWriteMs);
			oss << " ; avg=" << rdma::PerfTest::convertTime(avgWriteMs) << " ; median=" << rdma::PerfTest::convertTime(medianWriteMs) << ")" << std::endl;
		}
		if(hasTestOperation(READ_OPERATION)){
			oss << " - Read:          bandwidth = " << rdma::PerfTest::convertBandwidth(transferedBytes*tu/m_elapsedReadMs);
			oss << "  (range = " << rdma::PerfTest::convertBandwidth(transferedBytes*tu/maxReadMs) << " - " << rdma::PerfTest::convertBandwidth(transferedBytes*tu/minReadMs);
			oss << " ; avg=" << rdma::PerfTest::convertBandwidth(transferedBytes*tu/avgReadMs) << " ; median=";
			oss << rdma::PerfTest::convertBandwidth(transferedBytes*tu/minReadMs) << ")";
			oss << "   &   time = " << rdma::PerfTest::convertTime(m_elapsedReadMs) << "  (range=";
			oss << rdma::PerfTest::convertTime(minReadMs) << "-" << rdma::PerfTest::convertTime(maxReadMs);
			oss << " ; avg=" << rdma::PerfTest::convertTime(avgReadMs) << " ; median=" << rdma::PerfTest::convertTime(medianReadMs) << ")" << std::endl;
		}
		if(hasTestOperation(SEND_RECEIVE_OPERATION)){
			oss << " - Send:          bandwidth = " << rdma::PerfTest::convertBandwidth(transferedBytes*tu/m_elapsedSendMs);
			oss << "  (range = " << rdma::PerfTest::convertBandwidth(transferedBytes*tu/maxSendMs) << " - " << rdma::PerfTest::convertBandwidth(transferedBytes*tu/minSendMs);
			oss << " ; avg=" << rdma::PerfTest::convertBandwidth(transferedBytes*tu/avgSendMs) << " ; median=";
			oss << rdma::PerfTest::convertBandwidth(transferedBytes*tu/minSendMs) << ")";
			oss << "   &   time = " << rdma::PerfTest::convertTime(m_elapsedSendMs) << "  (range=";
			oss << rdma::PerfTest::convertTime(minSendMs) << "-" << rdma::PerfTest::convertTime(maxSendMs);
			oss << " ; avg=" << rdma::PerfTest::convertTime(avgSendMs) << " ; median=" << rdma::PerfTest::convertTime(medianSendMs) << ")" << std::endl;
		}
		return oss.str();

	}
	return NULL;
}