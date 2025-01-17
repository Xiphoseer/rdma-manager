#ifdef CUDA_ENABLED /* defined in CMakeLists.txt to globally enable/disable CUDA support */

#include "AbstractCudaMemory.h"

using namespace rdma;

// constructors
AbstractCudaMemory::AbstractCudaMemory(size_t mem_size, int device_index) : AbstractBaseMemory(mem_size){
    this->device_index = device_index;
}
AbstractCudaMemory::AbstractCudaMemory(void* buffer, size_t mem_size) : AbstractBaseMemory(buffer, mem_size){
    this->device_index = -1;
}
AbstractCudaMemory::AbstractCudaMemory(void* buffer, size_t mem_size, int device_index) : AbstractBaseMemory(buffer, mem_size){
    this->device_index = device_index;
}

AbstractCudaMemory::~AbstractCudaMemory(){
    if(this->open_context_counter == 0) return;
    this->open_context_counter = 1;
    closeContext();
}

void AbstractCudaMemory::openContext(){
    if(this->device_index < 0) return;
    this->open_context_counter++;
    if(this->open_context_counter > 1) return;
    this->previous_device_index = -1;
    checkCudaError(cudaGetDevice(&(this->previous_device_index)), "AbstractCudaMemory::openContext could not get selected device\n");
    if(this->previous_device_index == this->device_index) return;
    checkCudaError(cudaSetDevice(this->device_index), "AbstractCudaMemory::openContext could not set selected device\n");
}

void AbstractCudaMemory::closeContext(){
    if(this->device_index < 0 || this->open_context_counter==0) return;
    this->open_context_counter--;
    if(this->open_context_counter > 0) return;
    if(this->device_index != this->previous_device_index)
        checkCudaError(cudaSetDevice(this->previous_device_index), "AbstractCudaMemory::closeContext could not reset selected device\n");
    this->previous_device_index = -1;
}

void AbstractCudaMemory::setMemory(int value){
    setMemory(value, 0, this->mem_size);
}

void AbstractCudaMemory::setMemory(int value, size_t num){
    setMemory(value, 0, num);
}

void AbstractCudaMemory::setMemory(int value, size_t offset, size_t num){
    openContext();
    checkCudaError(cudaMemset((void*)((size_t)this->buffer + offset), value, num), "AbstractCudaMemory::setMemory could not set memory to value\n");
    closeContext();
}


void AbstractCudaMemory::copyTo(void *destination, MEMORY_TYPE memtype){
    size_t s = sizeof(destination);
    copyTo(destination, (s < this->mem_size ? s : this->mem_size), memtype);
}

void AbstractCudaMemory::copyTo(void *destination, size_t num, MEMORY_TYPE memtype){
    copyTo(destination, 0, 0, num, memtype);
}

void AbstractCudaMemory::copyTo(void *destination, size_t destOffset, size_t srcOffset, size_t num, MEMORY_TYPE memtype){
    destination = (void*)((size_t)destination + destOffset);
    void* source = (void*)((size_t)this->buffer + srcOffset);
    if((int)memtype <= (int)MEMORY_TYPE::MAIN){
        checkCudaError(cudaMemcpy(destination, source, num, cudaMemcpyDeviceToHost), 
                                    "AbstractCudaMemory::copyTo could not copy data from GPU to MAIN\n");
    } else {
        checkCudaError(cudaMemcpy(destination, source, num, cudaMemcpyDeviceToDevice), 
                                    "AbstractCudaMemory::copyTo could not copy data from GPU to GPU\n");
    }
}

void AbstractCudaMemory::copyFrom(const void *source, MEMORY_TYPE memtype){
    size_t s = sizeof(source);
    copyFrom(source, (s < this->mem_size ? s : this->mem_size), memtype);
}

void AbstractCudaMemory::copyFrom(const void *source, size_t num, MEMORY_TYPE memtype){
    copyFrom(source, 0, 0, num, memtype);
}

