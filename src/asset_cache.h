#ifndef SOE_ASSET_CACHE_H
#define SOE_ASSET_CACHE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct CpuState CpuState;

/*
 * Initialize the unified first-boot decompression cache (Extract/TILESETS/ and
 * Extract/SPRITES/), verify ROM CRC32, extract any missing assets using the
 * game's own decompression routines, load all cached assets into host RAM,
 * and register runtime memcpy hooks.
 */
void SoeAssetCacheInit(void);

/*
 * Runtime memcpy hook for $8C:988D (master decompression routine).
 * If the 24-bit ROM source address at DP+$36 is in the loaded TILESETS cache,
 * copies the pre-decompressed bytes to the WRAM target at DP+$3A, sets DP+$34
 * to the decompressed byte size, and returns true (caller should RTL).
 * Returns false to fall back to the original 65816 decompression routine.
 */
bool SoeAssetCacheTryDecompress8C988D(CpuState *cpu);

/*
 * Called from GameRunOneFrame() right before/after advancing a simulation frame.
 * When screen brightness (INIDISP / $7E:0106) is 0 (full black) immediately
 * prior to fade-in ($7E:0B83 = $800F), drains all pending entity spawns,
 * sprite tile allocations, and VRAM/CGRAM/tilemap DMA queues while holding
 * the display at full black, then resumes normal 60Hz fade-in.
 */
void SoeRoomTransitionDrainIfReady(void);

#endif /* SOE_ASSET_CACHE_H */
