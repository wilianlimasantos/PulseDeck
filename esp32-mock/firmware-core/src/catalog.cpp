#include "pd/catalog.h"

#include <string.h>

namespace pd {

// Reference catalog = protocol.json "effects.table" (ids 1..39; 6 and 24 are retired ids that the pedal reads as 27 and 29).
// Names, ranges and defaults come from that table verbatim. COST/MEM are NOT in the protocol: gate, boost, overdrive, ir, delay and
// reverb2 keep the figures the bench has always used; every other pair is an ESTIMATE (CPU ticks of a 1000 budget, bytes of effect
// arena) until it is measured on the pedal. The pedal's own FXT reply is the authority at run time.
static const FxDef kFx[] = {
    {1, "gate", "dynamics", 40, 256, 2, {{"umbral_db", -90, 0, -70}, {"release_ms", 5, 1000, 120}}},
    {2, "boost", "drive", 40, 256, 1, {{"ganancia_db", -12, 24, 6}}},
    {3, "overdrive", "drive", 45, 512, 5, {{"drive_db", 0, 40, 18}, {"tono_hz", 1000, 9000, 4000}, {"nivel_db", -30, 0, -8}, {"os_n", 0, 3, 1}, {"limpio_pct", 0, 100, 0}}},
    {4, "eq", "eq", 35, 256, 4, {{"graves_db", -15, 15, 0}, {"medios_db", -15, 15, 0}, {"medios_hz", 200, 5000, 800}, {"agudos_db", -15, 15, 0}}},
    {5, "delay", "delay", 50, 12288, 6, {{"tiempo_ms", 10, 2000, 350}, {"realim_pct", 0, 95, 35}, {"mezcla_pct", 0, 100, 30}, {"amort_pct", 0, 90, 30}, {"sync", 0, 9, 0}, {"lowcut_hz", 0, 800, 0}}},
    {7, "ir", "cab", 50, 8152, 4, {{"mezcla_pct", 0, 100, 100}, {"nivel_db", -30, 12, 0}, {"ir_slot", 0, 8, 0}, {"largo_ms", 0, 85, 21}}},
    {8, "comp", "dynamics", 45, 512, 6, {{"umbral_db", -50, 0, -24}, {"ratio_x10", 10, 200, 40}, {"ataque_ms", 1, 100, 8}, {"release_ms", 20, 1000, 150}, {"nivel_db", -12, 24, 6}, {"mezcla_pct", 0, 100, 100}}},
    {9, "ts9", "drive", 45, 512, 5, {{"drive_pct", 0, 100, 50}, {"tono_pct", 0, 100, 50}, {"nivel_db", -30, 6, -6}, {"os_n", 0, 3, 1}, {"limpio_pct", 0, 100, 0}}},
    {10, "ds1", "dist", 45, 512, 5, {{"dist_pct", 0, 100, 50}, {"tono_pct", 0, 100, 50}, {"nivel_db", -30, 6, -10}, {"os_n", 0, 3, 1}, {"limpio_pct", 0, 100, 0}}},
    {11, "chorus", "mod", 45, 2048, 5, {{"vel_chz", 5, 500, 80}, {"prof_pct", 0, 100, 50}, {"mezcla_pct", 0, 100, 50}, {"sync", 0, 12, 0}, {"lowcut_hz", 0, 800, 0}}},
    {12, "flanger", "mod", 45, 2048, 6, {{"vel_chz", 5, 300, 25}, {"prof_pct", 0, 100, 70}, {"realim_pct", -90, 90, 50}, {"mezcla_pct", 0, 100, 50}, {"sync", 0, 12, 0}, {"lowcut_hz", 0, 800, 0}}},
    {13, "phaser", "mod", 45, 512, 5, {{"vel_chz", 5, 500, 50}, {"prof_pct", 0, 100, 80}, {"realim_pct", 0, 90, 40}, {"sync", 0, 12, 0}, {"lowcut_hz", 0, 800, 0}}},
    {14, "tremolo", "mod", 30, 256, 4, {{"vel_chz", 50, 1500, 500}, {"prof_pct", 0, 100, 60}, {"forma_pct", 0, 100, 0}, {"sync", 0, 12, 0}}},
    {15, "vibrato", "mod", 40, 2048, 3, {{"vel_chz", 50, 1000, 500}, {"prof_pct", 0, 100, 40}, {"sync", 0, 12, 0}}},
    {16, "klon", "drive", 50, 512, 5, {{"ganancia_pct", 0, 100, 40}, {"agudos_pct", 0, 100, 50}, {"nivel_db", -30, 6, -6}, {"mezcla_pct", 0, 100, 50}, {"os_n", 0, 3, 1}}},
    {17, "rat", "dist", 45, 512, 5, {{"dist_pct", 0, 100, 50}, {"filtro_pct", 0, 100, 40}, {"nivel_db", -30, 6, -10}, {"os_n", 0, 3, 1}, {"limpio_pct", 0, 100, 0}}},
    {18, "muff", "dist", 50, 512, 5, {{"sustain_pct", 0, 100, 50}, {"tono_pct", 0, 100, 50}, {"nivel_db", -30, 6, -10}, {"os_n", 0, 3, 3}, {"limpio_pct", 0, 100, 0}}},
    {19, "fuzzface", "dist", 50, 512, 7, {{"fuzz_pct", 0, 100, 70}, {"bias_pct", 0, 100, 50}, {"tono_pct", 0, 100, 60}, {"nivel_db", -30, 6, -10}, {"os_n", 0, 3, 3}, {"limpio_pct", 0, 100, 0}, {"modelo", 0, 1, 0}}},
    {20, "bd2", "drive", 45, 512, 5, {{"drive_pct", 0, 100, 50}, {"tono_pct", 0, 100, 50}, {"nivel_db", -30, 6, -8}, {"os_n", 0, 3, 1}, {"limpio_pct", 0, 100, 0}}},
    {21, "amp_clean", "amp", 60, 1024, 7, {{"ganancia_pct", 0, 100, 40}, {"graves_pct", 0, 100, 50}, {"medios_pct", 0, 100, 50}, {"agudos_pct", 0, 100, 50}, {"presencia_pct", 0, 100, 40}, {"nivel_db", -30, 6, -6}, {"os_n", 0, 3, 1}}},
    {22, "amp_brit", "amp", 70, 1024, 7, {{"ganancia_pct", 0, 100, 50}, {"graves_pct", 0, 100, 50}, {"medios_pct", 0, 100, 60}, {"agudos_pct", 0, 100, 55}, {"presencia_pct", 0, 100, 50}, {"nivel_db", -30, 6, -6}, {"os_n", 0, 3, 1}}},
    {23, "amp_hi", "amp", 80, 1024, 7, {{"ganancia_pct", 0, 100, 60}, {"graves_pct", 0, 100, 55}, {"medios_pct", 0, 100, 30}, {"agudos_pct", 0, 100, 60}, {"presencia_pct", 0, 100, 50}, {"nivel_db", -30, 6, -8}, {"os_n", 0, 3, 3}}},
    {25, "doubler", "mod", 50, 4096, 4, {{"retardo_ms", 5, 40, 20}, {"detune_ct", -20, 20, 7}, {"mezcla_pct", 0, 100, 50}, {"nivel_db", -30, 12, 0}}},
    {26, "hum", "eq", 35, 512, 4, {{"mains_hz", 50, 60, 50}, {"depth_att", 3, 40, 24}, {"ancho_hz", 1, 20, 4}, {"armonicos_n", 1, 4, 2}}},
    {27, "reverb2", "reverb", 45, 8192, 7, {{"tamano_pct", 0, 100, 60}, {"decay_pct", 0, 100, 50}, {"amort_pct", 0, 100, 40}, {"predelay_ms", 0, 100, 15}, {"mezcla_pct", 0, 100, 30}, {"nivel_db", -24, 12, 0}, {"mod_pct", 0, 100, 40}}},
    {28, "sgate", "dynamics", 50, 512, 4, {{"umbral_db", -90, -20, -52}, {"rango_att", 6, 90, 60}, {"release_ms", 20, 1000, 200}, {"hush_pct", 0, 100, 40}}},
    {29, "pitchpro", "mod", 90, 4096, 7, {{"semitonos_st", -12, 12, 12}, {"cents_ct", -50, 50, 0}, {"mezcla_pct", 0, 100, 50}, {"nivel_db", -30, 12, 0}, {"ventana_ms", 10, 27, 25}, {"ataque_pct", 0, 100, 50}, {"modo", 0, 2, 0}}},
    {30, "spectral", "reverb", 80, 16384, 5, {{"decay_pct", 0, 100, 55}, {"amort_pct", 0, 100, 40}, {"difus_pct", 0, 100, 70}, {"mezcla_pct", 0, 100, 35}, {"nivel_db", -24, 12, 0}}},
    {31, "shimmer", "reverb", 100, 16384, 7, {{"tamano_pct", 0, 100, 55}, {"decay_pct", 0, 100, 70}, {"amort_pct", 0, 100, 45}, {"intervalo", 0, 3, 0}, {"mezcla_pct", 0, 100, 34}, {"nivel_db", -24, 12, 0}, {"shimmer_pct", 0, 100, 22}}},
    {32, "hybrid", "reverb", 90, 12288, 7, {{"tamano_pct", 0, 100, 50}, {"decay_pct", 0, 100, 45}, {"amort_pct", 0, 100, 40}, {"predelay_ms", 0, 100, 12}, {"mezcla_pct", 0, 100, 30}, {"nivel_db", -24, 12, 0}, {"er_pct", 0, 100, 55}}},
    {33, "plate", "reverb", 60, 8192, 7, {{"tamano_pct", 0, 100, 60}, {"decay_pct", 0, 100, 55}, {"amort_pct", 0, 100, 25}, {"predelay_ms", 0, 100, 10}, {"mezcla_pct", 0, 100, 30}, {"nivel_db", -24, 12, 0}, {"mod_pct", 0, 100, 40}}},
    {34, "spring", "reverb", 55, 6144, 7, {{"dwell_pct", 0, 100, 40}, {"decay_pct", 0, 100, 45}, {"tono_pct", 0, 100, 50}, {"resortes_n", 1, 3, 2}, {"mezcla_pct", 0, 100, 30}, {"nivel_db", -24, 12, 0}, {"drip_pct", 0, 100, 60}}},
    {35, "cloud", "reverb", 90, 16384, 7, {{"tamano_pct", 0, 100, 70}, {"decay_pct", 0, 100, 65}, {"amort_pct", 0, 100, 35}, {"mod_pct", 0, 100, 50}, {"mezcla_pct", 0, 100, 40}, {"nivel_db", -24, 12, 0}, {"freeze", 0, 1, 0}}},
    {36, "geq", "eq", 45, 512, 7, {{"g50_db", -12, 12, 0}, {"g125_db", -12, 12, 0}, {"g315_db", -12, 12, 0}, {"g800_db", -12, 12, 0}, {"g2k_db", -12, 12, 0}, {"g5k_db", -12, 12, 0}, {"nivel_db", -12, 12, 0}}},
    {37, "basseq", "eq", 45, 512, 7, {{"graves_db", -15, 15, 0}, {"bajos_db", -15, 15, 0}, {"medios_db", -15, 15, 0}, {"altos_db", -15, 15, 0}, {"agudos_db", -15, 15, 0}, {"nivel_db", -15, 15, 0}, {"geb7", 0, 1023, 0}}},
    {38, "envfilter", "mod", 45, 512, 7, {{"sens_pct", 0, 100, 50}, {"frec_lo_hz", 40, 1000, 300}, {"frec_hi_hz", 200, 8000, 2400}, {"reso_pct", 0, 100, 50}, {"decay_pct", 0, 100, 40}, {"filtro_modo", 0, 7, 1}, {"mezcla_pct", 0, 100, 100}}},
    {39, "rotary", "mod", 70, 4096, 7, {{"velocidad", 0, 2, 0}, {"drive_pct", 0, 100, 20}, {"rapido_chz", 300, 1000, 667}, {"inercia_pct", 0, 2660, 50}, {"bocina_pct", 0, 100, 50}, {"distancia_pct", 0, 100, 40}, {"mezcla_pct", 0, 100, 100}}},
};

int fxCount() { return (int)(sizeof(kFx) / sizeof(kFx[0])); }
const FxDef* fxAt(int i) { return (i >= 0 && i < fxCount()) ? &kFx[i] : nullptr; }
const FxDef* fxById(uint8_t id) {
  for (const auto& f : kFx)
    if (f.id == id) return &f;
  return nullptr;
}
const FxDef* fxByName(const char* name) {
  for (const auto& f : kFx)
    if (strcmp(f.name, name) == 0) return &f;
  return nullptr;
}

}  // namespace pd
