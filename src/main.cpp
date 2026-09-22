#include "uptick.h"
#include <LittleFS.h>

void setup()
{
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n%s %s\n", UPTICK_NAME, UPTICK_VERSION);
  Serial.printf("[sys] flash %u MB, psram %u KB free\n",
                (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)),
                (unsigned)(ESP.getFreePsram() / 1024));

  if (!LittleFS.begin(true))
  {
    Serial.println("[fs] mount failed - config will not persist");
  }

  configBegin();
  marketBegin();

  // Before netBegin, so the boot splash is up during the join rather than the
  // panel sitting dark for twenty seconds. A panel failure is not fatal: the
  // web UI stays fully usable and displayTask idles.
  displayBegin();

  netBegin();

  // Core 0 owns the radio, the web server and the market fetch. Core 1 owns
  // the display and must never block on network I/O.
  xTaskCreatePinnedToCore(netTask,     "net",  8192, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(displayTask, "disp", 4096, nullptr, 1, nullptr, 1);
}

void loop()
{
  vTaskDelay(pdMS_TO_TICKS(1000));
}
