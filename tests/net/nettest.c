/*
 * NETTEST - TCP/IP on modernDOS (DJGPP + Watt-32, through PKTDRV).
 *
 *   NETTEST host port
 *
 * Configures the network (WATTCP.CFG: DHCP), connects to host:port with
 * a BSD socket, sends "hello from modernDOS\n", reads a line back.
 * Logs to port E9h ("NETTEST: ..."); exit code 0 if the reply arrived.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <pc.h>
#include <tcp.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static void out(const char *fmt, ...)
{
  char buf[300], *p;
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  for (p = buf; *p; p++)
  {
    if (*p == '\n')
      outportb(0xe9, '\r');
    outportb(0xe9, *p);
  }
  fputs(buf, stdout);
}

int main(int argc, char **argv)
{
  struct sockaddr_in sa;
  char ip[20], reply[200];
  int s, n, total = 0;

  if (argc < 3)
  {
    printf("usage: NETTEST host port\n");
    return 2;
  }
  if (sock_init())
  {
    out("NETTEST: network setup failed\n");
    return 1;
  }
  out("NETTEST: my address %s\n", _inet_ntoa(ip, my_ip_addr));
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((unsigned short)atoi(argv[2]));
  sa.sin_addr.s_addr = inet_addr(argv[1]);
  s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0 || connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0)
  {
    out("NETTEST: cannot connect to %s:%s\n", argv[1], argv[2]);
    return 1;
  }
  out("NETTEST: connected to %s:%s\n", argv[1], argv[2]);
  send(s, "hello from modernDOS\n", 21, 0);
  while (total < (int)sizeof(reply) - 1 && (n = recv(s, reply + total, sizeof(reply) - 1 - total, 0)) > 0)
  {
    total += n;
    if (memchr(reply, '\n', total))
      break;
  }
  reply[total] = 0;
  closesocket(s);
  if (total <= 0)
  {
    out("NETTEST: no reply\n");
    return 1;
  }
  reply[strcspn(reply, "\r\n")] = 0;
  out("NETTEST: reply: %s\n", reply);
  return 0;
}
