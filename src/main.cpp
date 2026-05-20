#include <Arduino.h>
#include <SPIFFS.h>
#include <algorithm>
#include <vector>

#include <AnimatedGIF.h>

#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "lgfx_pros3_st7789_170x320.hpp"

#ifndef SD_CS
  // NOTE: Avoid ESP32-S3 strapping pins for SD CS (GPIO3 is a strapping pin).
  // Pick a "boring" GPIO with no boot function.
  #define SD_CS 15
#endif

#ifndef GIFPLAYER_DEBUG
  // 0 = quiet desk-toy mode, 1 = verbose logging
  #define GIFPLAYER_DEBUG 0
#endif

#if GIFPLAYER_DEBUG
  #define GIFLOG(...) Serial.printf(__VA_ARGS__)
#else
  #define GIFLOG(...) do {} while (0)
#endif

#define GIFERR(...) Serial.printf(__VA_ARGS__)

static const char* kSdMountPath = "/sd";

static LGFX_ST7789_170x320 tft;
static AnimatedGIF gif;

static std::vector<String> s_gifPaths;

static bool s_sdMounted = false;
static sdmmc_card_t* s_sdCard = nullptr;

static uint16_t s_lineBuf[320]; // max width we'll ever push (clipped)

static uint8_t* allocGifBytes(size_t size) {
#if defined(BOARD_HAS_PSRAM)
  uint8_t* p = (uint8_t*)ps_malloc(size);
  if (p) return p;
#endif
  return (uint8_t*)malloc(size);
}

static bool readFileToRam(const String& path, uint8_t** outBuf, size_t* outSize) {
  if (!outBuf || !outSize) return false;
  *outBuf = nullptr;
  *outSize = 0;

  // Keep this sane; your current GIFs are <1MB each.
  const size_t kMaxBytes = 2 * 1024 * 1024;

  if (path.startsWith("/sd/")) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    const long szL = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (szL <= 0 || (size_t)szL > kMaxBytes) {
      fclose(fp);
      return false;
    }
    const size_t sz = (size_t)szL;
    uint8_t* buf = allocGifBytes(sz);
    if (!buf) {
      fclose(fp);
      return false;
    }
    const size_t n = fread(buf, 1, sz, fp);
    fclose(fp);
    if (n != sz) {
      free(buf);
      return false;
    }
    *outBuf = buf;
    *outSize = sz;
    return true;
  }

  File f = SPIFFS.open(path, "r");
  if (!f) return false;
  const size_t sz = (size_t)f.size();
  if (sz == 0 || sz > kMaxBytes) {
    f.close();
    return false;
  }
  uint8_t* buf = allocGifBytes(sz);
  if (!buf) {
    f.close();
    return false;
  }
  const size_t n = (size_t)f.read(buf, sz);
  f.close();
  if (n != sz) {
    free(buf);
    return false;
  }
  *outBuf = buf;
  *outSize = sz;
  return true;
}

static bool endsWithGif(const String& s) {
  if (s.length() < 4) return false;
  String tail = s.substring(s.length() - 4);
  tail.toLowerCase();
  if (tail != ".gif") return false;

  // Ignore hidden files and macOS AppleDouble "._" sidecar files.
  // On SD cards created on macOS these commonly appear and are not real GIFs.
  int slash = s.lastIndexOf('/');
  const String base = (slash >= 0) ? s.substring(slash + 1) : s;
  if (base.startsWith(".")) return false;   // includes "._foo.gif" and ".foo.gif"
  return true;
}

static void addGifPathIfNew(const String& p) {
  for (const auto& existing : s_gifPaths) {
    if (existing == p) return;
  }
  s_gifPaths.push_back(p);
}

static void scanForGifsRecursiveSpiffs(const String& dir) {
  File root = SPIFFS.open(dir);
  if (!root || !root.isDirectory()) return;

  for (;;) {
    File f = root.openNextFile();
    if (!f) break;

    String p = String(f.name());
    if (!p.startsWith("/")) p = "/" + p;

    if (f.isDirectory()) {
      scanForGifsRecursiveSpiffs(p);
    } else if (endsWithGif(p)) {
      addGifPathIfNew(p);
    }

    f.close();
    delay(0);
  }

  root.close();
}

static void scanForGifsRecursiveSd(const String& dir) {
  DIR* d = opendir(dir.c_str());
  if (!d) return;

  for (;;) {
    struct dirent* e = readdir(d);
    if (!e) break;
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;

    String p = dir;
    if (!p.endsWith("/")) p += "/";
    p += e->d_name;

    struct stat st;
    if (stat(p.c_str(), &st) != 0) continue;

    if (S_ISDIR(st.st_mode)) {
      scanForGifsRecursiveSd(p);
    } else if (endsWithGif(p)) {
      addGifPathIfNew(p);
    }
  }

  closedir(d);
}

