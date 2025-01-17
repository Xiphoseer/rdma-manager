#include "AtomicsBandwidthPerfTest.h"

#include "../src/memory/BaseMemory.h"
#include "../src/memory/MainMemory.h"
#include "../src/memory/CudaMemory.h"
#include "../src/utils/Config.h"

#include <limits>
#include <algorithm>

mutex rdma::AtomicsBandwidthPerfTest::waitLock;
condition_variable rdma::AtomicsBandwidthPerfTest::waitCv;
bool rdma::AtomicsBandwidthPerfTest::signaled;
rdma::TestOperation rdma::AtomicsBandwidthPerfTest::testOperation;
size_t rdma::AtomicsBandwidthPerfTest::client_count;
size_t rdma::AtomicsBandwidthPerfTest::thread_count;

rdma::AtomicsBandwidthPerfClientThread::AtomicsBandwidthPerfClientThread(BaseMemory *memory, std::vector<std::string>& rdma_addresses, std::string ownIpPort, std::string sequencerIpPort, int buffer_slots, size_t iterations_per_thread) {
	this->m_client = new RDMAClient<ReliableRDMA>(memory, "AtomicsBandwidthPerfTestClient", ownIpPort, sequencerIpPort);
	this->m_rdma_addresses = rdma_addresses;
	this->m_remote_memory_per_thread = buffer_slots * rdma::ATOMICS_SIZE;
	this->m_memory_per_thread = m_remote_memory_per_thread * rdma_addresses.size();
	this->m_buffer_slots = buffer_slots;
	this->m_iterations_per_thread = iterations_per_thread;
	m_remOffsets = new size_t[m_rdma_addresses.size()];

	for (size_t i = 0; i < m_rdma_addresses.size(); ++i) {
	    NodeID  nodeId = 0;
		//ib_addr_t ibAddr;
		string conn = m_rdma_addresses[i];
		//std::cout << "Thread trying to connect to '" << conn << "' . . ." << std::endl; // TODO REMOVE
		if(!m_client->connect(conn, nodeId)) {
			std::cerr << "AtomicsBandwidthPerfThread::BandwidthPerfThread(): Could not connect to '" << conn << "'" << std::endl;
			throw invalid_argument("AtomicsBandwidthPerfThread connection failed");
		}
		//std::cout << "Thread connected to '" << conn << "'" << std::endl; // TODO REMOVE
		m_addr.push_back(nodeId);
		m_client->remoteAlloc(conn, m_remote_memory_per_thread, m_remOffsets[i]);
	}

	m_local_memory = m_client->localMalloc(m_memory_per_thread);
	m_local_memory->openContext();
	m_local_memory->setMemory(1);
}

rdma::AtomicsBandwidthPerfClientThread::~AtomicsBandwidthPerfClientThread() {
	for (size_t i = 0; i < m_rdma_addresses.size(); ++i) {
		string addr = m_rdma_addresses[i];
		m_client->remoteFree(addr, m_remote_memory_per_thread, m_remOffsets[i]);
	}
    delete m_remOffsets;
	delete m_local_memory; // implicitly deletes local allocs in RDMAClient
	delete m_client;
}

void rdma::AtomicsBandwidthPerfClientThread::run() {
	rdma::PerfTest::global_barrier_client(m_client, m_addr); // global barrier
	unique_lock<mutex> lck(AtomicsBandwidthPerfTest::waitLock); // local barrier
	if (!AtomicsBandwidthPerfTest::signaled) {
		m_ready = true;
		AtomicsBandwidthPerfTest::waitCv.wait(lck);
	}
	lck.unlock();
	m_ready = false;

	auto start = rdma::PerfTest::startTimer();
	switch(AtomicsBandwidthPerfTest::testOperation){
		case FETCH_ADD_OPERATION: // Fetch & Add
			for(size_t i = 0; i < m_iterations_per_thread; i++){
				for(size_t connIdx=0; connIdx < m_rdma_addresses.size(); connIdx++){
					bool signaled = (i == (m_iterations_per_thread - 1) || (i+1)%Config::RDMA_MAX_WR==0);
					int offset = (i % m_buffer_slots) * rdma::ATOMICS_SIZE;
					m_client->fetchAndAdd(m_addr[connIdx], m_remOffsets[connIdx] + offset, m_local_memory->pointer(offset), 1, rdma::ATOMICS_SIZE, signaled); // true=signaled
				}
			}
			if(AtomicsBandwidthPerfTest::client_count > 1) rdma::PerfTest::global_barrier_client(m_client, m_addr, false); // global end barrier if multiple nodes
			m_elapsedFetchAddMs = rdma::PerfTest::stopTimer(start);
			break;
		case COMPARE_SWAP_OPERATION: // Compare & Swap
			for(size_t i = 0; i < m_iterations_per_thread; i++){
				for(size_t connIdx=0; connIdx < m_rdma_addresses.size(); connIdx++){
					bool signaled = (i == (m_iterations_per_thread - 1) || (i+1)%Config::RDMA_MAX_WR==0);
					int offset = (i % m_buffer_slots) * rdma::ATOMICS_SIZE;
					m_client->compareAndSwap(m_addr[connIdx], m_remOffsets[connIdx] + offset, m_local_memory->pointer(offset), 2, 3, rdma::ATOMICS_SIZE, signaled); // true=signaled
				}
			}
			if(AtomicsBandwidthPerfTest::client_count > 1) rdma::PerfTest::global_barrier_client(m_client, m_addr, false); // global end barrier if multiple nodes
			m_elapsedCompareSwapMs = rdma::PerfTest::stopTimer(start);
			break;
		default: throw invalid_argument("BandwidthPerfClientThread unknown test mode");
	}
}




