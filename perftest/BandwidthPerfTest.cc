#include "BandwidthPerfTest.h"

#include "../src/memory/BaseMemory.h"
#include "../src/memory/MainMemory.h"
#include "../src/memory/CudaMemory.h"
#include "../src/utils/Config.h"

#include <limits>
#include <algorithm>

mutex rdma::BandwidthPerfTest::waitLock;
condition_variable rdma::BandwidthPerfTest::waitCv;
bool rdma::BandwidthPerfTest::signaled;
rdma::TestMode rdma::BandwidthPerfTest::testMode;

rdma::BandwidthPerfClientThread::BandwidthPerfClientThread(BaseMemory *memory, std::vector<std::string>& rdma_addresses, size_t packet_size, int buffer_slots, size_t iterations, size_t max_rdma_wr_per_thread, WriteMode write_mode) {
	this->m_client = new RDMAClient<ReliableRDMA>(memory, "BandwidthPerfTestClient");
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
			std::cerr << "BandwidthPerfThread::BandwidthPerfThread(): Could not connect to '" << conn << "'" << std::endl;
			throw invalid_argument("BandwidthPerfThread connection failed");
		}
		//std::cout << "Thread connected to '" << conn << "'" << std::endl; // TODO REMOVE
		m_addr.push_back(nodeId);
		m_client->remoteAlloc(conn, m_memory_size_per_thread, m_remOffsets[i]);
	}

	m_local_memory = m_client->localMalloc(m_memory_size_per_thread);
	m_local_memory->openContext();
	m_local_memory->setMemory(1);
}


rdma::BandwidthPerfClientThread::~BandwidthPerfClientThread() {
	for (size_t i = 0; i < m_rdma_addresses.size(); ++i) {
		string addr = m_rdma_addresses[i];
		m_client->remoteFree(addr, m_memory_size_per_thread, m_remOffsets[i]);
	}
    delete m_remOffsets;
	delete m_local_memory; // implicitly deletes local allocs in RDMAClient and also closes context
	delete m_client;
}


