// Chipset bus trace drain (debug, gfx-trio investigation).
//
// Pulls 8-byte entries from the chipset_bus_trace ring (rtl/chipset_bus_trace.v)
// over the UIO sub-channel at class 7'b1111011 (0xF600) and appends decoded
// records to /tmp/chipset_trace.csv. Matches the canonical schema produced by
// tools/drain_chipset_trace.py:
//
//   seq, vpos, hpos, src, reg, data, dbwe
//
// Always-on drain: the ring would otherwise overwrite older entries every 2-4
// PAL frames, so we poll-and-empty on every Minimig poll cycle (same cadence
// as akiko_drain_trace).
//
// When the agnus.v CHIPSET_TRACE gate is 0 (production build), the trace
// ring is generate-elided and every drain call sees byte 7 = 0x00 (empty
// sentinel) on the very first read, so this code costs one SPI round-trip
// per poll and zero file I/O.

#include "chipset_trace.h"

#include <cstdio>
#include <cstdint>
#include <cstring>

#include "../../spi.h"
#include "../../user_io.h"

// UIO class 7'b1111011 → io_din[15:9] = 0b1111011 → io_din[15:0] = 0xF600.
static constexpr uint32_t CHIPSET_TRACE_ADDR = 0xF600;
static constexpr int      RING_DEPTH         = 1024;
static constexpr char     OUT_PATH[]         = "/tmp/chipset_trace.csv";

static const char *kSrcLabels[8] = {
    "cpu", "cop", "blt", "spr", "bpl", "dsk", "aud", "ref"
};

static FILE *g_csv = nullptr;
static uint32_t g_seq = 0;

static void open_csv_if_needed(void)
{
    if (g_csv) return;
    g_csv = fopen(OUT_PATH, "w");
    if (!g_csv) return;
    fprintf(g_csv, "seq,vpos,hpos,src,reg,data,dbwe\n");
    fflush(g_csv);
    g_seq = 0;
}

void chipset_trace_drain(void)
{
    EnableIO();
    spi8(UIO_DMA_READ);
    spi32_w(CHIPSET_TRACE_ADDR);

    for (int i = 0; i < RING_DEPTH; i++) {
        uint8_t b0 = (uint8_t)spi_w(0);  // data[7:0]
        uint8_t b1 = (uint8_t)spi_w(0);  // data[15:8]
        uint8_t b2 = (uint8_t)spi_w(0);  // reg_addr[7:0]
        uint8_t b3 = (uint8_t)spi_w(0);  // {dbwe, src[2:0], 1'b0, vpos[10:8]}
        uint8_t b4 = (uint8_t)spi_w(0);  // vpos[7:0]
        uint8_t b5 = (uint8_t)spi_w(0);  // hpos[7:0]
        uint8_t b6 = (uint8_t)spi_w(0);  // {7'b0, hpos[8]}
        uint8_t b7 = (uint8_t)spi_w(0);  // 0xFF valid / 0x00 empty sentinel

        if (b7 != 0xFF) break;           // ring empty

        open_csv_if_needed();
        if (!g_csv) break;

        uint16_t data    = (uint16_t)b0 | ((uint16_t)b1 << 8);
        uint8_t  reg     = b2;
        uint8_t  dbwe    = (b3 >> 7) & 0x1;
        uint8_t  src     = (b3 >> 4) & 0x7;
        uint16_t vpos    = ((uint16_t)(b3 & 0x7) << 8) | b4;
        uint16_t hpos    = ((uint16_t)(b6 & 0x1) << 8) | b5;

        fprintf(g_csv, "%u,%u,%u,%s,0x%02X,0x%04X,%u\n",
                g_seq++, vpos, hpos, kSrcLabels[src], reg, data, dbwe);
    }

    DisableIO();
    if (g_csv) fflush(g_csv);
}