rdma::AtomicsBandwidthPerfTest::AtomicsBandwidthPerfTest(int testOperations, bool is_server, std::vector<std::string> rdma_addresses, int rdma_port, std::string ownIpPort, std::string sequencerIpPort, int local_gpu_index, int remote_gpu_index, int client_count, int thread_count, int buffer_slots, uint64_t iterations_per_thread) : PerfTest(testOperations){
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
	this->m_memory_size = thread_count * rdma::ATOMICS_SIZE * buffer_slots * rdma_addresses.size();
	this->m_buffer_slots = buffer_slots;
	this->m_iterations_per_thread = iterations_per_thread;
	this->m_rdma_addresses = rdma_addresses;
}
rdma::AtomicsBandwidthPerfTest::~AtomicsBandwidthPerfTest(){
	for (size_t i = 0; i < m_client_threads.size(); i++) {
		delete m_client_threads[i];
	}
	m_client_threads.clear();
	if(m_is_server)
		delete m_server;
	delete m_memory;
}

std::string rdma::AtomicsBandwidthPerfTest::getTestParameters(bool forCSV){
	std::ostringstream oss;
	oss << (m_is_server ? "Server" : "Client") << ", threads=" << thread_count << ", bufferslots=" << m_buffer_slots << ", packetsize=" << rdma::ATOMICS_SIZE;
	oss << ", memory=" << m_memory_size << " (2x " << thread_count << "x " << m_buffer_slots << "x ";
	if(!m_is_server){ oss << m_rdma_addresses.size() << "x "; } oss << rdma::ATOMICS_SIZE << ")";
	oss << ", memory_type=" << getMemoryName(m_local_gpu_index, m_actual_gpu_index) << (m_remote_gpu_index!=-404 ? "->"+getMemoryName(m_remote_gpu_index) : "");
	if(!forCSV){
		oss << ", iterations=" << (m_iterations_per_thread*thread_count);
		oss << ", clients=" << client_count << ", servers=" << m_rdma_addresses.size();
	}
	return oss.str();
}
std::string rdma::AtomicsBandwidthPerfTest::getTestParameters(){
	return getTestParameters(false);
}

void rdma::AtomicsBandwidthPerfTest::makeThreadsReady(TestOperation testOperation){
	AtomicsBandwidthPerfTest::testOperation = testOperation;
	AtomicsBandwidthPerfTest::signaled = false;
	if(m_is_server){
		rdma::PerfTest::global_barrier_server(m_server, (size_t)thread_count);

	} else {
		for(AtomicsBandwidthPerfClientThread* perfThread : m_client_threads){ perfThread->start(); }
		for(AtomicsBandwidthPerfClientThread* perfThread : m_client_threads){ while(!perfThread->ready()) usleep(Config::RDMA_SLEEP_INTERVAL); }
	}
}

void rdma::AtomicsBandwidthPerfTest::runThreads(){
	AtomicsBandwidthPerfTest::signaled = false;
	unique_lock<mutex> lck(AtomicsBandwidthPerfTest::waitLock);
	AtomicsBandwidthPerfTest::waitCv.notify_all();
	AtomicsBandwidthPerfTest::signaled = true;
	lck.unlock();

	if(m_is_server && client_count > 1)  // if is server and multiple client instances then sync with global end barrier
		PerfTest::global_barrier_server(m_server, thread_count, true); // finish global end barrier

	for (size_t i = 0; i < m_client_threads.size(); i++) {
		m_client_threads[i]->join();
	}
}