void AbstractCudaMemory::copyFrom(const void *source, size_t srcOffset, size_t destOffset, size_t num, MEMORY_TYPE memtype){
    source = (void*)((size_t)source + srcOffset);
    void* destination = (void*)((size_t)this->buffer + destOffset);
    if((int)memtype <= (int)MEMORY_TYPE::MAIN){
        checkCudaError(cudaMemcpy(destination, source, num, cudaMemcpyHostToDevice), 
                                    "AbstractCudaMemory::copyFrom could not copy data from MAIN TO GPU\n");
    } else {
        checkCudaError(cudaMemcpy(destination, source, num, cudaMemcpyDeviceToDevice), 
                                    "AbstractCudaMemory::copyFrom could not copy data from GPU TO GPU\n");
    }
}


char AbstractCudaMemory::getChar(size_t offset){
    char tmp[1];
    copyTo((void*)tmp, 0, offset, sizeof(tmp), MEMORY_TYPE::MAIN);
    return tmp[0];
}

void AbstractCudaMemory::set(char value, size_t offset){
    copyFrom((void*)&value, 0, offset, sizeof(value), MEMORY_TYPE::MAIN);
}

int8_t AbstractCudaMemory::getInt8(size_t offset){
    int8_t tmp[1];
    copyTo((void*)tmp, 0, offset, sizeof(tmp), MEMORY_TYPE::MAIN);
    return tmp[0];
}

void AbstractCudaMemory::set(int8_t value, size_t offset){
    copyFrom((void*)&value, 0, offset, sizeof(value), MEMORY_TYPE::MAIN);
}

uint8_t AbstractCudaMemory::getUInt8(size_t offset){
    uint8_t tmp[1];
    copyTo((void*)tmp, 0, offset, sizeof(tmp), MEMORY_TYPE::MAIN);
    return tmp[0];
}

void AbstractCudaMemory::set(uint8_t value, size_t offset){
    copyFrom((void*)&value, 0, offset, sizeof(value), MEMORY_TYPE::MAIN);
}

int16_t AbstractCudaMemory::getInt16(size_t offset){
    int16_t tmp[1];
    copyTo((void*)tmp, 0, offset, sizeof(tmp), MEMORY_TYPE::MAIN);
    return tmp[0];
}

void AbstractCudaMemory::set(int16_t value, size_t offset){
    copyFrom((void*)&value, 0, offset, sizeof(value), MEMORY_TYPE::MAIN);
}

uint16_t AbstractCudaMemory::getUInt16(size_t offset){
    uint16_t tmp[1];
    copyTo((void*)tmp, 0, offset, sizeof(tmp), MEMORY_TYPE::MAIN);
    return tmp[0];
}

void AbstractCudaMemory::set(uint16_t value, size_t offset){
    copyFrom((void*)&value, 0, offset, sizeof(value), MEMORY_TYPE::MAIN);
}

int32_t AbstractCudaMemory::getInt32(size_t offset){
    int32_t tmp[1];
    copyTo((void*)tmp, 0, offset, sizeof(tmp), MEMORY_TYPE::MAIN);
    return tmp[0];
}

void AbstractCudaMemory::set(int32_t value, size_t offset){
    copyFrom((void*)&value, 0, offset, sizeof(value), MEMORY_TYPE::MAIN);
}

uint32_t AbstractCudaMemory::getUInt32(size_t offset){
    uint32_t tmp[1];
    copyTo((void*)tmp, 0, offset, sizeof(tmp), MEMORY_TYPE::MAIN);
    return tmp[0];
}

void AbstractCudaMemory::set(uint32_t value, size_t offset){
    copyFrom((void*)&value, 0, offset, sizeof(value), MEMORY_TYPE::MAIN);
}

int64_t AbstractCudaMemory::getInt64(size_t offset){
    int64_t tmp[1];
    copyTo((void*)tmp, 0, offset, sizeof(tmp), MEMORY_TYPE::MAIN);
    return tmp[0];
}

void AbstractCudaMemory::set(int64_t value, size_t offset){
    copyFrom((void*)&value, 0, offset, sizeof(value), MEMORY_TYPE::MAIN);
}

uint64_t AbstractCudaMemory::getUInt64(size_t offset){
    uint64_t tmp[1];
    copyTo((void*)tmp, 0, offset, sizeof(tmp), MEMORY_TYPE::MAIN);
    return tmp[0];
}

void AbstractCudaMemory::set(uint64_t value, size_t offset){
    copyFrom((void*)&value, 0, offset, sizeof(value), MEMORY_TYPE::MAIN);
}

#endif /* CUDA support */