void rdma::BandwidthPerfClientThread::run() {
	unique_lock<mutex> lck(BandwidthPerfTest::waitLock);
	if (!BandwidthPerfTest::signaled) {
		m_ready = true;
		BandwidthPerfTest::waitCv.wait(lck);
	}
	lck.unlock();
	int offset = 0;
	int sendCounter = 0, receiveCounter = 0;
	uint32_t localBaseOffset = (uint32_t)m_local_memory->getRootOffset();
	auto start = rdma::PerfTest::startTimer();
	switch(BandwidthPerfTest::testMode){
		case TEST_WRITE: // Write
			switch(m_write_mode){
				case WRITE_MODE_NORMAL:
					for(size_t i = 0; i < m_iterations; i++){
						size_t connIdx = i % m_rdma_addresses.size();
						bool signaled = (i == (m_iterations - 1));
						offset = (i % m_buffer_slots) * m_packet_size;
						m_client->write(m_addr[connIdx], m_remOffsets[connIdx] + offset, m_local_memory->pointer(offset), m_packet_size, signaled);
					}
					break;
				case WRITE_MODE_IMMEDIATE:
					for(size_t i = 0; i < m_iterations; ){
						int budgetS = m_iterations - i;
						if(budgetS > (int)m_max_rdma_wr_per_thread){ budgetS = m_max_rdma_wr_per_thread; }

						size_t fi = i + budgetS;
						int budgetR = m_iterations - fi;
						if(budgetR >(int)m_max_rdma_wr_per_thread){ budgetR = m_max_rdma_wr_per_thread; }

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

						i += budgetS + budgetR;
					}
					break;
				default: throw invalid_argument("BandwidthPerfClientThread unknown write mode");
			}
			m_elapsedWriteMs = rdma::PerfTest::stopTimer(start);
			break;

		case TEST_READ: // Read
			for(size_t i = 0; i < m_iterations; i++){
				size_t connIdx = i % m_rdma_addresses.size();
				bool signaled = (i == (m_iterations - 1));
				offset = (i % m_buffer_slots) * m_packet_size;
				m_client->read(m_addr[connIdx], m_remOffsets[connIdx] + offset, m_local_memory->pointer(offset), m_packet_size, signaled);
			}
			m_elapsedReadMs = rdma::PerfTest::stopTimer(start);
			break;

		case TEST_SEND_AND_RECEIVE: // Send & Receive
			// alternating send/receive blocks to not overfill queues
			for(size_t i = 0; i < m_iterations; ){
				int budgetS = m_iterations - i;
				if(budgetS > (int)m_max_rdma_wr_per_thread){ budgetS = m_max_rdma_wr_per_thread; }

				size_t fi = i + budgetS;
				int budgetR = m_iterations - fi;
				if(budgetR >(int)m_max_rdma_wr_per_thread){ budgetR = m_max_rdma_wr_per_thread; }

				for(int j = 0; j < budgetR; j++){
					// TODO REMOVE std::cout << "Send: " << (i+j) << std::endl; // TODO REMOVE
					receiveCounter = (receiveCounter+1) % m_buffer_slots;
					int receiveOffset = receiveCounter * m_packet_size;
					m_client->receive(m_addr[(fi+j) % m_rdma_addresses.size()], m_local_memory->pointer(receiveOffset), m_packet_size);
				}

				for(int j = 0; j < budgetS; j++){
					// TODO REMOVE std::cout << "Send: " << (i+j) << std::endl; // TODO REMOVE
					sendCounter = (sendCounter+1) % m_buffer_slots;
					int sendOffset = sendCounter * m_packet_size + m_memory_size_per_thread;
					m_client->send(m_addr[(i+j) % m_rdma_addresses.size()], m_local_memory->pointer(sendOffset), m_packet_size, (j+1)==budgetS); // signaled: (j+1)==budget
				}

				for(int j = 0; j < budgetR; j++){
					m_client->pollReceive(m_addr[(fi+j) % m_rdma_addresses.size()], true);
				}

				i += budgetS + budgetR;
			}
			m_elapsedSendMs = rdma::PerfTest::stopTimer(start);
			break;
		default: throw invalid_argument("BandwidthPerfClientThread unknown test mode");
	}
}



