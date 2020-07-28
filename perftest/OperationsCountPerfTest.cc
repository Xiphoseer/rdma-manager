#include "OperationsCountPerfTest.h"

#include "../src/memory/BaseMemory.h"
#include "../src/memory/MainMemory.h"
#include "../src/memory/CudaMemory.h"
#include "../src/utils/Config.h"

#include <limits>
#include <algorithm>

mutex rdma::OperationsCountPerfTest::waitLock;
condition_variable rdma::OperationsCountPerfTest::waitCv;
bool rdma::OperationsCountPerfTest::signaled;
rdma::TestMode rdma::OperationsCountPerfTest::testMode;

rdma::OperationsCountPerfClientThread::OperationsCountPerfClientThread(BaseMemory *memory, std::vector<std::string>& rdma_addresses, size_t packet_size, int buffer_slots, size_t iterations, size_t max_rdma_wr_per_thread, WriteMode write_mode) {
	this->m_client = new RDMAClient<ReliableRDMA>(memory, "OperationsCountPerfTestClient");
	this->m_rdma_addresses = rdma_addresses;
	this->m_packet_size = packet_size;
	this->m_buffer_slots = buffer_slots;
	this->m_memory_size_per_thread = packet_size * buffer_slots;
	this->m_iterations = iterations;
	this->m_max_rdma_wr_per_thread = max_rdma_wr_per_thread;
	this->m_write_mode = write_mode;
	m_remOffsets = new size_t[m_rdma_addresses.size()];

	for (size_t i = 0; i < m_rdma_addresses.size(); ++i) {
	    NodeID  nodeId = 0;
		//ib_addr_t ibAddr;
		string conn = m_rdma_addresses[i];
		//std::cout << "Thread trying to connect to '" << conn << "' . . ." << std::endl; // TODO REMOVE
		if(!m_client->connect(conn, nodeId)) {
			std::cerr << "OperationsCountPerfClientThread::OperationsCountPerfClientThread(): Could not connect to '" << conn << "'" << std::endl;
			throw invalid_argument("OperationsCountPerfClientThread connection failed");
		}
		//std::cout << "Thread connected to '" << conn << "'" << std::endl; // TODO REMOVE
		m_addr.push_back(nodeId);
		m_client->remoteAlloc(conn, m_memory_size_per_thread, m_remOffsets[i]); // one chunk needed on remote side for write & read
	}

	m_local_memory = m_client->localMalloc(this->m_memory_size_per_thread * 2); // two chunks needed on local side for send & receive
	m_local_memory->openContext();
	m_local_memory->setMemory(1);
}

rdma::OperationsCountPerfClientThread::~OperationsCountPerfClientThread() {
	for (size_t i = 0; i < m_rdma_addresses.size(); ++i) {
		string addr = m_rdma_addresses[i];
		m_client->remoteFree(addr, m_memory_size_per_thread, m_remOffsets[i]);
	}
    delete m_remOffsets;
	delete m_local_memory; // implicitly deletes local allocs in RDMAClient
	delete m_client;
}

