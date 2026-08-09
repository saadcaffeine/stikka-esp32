#include "test_print.h"

#include <new>

#include "config.h"

// Both builders below use the same visual pattern: bands of STRIPE_ROWS rows,
// alternating fully solid (0xFF bytes) and fully blank (0x00 bytes / skipped
// entirely, protocol depending), starting solid. At native raster resolution
// (300dpi for QL, cfg.dpi for Seiko) this prints as clearly visible stripes
// across the full head width -- enough to confirm the printhead fires evenly
// and paper feeds correctly without needing a real decoded image.
static const int STRIPE_ROWS = 8;

bool buildQlTestPrintBytes(std::unique_ptr<uint8_t[]>& out, size_t& outLen) {
  const int printheadPx = cfg.qlPrintheadPx == 1296 ? 1296 : 720;
  const int rowBytes = printheadPx / 8;
  const int nativeDpi = 300;

  int targetWidthPx = (int)((cfg.labelWidth / 25.4) * nativeDpi + 0.5);
  if (targetWidthPx < 8) targetWidthPx = 8;
  if (targetWidthPx > printheadPx) targetWidthPx = printheadPx;

  int targetHeightPx = cfg.labelLength > 0
      ? (int)((cfg.labelLength / 25.4) * nativeDpi + 0.5)
      : 100; // fixed-length snippet for continuous/endless media
  if (targetHeightPx < 1) targetHeightPx = 1;

  const bool autoCut = cfg.qlAutoCut;
  const int invalidateBytes = cfg.qlInvalidateBytes > 0 ? cfg.qlInvalidateBytes : 0;
  const int feed = cfg.qlFeedMarginDots > 0 ? (cfg.qlFeedMarginDots & 0xFFFF) : 0;

  // See frontend/src/zpl-image.ts's buildQLHeader() for the command-by-command
  // reference this mirrors: switch-raster(4) + invalidate(N) + init(2) +
  // switch-raster(4) + status-request(3) + media/quality(13) +
  // autocut(0 or 8) + expanded-mode(4) + feed-margin(5).
  const size_t headerLen = 4 + (size_t)invalidateBytes + 2 + 4 + 3 + 13 +
                            (autoCut ? 8 : 0) + 4 + 5;
  const size_t rowsLen = (size_t)targetHeightPx * (3 + (size_t)rowBytes);
  outLen = headerLen + rowsLen + 1; // +1 = EOF byte

  out.reset(new (std::nothrow) uint8_t[outLen]);
  if (!out) {
    outLen = 0;
    return false;
  }

  size_t p = 0;
  auto put = [&](uint8_t b) { out[p++] = b; };
  auto putSwitchRaster = [&]() {
    put(0x1b); put(0x69); put(0x61); put(0x01); // ESC i a 1
  };

  putSwitchRaster();
  for (int i = 0; i < invalidateBytes; i++) put(0x00);
  put(0x1b); put(0x40); // ESC @ (init)
  putSwitchRaster();
  put(0x1b); put(0x69); put(0x53); // ESC i S (status request)

  const bool continuous = cfg.labelLength <= 0;
  const uint8_t mtype = continuous ? 0x0a : 0x0b;
  const uint8_t mwidth = (uint8_t)(cfg.labelWidth & 0xff);
  const uint8_t mlength = continuous ? 0x00 : (uint8_t)(cfg.labelLength & 0xff);
  const uint32_t rnumber = (uint32_t)targetHeightPx;
  put(0x1b); put(0x69); put(0x7a); // ESC i z (media & quality)
  put(0x80 | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 6));
  put(mtype);
  put(mwidth);
  put(mlength);
  put((uint8_t)(rnumber & 0xff));
  put((uint8_t)((rnumber >> 8) & 0xff));
  put((uint8_t)((rnumber >> 16) & 0xff));
  put((uint8_t)((rnumber >> 24) & 0xff));
  put(0x00); // page 0 (single-page job)
  put(0x00);

  if (autoCut) {
    put(0x1b); put(0x69); put(0x4d); put(0x40); // ESC i M, autocut on
    put(0x1b); put(0x69); put(0x41); put(0x01); // ESC i A, cut every label
  }
  put(0x1b); put(0x69); put(0x4b); put(autoCut ? (uint8_t)(1 << 3) : 0x00); // ESC i K
  put(0x1b); put(0x69); put(0x64); put((uint8_t)(feed & 0xff)); put((uint8_t)((feed >> 8) & 0xff)); // ESC i d

  for (int row = 0; row < targetHeightPx; row++) {
    const bool solid = ((row / STRIPE_ROWS) % 2) == 0;
    put(0x67); put(0x00); put((uint8_t)rowBytes); // raster line, uncompressed
    for (int i = 0; i < rowBytes; i++) put(solid ? 0xFF : 0x00);
  }

  put(0x1a); // EOF (last page)

  return true;
}

