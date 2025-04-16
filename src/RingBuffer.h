#include <thread>
#include <atomic>
#include <stdint.h>
#ifdef _WIN32
// #include <windows.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#endif
#include <string.h>
#include <malloc.h>

#ifdef _WIN32
#define SLEEP(x) Sleep((x))
#else
#define SLEEP(x) usleep((x) * 1000)
#endif

struct ContigousAsyncBufferItem {
    void* data;
    size_t num_objects;  // Number of objects available
};

class ContigousAsyncBuffer {
private:
    uint8_t* data;
    size_t size;         // Total buffer size in bytes (object_count * object_size)
    size_t object_size;  // Size of each object in bytes
    std::atomic<size_t> readPtr;   // Read pointer in bytes
    std::atomic<size_t> writePtr;  // Write pointer in bytes
    std::atomic<size_t> watermarkPtr;  // Watermark pointer in bytes

public:
    ~ContigousAsyncBuffer() {
        free(data);
    }

    ContigousAsyncBuffer(size_t object_count, size_t object_size)
        : size(object_count * object_size), object_size(object_size) {
        data = static_cast<uint8_t*>(malloc(size));
        readPtr.store(0);
        writePtr.store(0);
        watermarkPtr.store(size);
    }

    bool write(void* write_data, size_t num_objects) {
        const size_t write_size = num_objects * object_size;
        if (write_size > size) return false;

        size_t current_write = writePtr.load();
        size_t remaining_space = size - current_write;

        if (remaining_space >= write_size) {
            // Direct write without wrapping
            memcpy(data + current_write, write_data, write_size);
            watermarkPtr.store(current_write + write_size);
            writePtr.store(current_write + write_size);
        } else {
            // Wrap around needed - wait until there's enough space at the beginning
            size_t available_space = readPtr.load();
            while (write_size > available_space) {
                SLEEP(1);
                available_space = readPtr.load();
            }

            memcpy(data, write_data, write_size);
            watermarkPtr.store(current_write);
            writePtr.store(write_size);
        }
        return true;
    }

    void* read() {
        const size_t current_write = writePtr.load();
        const size_t current_read = readPtr.load();

        if (current_write != current_read) {
            void* result = data + current_read;
            size_t next_read = current_read + object_size;
            if (next_read == size) {
                readPtr.store(0);
                if (writePtr.load() <= current_read) {
                    watermarkPtr.store(size); // Reset watermark after full wrap
                }
            } else {
                readPtr.store(next_read);
            }
            return result;
        }
        return nullptr;
    }

    // Disable copy and move operations
    ContigousAsyncBuffer(const ContigousAsyncBuffer&) = delete;
    ContigousAsyncBuffer& operator=(const ContigousAsyncBuffer&) = delete;
};