rdma::BandwidthPerfServerThread::BandwidthPerfServerThread(RDMAServer<ReliableRDMA> *server, size_t packet_size, int buffer_slots, size_t iterations, size_t max_rdma_wr_per_thread, WriteMode write_mode, int thread_id) {
	this->m_server = server;
	this->m_packet_size = packet_size;
	this->m_buffer_slots = buffer_slots;
	this->m_memory_size_per_thread = packet_size * buffer_slots;
	this->m_iterations = iterations;
	this->m_max_rdma_wr_per_thread = max_rdma_wr_per_thread;
	this->m_write_mode = write_mode;
	this->m_thread_id = thread_id;
	this->m_local_memory = server->localMalloc(this->m_memory_size_per_thread);
	this->m_local_memory->openContext();
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
	const std::vector<size_t> clientIds = m_server->getConnectedConnIDs();

	int sendCounter = 0, receiveCounter = 0, totalBudget = m_iterations;
	int budgetR = totalBudget;
	uint32_t remoteBaseOffset;

	switch(BandwidthPerfTest::testMode){
		case TEST_WRITE:
			if(m_write_mode == WRITE_MODE_IMMEDIATE){
				// Calculate receive budget for even blocks
				if(budgetR > (int)m_max_rdma_wr_per_thread){ budgetR = m_max_rdma_wr_per_thread; }
				totalBudget -= budgetR;
				for(int j = 0; j < budgetR; j++){
					m_server->receiveWriteImm(clientIds[m_thread_id]);
				}

				for(size_t i = budgetR; i < m_iterations; ){

					// Calculate send budget for odd blocks
					int budgetS = totalBudget;
					if(budgetS > (int)m_max_rdma_wr_per_thread){ budgetS = m_max_rdma_wr_per_thread; }
					else if(budgetS < 0){ budgetS = 0; }
					totalBudget -= budgetS;

					for(int j = 0; j < budgetR; j++){
						m_server->pollReceive(clientIds[m_thread_id], true, &remoteBaseOffset);
					}

					// Calculate receive budget for even blocks
					budgetR = totalBudget; 
					if(budgetR > (int)m_max_rdma_wr_per_thread){ budgetR = m_max_rdma_wr_per_thread; }
					else if(budgetR < 0){ budgetR = 0; }
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

					i += budgetR + budgetS;
				}
			}
			break;

		case TEST_SEND_AND_RECEIVE:
			// Calculate receive budget for even blocks
			if(budgetR > (int)m_max_rdma_wr_per_thread){ budgetR = m_max_rdma_wr_per_thread; }
			totalBudget -= budgetR;
			for(int j = 0; j < budgetR; j++){
				// TODO REMOVE std::cout << "Send: " << (i+j) << std::endl; // TODO REMOVE
				receiveCounter = (receiveCounter+1) % m_buffer_slots;
				int receiveOffset = receiveCounter * m_packet_size;
				m_server->receive(clientIds[m_thread_id], m_local_memory->pointer(receiveOffset), m_packet_size);
			}

			for(size_t i = budgetR; i < m_iterations; ){

				// Calculate send budget for odd blocks
				int budgetS = totalBudget;
				if(budgetS > (int)m_max_rdma_wr_per_thread){ budgetS = m_max_rdma_wr_per_thread; }
				else if(budgetS < 0){ budgetS = 0; }
				totalBudget -= budgetS;

				for(int j = 0; j < budgetR; j++){
					m_server->pollReceive(clientIds[m_thread_id], true);
				}

				// Calculate receive budget for even blocks
				budgetR = totalBudget; 
				if(budgetR > (int)m_max_rdma_wr_per_thread){ budgetR = m_max_rdma_wr_per_thread; }
				else if(budgetR < 0){ budgetR = 0; }
				totalBudget -= budgetR;
				for(int j = 0; j < budgetR; j++){
					// TODO REMOVE std::cout << "Send: " << (i+j) << std::endl; // TODO REMOVE
					receiveCounter = (receiveCounter+1) % m_buffer_slots;
					int receiveOffset = receiveCounter * m_packet_size;
					m_server->receive(clientIds[m_thread_id], m_local_memory->pointer(receiveOffset), m_packet_size);
				}

				for(int j = 0; j < budgetS; j++){
					// TODO REMOVE std::cout << "Send: " << (i+j) << std::endl; // TODO REMOVE
					sendCounter = (sendCounter+1) % m_buffer_slots;
					int sendOffset = sendCounter * m_packet_size + m_memory_size_per_thread;
					m_server->send(clientIds[m_thread_id], m_local_memory->pointer(sendOffset), m_packet_size, (j+1)==budgetS); // signaled: (j+1)==budget
				}

				i += budgetR + budgetS;
			}
			break;

			default: break;
	}
	//m_elapsedReceive = rdma::PerfTest::stopTimer(start);
}



