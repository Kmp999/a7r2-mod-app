#include "preset_manager.h"
#include "presets/provia_like.h"
#include "presets/astia_like.h"
#include "presets/velvia_like.h"
#include "presets/classic_neg_like.h"
#include "presets/eterna_like.h"

static PresetDefinition g_presets[5];
static int g_initialized = 0;

FujiResult preset_manager_init(void) {
    if (g_initialized) return FUJI_SUCCESS;

    // 0: Provia-like
    g_presets[0].id = PRESET_PROVIA_LIKE;
    g_presets[0].name = "Provia-like";
    g_presets[0].expected_cost_ms_720p = 45;
    get_preset_provia_matrix(&g_presets[0].matrix);
    get_preset_provia_lut(&g_presets[0].lut);

    // 1: Astia-like
    g_presets[1].id = PRESET_ASTIA_LIKE;
    g_presets[1].name = "Astia-like";
    g_presets[1].expected_cost_ms_720p = 45;
    get_preset_astia_matrix(&g_presets[1].matrix);
    get_preset_astia_lut(&g_presets[1].lut);

    // 2: Velvia-like
    g_presets[2].id = PRESET_VELVIA_LIKE;
    g_presets[2].name = "Velvia-like";
    g_presets[2].expected_cost_ms_720p = 48;
    get_preset_velvia_matrix(&g_presets[2].matrix);
    get_preset_velvia_lut(&g_presets[2].lut);

    // 3: Classic Neg-like
    g_presets[3].id = PRESET_CLASSIC_NEG_LIKE;
    g_presets[3].name = "Classic Neg-like";
    g_presets[3].expected_cost_ms_720p = 50;
    get_preset_classic_neg_matrix(&g_presets[3].matrix);
    get_preset_classic_neg_lut(&g_presets[3].lut);

    // 4: Eterna-like
    g_presets[4].id = PRESET_ETERNA_LIKE;
    g_presets[4].name = "Eterna-like";
    g_presets[4].expected_cost_ms_720p = 42;
    get_preset_eterna_matrix(&g_presets[4].matrix);
    get_preset_eterna_lut(&g_presets[4].lut);

    g_initialized = 1;
    return FUJI_SUCCESS;
}

const PresetDefinition* preset_manager_get(PresetId id) {
    if (!g_initialized) preset_manager_init();
    if ((int)id < 0 || (int)id >= 5) return &g_presets[0];
    return &g_presets[id];
}

int preset_manager_count(void) {
    return 5;
}
