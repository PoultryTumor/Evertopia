#include "asset_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <sys/stat.h>
#define soe_ac_mkdir(p) _mkdir(p)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define soe_ac_mkdir(p) mkdir((p), 0755)
#endif

#include "beam_frame_driver.h"
#include "common_cpu_infra.h"
#include "cpu_state.h"
#include "snes/cart.h"
#include "snes/interp_bridge.h"
#include "snes/ppu.h"
#include "snes/snes.h"

extern CpuState g_cpu;
extern uint8_t g_ram[];
extern Snes *g_snes;
extern Ppu *g_ppu;

/* ── ROM address helpers (HiROM) ─────────────────────────────────────────── */

static uint32_t SoeSnesToRomOffset(uint32_t addr24)
{
    uint8_t bank = (uint8_t)((addr24 >> 16) & 0xFFu);
    uint16_t lo  = (uint16_t)(addr24 & 0xFFFFu);
    if (bank >= 0xC0u) {
        return (((uint32_t)(bank - 0xC0u)) << 16) | lo;
    }
    if (bank >= 0x80u && lo >= 0x8000u) {
        return (((uint32_t)(bank - 0x80u)) << 16) | lo;
    }
    if (bank < 0x40u && lo >= 0x8000u) {
        return (((uint32_t)bank) << 16) | lo;
    }
    if (bank >= 0x40u && bank < 0x70u) {
        return (((uint32_t)(bank - 0x40u)) << 16) | lo;
    }
    return UINT32_MAX;
}

static uint32_t SoeNormalizeRomAddr24(uint32_t addr24)
{
    uint8_t bank = (uint8_t)((addr24 >> 16) & 0xFFu);
    uint16_t lo  = (uint16_t)(addr24 & 0xFFFFu);
    if (bank < 0x40u && lo >= 0x8000u) {
        return (((uint32_t)(bank | 0x80u)) << 16) | lo;
    }
    if (bank >= 0x40u && bank < 0x70u) {
        return (((uint32_t)(bank | 0x80u)) << 16) | lo;
    }
    return addr24 & 0xFFFFFFu;
}

static uint16_t Rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/* ── IEEE 802.3 CRC32 ────────────────────────────────────────────────────── */

static uint32_t SoeComputeCrc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

/* ── File / Checksum Helpers ─────────────────────────────────────────────── */

static bool SoeFileSize(const char *path, size_t *out_size)
{
#ifdef _WIN32
    struct _stat st;
    if (_stat(path, &st) != 0) return false;
#else
    struct stat st;
    if (stat(path, &st) != 0) return false;
#endif
    if (out_size) *out_size = (size_t)st.st_size;
    return true;
}

static bool SoeReadWholeFile(const char *path, uint8_t *dst, size_t expected_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(dst, 1, expected_size, f);
    fclose(f);
    return n == expected_size;
}

static bool SoeWriteFileAtomic(const char *path, const uint8_t *data, size_t size)
{
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE *f = fopen(tmp_path, "wb");
    if (!f) return false;
    size_t n = size > 0 ? fwrite(data, 1, size, f) : 0;
    int err = ferror(f);
    fclose(f);
    if (err || n != size) {
        remove(tmp_path);
        return false;
    }
    if (rename(tmp_path, path) != 0) {
        remove(tmp_path);
        return false;
    }
    return true;
}

static void SoeClearStaleBinFiles(const char *dir_path)
{
#ifndef _WIN32
    DIR *d = opendir(dir_path);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len > 4 && strcmp(ent->d_name + len - 4, ".bin") == 0) {
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);
            remove(full);
        }
    }
    closedir(d);
#else
    (void)dir_path;
#endif
}

