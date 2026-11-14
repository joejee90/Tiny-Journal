#pragma once
#include "PrinterText.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <WiFiClient.h>
#include <algorithm>
#include <atomic>
namespace JournalPrinter {
constexpr size_t MAX_JOB_BYTES = 32768;
struct Config {
  String host;
  uint16_t port = 9100;
  uint16_t columns = 32;
  bool live = false;
  bool cut = true;
};
static Config config;
struct Job {
  char host[128];
  char ssid[64];
  char password[128];
  uint16_t port;
  uint8_t *bytes;
  size_t count;
};
static QueueHandle_t jobs = nullptr;
static TaskHandle_t worker = nullptr;
static std::atomic<bool> active{false};
enum Result {
  SENT = 1,
  WIFI_SETUP = -1,
  PARTIAL = -2,
  WIFI_FAILED = -3,
  TCP_FAILED = -4
};
enum Phase { IDLE, WIFI_CONNECTING, TCP_CONNECTING, SENDING };
static std::atomic<int> outcome{0};
static std::atomic<int> lastResult{0}, phase{IDLE}, lastWiFiStatus{0};
static std::atomic<size_t> lastBytesSent{0};
static std::atomic<unsigned long> releaseWiFiAt{0};
constexpr unsigned long WIFI_RELEASE_GRACE_MS = 3000;
static std::atomic<size_t> queuedBytes{0};
inline bool busy() { return queuedBytes.load() != 0; }
// A prior completion must never power down a newly queued job. Keep the radio
// alive briefly after closing TCP; write() means accepted by the stack, not
// that a receipt has physically printed.
inline bool releaseWiFiDue(unsigned long now) {
  unsigned long due = releaseWiFiAt.load();
  if (!due || busy() || config.live || (long)(now - due) < 0)
    return false;
  releaseWiFiAt = 0;
  return true;
}
static bool writeAll(WiFiClient &client, const uint8_t *p, size_t size) {
  size_t offset = 0;
  unsigned long deadline = millis();
  while (offset < size && client.connected()) {
    size_t n = client.write(p + offset, std::min((size_t)512, size - offset));
    if (n) {
      offset += n;
      lastBytesSent = offset;
      deadline = millis();
    } else if (millis() - deadline > 5000)
      return false;
    vTaskDelay(1);
  }
  return offset == size;
}
static void task(void *) {
  Job job;
  for (;;) {
    if (xQueueReceive(jobs, &job, portMAX_DELAY) != pdTRUE)
      continue;
    int result = WIFI_SETUP;
    lastBytesSent = 0;
    phase = WIFI_CONNECTING;
    if (WiFi.status() != WL_CONNECTED && job.ssid[0]) {
      WiFi.mode(WIFI_STA);
      WiFi.begin(job.ssid, job.password);
      unsigned long start = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - start < 15000)
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    lastWiFiStatus = WiFi.status();
    if (WiFi.status() == WL_CONNECTED) {
      phase = TCP_CONNECTING;
      result = TCP_FAILED;
      WiFiClient client;
      client.setTimeout(5000);
      if (client.connect(job.host, job.port, 3000)) {
        phase = SENDING;
        result = writeAll(client, job.bytes, job.count) ? SENT : PARTIAL;
        client.stop();
      }
    }
    if (WiFi.status() != WL_CONNECTED && job.ssid[0] && result != PARTIAL &&
        result != SENT)
      result = WIFI_FAILED;
    free(job.bytes);
    queuedBytes.fetch_sub(job.count);
    memset(job.password, 0, sizeof(job.password));
    lastResult = result;
    phase = IDLE;
    releaseWiFiAt = millis() + WIFI_RELEASE_GRACE_MS;
    if (result < 0)
      outcome = result;
    else {
      int expected = 0;
      outcome.compare_exchange_strong(expected, result);
    }
    active = queuedBytes.load() != 0;
  }
}
inline bool start() {
  if (worker)
    return true;
  if (!jobs)
    jobs = xQueueCreate(4, sizeof(Job));
  if (!jobs)
    return false;
  return xTaskCreate(task, "receipt", 6144, nullptr, 1, &worker) == pdPASS;
}
inline bool enqueue(const String &text, const String &ssid,
                    const String &password, bool finish = true) {
  if ((busy() && finish) || !config.host.length() || !start())
    return false;
  String encoded;
  if (!encoded.reserve(std::min<size_t>(MAX_JOB_BYTES, text.length() + 64)))
    return false;
  // Never turn an allocation failure into a successful, truncated receipt.
  // Leave room for the protocol prefix, final feed and optional cutter bytes.
  auto appendByte = [&](char value) {
    return encoded.length() < MAX_JOB_BYTES - 16 && encoded.concat(value);
  };
  unsigned column = 0;
  for (size_t i = 0; i < text.length();) {
    uint32_t c = PrinterText::next(text.c_str(), text.length(), i);
    if (c == '\r')
      continue;
    if (c == '\n') {
      if (!appendByte('\n'))
        return false;
      column = 0;
    } else if (c == '\t') {
      unsigned spaces = 4 - column % 4;
      while (spaces--) {
        if (column >= config.columns) {
          if (!appendByte('\n'))
            return false;
          column = 0;
        }
        if (!appendByte(' '))
          return false;
        ++column;
      }
    } else if (c >= 32 && c != 127) {
      if (column >= config.columns) {
        if (!appendByte('\n'))
          return false;
        column = 0;
      }
      if (!appendByte((char)PrinterText::cp437(c)))
        return false;
      ++column;
    }
    if (encoded.length() + 16 > MAX_JOB_BYTES)
      return false;
  }
  if (finish) {
    if (!encoded.concat("\n\n\n"))
      return false;
  } else if (!encoded.endsWith("\n") && !appendByte('\n')) {
    return false;
  }
  const uint8_t prefix[] = {27, '@', 27, 't', 0, 29, '!', 0};
  Job job = {};
  strlcpy(job.host, config.host.c_str(), sizeof(job.host));
  strlcpy(job.ssid, ssid.c_str(), sizeof(job.ssid));
  strlcpy(job.password, password.c_str(), sizeof(job.password));
  job.port = config.port;
  // GS V 66 0: feed to the cut position and partial-cut supported Epson
  // ESC/POS models. The command is optional for printers without a cutter.
  const uint8_t cut[] = {29, 'V', 66, 0};
  size_t cutBytes = finish && config.cut ? sizeof(cut) : 0;
  job.count = sizeof(prefix) + encoded.length() + cutBytes;
  if (queuedBytes.load() + job.count > MAX_JOB_BYTES)
    return false;
  job.bytes = (uint8_t *)malloc(job.count);
  if (!job.bytes)
    return false;
  memcpy(job.bytes, prefix, sizeof(prefix));
  memcpy(job.bytes + sizeof(prefix), encoded.c_str(), encoded.length());
  if (cutBytes)
    memcpy(job.bytes + sizeof(prefix) + encoded.length(), cut, cutBytes);
  releaseWiFiAt = 0;
  queuedBytes.fetch_add(job.count);
  active = true;
  if (xQueueSend(jobs, &job, 0) != pdTRUE) {
    queuedBytes.fetch_sub(job.count);
    active = queuedBytes.load() != 0;
    free(job.bytes);
    return false;
  }
  return true;
}
} // namespace JournalPrinter