static bool mountSdCardIfPresent() {
  if (s_sdMounted) return true;

  esp_vfs_fat_mount_config_t mount_config = VFS_FAT_MOUNT_DEFAULT_CONFIG();
  mount_config.max_files = 5;
  mount_config.allocation_unit_size = 16 * 1024;

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  // Use the same ESP-IDF SPI host as the TFT (LovyanGFX uses spi_master too).
  host.slot = TFT_SPI_HOST;
  // SD over SPI default is 20MHz. Reading the GIF into RAM first means the SD
  // bus isn't used while we're blasting pixels to the TFT.
  host.max_freq_khz = 20000; // 20 MHz

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = (gpio_num_t)SD_CS;
  slot_config.host_id = (spi_host_device_t)TFT_SPI_HOST;

  const esp_err_t err = esp_vfs_fat_sdspi_mount(
      kSdMountPath, &host, &slot_config, &mount_config, &s_sdCard);
  if (err != ESP_OK) {
    GIFERR("[SD] mount failed (cs=%d host=%d): %s (%d)\n",
           (int)SD_CS, (int)TFT_SPI_HOST, esp_err_to_name(err), (int)err);
    return false;
  }

  s_sdMounted = true;
  GIFLOG("[SD] mounted at %s\n", kSdMountPath);
  return true;
}

// --- AnimatedGIF draw callback (RGB565) ---
static void GIFDraw(GIFDRAW* pDraw) {
  // Center the GIF canvas on-screen.
  const int screenW = (int)tft.width();
  const int screenH = (int)tft.height();
  const int canvasW = gif.getCanvasWidth();
  const int canvasH = gif.getCanvasHeight();
  const int originX = (screenW - canvasW) / 2;
  const int originY = (screenH - canvasH) / 2;

  const int dstY = originY + (int)pDraw->iY + (int)pDraw->y;
  if (dstY < 0 || dstY >= screenH) return;

  int dstX = originX + (int)pDraw->iX;
  int drawW = (int)pDraw->iWidth;
  int srcStart = 0;

  // Horizontal clip.
  if (dstX < 0) {
    srcStart = -dstX;
    drawW -= srcStart;
    dstX = 0;
  }
  if (dstX + drawW > screenW) drawW = screenW - dstX;
  if (drawW <= 0) return;

  const uint8_t* src = pDraw->pPixels + srcStart;
  const uint16_t* pal = pDraw->pPalette;

  if (pDraw->ucHasTransparency) {
    const uint8_t tr = pDraw->ucTransparent;

    // Build line into buffer and draw only opaque runs.
    for (int i = 0; i < drawW; i++) {
      const uint8_t idx = src[i];
      s_lineBuf[i] = (idx == tr) ? 0 : pal[idx];
    }

    int runStart = -1;
    for (int i = 0; i <= drawW; i++) {
      const bool opaque = (i < drawW) && (src[i] != tr);
      if (opaque && runStart < 0) runStart = i;
      if ((!opaque || i == drawW) && runStart >= 0) {
        const int runLen = i - runStart;
        tft.pushImage(dstX + runStart, dstY, runLen, 1, &s_lineBuf[runStart]);
        runStart = -1;
      }
    }
  } else {
    for (int i = 0; i < drawW; i++) {
      s_lineBuf[i] = pal[src[i]];
    }
    tft.pushImage(dstX, dstY, drawW, 1, s_lineBuf);
  }
}