void rdma::AtomicsBandwidthPerfTest::setupTest(){
	m_elapsedFetchAddMs = -1;
	m_elapsedCompareSwapMs = -1;
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

	if(m_is_server){
		// Server
		m_server = new RDMAServer<ReliableRDMA>("AtomicsBandwidthTestRDMAServer", m_rdma_port, Network::getAddressOfConnection(m_ownIpPort), m_memory, m_sequencerIpPort);

	} else {
		// Client
		for (size_t i = 0; i < thread_count; i++) {
			AtomicsBandwidthPerfClientThread* perfThread = new AtomicsBandwidthPerfClientThread(m_memory, m_rdma_addresses, m_ownIpPort, m_sequencerIpPort, m_buffer_slots, m_iterations_per_thread);
			m_client_threads.push_back(perfThread);
		}
	}
}

void rdma::AtomicsBandwidthPerfTest::runTest(){
	if(m_is_server){
		// Server
		std::cout << "Starting server on '" << rdma::Config::getIP(rdma::Config::RDMA_INTERFACE) << ":" << m_rdma_port << "' . . ." << std::endl;
		if(!m_server->startServer()){
			std::cerr << "AtomicsBandwidthPerfTest::runTest(): Could not start server" << std::endl;
			throw invalid_argument("AtomicsBandwidthPerfTest server startup failed");
		} else {
			std::cout << "Server running on '" << rdma::Config::getIP(rdma::Config::RDMA_INTERFACE) << ":" << m_rdma_port << "'" << std::endl; // TODO REMOVE
		}
		
		// Measure Latency for fetching & adding
		if(hasTestOperation(FETCH_ADD_OPERATION)){
			makeThreadsReady(FETCH_ADD_OPERATION); // fetch & add
			runThreads();
		}

		// Measure Latency for comparing & swaping
		if(hasTestOperation(COMPARE_SWAP_OPERATION)){
			makeThreadsReady(COMPARE_SWAP_OPERATION); // compare & swap
			runThreads();
		}

		// waiting until clients have connected
		while(m_server->getConnectedConnIDs().size() < (size_t)thread_count) usleep(Config::RDMA_SLEEP_INTERVAL);

		// wait until clients have finished
		while (m_server->isRunning() && m_server->getConnectedConnIDs().size() > 0) usleep(Config::RDMA_SLEEP_INTERVAL);
		std::cout << "Server stopped" << std::endl;

	} else {
		// Client

		// Measure bandwidth for fetching & adding
		if(hasTestOperation(FETCH_ADD_OPERATION)){
			makeThreadsReady(FETCH_ADD_OPERATION); // fetch & add
			auto startFetchAdd = rdma::PerfTest::startTimer();
			runThreads();
			m_elapsedFetchAddMs = rdma::PerfTest::stopTimer(startFetchAdd); 
		}

		// Measure bandwidth for comparing & swaping
		if(hasTestOperation(COMPARE_SWAP_OPERATION)){
			makeThreadsReady(COMPARE_SWAP_OPERATION); // compare & swap
			auto startCompareSwap = rdma::PerfTest::startTimer();
			runThreads();
			m_elapsedCompareSwapMs = rdma::PerfTest::stopTimer(startCompareSwap);
		}
	}
}


