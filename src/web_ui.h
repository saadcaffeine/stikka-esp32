#pragma once

// Registers routes (config page, logs page, captive-portal redirects) and
// starts the HTTP server. Call once from setup(), after wifiManagerSetup().
void webUiSetup();

// Services pending HTTP requests. Call once per loop() iteration.
void webUiLoop();
