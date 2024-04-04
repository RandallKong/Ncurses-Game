#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

static void parse_arguments(int argc, char *argv[], char **ip_address,
                            char **port);
static void handle_arguments(const char *binary_name, const char *ip_address,
                             const char *port_str, in_port_t *port);
static in_port_t parse_in_port_t(const char *binary_name, const char *port_str);
_Noreturn static void usage(const char *program_name, int exit_code,
                            const char *message);
static void convert_address(const char *address, struct sockaddr_storage *addr);
static int socket_create(int domain, int type, int protocol);
static void socket_bind(int sockfd, struct sockaddr_storage *addr,
                        in_port_t port);
static void handle_packet(int client_sockfd,
                          const struct sockaddr_storage *client_addr,
                          const char *buffer, size_t bytes);
static void socket_close(int sockfd);

static void initialize_clients(void);
static int add_client(int sockfd, const struct sockaddr_storage *client_addr);
static int get_client_index(int sockfd,
                            const struct sockaddr_storage *client_addr);
static void remove_client(int index);
static void broadcast(int sockfd, const char *message, int sender_index);

static void setup_signal_handler(void);
static void sigint_handler(int signum);

void get_terminal_dimensions(void);
int handle_position_change(const char *buffer, int sender_index);

void serialize_all_client_positions(char *buffer);

void set_init_position(int sender_index);

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static volatile sig_atomic_t exit_flag = 0;

#define BUFFER_SIZE 1024
#define BASE_TEN 10
#define MAX_USERNAME_LENGTH 20
#define MAX_CLIENTS 32
#define MIN_X 0
#define MIN_Y 0

typedef struct
{
  struct sockaddr_storage addr;
  socklen_t addr_len;
  char username[MAX_USERNAME_LENGTH];
  int x_coord;
  int y_coord;
} ClientInfo;

typedef struct
{
  int height;
  int width;
} WindowDimensions;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
ClientInfo clients[MAX_CLIENTS];

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
WindowDimensions window;

int main(int argc, char *argv[])
{
  char *address;
  char *port_str;
  in_port_t port;
  int sockfd;
  char buffer[BUFFER_SIZE + 1];
  struct sockaddr_storage client_addr;
  socklen_t client_addr_len;
  struct sockaddr_storage addr;

  address = NULL;
  port_str = NULL;

  parse_arguments(argc, argv, &address, &port_str);
  handle_arguments(argv[0], address, port_str, &port);
  convert_address(address, &addr);

  sockfd = socket_create(addr.ss_family, SOCK_DGRAM, 0);
  socket_bind(sockfd, &addr, port);

  setup_signal_handler();

  get_terminal_dimensions();
  printf("width: %d, height: %d\n", window.width, window.height);
  initialize_clients();

  while (!exit_flag)
  {
    ssize_t bytes_received;
    client_addr_len = sizeof(client_addr);
    bytes_received =
        recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0,
                 (struct sockaddr *)&client_addr, &client_addr_len);

    if (bytes_received == -1)
    {
      break;
    }

    buffer[(size_t)bytes_received] = '\0';
    handle_packet(sockfd, &client_addr, buffer, (size_t)bytes_received);
  }

  broadcast(sockfd, "QUIT", -1);

  socket_close(sockfd);

  return EXIT_SUCCESS;
}

void get_terminal_dimensions(void)
{
  struct winsize ws;
  ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
  window.height = ws.ws_row;
  window.width = ws.ws_col;
}

void serialize_all_client_positions(char *buffer)
{
  buffer[0] = '\0';


  for (int i = 0; i < MAX_CLIENTS; i++)
  {
    if (clients[i].addr_len != 0)
    {

      snprintf(buffer + strlen(buffer), BUFFER_SIZE - strlen(buffer),
               "(%s, %d, %d) ", clients[i].username, clients[i].x_coord,
               clients[i].y_coord);
    }
  }
}