std::string rdma::AtomicsBandwidthPerfTest::getTestResults(std::string csvFileName, bool csvAddHeader){
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
		
		const uint64_t iters = m_iterations_per_thread * thread_count;
		uint64_t transferedBytesFetchAdd = iters * rdma::ATOMICS_SIZE * m_rdma_addresses.size() * 1 ; // 8 bytes send (IGNORE: + 8 bytes receive)
		uint64_t transferedBytesCompSwap = iters * rdma::ATOMICS_SIZE * m_rdma_addresses.size() * 1; // 8 bytes send (IGNORE: + 8 bytes receive)
		int64_t maxFetchAddMs=-1, minFetchAddMs=std::numeric_limits<int64_t>::max();
		int64_t maxCompareSwapMs=-1, minCompareSwapMs=std::numeric_limits<int64_t>::max();
		int64_t arrFetchAddMs[thread_count];
		int64_t arrCompareSwapMs[thread_count];
		long double avgFetchAddMs=0, medianFetchAddMs, avgCompareSwapMs=0, medianCompareSwapMs;
		const long double div = 1; // TODO not sure why additional   thread_count  is too much
		const long double divAvg = m_client_threads.size() * div;
		for(size_t i=0; i<m_client_threads.size(); i++){
			AtomicsBandwidthPerfClientThread *thr = m_client_threads[i];
			if(thr->m_elapsedFetchAddMs < minFetchAddMs) minFetchAddMs = thr->m_elapsedFetchAddMs;
			if(thr->m_elapsedFetchAddMs > maxFetchAddMs) maxFetchAddMs = thr->m_elapsedFetchAddMs;
			avgFetchAddMs += (long double) thr->m_elapsedFetchAddMs / divAvg;
			arrFetchAddMs[i] = thr->m_elapsedFetchAddMs;
			if(thr->m_elapsedCompareSwapMs < minCompareSwapMs) minCompareSwapMs = thr->m_elapsedCompareSwapMs;
			if(thr->m_elapsedCompareSwapMs > maxCompareSwapMs) maxCompareSwapMs = thr->m_elapsedCompareSwapMs;
			avgCompareSwapMs += (long double) thr->m_elapsedCompareSwapMs / divAvg;
			arrCompareSwapMs[i] = thr->m_elapsedCompareSwapMs;
		}
		minFetchAddMs /= div; maxFetchAddMs /= div;
		minCompareSwapMs /= div; maxCompareSwapMs /= div;

		std::sort(arrFetchAddMs, arrFetchAddMs + thread_count);
		std::sort(arrCompareSwapMs, arrCompareSwapMs + thread_count);
		medianFetchAddMs = arrFetchAddMs[(int)(thread_count/2)] / div;
		medianCompareSwapMs = arrCompareSwapMs[(int)(thread_count/2)] / div;

		// write results into CSV file
		if(!csvFileName.empty()){
			const long double su = 1024*1024; // size unit for MebiBytes
			std::ofstream ofs;
			ofs.open(csvFileName, std::ofstream::out | std::ofstream::app);
			ofs << rdma::CSV_PRINT_NOTATION << rdma::CSV_PRINT_PRECISION;
			if(csvAddHeader){
				ofs << std::endl << "ATOMICS BANDWIDTH, " << getTestParameters(true) << std::endl;
				ofs << "Iterations";
				if(hasTestOperation(FETCH_ADD_OPERATION)){
					ofs << ", Fetch&Add [MB/s], Avg Fetch&Add [MB/s], Median Fetch&Add [MB/s], Min Fetch&Add [MB/s], Max Fetch&Add [MB/s], ";
					ofs << "Fetch&Add [Sec], Avg Fetch&Add [Sec], Median Fetch&Add [Sec], Min Fetch&Add [Sec], Max Fetch&Add [Sec]";
				}
				if(hasTestOperation(COMPARE_SWAP_OPERATION)){
					ofs << ", Comp&Swap [MB/s], Avg Comp&Swap [MB/s], Median Comp&Swap [MB/s], Min Comp&Swap [MB/s], Max Comp&Swap [MB/s], ";
					ofs << "Comp&Swap [Sec], Avg Comp&Swap [Sec], Median Comp&Swap [Sec], Min Comp&Swap [Sec], Max Comp&Swap [Sec]";
				}
				ofs << std::endl; 
			}
			ofs << iters;
			if(hasTestOperation(FETCH_ADD_OPERATION)){
				ofs << ", " << (round(transferedBytesFetchAdd*tu/su/m_elapsedFetchAddMs * 100000)/100000.0) << ", "; // fetch&add MB/s
				ofs << (round(transferedBytesFetchAdd*tu/su/avgFetchAddMs * 100000)/100000.0) << ", "; // avg fetch&add MB/s
				ofs << (round(transferedBytesFetchAdd*tu/su/medianFetchAddMs * 100000)/100000.0) << ", "; // median fetch&add MB/s
				ofs << (round(transferedBytesFetchAdd*tu/su/maxFetchAddMs * 100000)/100000.0) << ", "; // min fetch&add MB/s
				ofs << (round(transferedBytesFetchAdd*tu/su/minFetchAddMs * 100000)/100000.0) << ", "; // max fetch&add MB/s
				ofs << (round(m_elapsedFetchAddMs/tu * 100000)/100000.0) << ", "; // fetch&add Sec
				ofs << (round(avgFetchAddMs/tu * 100000)/100000.0) << ", "; // avg fetch&add Sec
				ofs << (round(medianFetchAddMs/tu * 100000)/100000.0) << ", "; // median fetch&add Sec
				ofs << (round(minFetchAddMs/tu * 100000)/100000.0) << ", "; // min fetch&add Sec
				ofs << (round(maxFetchAddMs/tu * 100000)/100000.0); // max fetch&add Sec
			}
			if(hasTestOperation(COMPARE_SWAP_OPERATION)){
				ofs << ", " << (round(transferedBytesCompSwap*tu/su/m_elapsedCompareSwapMs * 100000)/100000.0) << ", "; // comp&swap MB/s
				ofs << (round(transferedBytesCompSwap*tu/su/avgCompareSwapMs * 100000)/100000.0) << ", "; // avg comp&swap MB/s
				ofs << (round(transferedBytesCompSwap*tu/su/medianCompareSwapMs * 100000)/100000.0) << ", "; // median comp&swap MB/s
				ofs << (round(transferedBytesCompSwap*tu/su/maxCompareSwapMs * 100000)/100000.0) << ", "; // min comp&swap MB/s
				ofs << (round(transferedBytesCompSwap*tu/su/minCompareSwapMs * 100000)/100000.0) << ", "; // max comp&swap MB/s
				ofs << (round(m_elapsedCompareSwapMs/tu * 100000)/100000.0) << ", "; // comp&swap Sec
				ofs << (round(avgCompareSwapMs/tu * 100000)/100000.0) << ", "; // avg comp&swap Sec
				ofs << (round(medianCompareSwapMs/tu * 100000)/100000.0) << ", "; // median comp&swap Sec
				ofs << (round(minCompareSwapMs/tu * 100000)/100000.0) << ", "; // min comp&swap Sec
				ofs << (round(maxCompareSwapMs/tu * 100000)/100000.0); // max comp&swap Sec
			}
			ofs << std::endl; ofs.close();
		}

		// generate result string
		std::ostringstream oss;
		oss << rdma::CONSOLE_PRINT_NOTATION << rdma::CONSOLE_PRINT_PRECISION;
		oss << " measurements are executed as bursts with " << Config::RDMA_MAX_WR << " operations per burst" << std::endl;
		if(hasTestOperation(FETCH_ADD_OPERATION)){
			oss << std::endl << " - Fetch&Add:     bandwidth = " << rdma::PerfTest::convertBandwidth(transferedBytesFetchAdd*tu/m_elapsedFetchAddMs);
			oss << "  (range = " << rdma::PerfTest::convertBandwidth(transferedBytesFetchAdd*tu/maxFetchAddMs) << " - ";
			oss << rdma::PerfTest::convertBandwidth(transferedBytesFetchAdd*tu/minFetchAddMs);
			oss << " ; avg=" << rdma::PerfTest::convertBandwidth(transferedBytesFetchAdd*tu/avgFetchAddMs) << " ; median=";
			oss << rdma::PerfTest::convertBandwidth(transferedBytesFetchAdd*tu/minFetchAddMs) << ")";
			oss << "   &   time = " << rdma::PerfTest::convertTime(m_elapsedFetchAddMs) << "  (range=";
			oss << rdma::PerfTest::convertTime(minFetchAddMs) << "-" << rdma::PerfTest::convertTime(maxFetchAddMs);
			oss << " ; avg=" << rdma::PerfTest::convertTime(avgFetchAddMs) << " ; median=" << rdma::PerfTest::convertTime(medianFetchAddMs) << ")" << std::endl;
		}
		if(hasTestOperation(COMPARE_SWAP_OPERATION)){
			oss << " - Compare&Swap:  bandwidth = " << rdma::PerfTest::convertBandwidth(transferedBytesCompSwap*tu/m_elapsedCompareSwapMs);
			oss << "  (range = " << rdma::PerfTest::convertBandwidth(transferedBytesCompSwap*tu/maxCompareSwapMs) << " - ";
			oss << rdma::PerfTest::convertBandwidth(transferedBytesCompSwap*tu/minCompareSwapMs);
			oss << " ; avg=" << rdma::PerfTest::convertBandwidth(transferedBytesCompSwap*tu/avgCompareSwapMs) << " ; median=";
			oss << rdma::PerfTest::convertBandwidth(transferedBytesCompSwap*tu/minCompareSwapMs) << ")";
			oss << "   &   time = " << rdma::PerfTest::convertTime(m_elapsedCompareSwapMs) << "  (range=";
			oss << rdma::PerfTest::convertTime(minCompareSwapMs) << "-" << rdma::PerfTest::convertTime(maxCompareSwapMs);
			oss << " ; avg=" << rdma::PerfTest::convertTime(avgCompareSwapMs) << " ; median=" << rdma::PerfTest::convertTime(medianCompareSwapMs) << ")" << std::endl;
		}
		return oss.str();
	}
	return NULL;
}