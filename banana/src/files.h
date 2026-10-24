// Files on the card (docs/EMULATORS.md's places), and the save backups -
// the repo's "save files are sacred" rule: every start, and every state load,
// copies the save aside first, the newest MAX_BACKUPS per game kept.
#ifndef BANANA_FILES_H
#define BANANA_FILES_H

#include <stdbool.h>
#include <stddef.h>

#define MAX_BACKUPS 10

bool file_exists(const char* path); // and not empty
bool copy_file(const char* from, const char* to);
// mkdir -p
void make_dirs(const char* path);
// The game's name: the ROM's file name without its folder and extension.
void rom_base(const char* rom, char* out, size_t len);
// Copies the save into dir as "<base>-YYYYmmdd-HHMMSS.sav" and prunes the oldest.
void backup_save(const char* dir, const char* base, const char* savePath);
// Flushes a file to the card (after the save is written back).
void sync_file(const char* path);

#endif