// NOLINTNEXTLINE(misc-no-recursion,-warnings-as-errors)
static void broadcast(int sockfd, const char *message, int sender_index)
{
  char message_with_identifier[BUFFER_SIZE];

  snprintf(message_with_identifier, BUFFER_SIZE, "%s", message);

  for (int i = 0; i < MAX_CLIENTS; i++)
  {
    if (clients[i].addr_len != 0)
    {
      ssize_t bytes_sent;

      bytes_sent = sendto(
          sockfd, message_with_identifier, strlen(message_with_identifier), 0,
          (const struct sockaddr *)&clients[i].addr, sizeof(struct sockaddr));

      if (bytes_sent == -1)
      {
        char all_positions[BUFFER_SIZE];
        remove_client(i);
        serialize_all_client_positions(all_positions);

        broadcast(sockfd, all_positions, i);
        perror("sendto");
        return;
      }
    }
  }

  if (sender_index != -1)
  {
    ssize_t confirmation_bytes;

    confirmation_bytes =
        sendto(sockfd, "Server: message confirmation",
               strlen("Server: message confirmation"), 0,
               (const struct sockaddr *)&clients[sender_index].addr,
               sizeof(struct sockaddr));

    if (confirmation_bytes == -1)
    {
      perror("sendto");
      return;
    }
  }
}

static void initialize_clients(void)
{
  for (int i = 0; i < MAX_CLIENTS; i++)
  {
    clients[i].addr_len = 0;
    sprintf(clients[i].username, "client%d", i + 1);
  }
}

static int add_client(int sockfd, const struct sockaddr_storage *client_addr)
{
  const char *no_room_message;
  ssize_t error_bytes;
  char client_confirmation[BUFFER_SIZE];
  char screen_dimentions[BUFFER_SIZE];
  char all_positions[BUFFER_SIZE];

  for (int i = 0; i < MAX_CLIENTS; i++)
  {
    if (clients[i].addr_len == 0)
    {
      ssize_t bytes_sent;
      ssize_t dimention_bytes;

      clients[i].addr = *client_addr;
      clients[i].addr_len = sizeof(struct sockaddr_storage);

      set_init_position(i);

      serialize_all_client_positions(all_positions);

      sprintf(client_confirmation,
              "Server: Successfully joined the game. You're %s",
              clients[i].username);
      sprintf(screen_dimentions, "INIT:%s|%d|%d", clients[i].username,
              window.height, window.width);

      dimention_bytes =
          sendto(sockfd, screen_dimentions, strlen(screen_dimentions), 0,
                 (const struct sockaddr *)client_addr, sizeof(struct sockaddr));
      bytes_sent =
          sendto(sockfd, client_confirmation, strlen(client_confirmation), 0,
                 (const struct sockaddr *)client_addr, sizeof(struct sockaddr));

      broadcast(sockfd, all_positions, i);

      if (bytes_sent == -1 || dimention_bytes == -1)
      {
        perror("sendto");
        return -1;
      }

      return i;
    }
  }

  no_room_message = "Server: No room available for new clients.";
  error_bytes =
      sendto(sockfd, no_room_message, strlen(no_room_message), 0,
             (const struct sockaddr *)client_addr, sizeof(struct sockaddr));
  printf("No available space to add client\n");
  if (error_bytes == -1)
  {
    perror("sendto");
  }

  return -1;
}

static int get_client_index(int sockfd,
                            const struct sockaddr_storage *client_addr)
{
  for (int i = 0; i < MAX_CLIENTS; i++)
  {
    if (clients[i].addr_len != 0 &&
        memcmp(client_addr, &clients[i].addr,
               sizeof(struct sockaddr_storage)) == 0)
    {
      return i;
    }
  }

  return add_client(sockfd, client_addr);
}