static void SoeEnsureChecksumDir(const char *dir_path, uint32_t rom_crc32)
{
    soe_ac_mkdir("Extract");
    soe_ac_mkdir(dir_path);

    char crc_path[512];
    snprintf(crc_path, sizeof(crc_path), "%s/rom_crc32.txt", dir_path);

    char expected[16];
    snprintf(expected, sizeof(expected), "%08x", rom_crc32);

    FILE *f = fopen(crc_path, "r");
    if (f) {
        char buf[32] = {0};
        if (fgets(buf, sizeof(buf), f)) {
            buf[strcspn(buf, "\r\n \t")] = '\0';
            if (strcmp(buf, expected) == 0) {
                fclose(f);
                return; /* Same ROM — keep existing extracted files! */
            }
        }
        fclose(f);
        /* Different ROM: clear stale .bin files before updating checksum */
        SoeClearStaleBinFiles(dir_path);
    }

    char tmp_crc[520];
    snprintf(tmp_crc, sizeof(tmp_crc), "%s.tmp", crc_path);
    FILE *wf = fopen(tmp_crc, "w");
    if (wf) {
        fprintf(wf, "%s\n", expected);
        fclose(wf);
        rename(tmp_crc, crc_path);
    }
}

/* ── Tileset / Map Block Cache (Extract/TILESETS/) ───────────────────────── */

#define TILESET_HASH_CAP 1024u /* Power of 2, holds ~380+ unique blocks */

typedef struct {
    uint32_t rom_addr24; /* 0 = empty slot */
    uint8_t *data;
    uint32_t size;
} SoeTilesetSlot;

static SoeTilesetSlot s_tileset_table[TILESET_HASH_CAP];
static bool s_cache_ready = false;
static bool s_extracting_now = false;

static uint32_t TilesetHash(uint32_t addr24)
{
    uint32_t x = addr24 ^ (addr24 >> 16);
    x *= 0x9E3779B1u;
    return (x >> 16) & (TILESET_HASH_CAP - 1u);
}

static SoeTilesetSlot *TilesetLookup(uint32_t addr24)
{
    uint32_t idx = TilesetHash(addr24);
    for (uint32_t probe = 0; probe < TILESET_HASH_CAP; probe++) {
        SoeTilesetSlot *s = &s_tileset_table[(idx + probe) & (TILESET_HASH_CAP - 1u)];
        if (s->rom_addr24 == 0) return NULL;
        if (s->rom_addr24 == addr24) return s;
    }
    return NULL;
}

static void TilesetInsert(uint32_t addr24, uint8_t *data, uint32_t size)
{
    uint32_t idx = TilesetHash(addr24);
    for (uint32_t probe = 0; probe < TILESET_HASH_CAP; probe++) {
        SoeTilesetSlot *s = &s_tileset_table[(idx + probe) & (TILESET_HASH_CAP - 1u)];
        if (s->rom_addr24 == 0 || s->rom_addr24 == addr24) {
            if (s->data && s->data != data) free(s->data);
            s->rom_addr24 = addr24;
            s->data = data;
            s->size = size;
            return;
        }
    }
}