void rdma::OperationsCountPerfClientThread::run() {
	unique_lock<mutex> lck(OperationsCountPerfTest::waitLock);
	if (!OperationsCountPerfTest::signaled) {
		m_ready = true;
		OperationsCountPerfTest::waitCv.wait(lck);
	}
	lck.unlock();
	int sendCounter = 0, receiveCounter = 0, totalBudget = m_iterations, budgetR;
	uint32_t localBaseOffset = (uint32_t)m_local_memory->getRootOffset();
	auto start = rdma::PerfTest::startTimer();
	switch(OperationsCountPerfTest::testMode){
		case TEST_WRITE: // Write
			switch(m_write_mode){
				case WRITE_MODE_NORMAL:
					for(size_t i = 0; i < m_iterations; i++){
						size_t connIdx = i % m_rdma_addresses.size();
						bool signaled = (i == (m_iterations - 1));
						int offset = (i % m_buffer_slots) * m_packet_size;
						m_client->write(m_addr[connIdx], m_remOffsets[connIdx]+offset, m_local_memory->pointer(offset), m_packet_size, signaled);
					}
					break;
				case WRITE_MODE_IMMEDIATE:
					for(size_t i = 0; i < m_iterations; i+=2*m_max_rdma_wr_per_thread){
						int budgetS = totalBudget;
						if(budgetS > (int)m_max_rdma_wr_per_thread){ budgetS = m_max_rdma_wr_per_thread; }
						totalBudget -= budgetS;

						size_t fi = i + budgetS;
						budgetR = totalBudget;
						if(budgetR >(int)m_max_rdma_wr_per_thread){ budgetR = m_max_rdma_wr_per_thread; }
						totalBudget -= budgetR;

						for(int j = 0; j < budgetR; j++){
							m_client->receiveWriteImm(m_addr[(fi+j) % m_rdma_addresses.size()]);
						}
						for(int j = 0; j < budgetS; j++){
							size_t connIdx = i % m_rdma_addresses.size();
							int offset = (i % m_buffer_slots) * m_packet_size;
							m_client->writeImm(m_addr[(i+j) % m_rdma_addresses.size()], m_remOffsets[connIdx]+offset, m_local_memory->pointer(offset), m_packet_size, localBaseOffset, (j+1)==budgetS);
						}

						for(int j = 0; j < budgetR; j++){
							m_client->pollReceive(m_addr[(fi+j) % m_rdma_addresses.size()], true);
						}
					}
					break;
				default: throw invalid_argument("OperationsCountPerfClientThread unknown write mode");
			}
			m_elapsedWrite = rdma::PerfTest::stopTimer(start);
			break;
		case TEST_READ: // Read
			for(size_t i = 0; i < m_iterations; i++){
				size_t connIdx = i % m_rdma_addresses.size();
				bool signaled = (i == (m_iterations - 1));
				int offset = (i % m_buffer_slots) * m_packet_size;
				m_client->read(m_addr[connIdx], m_remOffsets[connIdx]+offset, m_local_memory->pointer(offset), m_packet_size, signaled);
			}
			m_elapsedRead = rdma::PerfTest::stopTimer(start);
			break;
		case TEST_SEND_AND_RECEIVE: // Send & Receive
			// alternating send/receive blocks to not overfill queues
			for(size_t i = 0; i < m_iterations; i+=2*m_max_rdma_wr_per_thread){
				int budgetS = m_iterations - i;
				if(budgetS > (int)m_max_rdma_wr_per_thread){ budgetS = m_max_rdma_wr_per_thread; }
				totalBudget -= budgetS;

				size_t fi = i + budgetS;
				budgetR = totalBudget;
				if(budgetR >(int)m_max_rdma_wr_per_thread){ budgetR = m_max_rdma_wr_per_thread; }
				totalBudget -= budgetR;

				for(int j = 0; j < budgetR; j++){
					receiveCounter = (receiveCounter+1) % m_buffer_slots;
					int receiveOffset = receiveCounter * m_packet_size;
					m_client->receive(m_addr[(fi+j) % m_rdma_addresses.size()], m_local_memory->pointer(receiveOffset), m_packet_size);
				}

				for(int j = 0; j < budgetS; j++){
					sendCounter = (sendCounter+1) % m_buffer_slots;
					int sendOffset = sendCounter * m_packet_size + m_memory_size_per_thread;
					m_client->send(m_addr[(i+j) % m_rdma_addresses.size()], m_local_memory->pointer(sendOffset), m_packet_size, (j+1)==budgetS); // signaled: (j+1)==budget
				}

				for(int j = 0; j < budgetR; j++){
					m_client->pollReceive(m_addr[(fi+j) % m_rdma_addresses.size()], true);
				}
			}
			m_elapsedSend = rdma::PerfTest::stopTimer(start);
			break;
		default: throw invalid_argument("OperationsCountPerfClientThread unknown test mode");
	}
}



