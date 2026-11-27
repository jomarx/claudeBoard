#pragma once
#include <Arduino.h>

// Wi-Fi setup on the board itself, for when no PC is at hand: the board opens its own
// network "claudeBoard-XXXX" (WPA2, a fresh 8-digit password shown on its screen), answers
// every DNS name with itself so a phone opens the page, and serves a form at 192.168.4.1.
// Saving stores the settings, sets Sync to Wi-Fi and restarts the board. Nothing changes
// until then: Cancel on the screen just closes the setup network.

void portalStart();
void portalStop();
void portalLoop();
bool portalActive();
const char *portalSsid();
const char *portalPass();
int portalClients();   // phones joined to the setup network