/* Decompress one $8C:988D block using the game's own 65816 routine into $7F:0000. */
static uint8_t *DecompressBlockViaGame(uint32_t rom_addr24, uint32_t *out_size)
{
    const uint8_t *rom = g_snes->cart->rom;
    uint32_t rom_size = g_snes->cart->romSize;
    uint32_t off = SoeSnesToRomOffset(rom_addr24);
    if (off == UINT32_MAX || off + 3u > rom_size) return NULL;

    uint8_t mode = rom[off];
    if (mode != 0 && mode != 3 && mode != 4) return NULL;

    /* Save CPU state, DP ($00..$FF), and WRAM bank $7F ($10000..$1FFFF) */
    CpuState saved_cpu = g_cpu;
    uint8_t saved_dp[256];
    memcpy(saved_dp, &g_ram[0x0000], 256);
    uint8_t *saved_7f = (uint8_t *)malloc(0x10000u);
    if (!saved_7f) return NULL;
    memcpy(saved_7f, &g_ram[0x10000u], 0x10000u);

    s_extracting_now = true;
    WatchdogSetEnabled(0);

    g_cpu.A = 0;
    g_cpu.X = 0;
    g_cpu.Y = 0;
    g_cpu.D = 0x0000;
    g_cpu.DB = 0x8C;
    g_cpu.PB = 0x8C;
    g_cpu.S = 0x1FF0;
    g_cpu.emulation = 0;
    g_cpu.m_flag = 0;
    g_cpu.x_flag = 0;
    g_cpu.P = (uint8_t)((g_cpu.P & ~0x30u) | 0x04u);
    cpu_p_to_mirrors(&g_cpu);
    g_cpu.host_return_valid = 0;

    /* Source $36..$38 = rom_addr24 */
    g_ram[0x0036] = (uint8_t)(rom_addr24 & 0xFFu);
    g_ram[0x0037] = (uint8_t)((rom_addr24 >> 8) & 0xFFu);
    g_ram[0x0038] = (uint8_t)((rom_addr24 >> 16) & 0xFFu);
    /* Dest $3A..$3C = $7F:0000 */
    g_ram[0x003A] = 0x00;
    g_ram[0x003B] = 0x00;
    g_ram[0x003C] = 0x7F;
    g_ram[0x0034] = 0x00;
    g_ram[0x0035] = 0x00;

    /* JSL return sentinel ($8C:8000) */
    cpu_write8(&g_cpu, 0x00, g_cpu.S, 0x8C); g_cpu.S = (uint16_t)(g_cpu.S - 1);
    cpu_write8(&g_cpu, 0x00, g_cpu.S, 0x80); g_cpu.S = (uint16_t)(g_cpu.S - 1);
    cpu_write8(&g_cpu, 0x00, g_cpu.S, 0x00); g_cpu.S = (uint16_t)(g_cpu.S - 1);

    interp_bridge_run(&g_cpu, 0x8C988Du);

    uint32_t usize = (uint32_t)g_ram[0x0034] | ((uint32_t)g_ram[0x0035] << 8);
    if (usize == 0) {
        usize = (uint32_t)rom[off + 1] | ((uint32_t)rom[off + 2] << 8);
    }

    uint8_t *buf = NULL;
    if (usize > 0 && usize <= 0x10000u) {
        buf = (uint8_t *)malloc(usize);
        if (buf) {
            memcpy(buf, &g_ram[0x10000u], usize);
            *out_size = usize;
        }
    }

    WatchdogSetEnabled(1);
    s_extracting_now = false;

    memcpy(&g_ram[0x10000u], saved_7f, 0x10000u);
    free(saved_7f);
    memcpy(&g_ram[0x0000], saved_dp, 256);
    g_cpu = saved_cpu;

    return buf;
}