static void playGif(const String& path) {
  const uint32_t showStartMs = millis();
  tft.fillScreen(TFT_BLACK);

  uint8_t* gifBytes = nullptr;
  size_t gifSize = 0;
  if (!readFileToRam(path, &gifBytes, &gifSize)) {
    GIFERR("[GIF] read failed: %s\n", path.c_str());
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.println("READ FAIL");
    delay(1500);
    return;
  }
  GIFLOG("[GIF] loaded to RAM: %s size=%u\n", path.c_str(), (unsigned)gifSize);

  if (!gif.open(gifBytes, (int)gifSize, GIFDraw)) {
    GIFERR("[GIF] open failed (mem): %s err=%d\n", path.c_str(), gif.getLastError());
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.println("OPEN FAIL");
    free(gifBytes);
    delay(1500);
    return;
  }

  GIFINFO info;
  const int infoOk = gif.getInfo(&info);
  GIFLOG("[GIF] canvas=%dx%d infoOk=%d frames=%ld durationMs=%ld\n",
         gif.getCanvasWidth(), gif.getCanvasHeight(), infoOk,
         (long)info.iFrameCount, (long)info.iDuration);

  int frameCount = 0;
  int delayMs = 0;
  const int loopsToPlay = 3;
  const uint32_t startMs = millis();
  const uint32_t maxGifMs = 30000; // safety cap

  // Now that we don't read from SD during playback, we can hold the TFT SPI bus
  // for much faster writes.
  tft.startWrite();
  if (infoOk && info.iFrameCount > 0) {
    const int framesPerLoop = (int)info.iFrameCount;

    for (int loop = 0; loop < loopsToPlay; loop++) {
      if (loop > 0) gif.reset(); // restart from the first frame

      int framesThisLoop = 0;
      for (int i = 0; i < framesPerLoop; i++) {
        // Some GIFs report a repeat count of 1 and will return false at end-of-animation.
        // We still want to force 3 loops, so treat "false" as end-of-this-loop.
        if (!gif.playFrame(false, &delayMs)) break;
        framesThisLoop++;
        frameCount++;

        if (delayMs > 0) delay((uint32_t)delayMs);
        else delay(1);

        if ((uint32_t)(millis() - startMs) >= maxGifMs) {
          GIFLOG("[GIF] stopping after %lu ms (cap)\n", (unsigned long)maxGifMs);
          loop = loopsToPlay; // break outer loop
          break;
        }
      }

      if (framesThisLoop == 0) break; // couldn't play anything
    }

    GIFLOG("[GIF] stopping after %d loops (target), frames/loop=%d\n", loopsToPlay, framesPerLoop);
  } else {
    // Fallback: no frame count; do 3 loops by running until end, then reset.
    for (int loop = 0; loop < loopsToPlay; loop++) {
      if (loop > 0) gif.reset();
      int framesThisLoop = 0;
      while (gif.playFrame(false, &delayMs)) {
        framesThisLoop++;
        frameCount++;
        if (delayMs > 0) delay((uint32_t)delayMs);
        else delay(1);
        if ((uint32_t)(millis() - startMs) >= maxGifMs) {
          GIFLOG("[GIF] stopping after %lu ms (cap)\n", (unsigned long)maxGifMs);
          loop = loopsToPlay;
          break;
        }
      }
      if (framesThisLoop == 0) break;
    }
  }
  tft.endWrite();
  GIFLOG("[GIF] done frames=%d\n", frameCount);
  gif.close();
  free(gifBytes);

  if (frameCount == 0) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.println("DECODE FAIL");
    delay(1500);
  } else {
    // Brief yield between GIFs.
    const uint32_t elapsed = (uint32_t)(millis() - showStartMs);
    if (elapsed < 750) delay(750 - elapsed);
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // Force backlight on (many modules just need BL=HIGH).
  pinMode(14, OUTPUT);
  digitalWrite(14, HIGH);

  // Hard reset pulse (some ST7789 breakouts need a real reset edge).
  pinMode(2, OUTPUT);
  digitalWrite(2, HIGH);
  delay(10);
  digitalWrite(2, LOW);
  delay(20);
  digitalWrite(2, HIGH);
  delay(120);

  tft.init();
  tft.setRotation(1); // rotate 90° (landscape)
  GIFLOG("[TFT] init ok w=%d h=%d\n", tft.width(), tft.height());
  tft.fillScreen(TFT_BLACK);

  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(0, 0);
  tft.println("GIFPlayer");

  if (!SPIFFS.begin(true)) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.println("SPIFFS mount failed");
    for (;;) delay(1000);
  }
  tft.println("SPIFFS ok");

  // Prefer SD card GIFs if present (mounts at /sd).
  s_gifPaths.clear();
  if (mountSdCardIfPresent()) {
    scanForGifsRecursiveSd(String(kSdMountPath));
    scanForGifsRecursiveSd(String(kSdMountPath) + "/gifs");
  }

  // Fall back to SPIFFS GIFs.
  if (s_gifPaths.empty()) {
    // With `data_dir = gifs`, the files land at SPIFFS root.
    scanForGifsRecursiveSpiffs("/");
    scanForGifsRecursiveSpiffs("/gifs");
  }

  std::sort(s_gifPaths.begin(), s_gifPaths.end(),
            [](const String& a, const String& b) { return a < b; });

  GIFLOG("[GIF] found %u files\n", (unsigned)s_gifPaths.size());
  for (size_t i = 0; i < s_gifPaths.size(); i++) {
    GIFLOG("[GIF] #%u %s\n", (unsigned)(i + 1), s_gifPaths[i].c_str());
  }
  if (s_gifPaths.empty()) {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.println("No .gif files");
    tft.println("Upload FS");
    for (;;) delay(1000);
  }

  // The library can emit RGB565 palettes already byte-swapped for display writes.
  gif.begin(BIG_ENDIAN_PIXELS);
  randomSeed((uint32_t)esp_random());
}

void loop() {
  if (s_gifPaths.empty()) {
    delay(1000);
    return;
  }

  // Random selection (avoid immediate repeats).
  static int lastIdx = -1;
  int idx = (int)random((long)s_gifPaths.size());
  if ((int)s_gifPaths.size() > 1 && idx == lastIdx) {
    idx = (idx + 1) % (int)s_gifPaths.size();
  }
  lastIdx = idx;
  const String path = s_gifPaths[(size_t)idx];

  GIFLOG("[GIF] %s\n", path.c_str());
  playGif(path);
  delay(50);
}

