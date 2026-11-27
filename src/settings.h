#pragma once
#include "model.h"

// Persists g_settings in NVS so the board keeps them without the companion.
void settingsLoad();
// Writes only when something changed since the last load or save.
void settingsSave();
