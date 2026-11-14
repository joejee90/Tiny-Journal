// Exercises the actual printer encoder and checked write path against
// localhost. FreeRTOS scheduling is stubbed; radio, the real printer, and
// worker timing aren't tested.
#include "../JournalPrinter.h"
#include <cassert>
#include <iostream>
int main() {
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(listener >= 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  assert(bind(listener, (sockaddr *)&addr, sizeof(addr)) == 0);
  assert(listen(listener, 1) == 0);
  socklen_t length = sizeof(addr);
  assert(getsockname(listener, (sockaddr *)&addr, &length) == 0);
  std::vector<uint8_t> received;
  std::thread server([&] {
    int fd = accept(listener, nullptr, nullptr);
    assert(fd >= 0);
    uint8_t buf[1024];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
      received.insert(received.end(), buf, buf + n);
    close(fd);
  });
  JournalPrinter::config.host = "127.0.0.1";
  JournalPrinter::config.port = ntohs(addr.sin_port);
  JournalPrinter::config.columns = 16;
  assert(JournalPrinter::enqueue("ABC \xc2\xa3\n\x1b"
                                 "evil",
                                 "testssid", "testpassword"));
  JournalPrinter::Job job;
  memcpy(&job, JournalPrinter::jobs->last.data(), sizeof(job));
  WiFiClient client;
  assert(client.connect(job.host, job.port, 3000));
  assert(JournalPrinter::writeAll(client, job.bytes, job.count));
  client.stop();
  server.join();
  close(listener);
  const uint8_t prefix[] = {27, '@', 27, 't', 0, 29, '!', 0};
  assert(received.size() > sizeof(prefix));
  assert(memcmp(received.data(), prefix, sizeof(prefix)) == 0);
  std::string output(received.begin() + sizeof(prefix), received.end());
  const char cut[] = {29, 'V', 66, 0};
  assert(output == std::string("ABC ") + char(156) + "\nevil\n\n\n" +
                       std::string(cut, sizeof(cut)));
  assert(JournalPrinter::lastBytesSent.load() == received.size());
  free(job.bytes);
  JournalPrinter::queuedBytes = 0;
  JournalPrinter::active = false;
  assert(!JournalPrinter::enqueue(String(40000, 'x'), "s", "p"));
  JournalPrinter::config.host = "";
  assert(!JournalPrinter::enqueue("x", "s", "p"));
  // Completion grace never releases the radio while another job is queued.
  JournalPrinter::releaseWiFiAt = 100;
  assert(!JournalPrinter::releaseWiFiDue(99));
  JournalPrinter::queuedBytes = 1;
  assert(!JournalPrinter::releaseWiFiDue(200));
  JournalPrinter::queuedBytes = 0;
  JournalPrinter::config.live = true;
  assert(!JournalPrinter::releaseWiFiDue(200));
  JournalPrinter::config.live = false;
  assert(JournalPrinter::releaseWiFiDue(200));
  assert(!JournalPrinter::releaseWiFiDue(201));
  // Live lines never feed/cut, and printer-without-cutter configuration is
  // honoured.
  JournalPrinter::config.host = "127.0.0.1";
  assert(JournalPrinter::enqueue("live line", "s", "p", false));
  memcpy(&job, JournalPrinter::jobs->last.data(), sizeof(job));
  std::string live((const char *)job.bytes, job.count);
  assert(live.substr(sizeof(prefix)) == "live line\n");
  free(job.bytes);
  JournalPrinter::queuedBytes = 0;
  JournalPrinter::active = false;
  JournalPrinter::config.cut = false;
  assert(JournalPrinter::enqueue("no cut", "s", "p", true));
  memcpy(&job, JournalPrinter::jobs->last.data(), sizeof(job));
  std::string noCut((const char *)job.bytes, job.count);
  assert(noCut.substr(sizeof(prefix)) == "no cut\n\n\n");
  free(job.bytes);
  JournalPrinter::queuedBytes = 0;
  JournalPrinter::active = false;
  // Exercise the real enqueue path: no failure may publish a shortened job.
  const auto previousQueueItem = JournalPrinter::jobs->last;
  String::failNextReserve = true;
  assert(!JournalPrinter::enqueue("reserve fails", "s", "p"));
  assert(JournalPrinter::queuedBytes == 0);
  assert(JournalPrinter::jobs->last == previousQueueItem);
  const char *allocationCases[] = {"a", "a\tb", "12345678901234567"};
  const int appendCounts[] = {2, 6,
                              19}; // Includes final three-line feed append.
  for (size_t example = 0; example < 3; ++example) {
    for (int accepted = 0; accepted < appendCounts[example]; ++accepted) {
      String::concatBudget = accepted;
      assert(!JournalPrinter::enqueue(allocationCases[example], "s", "p"));
      assert(JournalPrinter::queuedBytes == 0);
      assert(!JournalPrinter::busy());
      assert(JournalPrinter::jobs->last == previousQueueItem);
    }
  }
  // Live-line trailing LF is also checked, not just receipt suffixes.
  String::concatBudget = 1;
  assert(!JournalPrinter::enqueue("a", "s", "p", false));
  assert(JournalPrinter::jobs->last == previousQueueItem);
  String::concatBudget = -1;
  delete JournalPrinter::jobs;
  std::cout << "Printer actual encoding + local TCP send passed; ESC injection "
               "stripped, CP437 pound preserved; receipt cut/live no-cut and "
               "deferred WiFi release and allocation/append failure rejection "
               "passed.\n";
}
