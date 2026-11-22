#include <stdio.h>
#include <stdint.h>
#include <string.h>

// Build a static PAT packet (188 bytes)
void build_pat(uint8_t *pkt, uint8_t cc) {
    memset(pkt, 0xFF, 188);
    pkt[0] = 0x47; // Sync
    pkt[1] = 0x40; // Payload unit start indicator, PID = 0
    pkt[2] = 0x00;
    pkt[3] = 0x10 | (cc & 0x0F); // Payload only, CC
    pkt[4] = 0x00; // Pointer field

    // Table ID 0x00 (PAT)
    pkt[5] = 0x00;
    // Section syntax indicator (1), '0' (0), reserved (11), section length (13 bytes = 0x00D)
    pkt[6] = 0xB0;
    pkt[7] = 0x0D;
    // Transport stream ID = 1
    pkt[8] = 0x00;
    pkt[9] = 0x01;
    // Version 0, current_next 1
    pkt[10] = 0xC1;
    // Section number 0, last section 0
    pkt[11] = 0x00;
    pkt[12] = 0x00;
    // Program 1 -> PMT PID 4096 (0x1000)
    pkt[13] = 0x00;
    pkt[14] = 0x01;
    pkt[15] = 0xE0 | 0x10; // Reserved 111, PID high
    pkt[16] = 0x00;        // PID low

    // CRC32 of bytes 5..16
    // Calculated CRC32 for this standard PAT section:
    // We can precompute or compute CRC32
}

int main() {
    printf("MPEG-TS builder ready\n");
    return 0;
}