static void CollectAndLoadTilesets(void)
{
    const uint8_t *rom = g_snes->cart->rom;
    uint32_t rom_size = g_snes->cart->romSize;

    /* Collect unique ROM addresses for all maps (0..126) + static $80 blocks */
    uint32_t addrs[1024];
    uint32_t addr_count = 0;

    #define ADD_ADDR(a_) do { \
        uint32_t na_ = SoeNormalizeRomAddr24((a_)); \
        uint32_t o_ = SoeSnesToRomOffset(na_); \
        if (o_ != UINT32_MAX && o_ + 3u <= rom_size && \
            (rom[o_] == 0 || rom[o_] == 3 || rom[o_] == 4)) { \
            bool dup_ = false; \
            for (uint32_t k_ = 0; k_ < addr_count; k_++) { \
                if (addrs[k_] == na_) { dup_ = true; break; } \
            } \
            if (!dup_ && addr_count < 1024u) addrs[addr_count++] = na_; \
        } \
    } while (0)

    /* Static bank $80 UI/title/menu blocks */
    ADD_ADDR(0xC60100u);
    ADD_ADDR(0xC6174Du);
    ADD_ADDR(0xC619EDu);

    /* All map headers in $9F:FDE7 (0..126) */
    const uint32_t off_table = 0x1FFDE7u;
    for (uint32_t m = 0; m <= 126u; m++) {
        uint32_t mp = off_table + m * 4u;
        if (mp + 4u > rom_size) break;
        uint32_t map_addr = (uint32_t)rom[mp] | ((uint32_t)rom[mp + 1] << 8) | ((uint32_t)rom[mp + 2] << 16);
        if (map_addr < 0x800000u || map_addr >= 0xC00000u) continue;
        uint32_t off = SoeSnesToRomOffset(map_addr);
        if (off == UINT32_MAX || off + 32u > rom_size) continue;

        uint32_t y = 13u;
        if (off + y + 2u > rom_size) continue;
        uint16_t s1 = Rd16(&rom[off + y]); y += 2u + s1;
        if (off + y + 2u > rom_size) continue;
        uint16_t s2 = Rd16(&rom[off + y]); y += 2u + s2;
        if (off + y + 1u > rom_size) continue;
        uint8_t n_ent = rom[off + y]; y += 1u + (uint32_t)n_ent * 2u;
        if (off + y + 1u > rom_size) continue;
        uint8_t n_pal = rom[off + y]; y += 1u + (uint32_t)n_pal * 3u;
        if (off + y + 2u > rom_size) continue;
        uint32_t b1_addr = map_addr + y + 2u;
        ADD_ADDR(b1_addr);
        uint16_t b1_size = Rd16(&rom[off + y]); y += 2u + b1_size;

        y += 1u;
        if (off + y + 2u > rom_size) continue;
        uint16_t skip_s = Rd16(&rom[off + y]); y += 2u + skip_s;
        if (off + y + 1u > rom_size) continue;
        uint8_t n_lay = rom[off + y]; y += 1u + (uint32_t)n_lay * 2u;
        if (off + y + 2u > rom_size) continue;
        uint32_t b2_addr = map_addr + y + 2u;
        ADD_ADDR(b2_addr);
        uint16_t b2_size = Rd16(&rom[off + y]); y += 2u + b2_size;

        if (off + y + 2u > rom_size) continue;
        uint16_t s_mid = Rd16(&rom[off + y]);
        uint32_t b3_addr = map_addr + y + 2u + s_mid + 2u;
        ADD_ADDR(b3_addr);
    }
    #undef ADD_ADDR

    uint32_t loaded_existing = 0;
    uint32_t extracted_missing = 0;

    for (uint32_t i = 0; i < addr_count; i++) {
        uint32_t a = addrs[i];
        uint32_t o = SoeSnesToRomOffset(a);
        uint32_t expected_sz = Rd16(&rom[o + 1]);

        char path[256];
        snprintf(path, sizeof(path), "Extract/TILESETS/%06X.bin", a);

        size_t disk_sz = 0;
        if (SoeFileSize(path, &disk_sz) && disk_sz > 0 &&
            (expected_sz == 0 || disk_sz == expected_sz)) {
            uint8_t *buf = (uint8_t *)malloc(disk_sz);
            if (buf && SoeReadWholeFile(path, buf, disk_sz)) {
                TilesetInsert(a, buf, (uint32_t)disk_sz);
                loaded_existing++;
                continue;
            }
            free(buf);
        }

        /* Missing or incomplete: decompress only this block and write atomically */
        uint32_t out_sz = 0;
        uint8_t *buf = DecompressBlockViaGame(a, &out_sz);
        if (buf && out_sz > 0) {
            SoeWriteFileAtomic(path, buf, out_sz);
            TilesetInsert(a, buf, out_sz);
            extracted_missing++;
        } else {
            free(buf);
        }
    }

    fprintf(stderr,
            "[AssetCache] TILESETS: %u loaded from Extract/TILESETS/, %u newly extracted\n",
            loaded_existing, extracted_missing);
}

/* ── Sprite 4bpp Cache (Extract/SPRITES/) ────────────────────────────────── */

#define SPR16_COUNT 6266u
#define SPR32_COUNT 4854u
#define SPR_CHUNK_SIZE 256u

static uint8_t *s_spr16_ram = NULL; /* SPR16_COUNT * 32 bytes */
static uint8_t *s_spr32_ram = NULL; /* SPR32_COUNT * 128 bytes */