rdma::OperationsCountPerfServerThread::OperationsCountPerfServerThread(RDMAServer<ReliableRDMA> *server, size_t packet_size, int buffer_slots, size_t iterations, size_t max_rdma_wr_per_thread, WriteMode write_mode, int thread_id) {
	this->m_server = server;
	this->m_packet_size = packet_size;
	this->m_buffer_slots = buffer_slots;
	this->m_memory_size_per_thread = packet_size * buffer_slots;
	this->m_iterations = iterations;
	this->m_max_rdma_wr_per_thread = max_rdma_wr_per_thread;
	this->m_write_mode = write_mode;
	this->m_thread_id = thread_id;
	this->m_local_memory = server->localMalloc(this->m_memory_size_per_thread * 2); // two chunks needed on local side for send & receive + one chunk by remote
	this->m_local_memory->openContext();
}


rdma::OperationsCountPerfServerThread::~OperationsCountPerfServerThread() {
	delete m_local_memory;  // implicitly deletes local allocs in RDMAServer
}


void rdma::OperationsCountPerfServerThread::run() {
	unique_lock<mutex> lck(OperationsCountPerfTest::waitLock);
	if (!OperationsCountPerfTest::signaled) {
		m_ready = true;
		OperationsCountPerfTest::waitCv.wait(lck);
	}
	lck.unlock();
	const std::vector<size_t> clientIds = m_server->getConnectedConnIDs();

	// Measure operations/s for receiving
	//auto start = rdma::PerfTest::startTimer();
	size_t i = 0;
	int sendCounter = 0, receiveCounter = 0, totalBudget = m_iterations, budgetR = totalBudget, budgetS;
	uint32_t remoteBaseOffset;

	switch(OperationsCountPerfTest::testMode){
		case TEST_WRITE:
			if(m_write_mode == WRITE_MODE_IMMEDIATE){
				// Calculate receive budget for even blocks
				if(budgetR > (int)m_max_rdma_wr_per_thread){ budgetR = m_max_rdma_wr_per_thread; }
				totalBudget -= budgetR;
				i += budgetR;
				for(int j = 0; j < budgetR; j++){
					m_server->receiveWriteImm(clientIds[m_thread_id]);
				}

				do {
					// Calculate send budget for odd blocks
					budgetS = totalBudget;
					if(budgetS > (int)m_max_rdma_wr_per_thread){ budgetS = m_max_rdma_wr_per_thread; }
					totalBudget -= budgetS;
					for(int j = 0; j < budgetR; j++){
						m_server->pollReceive(clientIds[m_thread_id], true, &remoteBaseOffset);
					}

					// Calculate receive budget for even blocks
					budgetR = totalBudget; 
					if(budgetR > (int)m_max_rdma_wr_per_thread){ budgetR = m_max_rdma_wr_per_thread; }
					totalBudget -= budgetR;

					for(int j = 0; j < budgetR; j++){
						m_server->receiveWriteImm(clientIds[m_thread_id]);
					}
					for(int j = 0; j < budgetS; j++){
						sendCounter = (sendCounter+1) % m_buffer_slots;
						int sendOffset = sendCounter * m_packet_size + m_memory_size_per_thread;
						uint32_t remoteOffset = remoteBaseOffset + sendCounter * m_packet_size; 
						m_server->writeImm(clientIds[m_thread_id], remoteOffset, m_local_memory->pointer(sendOffset), m_packet_size, (uint32_t)(i+j), (j+1)==budgetS); // signaled: (j+1)==budget
					}

					i += 2 * m_max_rdma_wr_per_thread;
				} while(i < m_iterations);

				for(int j = 0; j < budgetR; j++){ // final poll to sync up with client again
					m_server->pollReceive(clientIds[m_thread_id], true, &remoteBaseOffset);
				}
			}
			break;

		case TEST_SEND_AND_RECEIVE:
			// Calculate receive budget for even blocks
			if(budgetR > (int)m_max_rdma_wr_per_thread){ budgetR = m_max_rdma_wr_per_thread; }
			totalBudget -= budgetR;
			i += budgetR;
			for(int j = 0; j < budgetR; j++){
				receiveCounter = (receiveCounter+1) % m_buffer_slots;
				int receiveOffset = receiveCounter * m_packet_size;
				m_server->receive(clientIds[m_thread_id], m_local_memory->pointer(receiveOffset), m_packet_size);
			}

			do {
				// Calculate send budget for odd blocks
				budgetS = totalBudget;
				if(budgetS > (int)m_max_rdma_wr_per_thread){ budgetS = m_max_rdma_wr_per_thread; }
				totalBudget -= budgetS;

				for(int j = 0; j < budgetR; j++){
					m_server->pollReceive(clientIds[m_thread_id], true);
				}

				// Calculate receive budget for even blocks
				budgetR = totalBudget; 
				if(budgetR > (int)m_max_rdma_wr_per_thread){ budgetR = m_max_rdma_wr_per_thread; }
				totalBudget -= budgetR;
				for(int j = 0; j < budgetR; j++){
					receiveCounter = (receiveCounter+1) % m_buffer_slots;
					int receiveOffset = receiveCounter * m_packet_size;
					m_server->receive(clientIds[m_thread_id], m_local_memory->pointer(receiveOffset), m_packet_size);
				}

				for(int j = 0; j < budgetS; j++){
					sendCounter = (sendCounter+1) % m_buffer_slots;
					int sendOffset = sendCounter * m_packet_size + m_memory_size_per_thread;
					m_server->send(clientIds[m_thread_id], m_local_memory->pointer(sendOffset), m_packet_size, (j+1)==budgetS); // signaled: (j+1)==budget
				}

				i += 2 * m_max_rdma_wr_per_thread;
			} while(i < m_iterations);
			
			for(int j = 0; j < budgetR; j++){ // Finaly poll to sync up with client again
				m_server->pollReceive(clientIds[m_thread_id], true);
			}
			break;

		default: break;
	}
	//m_elapsedReceive = rdma::PerfTest::stopTimer(start);
}


