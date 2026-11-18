/* SPDX-License-Identifier: GPL-3.0-only */
/* The exported CLAP entry, from the three functions in the plugin's static library (plugin/src/textura_clap.c).
 * clap-wrapper compiles this file into each format it assembles (the .clap, the .vst3, the .component), so every
 * bundle carries the whole plugin. */
#include <clap/clap.h>

extern "C" {
bool txp_entry_init(const char *plugin_path);
void txp_entry_deinit(void);
const void *txp_entry_get_factory(const char *factory_id);

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#endif
CLAP_EXPORT extern const clap_plugin_entry_t clap_entry;
const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, txp_entry_init, txp_entry_deinit, txp_entry_get_factory };
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
}
