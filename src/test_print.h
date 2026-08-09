#pragma once

#include <Arduino.h>

#include <memory>

// Builds a small synthetic striped test pattern (alternating solid-black and
// blank bands across the full configured printhead width) in the Brother QL
// raster protocol -- sized from cfg.dpi/labelWidth/labelLength like a real
// job's header would be, but with placeholder row content instead of a real
// decoded image, since there's no client-supplied image for a
// firmware-triggered test print. Mirrors the header-building logic in
// frontend/src/zpl-image.ts's imageDataURLToQLRasterBase64() so a real
// printer sees the exact same framing a normal job would send. Returns
// false (leaving out/outLen untouched) only on allocation failure.
bool buildQlTestPrintBytes(std::unique_ptr<uint8_t[]>& out, size_t& outLen);

// Same idea for the Seiko SLP raster protocol -- mirrors
// frontend/src/zpl-image.ts's imageDataURLToSeikoRasterBase64().
bool buildSeikoTestPrintBytes(std::unique_ptr<uint8_t[]>& out, size_t& outLen);
