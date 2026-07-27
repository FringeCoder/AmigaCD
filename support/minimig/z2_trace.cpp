// Z2 fast-RAM hang trace drain.
//
// Pulls 16-byte entries from the z2_trace ring (rtl/z2_trace.v) over the
// UIO sub-channel at class 7'b1111101 (0xFA00) and appends decoded
// records to /tmp/z2_trace.csv.
//
// Per-entry layout (LSB-first, matches rtl/z2_trace.v header):
//   byte 0..3 : timestamp[31:0]
//   byte 4..7 : cpu_addr[31:0]
//   byte 8    : flags  = {wr, ramready, cpustate[1:0], cchip, ckick, uds_in, lds_in}
//   byte 9    : sels   = {sel_z2ram, sel_z3ram0, sel_z3ram1, sel_kickram,
//                         sel_chipram, sel_dd, sel_rtg, z2ram_ena}
//   byte 10..13: {2'b0, ev_type[1:0], ramaddr[28:1]}
//   byte 14..15: ramdat[15:0]
//
// Empty detection: all 16 bytes == 0x00 (timestamp is free-running so a
// real entry is almost never all-zero).
//
// 256 ring entries × 16 bytes = 4096 bytes drained per call worst case.

#include "z2_trace.h"

#if AKIKO_Z2_TRACE

#include <cstdio>
#include <cstdint>
#include <cstring>

#include "../../spi.h"
#include "../../user_io.h"

// UIO class 7'b1111101 → io_din[15:9] = 0b1111101 → io_din[15:0] = 0xFA00.
static constexpr uint32_t Z2_TRACE_ADDR = 0xFA00;
static constexpr int      RING_DEPTH    = 256;
static constexpr char     OUT_PATH[]    = "/tmp/z2_trace.csv";

static const char *kEvType[4] = { "acc", "ac_done", "stall", "rsv" };

static FILE *g_csv = nullptr;
static uint32_t g_seq = 0;

static void open_csv_if_needed(void)
{
    if (g_csv) return;
    g_csv = fopen(OUT_PATH, "w");
    if (!g_csv) return;
    fprintf(g_csv,
            "seq,ts,ev,cpu_addr,ramaddr,wr,ramready,uds,lds,cpustate,"
            "cchip,ckick,sel_z2,sel_z30,sel_z31,sel_kick,sel_chip,sel_dd,"
            "sel_rtg,z2_ena,ramdat\n");
    fflush(g_csv);
    g_seq = 0;
}

void z2_trace_drain(void)
{
    EnableIO();
    spi8(UIO_DMA_READ);
    spi32_w(Z2_TRACE_ADDR);

    for (int i = 0; i < RING_DEPTH; i++) {
        uint8_t b[16];
        for (int j = 0; j < 16; j++) b[j] = (uint8_t)spi_w(0);

        uint32_t all_or = 0;
        for (int j = 0; j < 16; j++) all_or |= b[j];
        if (!all_or) break;  // ring empty

        open_csv_if_needed();
        if (!g_csv) break;

        uint32_t ts       = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                            ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        uint32_t cpu_addr = (uint32_t)b[4] | ((uint32_t)b[5] << 8) |
                            ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
        uint8_t  flags    = b[8];
        uint8_t  sels     = b[9];
        // bytes 10..13 = {2'b0, ev_type[1:0], ramaddr[28:1]}.
        uint32_t ram_raw  = (uint32_t)b[10] | ((uint32_t)b[11] << 8) |
                            ((uint32_t)b[12] << 16) | ((uint32_t)b[13] << 24);
        uint32_t ramaddr  =  ram_raw        & 0x0FFFFFFFu;  // [27:0] = ramaddr[28:1]
        uint8_t  ev       = (uint8_t)((ram_raw >> 28) & 0x3);
        uint16_t ramdat   = (uint16_t)b[14] | ((uint16_t)b[15] << 8);

        // flags = {wr, ramready, cpustate[1:0], cchip, ckick, uds_in, lds_in}
        uint8_t wr        = (flags >> 7) & 0x1;
        uint8_t ramready  = (flags >> 6) & 0x1;
        uint8_t cpustate  = (flags >> 4) & 0x3;
        uint8_t cchip     = (flags >> 3) & 0x1;
        uint8_t ckick     = (flags >> 2) & 0x1;
        uint8_t uds       = (flags >> 1) & 0x1;
        uint8_t lds       =  flags       & 0x1;

        uint8_t sel_z2    = (sels >> 7) & 0x1;
        uint8_t sel_z30   = (sels >> 6) & 0x1;
        uint8_t sel_z31   = (sels >> 5) & 0x1;
        uint8_t sel_kick  = (sels >> 4) & 0x1;
        uint8_t sel_chip  = (sels >> 3) & 0x1;
        uint8_t sel_dd    = (sels >> 2) & 0x1;
        uint8_t sel_rtg   = (sels >> 1) & 0x1;
        uint8_t z2_ena    =  sels       & 0x1;

        fprintf(g_csv,
                "%u,%u,%s,0x%08X,0x%07X,%u,%u,%u,%u,%u,%u,%u,"
                "%u,%u,%u,%u,%u,%u,%u,%u,0x%04X\n",
                g_seq++, ts, kEvType[ev], cpu_addr, ramaddr,
                wr, ramready, uds, lds, cpustate, cchip, ckick,
                sel_z2, sel_z30, sel_z31, sel_kick, sel_chip, sel_dd, sel_rtg,
                z2_ena, ramdat);
    }

    DisableIO();
    if (g_csv) fflush(g_csv);
}

#else  // AKIKO_Z2_TRACE

void z2_trace_drain(void) {}

#endif  // AKIKO_Z2_TRACE