static void UnpackZeroWordRleHalf(const uint8_t *src, uint32_t *src_pos, uint8_t *dst16)
{
    uint8_t mask = src[*src_pos];
    (*src_pos)++;
    for (int w = 0; w < 8; w++) {
        if (mask & (1u << w)) {
            dst16[w * 2] = 0;
            dst16[w * 2 + 1] = 0;
        } else {
            dst16[w * 2] = src[*src_pos];
            dst16[w * 2 + 1] = src[*src_pos + 1];
            (*src_pos) += 2;
        }
    }
}

static bool UnpackSpriteEntry(uint32_t table_off, uint8_t bank_comp, uint8_t bank_raw,
                              uint32_t entry_idx, uint32_t tile_bytes, uint8_t *dst)
{
    const uint8_t *rom = g_snes->cart->rom;
    uint32_t rom_size = g_snes->cart->romSize;
    uint32_t p = table_off + entry_idx * 3u;
    if (p + 3u > rom_size) return false;

    uint16_t addr16 = (uint16_t)rom[p] | ((uint16_t)rom[p + 1] << 8);
    uint16_t w1     = (uint16_t)rom[p + 1] | ((uint16_t)rom[p + 2] << 8);
    bool is_comp    = (w1 & 0x8000u) != 0;
    uint8_t bank    = (uint8_t)(((w1 + ((uint16_t)(is_comp ? bank_comp : bank_raw) << 8)) >> 8) & 0xFFu);
    uint32_t off    = SoeSnesToRomOffset(((uint32_t)bank << 16) | addr16);
    if (off == UINT32_MAX || off >= rom_size) return false;

    if (!is_comp) {
        if (off + tile_bytes > rom_size) return false;
        memcpy(dst, &rom[off], tile_bytes);
        return true;
    }

    uint32_t halves = tile_bytes / 16u;
    uint32_t pos = 0;
    for (uint32_t h = 0; h < halves; h++) {
        if (off + pos + 17u > rom_size && off + pos + 1u > rom_size) return false;
        UnpackZeroWordRleHalf(&rom[off], &pos, dst + h * 16u);
    }
    return true;
}

static void CollectAndLoadSprites(void)
{
    s_spr16_ram = (uint8_t *)calloc(SPR16_COUNT, 32u);
    s_spr32_ram = (uint8_t *)calloc(SPR32_COUNT, 128u);
    if (!s_spr16_ram || !s_spr32_ram) return;

    uint32_t loaded_chunks = 0;
    uint32_t extracted_chunks = 0;

    /* 16x16 ($D8:0000, 32 bytes each) */
    for (uint32_t base = 0; base < SPR16_COUNT; base += SPR_CHUNK_SIZE) {
        uint32_t cnt = (base + SPR_CHUNK_SIZE <= SPR16_COUNT) ? SPR_CHUNK_SIZE : (SPR16_COUNT - base);
        size_t chunk_bytes = (size_t)cnt * 32u;
        char path[256];
        snprintf(path, sizeof(path), "Extract/SPRITES/spr16_%04X.bin", base);

        size_t disk_sz = 0;
        if (SoeFileSize(path, &disk_sz) && disk_sz == chunk_bytes &&
            SoeReadWholeFile(path, s_spr16_ram + (size_t)base * 32u, chunk_bytes)) {
            loaded_chunks++;
            continue;
        }

        for (uint32_t i = 0; i < cnt; i++) {
            UnpackSpriteEntry(0x180000u, 0x51u, 0xD1u, base + i, 32u,
                              s_spr16_ram + (size_t)(base + i) * 32u);
        }
        SoeWriteFileAtomic(path, s_spr16_ram + (size_t)base * 32u, chunk_bytes);
        extracted_chunks++;
    }

    /* 32x32 ($EC:0000, 128 bytes each) */
    for (uint32_t base = 0; base < SPR32_COUNT; base += SPR_CHUNK_SIZE) {
        uint32_t cnt = (base + SPR_CHUNK_SIZE <= SPR32_COUNT) ? SPR_CHUNK_SIZE : (SPR32_COUNT - base);
        size_t chunk_bytes = (size_t)cnt * 128u;
        char path[256];
        snprintf(path, sizeof(path), "Extract/SPRITES/spr32_%04X.bin", base);

        size_t disk_sz = 0;
        if (SoeFileSize(path, &disk_sz) && disk_sz == chunk_bytes &&
            SoeReadWholeFile(path, s_spr32_ram + (size_t)base * 128u, chunk_bytes)) {
            loaded_chunks++;
            continue;
        }

        for (uint32_t i = 0; i < cnt; i++) {
            UnpackSpriteEntry(0x2C0000u, 0x59u, 0xD9u, base + i, 128u,
                              s_spr32_ram + (size_t)(base + i) * 128u);
        }
        SoeWriteFileAtomic(path, s_spr32_ram + (size_t)base * 128u, chunk_bytes);
        extracted_chunks++;
    }

    fprintf(stderr,
            "[AssetCache] SPRITES: %u chunks loaded from Extract/SPRITES/, %u chunks newly extracted\n",
            loaded_chunks, extracted_chunks);
}

