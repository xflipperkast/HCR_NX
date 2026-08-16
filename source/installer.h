#ifndef HCR_INSTALLER_H
#define HCR_INSTALLER_H

/* Installs libgame.so and assets/ from DATA_DIR "/hcr.apk". */
int installer_prepare_game_files(void);
const char *installer_last_error(void);

#endif
