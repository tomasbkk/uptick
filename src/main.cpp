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

  // marketBegin first: it creates the quote mutex, and configBegin loads the
  // stored config through the same path a POST takes - which remaps quotes and
  // therefore takes that mutex. The other way round it takes a null handle and
  // the boot hangs with the panel dark.
  marketBegin();
  configBegin();

  // Before netBegin, so the boot splash is up during the join rather than the
  // panel sitting dark for twenty seconds. A panel failure is not fatal: the
  // web UI stays fully usable and displayTask idles.
  displayBegin();

  netBegin();

  // Core 0 owns the radio, the web server and the market fetch. Core 1 owns
  // the display and must never block on network I/O.
  // Stack sizes are reported as measured minimums in /api/status (stackNet,
  // stackDisplay), so they can be checked rather than guessed at. The net
  // task runs TLS handshakes and, on a save, holds a scratch Config plus two
  // String arrays sized by UPTICK_MAX_FEEDS - about 3 KB at 32 feeds.
  xTaskCreatePinnedToCore(netTask,     "net",  12288, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(displayTask, "disp", 6144,  nullptr, 1, nullptr, 1);
}

void loop()
{
  vTaskDelay(pdMS_TO_TICKS(1000));
}
