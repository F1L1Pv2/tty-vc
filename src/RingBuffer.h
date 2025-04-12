#include <thread>
#include <atomic>
#include <stdint.h>
#ifdef _WIN32
// #include "MinWin.h"
#endif
#include <string.h>
#include <malloc.h>

#ifdef _WIN32
    #define SLEEP(x) Sleep(x)
#else
    #include <unistd.h>
    #include <sys/stat.h>
    #define SLEEP(x) usleep(x)
#endif


struct ContigousAsyncBufferItem{
    void* data;
    size_t size;
};

struct ContigousAsyncBuffer{
    private:
    uint8_t* data;
    size_t size;

    std::atomic<size_t> readPtr;
    std::atomic<size_t> writePtr;
    std::atomic<size_t> watermarkPtr;

    public:

    ~ContigousAsyncBuffer(){
        free(data);
    }

    ContigousAsyncBuffer(size_t size): size(size){
        data = (uint8_t*)malloc(size);
        readPtr.store(0);
        writePtr.store(0);
        watermarkPtr.store(size);
    }

    bool write(void* write_data, size_t write_size){
        if(write_size > size) return false;

        if(size - writePtr.load() >= write_size){
            memcpy(data+writePtr.load(),write_data,write_size);
            watermarkPtr.store(writePtr.load() + write_size);
            writePtr.store(writePtr.load() + write_size);
        }else{
            while(write_size >= readPtr.load()){
                SLEEP(1);
            }
            memcpy(data,write_data,write_size);
            watermarkPtr.store(writePtr.load());
            writePtr.store(0 + write_size);
        }

        return true;
    }

    ContigousAsyncBufferItem read(){
        ContigousAsyncBufferItem out = {};

        if(writePtr.load() != readPtr.load()){
            if(writePtr.load() > readPtr.load()){
                out.data = data+readPtr.load();
                out.size = writePtr.load() - readPtr.load();
                readPtr.store(writePtr.load());
            }else{
                if(watermarkPtr.load() != readPtr.load()){
                    if(watermarkPtr.load() - readPtr.load() > 0){
                        out.data = data+readPtr.load();
                        out.size = watermarkPtr.load() - readPtr.load();
                    }
                    readPtr.store(watermarkPtr.load());
                }else{
                    out.data = data;
                    out.size = writePtr.load();
                    readPtr.store(writePtr.load());
                }
            }
        }

        return out;
    }
};