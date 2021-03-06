#if defined(__LIBRETRO__)
#include <retro_miscellaneous.h>
#include <net/net_compat.h>
#else
#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
    #define close closesocket
#else
    #include <netdb.h>
    #include <unistd.h>
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "client.h"
#include "tinycthread.h"

#include <retro_timers.h>

#define QUEUE_SIZE 1048576
#define RECV_SIZE 4096

static int client_enabled = 0;
static int running = 0;
static int sd = 0;
static int bytes_sent = 0;
static int bytes_received = 0;
static char *queue = 0;
static int qsize = 0;
static thrd_t recv_thread;
static mtx_t mutex;

void client_enable(void)
{
    client_enabled = 1;
}

void client_disable(void)
{
    client_enabled = 0;
}

int get_client_enabled(void)
{
   return client_enabled;
}

int client_sendall(int sd, char *data, int length)
{
   int count = 0;
   if (!client_enabled)
      return 0;

   while (count < length)
   {
      int n = send(sd, data + count, length, 0);
      if (n == -1)
         return -1;
      count += n;
      length -= n;
      bytes_sent += n;
   }
   return 0;
}

void client_send(char *data)
{
    if (!client_enabled)
        return;
    if (client_sendall(sd, data, strlen(data)) == -1)
    {
        perror("client_sendall");
        exit(1);
    }
}

void client_version(int version)
{
    char buffer[1024];
    if (!client_enabled)
        return;
    snprintf(buffer, 1024, "V,%d\n", version);
    client_send(buffer);
}

void client_login(const char *username, const char *identity_token)
{
    char buffer[1024];
    if (!client_enabled)
        return;
    snprintf(buffer, 1024, "A,%s,%s\n", username, identity_token);
    client_send(buffer);
}

void client_position(float x, float y, float z, float rx, float ry)
{
    static float px, py, pz, prx, pry = 0;
    char buffer[1024];
    float distance =
        (px - x) * (px - x) +
        (py - y) * (py - y) +
        (pz - z) * (pz - z) +
        (prx - rx) * (prx - rx) +
        (pry - ry) * (pry - ry);
    if (!client_enabled)
        return;
    if (distance < 0.0001)
        return;
    px = x; py = y; pz = z; prx = rx; pry = ry;
    snprintf(buffer, 1024, "P,%.2f,%.2f,%.2f,%.2f,%.2f\n", x, y, z, rx, ry);
    client_send(buffer);
}

void client_chunk(int p, int q, int key)
{
    char buffer[1024];
    if (!client_enabled)
        return;
    snprintf(buffer, 1024, "C,%d,%d,%d\n", p, q, key);
    client_send(buffer);
}

void client_block(int x, int y, int z, int w)
{
    char buffer[1024];
    if (!client_enabled)
        return;
    snprintf(buffer, 1024, "B,%d,%d,%d,%d\n", x, y, z, w);
    client_send(buffer);
}

void client_light(int x, int y, int z, int w)
{
    char buffer[1024];
    if (!client_enabled)
        return;
    snprintf(buffer, 1024, "L,%d,%d,%d,%d\n", x, y, z, w);
    client_send(buffer);
}

void client_sign(int x, int y, int z, int face, const char *text)
{
    char buffer[1024];
    if (!client_enabled)
        return;
    snprintf(buffer, 1024, "S,%d,%d,%d,%d,%s\n", x, y, z, face, text);
    client_send(buffer);
}

void client_talk(const char *text)
{
    char buffer[1024];
    if (!client_enabled)
        return;
    if (strlen(text) == 0)
        return;
    snprintf(buffer, 1024, "T,%s\n", text);
    client_send(buffer);
}

char *client_recv()
{
   char *result = 0;
   char *p      = NULL;
   if (!client_enabled)
      return 0;
   mtx_lock(&mutex);
   p = queue + qsize - 1;

   while (p >= queue && *p != '\n')
      p--;

   if (p >= queue)
   {
      int remaining;
      int length = p - queue + 1;
      result = malloc(sizeof(char) * (length + 1));
      memcpy(result, queue, sizeof(char) * length);
      result[length] = '\0';
      remaining = qsize - length;
      memmove(queue, p + 1, remaining);
      qsize -= length;
      bytes_received += length;
   }
   mtx_unlock(&mutex);
   return result;
}

int recv_worker(void *arg)
{
   char *data = malloc(sizeof(char) * RECV_SIZE);
   while (1)
   {
      int length;
      if ((length = recv(sd, data, RECV_SIZE - 1, 0)) <= 0)
      {
         if (running)
         {
            perror("recv");
            exit(1);
         }
         else
            break;
      }
      data[length] = '\0';

      while (1)
      {
         int done = 0;
         mtx_lock(&mutex);
         if (qsize + length < QUEUE_SIZE)
         {
            memcpy(queue + qsize, data, sizeof(char) * (length + 1));
            qsize += length;
            done = 1;
         }
         mtx_unlock(&mutex);
         if (done)
            break;
         retro_sleep(0);
      }
   }
   free(data);
   return 0;
}

void client_connect(char *hostname, int port)
{
    struct hostent *host;
#if defined(HAVE_IPV6) || defined(ANDROID)
    struct sockaddr_in6 address;
#else
    struct sockaddr_in address;
#endif
    if (!client_enabled)
        return;
    if ((host = gethostbyname(hostname)) == 0)
    {
        perror("gethostbyname");
        exit(1);
    }
    memset(&address, 0, sizeof(address));

#if defined(HAVE_IPV6) || defined(ANDROID)
    address.sin6_family = AF_INET6;
#if 0
    /* TODO/FIXME */
    address.sin6_addr.s6_addr = ((struct in6_addr *)(host->h_addr_list[0]))->s6_addr;
#endif
    address.sin6_port = htons(port);
#else
    address.sin_family  = AF_INET;
    address.sin_addr.s_addr = ((struct in_addr *)(host->h_addr_list[0]))->s_addr;
    address.sin_port = htons(port);
#endif
    if ((sd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("socket");
        exit(1);
    }
    if (connect(sd, (struct sockaddr *)&address, sizeof(address)) == -1)
    {
        perror("connect");
        exit(1);
    }
}

void client_start(void)
{
    if (!client_enabled)
        return;
    running = 1;
    queue = (char *)calloc(QUEUE_SIZE, sizeof(char));
    qsize = 0;
    mtx_init(&mutex, mtx_plain);

    if (thrd_create(&recv_thread, recv_worker, NULL) != thrd_success)
    {
        perror("thrd_create");
        exit(1);
    }
}

void client_stop(void)
{
   if (!client_enabled)
      return;
   running = 0;
   close(sd);

#if 0
   if (thrd_join(recv_thread, NULL) != thrd_success)
   {
      perror("thrd_join");
      exit(1);
   }
   mtx_destroy(&mutex);
#endif
   qsize = 0;
   free(queue);

#if 0
   printf("Bytes Sent: %d, Bytes Received: %d\n",
         bytes_sent, bytes_received);
#endif
}
