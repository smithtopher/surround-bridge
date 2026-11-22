// Windows-only helpers for the tray app (bridgecore.dll): the system's default output endpoint,
// which the app points at the virtual cable while the bridge is on, and back afterwards.
#ifndef SB_BRIDGE_WIN_H
#define SB_BRIDGE_WIN_H

#include "pc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// role: 0 = console + multimedia (what apps play to), 1 = communications (calls, chat apps)
// The default render endpoint's id (as in PCDevice.uid) into uid. 0 on success.
PC_API int sb_win_default_output(int role, char *uid, int len);
// Makes uid the default render endpoint for that role. 0 on success.
// Uses the undocumented IPolicyConfig interface, as Windows' own Sound settings do.
PC_API int sb_win_set_default_output(int role, const char *uid);

#ifdef __cplusplus
}
#endif
#endif