rdma::BandwidthPerfTest::BandwidthPerfTest(bool is_server, std::vector<std::string> rdma_addresses, int rdma_port, int gpu_index, int thread_count, uint64_t packet_size, int buffer_slots, uint64_t iterations, WriteMode write_mode) : PerfTest(){
	this->m_is_server = is_server;
	this->m_rdma_port = rdma_port;
	this->m_gpu_index = gpu_index;
	this->m_thread_count = thread_count;
	this->m_packet_size = packet_size;
	this->m_buffer_slots = buffer_slots;
	this->m_memory_size = 2 * thread_count * packet_size * buffer_slots; // 2x because for send & receive separat
	this->m_iterations = iterations;
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
std::string rdma::BandwidthPerfTest::getTestParameters(){
	return getTestParameters(false);
}

void rdma::BandwidthPerfTest::makeThreadsReady(TestMode testMode){
	BandwidthPerfTest::testMode = testMode;
	BandwidthPerfTest::signaled = false;
	for(BandwidthPerfServerThread* perfThread : m_server_threads){
		perfThread->start();
		while(!perfThread->ready()) {
			usleep(Config::RDMA_SLEEP_INTERVAL);
		}
	}
	for(BandwidthPerfClientThread* perfThread : m_client_threads){
		perfThread->start();
		while(!perfThread->ready()) {
			usleep(Config::RDMA_SLEEP_INTERVAL);
		}
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
	for (size_t i = 0; i < m_client_threads.size(); i++) {
		m_client_threads[i]->join();
	}
}

void rdma::BandwidthPerfTest::setupTest(){
	m_elapsedWriteMs = -1;
	m_elapsedReadMs = -1;
	m_elapsedSendMs = -1;
	#ifdef CUDA_ENABLED /* defined in CMakeLists.txt to globally enable/disable CUDA support */
		m_memory = (m_gpu_index<0 ? (rdma::BaseMemory*)new rdma::MainMemory(m_memory_size) : (rdma::BaseMemory*)new rdma::CudaMemory(m_memory_size, m_gpu_index));
	#else
		m_memory = (rdma::BaseMemory*)new MainMemory(m_memory_size);
	#endif

	const int max_rdma_wr_per_thread = rdma::Config::RDMA_MAX_WR / m_thread_count;

	if(m_is_server){
		// Server
		m_server = new RDMAServer<ReliableRDMA>("BandwidthTestRDMAServer", m_rdma_port, m_memory);
		for (int thread_id = 0; thread_id < m_thread_count; thread_id++) {
			BandwidthPerfServerThread* perfThread = new BandwidthPerfServerThread(m_server, m_packet_size, m_buffer_slots, m_iterations, max_rdma_wr_per_thread, m_write_mode, thread_id);
			m_server_threads.push_back(perfThread);
		}

	} else {
		// Client
		for (int i = 0; i < m_thread_count; i++) {
			BandwidthPerfClientThread* perfThread = new BandwidthPerfClientThread(m_memory, m_rdma_addresses, m_packet_size, m_buffer_slots, m_iterations, max_rdma_wr_per_thread, m_write_mode);
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

		// waiting until clients have connected to sync
		while(m_server->getConnectedConnIDs().size() < (size_t)m_thread_count) usleep(Config::RDMA_SLEEP_INTERVAL);
		
		makeThreadsReady(TEST_SEND_AND_RECEIVE); // receive
		//auto startReceive = rdma::PerfTest::startTimer();
        runThreads();
		//m_elapsedReceiveMs = rdma::PerfTest::stopTimer(startReceive);

		// wait until server is done
		while (m_server->isRunning() && m_server->getConnectedConnIDs().size() > 0) usleep(Config::RDMA_SLEEP_INTERVAL);
		std::cout << "Server stopped" << std::endl;

	} else {
		// Client


        // Measure bandwidth for writing
		makeThreadsReady(TEST_WRITE); // write
		auto startWrite = rdma::PerfTest::startTimer();
        runThreads();
		m_elapsedWriteMs = rdma::PerfTest::stopTimer(startWrite);

		// Measure bandwidth for reading
		makeThreadsReady(TEST_READ); // read
		auto startRead = rdma::PerfTest::startTimer();
        runThreads();
		m_elapsedReadMs = rdma::PerfTest::stopTimer(startRead);

		// Measure bandwidth for sending
		makeThreadsReady(TEST_SEND_AND_RECEIVE); // send
		usleep(4 * Config::RDMA_SLEEP_INTERVAL); // let server first post the receives
		auto startSend = rdma::PerfTest::startTimer();
        runThreads();
		m_elapsedSendMs = rdma::PerfTest::stopTimer(startSend);
	}
}


std::string rdma::BandwidthPerfTest::getTestResults(std::string csvFileName, bool csvAddHeader){
	if(m_is_server){
		return "only client";
	} else {

		const long double tu = (long double)NANO_SEC; // 1sec (nano to seconds as time unit)
		
		uint64_t transBytePerThr = m_iterations * m_packet_size;
		uint64_t transferedBytes = m_thread_count * transBytePerThr;
		int64_t maxWriteMs=-1, minWriteMs=std::numeric_limits<int64_t>::max();
		int64_t maxReadMs=-1, minReadMs=std::numeric_limits<int64_t>::max();
		int64_t maxSendMs=-1, minSendMs=std::numeric_limits<int64_t>::max();
		int64_t arrWriteMs[m_thread_count];
		int64_t arrReadMs[m_thread_count];
		int64_t arrSendMs[m_thread_count];
		long double avgWriteMs=0, medianWriteMs, avgReadMs=0, medianReadMs, avgSendMs=0, medianSendMs;

		for(size_t i=0; i<m_client_threads.size(); i++){
			BandwidthPerfClientThread *thr = m_client_threads[i];
			if(thr->m_elapsedWriteMs < minWriteMs) minWriteMs = thr->m_elapsedWriteMs;
			if(thr->m_elapsedWriteMs > maxWriteMs) maxWriteMs = thr->m_elapsedWriteMs;
			avgWriteMs += (long double) thr->m_elapsedWriteMs;
			arrWriteMs[i] = thr->m_elapsedWriteMs;
			if(thr->m_elapsedReadMs < minReadMs) minReadMs = thr->m_elapsedReadMs;
			if(thr->m_elapsedReadMs > maxReadMs) maxReadMs = thr->m_elapsedReadMs;
			avgReadMs += (long double) thr->m_elapsedReadMs;
			arrReadMs[i] = thr->m_elapsedReadMs;
			if(thr->m_elapsedSendMs < minSendMs) minSendMs = thr->m_elapsedSendMs;
			if(thr->m_elapsedSendMs > maxSendMs) maxSendMs = thr->m_elapsedSendMs;
			avgSendMs += (long double) thr->m_elapsedSendMs;
			arrSendMs[i] = thr->m_elapsedSendMs;
		}
		avgWriteMs /= (long double) m_thread_count;
		avgReadMs /= (long double) m_thread_count;
		avgSendMs /= (long double) m_thread_count;
		std::sort(arrWriteMs, arrWriteMs + m_thread_count);
		std::sort(arrReadMs, arrReadMs + m_thread_count);
		std::sort(arrSendMs, arrSendMs + m_thread_count);
		medianWriteMs = arrWriteMs[(int)(m_thread_count/2)];
		medianReadMs = arrReadMs[(int)(m_thread_count/2)];
		medianSendMs = arrSendMs[(int)(m_thread_count/2)];

		// write results into CSV file
		if(!csvFileName.empty()){
			const uint64_t su = 1000*1000; // size unit (bytes to MegaBytes) | use 1024*1024 for MebiBytes
			std::ofstream ofs;
			ofs.open(csvFileName, std::ofstream::out | std::ofstream::app);
			if(csvAddHeader){
				ofs << std::endl << "BANDWIDTH, " << getTestParameters(true) << std::endl;
				ofs << "PacketSize [Bytes], Transfered [MB], Write [MB/s], Read [MB/s], Send/Recv [MB/s], ";
				ofs << "Min Write [MB/s], Min Read [MB/s], Min Send/Recv [MB/s], ";
				ofs << "Max Write [MB/s], Max Read [MB/s], Max Send/Recv [MB/s], ";
				ofs << "Avg Write [MB/s], Avg Read [MB/s], Avg Send/Recv [MB/s], ";
				ofs << "Median Write [MB/s], Median Read [MB/s], Median Send/Recv [MB/s], ";
				ofs << "Write [Sec], Read [Sec], Send/Recv [Sec], ";
				ofs << "Min Write [Sec], Min Read [Sec], Min Send/Recv [Sec], ";
				ofs << "Max Write [Sec], Max Read [Sec], Max Send/Recv [Sec], ";
				ofs << "Avg Write [Sec], Avg Read [Sec], Avg Send/Recv [Sec], ";
				ofs << "Median Write [Sec], Median Read [Sec], Median Send/Recv [Sec]" << std::endl;
			}
			ofs << m_packet_size << ", " << (round(transferedBytes/su * 100000)/100000.0) << ", "; // packet size Bytes
			ofs << (round(transferedBytes*tu/su/m_elapsedWriteMs * 100000)/100000.0) << ", "; // write MB/s
			ofs << (round(transferedBytes*tu/su/m_elapsedReadMs * 100000)/100000.0) << ", "; // read MB/s
			ofs << (round(transferedBytes*tu/su/m_elapsedSendMs * 100000)/100000.0) << ", "; // send/recv MB/s
			ofs << (round(transBytePerThr*tu/su/maxWriteMs * 100000)/100000.0) << ", "; // min write MB/s
			ofs << (round(transBytePerThr*tu/su/maxReadMs * 100000)/100000.0) << ", "; // min read MB/s
			ofs << (round(transBytePerThr*tu/su/maxSendMs * 100000)/100000.0) << ", "; // min send/recv MB/s
			ofs << (round(transBytePerThr*tu/su/minWriteMs * 100000)/100000.0) << ", "; // max write MB/s
			ofs << (round(transBytePerThr*tu/su/minReadMs * 100000)/100000.0) << ", "; // max read MB/s
			ofs << (round(transBytePerThr*tu/su/minSendMs * 100000)/100000.0) << ", "; // max send/recv MB/s
			ofs << (round(transBytePerThr*tu/su/avgWriteMs * 100000)/100000.0) << ", "; // avg write MB/s
			ofs << (round(transBytePerThr*tu/su/avgReadMs * 100000)/100000.0) << ", "; // avg read MB/s
			ofs << (round(transBytePerThr*tu/su/avgSendMs * 100000)/100000.0) << ", "; // avg send/recv MB/s
			ofs << (round(transBytePerThr*tu/su/medianWriteMs * 100000)/100000.0) << ", "; // median write MB/s
			ofs << (round(transBytePerThr*tu/su/medianReadMs * 100000)/100000.0) << ", "; // median read MB/s
			ofs << (round(transBytePerThr*tu/su/medianSendMs * 100000)/100000.0) << ", "; // median send/recv MB/s
			ofs << (round(m_elapsedWriteMs/tu * 100000)/100000.0) << ", " << (round(m_elapsedReadMs/tu * 100000)/100000.0) << ", "; // write, read Sec
			ofs << (round(m_elapsedSendMs/tu * 100000)/100000.0) << ", "; // send Sec
			ofs << (round(minWriteMs/tu * 100000)/100000.0) << ", " << (round(minReadMs/tu * 100000)/100000.0) << ", "; // min write, read Sec
			ofs << (round(minSendMs/tu * 100000)/100000.0) << ", "; // min send Sec
			ofs << (round(maxWriteMs/tu * 100000)/100000.0) << ", " << (round(maxReadMs/tu * 100000)/100000.0) << ", "; // max write, read Sec
			ofs << (round(maxSendMs/tu * 100000)/100000.0) << ", "; // max send Sec
			ofs << (round(avgWriteMs/tu * 100000)/100000.0) << ", " << (round(avgReadMs/tu * 100000)/100000.0) << ", "; // avg write, read Sec
			ofs << (round(avgSendMs/tu * 100000)/100000.0) << ", "; // avg send Sec
			ofs << (round(medianWriteMs/tu * 100000)/100000.0) << ", " << (round(medianReadMs/tu * 100000)/100000.0) << ", "; // median write, read Sec
			ofs << (round(medianSendMs/tu * 100000)/100000.0) << std::endl; // median send Sec
			ofs.close();
		}

		// generate result string
		std::ostringstream oss;
		oss << "transfered = " << rdma::PerfTest::convertByteSize(transferedBytes) << std::endl;
		oss << " - Write:         bandwidth = " << rdma::PerfTest::convertBandwidth(transferedBytes*tu/m_elapsedWriteMs); 
		oss << "  (range = " << rdma::PerfTest::convertBandwidth(transBytePerThr*tu/maxWriteMs) << " - " << rdma::PerfTest::convertBandwidth(transBytePerThr*tu/minWriteMs);
		oss << " ; avg=" << rdma::PerfTest::convertBandwidth(transBytePerThr*tu/avgWriteMs) << " ; median=";
		oss << rdma::PerfTest::convertBandwidth(transBytePerThr*tu/minWriteMs) << ")";
		oss << "   &   time = " << rdma::PerfTest::convertTime(m_elapsedWriteMs) << "  (range=";
		oss << rdma::PerfTest::convertTime(minWriteMs) << "-" << rdma::PerfTest::convertTime(maxWriteMs);
		oss << " ; avg=" << rdma::PerfTest::convertTime(avgWriteMs) << " ; median=" << rdma::PerfTest::convertTime(medianWriteMs) << ")" << std::endl;
		oss << " - Read:          bandwidth = " << rdma::PerfTest::convertBandwidth(transferedBytes*tu/m_elapsedReadMs);
		oss << "  (range = " << rdma::PerfTest::convertBandwidth(transBytePerThr*tu/maxReadMs) << " - " << rdma::PerfTest::convertBandwidth(transBytePerThr*tu/minReadMs);
		oss << ", avg=" << rdma::PerfTest::convertBandwidth(transBytePerThr*tu/avgReadMs) << " ; median=";
		oss << rdma::PerfTest::convertBandwidth(transBytePerThr*tu/minReadMs) << ")";
		oss << "   &   time = " << rdma::PerfTest::convertTime(m_elapsedReadMs) << "  (range=";
		oss << rdma::PerfTest::convertTime(minReadMs) << "-" << rdma::PerfTest::convertTime(maxReadMs);
		oss << " ; avg=" << rdma::PerfTest::convertTime(avgReadMs) << " ; median=" << rdma::PerfTest::convertTime(medianReadMs) << ")" << std::endl;
		oss << " - Send:          bandwidth = " << rdma::PerfTest::convertBandwidth(transferedBytes*tu/m_elapsedSendMs);
		oss << "  (range = " << rdma::PerfTest::convertBandwidth(transBytePerThr*tu/maxSendMs) << " - " << rdma::PerfTest::convertBandwidth(transBytePerThr*tu/minSendMs);
		oss << " ; avg=" << rdma::PerfTest::convertBandwidth(transBytePerThr*tu/avgSendMs) << " ; median=";
		oss << rdma::PerfTest::convertBandwidth(transBytePerThr*tu/minSendMs) << ")";
		oss << "   &   time = " << rdma::PerfTest::convertTime(m_elapsedSendMs) << "  (range=";
		oss << rdma::PerfTest::convertTime(minSendMs) << "-" << rdma::PerfTest::convertTime(maxSendMs);
		oss << " ; avg=" << rdma::PerfTest::convertTime(avgSendMs) << " ; median=" << rdma::PerfTest::convertTime(medianSendMs) << ")" << std::endl;
		return oss.str();

	}
	return NULL;
}