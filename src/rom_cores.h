#ifndef ME_ROM_CORES_H
#define ME_ROM_CORES_H

/* Opinionated ROM-extension -> core table. Each extension opens in exactly
   one core, picked by us; users don't choose. Used when a ROM is launched
   without an explicit core (single CLI argument, or dropped on the window).

   Returns the core's DLL filename (looked up in the cores/ folder next to
   the exe), or NULL if no core is assigned to this ROM's extension. */
const char *me_rom_core_for(const char *rom_path);

#endif
