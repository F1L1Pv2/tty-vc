#ifndef TTYVC_COMMON
#define TTYVC_COMMON

#include <stdint.h>

#define MAX_PACKET_SIZE 1500

struct AudioPacketHeader{
    uint32_t offset; // offset in data of AudioPacket
    uint32_t size; // size of data in bytes
};

#endif