rdma::OperationsCountPerfTest::OperationsCountPerfTest(bool is_server, std::vector<std::string> rdma_addresses, int rdma_port, int gpu_index, int thread_count, uint64_t packet_size, int buffer_slots, uint64_t iterations, WriteMode write_mode) : PerfTest(){
	this->m_is_server = is_server;
	this->m_rdma_port = rdma_port;
	this->m_gpu_index = gpu_index;
	this->m_thread_count = thread_count;
	this->m_packet_size = packet_size;
	this->m_buffer_slots = buffer_slots;
	this->m_memory_size = thread_count * packet_size * buffer_slots * 3; // 3x because for send + receive + write/read separat
	this->m_iterations = iterations;
	this->m_write_mode = (write_mode!=WRITE_MODE_AUTO ? write_mode : rdma::OperationsCountPerfTest::DEFAULT_WRITE_MODE);
	this->m_rdma_addresses = rdma_addresses;
}
rdma::OperationsCountPerfTest::~OperationsCountPerfTest(){
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

std::string rdma::OperationsCountPerfTest::getTestParameters(bool forCSV){
	std::ostringstream oss;
	oss << (m_is_server ? "Server" : "Client") << ", threads=" << m_thread_count << ", bufferslots=" << m_buffer_slots;
	if(!forCSV){ oss << ", packetsize=" << m_packet_size; }
	oss << ", memory=" << m_memory_size << " (2x " << m_thread_count << "x " << m_buffer_slots << "x " << m_packet_size << ") [";
	if(m_gpu_index < 0){
		oss << "MAIN";
	} else {
		oss << "GPU." << m_gpu_index; 
	}
	oss << " mem], iterations=" << (m_iterations*m_thread_count) << ", writemode=" << (m_write_mode==WRITE_MODE_NORMAL ? "Normal" : "Immediate");
	return oss.str();
}

std::string rdma::OperationsCountPerfTest::getTestParameters(){
	return getTestParameters(false);
}

void rdma::OperationsCountPerfTest::makeThreadsReady(TestMode testMode){
	OperationsCountPerfTest::testMode = testMode;
	OperationsCountPerfTest::signaled = false;
	for(OperationsCountPerfServerThread* perfThread : m_server_threads){
		perfThread->start();
		while(!perfThread->ready()) {
			usleep(Config::RDMA_SLEEP_INTERVAL);
		}
	}
	for(OperationsCountPerfClientThread* perfThread : m_client_threads){
		perfThread->start();
		while(!perfThread->ready()) {
			usleep(Config::RDMA_SLEEP_INTERVAL);
		}
	}
}

void rdma::OperationsCountPerfTest::runThreads(){
	OperationsCountPerfTest::signaled = false;
	unique_lock<mutex> lck(OperationsCountPerfTest::waitLock);
	OperationsCountPerfTest::waitCv.notify_all();
	OperationsCountPerfTest::signaled = true;
	lck.unlock();
	for (size_t i = 0; i < m_server_threads.size(); i++) {
		m_server_threads[i]->join();
	}
	for (size_t i = 0; i < m_client_threads.size(); i++) {
		m_client_threads[i]->join();
	}
}

void rdma::OperationsCountPerfTest::setupTest(){
	m_elapsedWrite = -1;
	m_elapsedRead = -1;
	m_elapsedSend = -1;
	#ifdef CUDA_ENABLED /* defined in CMakeLists.txt to globally enable/disable CUDA support */
		m_memory = (m_gpu_index<0 ? (rdma::BaseMemory*)new rdma::MainMemory(m_memory_size) : (rdma::BaseMemory*)new rdma::CudaMemory(m_memory_size, m_gpu_index));
	#else
		m_memory = (rdma::BaseMemory*)new MainMemory(m_memory_size);
	#endif

	size_t max_rdma_wr_per_thread = rdma::Config::RDMA_MAX_WR / m_thread_count;

	if(m_is_server){
		// Server
		m_server = new RDMAServer<ReliableRDMA>("OperationsCountTestRDMAServer", m_rdma_port, m_memory);
		for (int thread_id = 0; thread_id < m_thread_count; thread_id++) {
			OperationsCountPerfServerThread* perfThread = new OperationsCountPerfServerThread(m_server, m_packet_size, m_buffer_slots, m_iterations, max_rdma_wr_per_thread, m_write_mode, thread_id);
			m_server_threads.push_back(perfThread);
		}
		/* If server only allows to be single threaded
		OperationsCountPerfServerThread* perfThread = new OperationsCountPerfServerThread(m_server, m_memory_size_per_thread*m_thread_count, m_iterations*m_thread_count);
		m_server_threads.push_back(perfThread); */

	} else {
		// Client
		for (int i = 0; i < m_thread_count; i++) {
			OperationsCountPerfClientThread* perfThread = new OperationsCountPerfClientThread(m_memory, m_rdma_addresses, m_packet_size, m_buffer_slots, m_iterations, max_rdma_wr_per_thread, m_write_mode);
			m_client_threads.push_back(perfThread);
		}
	}
}

void rdma::OperationsCountPerfTest::runTest(){
	if(m_is_server){
		// Server
		std::cout << "Starting server on '" << rdma::Config::getIP(rdma::Config::RDMA_INTERFACE) << ":" << m_rdma_port << "' . . ." << std::endl;
		if(!m_server->startServer()){
			std::cerr << "OperationsCountPerfTest::runTest(): Could not start server" << std::endl;
			throw invalid_argument("OperationsCountPerfTest server startup failed");
		} else {
			std::cout << "Server running on '" << rdma::Config::getIP(rdma::Config::RDMA_INTERFACE) << ":" << m_rdma_port << "'" << std::endl; // TODO REMOVE
		}

		// waiting until clients have connected
		while(m_server->getConnectedConnIDs().size() < (size_t)m_thread_count) usleep(Config::RDMA_SLEEP_INTERVAL);

		makeThreadsReady(TEST_WRITE); // receive
		//auto startReceive = rdma::PerfTest::startTimer();
        runThreads();
		//m_elapsedReceive = rdma::PerfTest::stopTimer(startReceive);

		makeThreadsReady(TEST_SEND_AND_RECEIVE); // receive
		//auto startReceive = rdma::PerfTest::startTimer();
        runThreads();
		//m_elapsedReceive = rdma::PerfTest::stopTimer(startReceive);

		// wait until server is done
		while (m_server->isRunning() && m_server->getConnectedConnIDs().size() > 0) usleep(Config::RDMA_SLEEP_INTERVAL);
		std::cout << "Server stopped" << std::endl;

	} else {
		// Client

        // Measure operations/s for writing
		makeThreadsReady(TEST_WRITE); // write
		usleep(4 * Config::RDMA_SLEEP_INTERVAL); // let server first post the receives if writeImm
		auto startWrite = rdma::PerfTest::startTimer();
        runThreads();
		m_elapsedWrite = rdma::PerfTest::stopTimer(startWrite);

		// Measure operations/s for reading
		makeThreadsReady(TEST_READ); // read
		auto startRead = rdma::PerfTest::startTimer();
        runThreads();
		m_elapsedRead = rdma::PerfTest::stopTimer(startRead);

		// Measure operations/s for sending
		makeThreadsReady(TEST_SEND_AND_RECEIVE); // send
		usleep(4 * Config::RDMA_SLEEP_INTERVAL); // let server first post the receives
		auto startSend = rdma::PerfTest::startTimer();
        runThreads();
		m_elapsedSend = rdma::PerfTest::stopTimer(startSend);
	}
}


std::string rdma::OperationsCountPerfTest::getTestResults(std::string csvFileName, bool csvAddHeader){
	if(m_is_server){
		return "only client";
	} else {

		const long double tu = (long double)NANO_SEC; // 1sec (nano to seconds as time unit)
        const long double itrs = (long double)m_iterations, totalItrs = itrs*m_thread_count;

		int64_t maxWriteNs=-1, minWriteNs=std::numeric_limits<int64_t>::max();
		int64_t maxReadNs=-1, minReadNs=std::numeric_limits<int64_t>::max();
		int64_t maxSendNs=-1, minSendNs=std::numeric_limits<int64_t>::max();
		int64_t arrWriteNs[m_thread_count];
		int64_t arrReadNs[m_thread_count];
		int64_t arrSendNs[m_thread_count];
		long double avgWriteNs=0, medianWriteNs, avgReadNs=0, medianReadNs, avgSendNs=0, medianSendNs;

		for(size_t i=0; i<m_client_threads.size(); i++){
			OperationsCountPerfClientThread *thr = m_client_threads[i];
			if(thr->m_elapsedWrite < minWriteNs) minWriteNs = thr->m_elapsedWrite;
			if(thr->m_elapsedWrite > maxWriteNs) maxWriteNs = thr->m_elapsedWrite;
			avgWriteNs += (long double) thr->m_elapsedWrite;
			arrWriteNs[i] = thr->m_elapsedWrite;
			if(thr->m_elapsedRead < minReadNs) minReadNs = thr->m_elapsedRead;
			if(thr->m_elapsedRead > maxReadNs) maxReadNs = thr->m_elapsedRead;
			avgReadNs += (long double) thr->m_elapsedRead;
			arrReadNs[i] = thr->m_elapsedRead;
			if(thr->m_elapsedSend < minSendNs) minSendNs = thr->m_elapsedSend;
			if(thr->m_elapsedSend > maxSendNs) maxSendNs = thr->m_elapsedSend;
			avgSendNs += (long double) thr->m_elapsedSend;
			arrSendNs[i] = thr->m_elapsedSend;
		}
		avgWriteNs /= (long double) m_thread_count;
		avgReadNs /= (long double) m_thread_count;
		avgSendNs /= (long double) m_thread_count;
		std::sort(arrWriteNs, arrWriteNs + m_thread_count);
		std::sort(arrReadNs, arrReadNs + m_thread_count);
		std::sort(arrSendNs, arrSendNs + m_thread_count);
		medianWriteNs = arrWriteNs[(int)(m_thread_count/2)];
		medianReadNs = arrReadNs[(int)(m_thread_count/2)];
		medianSendNs = arrSendNs[(int)(m_thread_count/2)];

		// write results into CSV file
		if(!csvFileName.empty()){
			const uint64_t su = 1000*1000; // size unit (operations to megaOps)
			std::ofstream ofs;
			ofs.open(csvFileName, std::ofstream::out | std::ofstream::app);
			if(csvAddHeader){
				ofs << std::endl << "OPERATIONS PER SECOND, " << getTestParameters(true) << std::endl;
				ofs << "PacketSize [Bytes], Write [megaOp/s], Read [megaOp/s], Send/Recv [megaOp/s], ";
				ofs << "Min Write [megaOp/s], Min Read [megaOp/s], Min Send/Recv [megaOp/s], ";
				ofs << "Max Write [megaOp/s], Max Read [megaOp/s], Max Send/Recv [megaOp/s], ";
				ofs << "Avg Write [megaOp/s], Avg Read [megaOp/s], Avg Send/Recv [megaOp/s], ";
				ofs << "Median Write [megaOp/s], Median Read [megaOp/s], Median Send/Recv [megaOp/s], ";
				ofs << "Write [Sec], Read [Sec], Send/Recv [Sec], ";
				ofs << "Min Write [Sec], Min Read [Sec], Min Send/Recv [Sec], ";
				ofs << "Max Write [Sec], Max Read [Sec], Max Send/Recv [Sec], ";
				ofs << "Avg Write [Sec], Avg Read [Sec], Avg Send/Recv [Sec], ";
				ofs << "Median Write [Sec], Median Read [Sec], Median Send/Recv [Sec]" << std::endl;
			}
			ofs << m_packet_size << ", "; // packet size Bytes
			ofs << (round(totalItrs*tu/su/m_elapsedWrite * 100000)/100000.0) << ", "; // write Op/s
			ofs << (round(totalItrs*tu/su/m_elapsedRead * 100000)/100000.0) << ", "; // read Op/s
			ofs << (round(totalItrs*tu/su/m_elapsedSend * 100000)/100000.0) << ", "; // send/recv Op/s
			ofs << (round(itrs*tu/su/maxWriteNs * 100000)/100000.0) << ", "; // min write Op/s
			ofs << (round(itrs*tu/su/maxReadNs * 100000)/100000.0) << ", "; // min read Op/s
			ofs << (round(itrs*tu/su/maxSendNs * 100000)/100000.0) << ", "; // min send/recv Op/s
			ofs << (round(itrs*tu/su/minWriteNs * 100000)/100000.0) << ", "; // max write Op/s
			ofs << (round(itrs*tu/su/minReadNs * 100000)/100000.0) << ", "; // max read Op/s
			ofs << (round(itrs*tu/su/minSendNs * 100000)/100000.0) << ", "; // max send/recv Op/s
			ofs << (round(itrs*tu/su/avgWriteNs * 100000)/100000.0) << ", "; // avg write Op/s
			ofs << (round(itrs*tu/su/avgReadNs * 100000)/100000.0) << ", "; // avg read Op/s
			ofs << (round(itrs*tu/su/avgSendNs * 100000)/100000.0) << ", "; // avg send/recv Op/s
			ofs << (round(itrs*tu/su/medianWriteNs * 100000)/100000.0) << ", "; // median write Op/s
			ofs << (round(itrs*tu/su/medianReadNs * 100000)/100000.0) << ", "; // median read Op/s
			ofs << (round(itrs*tu/su/medianSendNs * 100000)/100000.0) << ", "; // median send/recv Op/s
			ofs << (round(m_elapsedWrite/tu * 100000)/100000.0) << ", " << (round(m_elapsedRead/tu * 100000)/100000.0) << ", "; // write, read Sec
			ofs << (round(m_elapsedSend/tu * 100000)/100000.0) << ", "; // send Sec
			ofs << (round(minWriteNs/tu * 100000)/100000.0) << ", " << (round(minReadNs/tu * 100000)/100000.0) << ", "; // min write, read Sec
			ofs << (round(minSendNs/tu * 100000)/100000.0) << ", "; // min send Sec
			ofs << (round(maxWriteNs/tu * 100000)/100000.0) << ", " << (round(maxReadNs/tu * 100000)/100000.0) << ", "; // max write, read Sec
			ofs << (round(maxSendNs/tu * 100000)/100000.0) << ", "; // max send Sec
			ofs << (round(avgWriteNs/tu * 100000)/100000.0) << ", " << (round(avgReadNs/tu * 100000)/100000.0) << ", "; // avg write, read Sec
			ofs << (round(avgSendNs/tu * 100000)/100000.0) << ", "; // avg send Sec
			ofs << (round(medianWriteNs/tu * 100000)/100000.0) << ", " << (round(medianReadNs/tu * 100000)/100000.0) << ", "; // median write, read Sec
			ofs << (round(medianSendNs/tu * 100000)/100000.0) << std::endl; // median send Sec
			ofs.close();
		}

		// generate result string
		std::ostringstream oss;
		oss << " measurement for sending and writeImm is executed as alternating send/receive bursts with " << (Config::RDMA_MAX_WR/m_thread_count) << " operations per burst" << std::endl;
		oss << " - Write:         operations = " << rdma::PerfTest::convertCountPerSec(totalItrs*tu/m_elapsedWrite); 
		oss << "  (range = " << rdma::PerfTest::convertCountPerSec(itrs*tu/maxWriteNs) << " - " << rdma::PerfTest::convertCountPerSec(itrs*tu/minWriteNs);
		oss << " ; avg=" << rdma::PerfTest::convertCountPerSec(itrs*tu/avgWriteNs) << " ; median=";
		oss << rdma::PerfTest::convertCountPerSec(itrs*tu/minWriteNs) << ")";
		oss << "   &   time = " << rdma::PerfTest::convertTime(m_elapsedWrite) << "  (range=";
		oss << rdma::PerfTest::convertTime(minWriteNs) << "-" << rdma::PerfTest::convertTime(maxWriteNs);
		oss << " ; avg=" << rdma::PerfTest::convertTime(avgWriteNs) << " ; median=" << rdma::PerfTest::convertTime(medianWriteNs) << ")" << std::endl;
		oss << " - Read:          operations = " << rdma::PerfTest::convertCountPerSec(totalItrs*tu/m_elapsedRead);
		oss << "  (range = " << rdma::PerfTest::convertCountPerSec(itrs*tu/maxReadNs) << " - " << rdma::PerfTest::convertCountPerSec(itrs*tu/minReadNs);
		oss << ", avg=" << rdma::PerfTest::convertCountPerSec(itrs*tu/avgReadNs) << " ; median=";
		oss << rdma::PerfTest::convertCountPerSec(itrs*tu/minReadNs) << ")";
		oss << "   &   time = " << rdma::PerfTest::convertTime(m_elapsedRead) << "  (range=";
		oss << rdma::PerfTest::convertTime(minReadNs) << "-" << rdma::PerfTest::convertTime(maxReadNs);
		oss << " ; avg=" << rdma::PerfTest::convertTime(avgReadNs) << " ; median=" << rdma::PerfTest::convertTime(medianReadNs) << ")" << std::endl;
		oss << " - Send:          operations = " << rdma::PerfTest::convertCountPerSec(totalItrs*tu/m_elapsedSend);
		oss << "  (range = " << rdma::PerfTest::convertCountPerSec(itrs*tu/maxSendNs) << " - " << rdma::PerfTest::convertCountPerSec(itrs*tu/minSendNs);
		oss << " ; avg=" << rdma::PerfTest::convertCountPerSec(itrs*tu/avgSendNs) << " ; median=";
		oss << rdma::PerfTest::convertCountPerSec(itrs*tu/minSendNs) << ")";
		oss << "   &   time = " << rdma::PerfTest::convertTime(m_elapsedSend) << "  (range=";
		oss << rdma::PerfTest::convertTime(minSendNs) << "-" << rdma::PerfTest::convertTime(maxSendNs);
		oss << " ; avg=" << rdma::PerfTest::convertTime(avgSendNs) << " ; median=" << rdma::PerfTest::convertTime(medianSendNs) << ")" << std::endl;
		return oss.str();

	}
	return NULL;
}