bool buildSeikoTestPrintBytes(std::unique_ptr<uint8_t[]>& out, size_t& outLen) {
  const int dpi = cfg.dpi > 0 ? cfg.dpi : 300;
  int maxDots = cfg.seikoMaxDots > 0 ? cfg.seikoMaxDots : 576;
  if (maxDots < 8) maxDots = 8;

  int targetWidthPx = (int)((cfg.labelWidth / 25.4) * dpi + 0.5);
  if (targetWidthPx < 8) targetWidthPx = 8;
  if (targetWidthPx > maxDots) targetWidthPx = maxDots;

  int targetHeightPx = cfg.labelLength > 0
      ? (int)((cfg.labelLength / 25.4) * dpi + 0.5)
      : 100; // fixed-length snippet for continuous/endless media
  if (targetHeightPx < 1) targetHeightPx = 1;

  int bytesPerRow = (targetWidthPx + 7) / 8;
  if (bytesPerRow > 255) bytesPerRow = 255; // row-length command field is one byte

  const int marginDots = maxDots - bytesPerRow * 8 > 0 ? maxDots - bytesPerRow * 8 : 0;
  int marginMm = (int)(12.7 * marginDots / dpi + 0.5);
  if (marginMm < 0) marginMm = 0;
  if (marginMm > 255) marginMm = 255;

  const int density = cfg.seikoDensity & 0xff;
  const int speed = cfg.seikoSpeed & 0xff;

  // Bands of STRIPE_ROWS rows, alternating solid (each row: 0x04 len data...)
  // and blank (one 0x0a/0x0b skip command per band, never per row) -- see
  // frontend/src/zpl-image.ts's buildSeikoRowCommands() for the command
  // reference this mirrors.
  size_t rowsLen = 0;
  {
    int row = 0;
    bool solidBand = true;
    while (row < targetHeightPx) {
      const int remaining = targetHeightPx - row;
      const int bandHeight = STRIPE_ROWS < remaining ? STRIPE_ROWS : remaining;
      if (solidBand) {
        rowsLen += (size_t)bandHeight * (2 + (size_t)bytesPerRow);
      } else {
        rowsLen += (bandHeight == 1) ? 1 : 2;
      }
      row += bandHeight;
      solidBand = !solidBand;
    }
  }

  const size_t headerLen = 6; // 0x06 mm, 0x0e density, 0x0d speed
  const size_t footerLen = 1; // 0x0c eject/cut
  outLen = headerLen + rowsLen + footerLen;

  out.reset(new (std::nothrow) uint8_t[outLen]);
  if (!out) {
    outLen = 0;
    return false;
  }

  size_t p = 0;
  auto put = [&](uint8_t b) { out[p++] = b; };

  put(0x06); put((uint8_t)marginMm);
  put(0x0e); put((uint8_t)density);
  put(0x0d); put((uint8_t)speed);

  {
    int row = 0;
    bool solidBand = true;
    while (row < targetHeightPx) {
      const int remaining = targetHeightPx - row;
      const int bandHeight = STRIPE_ROWS < remaining ? STRIPE_ROWS : remaining;
      if (solidBand) {
        for (int i = 0; i < bandHeight; i++) {
          put(0x04); put((uint8_t)bytesPerRow);
          for (int b = 0; b < bytesPerRow; b++) put(0xFF);
        }
      } else if (bandHeight == 1) {
        put(0x0a);
      } else {
        put(0x0b); put((uint8_t)bandHeight);
      }
      row += bandHeight;
      solidBand = !solidBand;
    }
  }

  put(0x0c); // eject/cut

  return true;
}