static void remove_client(int index)
{

  if (index < 0 || index >= MAX_CLIENTS)
  {
    fprintf(stderr, "Invalid client index\n");
    return;
  }

  printf("Removing client at index %d\n", index);

  clients[index].addr_len = 0;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

static void sigint_handler(int signum) { exit_flag = 1; }

#pragma GCC diagnostic pop

static void setup_signal_handler(void)
{
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
#endif
  sa.sa_handler = sigint_handler;
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;

  if (sigaction(SIGINT, &sa, NULL) == -1)
  {
    perror("sigaction");
    exit(EXIT_FAILURE);
  }
}

void parse_arguments(int argc, char *argv[], char **address, char **port_str)
{
  if (argc < 3)
  {
    fprintf(stderr, "Usage: %s <server_address> <port>\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  *address = argv[1];
  *port_str = argv[2];
}

static void handle_arguments(const char *binary_name, const char *ip_address,
                             const char *port_str, in_port_t *port)
{
  if (ip_address == NULL)
  {
    usage(binary_name, EXIT_FAILURE, "The ip address is required.");
  }

  if (port_str == NULL)
  {
    usage(binary_name, EXIT_FAILURE, "The port is required.");
  }

  *port = parse_in_port_t(binary_name, port_str);
}

in_port_t parse_in_port_t(const char *binary_name, const char *str)
{
  char *endptr;
  uintmax_t parsed_value;

  errno = 0;
  parsed_value = strtoumax(str, &endptr, BASE_TEN);

  if (errno != 0)
  {
    perror("Error parsing in_port_t");
    exit(EXIT_FAILURE);
  }

  if (*endptr != '\0')
  {
    usage(binary_name, EXIT_FAILURE, "Invalid characters in input.");
  }

  if (parsed_value > UINT16_MAX)
  {
    usage(binary_name, EXIT_FAILURE, "in_port_t value out of range.");
  }

  return (in_port_t)parsed_value;
}

_Noreturn static void usage(const char *program_name, int exit_code,
                            const char *message)
{
  if (message)
  {
    fprintf(stderr, "%s\n", message);
  }

  fprintf(stderr, "Usage: %s [-h] <ip address> <port>\n", program_name);
  fputs("Options:\n", stderr);
  fputs("  -h  Display this help message\n", stderr);
  exit(exit_code);
}

static void convert_address(const char *address,
                            struct sockaddr_storage *addr)
{
  memset(addr, 0, sizeof(*addr));

  if (inet_pton(AF_INET, address, &(((struct sockaddr_in *)addr)->sin_addr)) ==
      1)
  {
    addr->ss_family = AF_INET;
  }
  else if (inet_pton(AF_INET6, address,
                       &(((struct sockaddr_in6 *)addr)->sin6_addr)) == 1)
  {
    addr->ss_family = AF_INET6;
  }
  else
  {
    fprintf(stderr, "%s is not an IPv4 or an IPv6 address\n", address);
    exit(EXIT_FAILURE);
  }
}

static int socket_create(int domain, int type, int protocol)
{
  int sockfd;

  sockfd = socket(domain, type, protocol);

  if (sockfd == -1)
  {
    perror("Socket creation failed");
    exit(EXIT_FAILURE);
  }

  return sockfd;
}

static void socket_bind(int sockfd, struct sockaddr_storage *addr,
                        in_port_t port)
{
  char addr_str[INET6_ADDRSTRLEN];
  socklen_t addr_len;
  void *vaddr;
  in_port_t net_port;

  net_port = htons(port);

  if (addr->ss_family == AF_INET)
  {
    struct sockaddr_in *ipv4_addr;

    ipv4_addr = (struct sockaddr_in *)addr;
    addr_len = sizeof(*ipv4_addr);
    ipv4_addr->sin_port = net_port;
    vaddr = (void *)&(((struct sockaddr_in *)addr)->sin_addr);
  }
  else if (addr->ss_family == AF_INET6)
  {
    struct sockaddr_in6 *ipv6_addr;

    ipv6_addr = (struct sockaddr_in6 *)addr;
    addr_len = sizeof(*ipv6_addr);
    ipv6_addr->sin6_port = net_port;
    vaddr = (void *)&(((struct sockaddr_in6 *)addr)->sin6_addr);
  }
  else
  {
    fprintf(stderr,
            "Internal error: addr->ss_family must be AF_INET or AF_INET6, was: "
            "%d\n",
            addr->ss_family);
    exit(EXIT_FAILURE);
  }

  if (inet_ntop(addr->ss_family, vaddr, addr_str, sizeof(addr_str)) == NULL)
  {
    perror("inet_ntop");
    exit(EXIT_FAILURE);
  }

  printf("Binding to: %s:%u\n", addr_str, port);

  if (bind(sockfd, (struct sockaddr *)addr, addr_len) == -1)
  {
    perror("Binding failed");
    fprintf(stderr, "Error code: %d\n", errno);
    exit(EXIT_FAILURE);
  }

  printf("Bound to socket: %s:%u\n", addr_str, port);
}

int handle_position_change(const char *buffer, int sender_index)
{

  int prev_x = clients[sender_index].x_coord;
  int prev_y = clients[sender_index].y_coord;

  if (strcmp(buffer, "Up") == 0)
  {
    if (clients[sender_index].y_coord - 1 > MIN_Y)
    {
      clients[sender_index].y_coord -= 1;
    }
  }
  else if (strcmp(buffer, "Down") == 0)
  {
    if (clients[sender_index].y_coord + 1 < window.height - 1)
    {
      clients[sender_index].y_coord += 1;
    }
  }
  else if (strcmp(buffer, "Left") == 0)
  {
    if (clients[sender_index].x_coord - 1 > MIN_X)
    {
      clients[sender_index].x_coord -= 1;
    }

  } else if (strcmp(buffer, "Right") == 0)
  {
    if (clients[sender_index].x_coord + 1 < window.width - 1)
    {
      clients[sender_index].x_coord += 1;
    }
  }
  else
  {
    return -1;
  }

  if (prev_x != clients[sender_index].x_coord ||
      prev_y != clients[sender_index].y_coord)
  {
    printf("%s: (%d, %d) -> (%d, %d)\n", clients[sender_index].username, prev_x,
           prev_y, clients[sender_index].x_coord,
           clients[sender_index].y_coord);
  }
  else
  {
    return -1;
  }

  return 0;
}

void set_init_position(int sender_index)
{
  unsigned int seed = arc4random_uniform(UINT_MAX);
  srand(seed);

  clients[sender_index].x_coord =
      MIN_X + 1 + rand() % (window.width - MIN_X - 1);
  clients[sender_index].y_coord =
      MIN_Y + 1 + rand() % (window.height - MIN_Y - 1);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

void handle_packet(int sockfd, const struct sockaddr_storage *client_addr,
                   const char *buffer, size_t bytes)
{
  char client_host[NI_MAXHOST];
  char client_port[NI_MAXSERV];
  char all_positions[BUFFER_SIZE];
  int sender_index = 0;

  int ret =
      getnameinfo((const struct sockaddr *)client_addr,
                  sizeof(struct sockaddr_storage), client_host, NI_MAXHOST,
                  client_port, NI_MAXSERV, NI_NUMERICHOST | NI_NUMERICSERV);

  if (ret != 0)
  {
    fprintf(stderr, "getnameinfo: %s\n", gai_strerror(ret));
    return;
  }

  if (strcmp(buffer, "INIT") == 0)
  {
    int client_index;

    printf("Received 'INIT' message from %s:%s. Sending confirmation...\n",
           client_host, client_port);

    client_index = add_client(sockfd, client_addr);
    if (client_index == -1)
    {
      return;
    }

    return;
  }

  if (strcmp(buffer, "QUIT") == 0)
  {

    char positions[BUFFER_SIZE];
    remove_client(get_client_index(sockfd, client_addr));

    serialize_all_client_positions(positions);

    broadcast(sockfd, positions, sender_index);
    return;
  }

  // Get the sender index
  sender_index = get_client_index(sockfd, client_addr);
  if (sender_index == -1)
  {
    return;
  }

  if (handle_position_change(buffer, sender_index) == -1)
  {
    return;
  }

  serialize_all_client_positions(all_positions);

  printf("BROADCASTING: %s\n", all_positions);
  broadcast(sockfd, all_positions, sender_index);
}

#pragma GCC diagnostic pop

static void socket_close(int sockfd)
{
  if (close(sockfd) == -1)
  {
    perror("Error closing socket");
    exit(EXIT_FAILURE);
  }
}