/* ── Runtime memcpy Hooks ────────────────────────────────────────────────── */

static void HookSprite16x16PreOpcode(CpuState *cpu, uint32_t pc24)
{
    (void)pc24;
    if (!s_cache_ready || !s_spr16_ram) return;

    uint16_t spr_id = (uint16_t)g_ram[0x00DD] | ((uint16_t)g_ram[0x00DE] << 8);
    if (spr_id >= SPR16_COUNT) return;

    uint16_t dst_off = (uint16_t)g_ram[0x00CF] | ((uint16_t)g_ram[0x00D0] << 8);
    uint16_t next_off = (uint16_t)(dst_off + 32u);
    if ((uint32_t)dst_off + 32u > 0x0640u) return;

    memcpy(&g_ram[0x6C91u + dst_off], s_spr16_ram + (size_t)spr_id * 32u, 32u);
    g_ram[0x00CF] = (uint8_t)(next_off & 0xFFu);
    g_ram[0x00D0] = (uint8_t)(next_off >> 8);

    /* Redirect past the decompression loop straight to RTS at $80:A7F9 */
    interp_bridge_pre_opcode_redirect(0x80A7F9u);
    (void)cpu;
}

bool SoeAssetCacheTryDecompress8C988D(CpuState *cpu)
{
    if (s_extracting_now || !s_cache_ready || !cpu) return false;

    uint32_t src_addr = (uint32_t)cpu_read16(cpu, 0x00, (uint16_t)(cpu->D + 0x0036u)) |
                        ((uint32_t)cpu_read8(cpu, 0x00, (uint16_t)(cpu->D + 0x0038u)) << 16);
    src_addr = SoeNormalizeRomAddr24(src_addr);

    SoeTilesetSlot *slot = TilesetLookup(src_addr);
    if (!slot || !slot->data || slot->size == 0) return false;

    uint32_t dst_addr = (uint32_t)cpu_read16(cpu, 0x00, (uint16_t)(cpu->D + 0x003Au)) |
                        ((uint32_t)cpu_read8(cpu, 0x00, (uint16_t)(cpu->D + 0x003Cu)) << 16);
    uint8_t  dst_bank = (uint8_t)((dst_addr >> 16) & 0xFFu);
    uint16_t dst_off  = (uint16_t)(dst_addr & 0xFFFFu);

    uint32_t ram_off = UINT32_MAX;
    if (dst_bank == 0x7Eu) {
        ram_off = dst_off;
    } else if (dst_bank == 0x7Fu) {
        ram_off = 0x10000u + dst_off;
    } else if ((dst_bank < 0x40u || (dst_bank >= 0x80u && dst_bank < 0xC0u)) && dst_off < 0x2000u) {
        ram_off = dst_off;
    }

    if (ram_off == UINT32_MAX || ram_off + slot->size > 0x20000u) {
        return false; /* Fallback to original 65816 routine */
    }

    memcpy(&g_ram[ram_off], slot->data, slot->size);
    cpu_write16(cpu, 0x00, (uint16_t)(cpu->D + 0x0034u), (uint16_t)slot->size);
    return true;
}

void SoeAssetCacheInit(void)
{
    if (!g_snes || !g_snes->cart || !g_snes->cart->rom || g_snes->cart->romSize == 0) {
        return;
    }

    uint32_t crc32 = SoeComputeCrc32(g_snes->cart->rom, g_snes->cart->romSize);
    SoeEnsureChecksumDir("Extract/TILESETS", crc32);
    SoeEnsureChecksumDir("Extract/SPRITES", crc32);

    CollectAndLoadTilesets();
    CollectAndLoadSprites();

    interp_bridge_add_pre_opcode_hook(0x80A4FBu, HookSprite16x16PreOpcode);
    s_cache_ready = true;
}

/* ── Part 3: Black-Screen Entity Spawn & VRAM DMA Drain ──────────────────── */

static bool s_drained_this_transition = false;

static bool SoeQueuesAreEmpty(void)
{
    bool vram_dma_empty = (g_ram[0x013B] == g_ram[0x013D]) && (g_ram[0x013C] == g_ram[0x013E]);
    bool cgram_dma_empty = (g_ram[0x1290] == g_ram[0x128E]) && (g_ram[0x1291] == g_ram[0x128F]);
    bool tilemap_empty = (g_ram[0x1170] == g_ram[0x1172]) && (g_ram[0x1171] == g_ram[0x1173]);
    bool spr16_empty = (g_ram[0x15D6] == 0 && g_ram[0x15D7] == 0);
    bool spr32_empty = (g_ram[0x163C] == 0 && g_ram[0x163D] == 0);
    return vram_dma_empty && cgram_dma_empty && tilemap_empty && spr16_empty && spr32_empty;
}

void SoeRoomTransitionDrainIfReady(void)
{
    if (!g_snes || !g_ppu) return;

    uint8_t inidisp = g_ram[0x0106];
    uint16_t fade_target = (uint16_t)g_ram[0x0B83] | ((uint16_t)g_ram[0x0B84] << 8);

    /* Reset latch once screen has faded in past brightness 2 */
    if ((inidisp & 0x80u) == 0 && (inidisp & 0x0Fu) > 2u) {
        s_drained_this_transition = false;
        return;
    }

    /* Trigger at the exact moment INIDISP is 0 (full black) immediately prior to fade-in */
    bool at_black = ((inidisp & 0x0Fu) == 0) || ((inidisp & 0x80u) != 0);
    bool fading_in = ((fade_target & 0x8000u) != 0) && ((fade_target & 0x000Fu) > 0);
    if (!s_drained_this_transition && at_black && fading_in) {
        s_drained_this_transition = true;

        uint8_t saved_fade_lo = g_ram[0x0B83];
        uint8_t saved_fade_hi = g_ram[0x0B84];
        int consecutive_empty = 0;

        WatchdogSetEnabled(0);
        for (int f = 0; f < 16; f++) {
            /* Hold INIDISP at 0 (full black) while draining entity spawns & DMA queues */
            g_ram[0x0106] = 0x00;
            g_ram[0x0B83] = saved_fade_lo;
            g_ram[0x0B84] = saved_fade_hi;

            snes_beam_frame_driver_run_frame();

            if (SoeQueuesAreEmpty()) {
                consecutive_empty++;
                if (f >= 1 && consecutive_empty >= 2) {
                    break;
                }
            } else {
                consecutive_empty = 0;
            }
        }
        /* Restore black brightness 0 and fade-in target so 60Hz fade-in starts cleanly */
        g_ram[0x0106] = 0x00;
        g_ram[0x0B83] = saved_fade_lo;
        g_ram[0x0B84] = saved_fade_hi;
        WatchdogSetEnabled(1);
    }
}
