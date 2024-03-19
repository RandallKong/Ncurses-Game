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
#include <sys/socket.h>
#include <unistd.h>
// #include <stdbool.h>

static void           parse_arguments(int argc, char *argv[], char **ip_address, char **port);
static void           handle_arguments(const char *binary_name, const char *ip_address, const char *port_str, in_port_t *port);
static in_port_t      parse_in_port_t(const char *binary_name, const char *port_str);
_Noreturn static void usage(const char *program_name, int exit_code, const char *message);
static void           convert_address(const char *address, struct sockaddr_storage *addr);
static int            socket_create(int domain, int type, int protocol);
static void           socket_bind(int sockfd, struct sockaddr_storage *addr, in_port_t port);
static void           handle_packet(int client_sockfd, const struct sockaddr_storage *client_addr, const char *buffer, size_t bytes);
static void           socket_close(int sockfd);

static void initialize_clients(void);
static int  add_client(int sockfd, const struct sockaddr_storage *client_addr);
static int  get_client_index(int sockfd, const struct sockaddr_storage *client_addr);
static void remove_client(int index);
static void broadcast(int sockfd, const char *message, int sender_index);

static void setup_signal_handler(void);
static void sigint_handler(int signum);

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static volatile sig_atomic_t exit_flag = 0;

// #define UNKNOWN_OPTION_MESSAGE_LEN 24
#define BUFFER_SIZE 1024
#define BASE_TEN 10
#define MAX_USERNAME_LENGTH 20
#define MAX_CLIENTS 32

// Struct to store client information
typedef struct
{
    struct sockaddr_storage addr;                             // Client address information
    socklen_t               addr_len;                         // Length of the client address
    char                    username[MAX_USERNAME_LENGTH];    // Username of the client
} ClientInfo;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
ClientInfo clients[MAX_CLIENTS];

int main(int argc, char *argv[])
{
    char                   *address;
    char                   *port_str;
    in_port_t               port;
    int                     sockfd;
    char                    buffer[BUFFER_SIZE + 1];
    struct sockaddr_storage client_addr;
    socklen_t               client_addr_len;
    struct sockaddr_storage addr;

    address  = NULL;
    port_str = NULL;
    parse_arguments(argc, argv, &address, &port_str);
    handle_arguments(argv[0], address, port_str, &port);
    convert_address(address, &addr);
    sockfd = socket_create(addr.ss_family, SOCK_DGRAM, 0);
    socket_bind(sockfd, &addr, port);

    setup_signal_handler();

    initialize_clients();

    while(!exit_flag)
    {    // Loop indefinitely to receive messages continuously
        ssize_t bytes_received;
        client_addr_len = sizeof(client_addr);
        bytes_received  = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *)&client_addr, &client_addr_len);

        if(bytes_received == -1)
        {
            //            perror("recvfrom");
            break;    // Skip processing this message and continue to next iteration
        }

        buffer[(size_t)bytes_received] = '\0';
        handle_packet(sockfd, &client_addr, buffer, (size_t)bytes_received);
    }

    broadcast(sockfd, "QUIT", -1);

    socket_close(sockfd);

    return EXIT_SUCCESS;
}

static void broadcast(int sockfd, const char *message, int sender_index)
{
    char message_with_identifier[BUFFER_SIZE];

    if(sender_index != -1)
    {
        // Prepend the sender's username to the message
        snprintf(message_with_identifier, BUFFER_SIZE, "%s: %s", clients[sender_index].username, message);
    }
    else
    {
        snprintf(message_with_identifier, BUFFER_SIZE, "%s", message);
    }

    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if(i != sender_index && clients[i].addr_len != 0)
        {
            ssize_t bytes_sent;

            bytes_sent = sendto(sockfd, message_with_identifier, strlen(message_with_identifier), 0, (const struct sockaddr *)&clients[i].addr, sizeof(struct sockaddr));

            if(bytes_sent == -1)
            {
                remove_client(i);    //  TODO: THIS DOESENT WORK.
                perror("sendto");
                return;
            }
        }
    }

    if(sender_index != -1)
    {
        ssize_t confirmation_bytes;

        confirmation_bytes = sendto(sockfd, "Server: message confirmation", strlen("Server: message confirmation"), 0, (const struct sockaddr *)&clients[sender_index].addr, sizeof(struct sockaddr));

        if(confirmation_bytes == -1)
        {
            perror("sendto");
            return;
        }
    }
}

static void initialize_clients(void)
{
    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        clients[i].addr_len = 0;
        sprintf(clients[i].username, "client%d", i + 1);
    }
}

static int add_client(int sockfd, const struct sockaddr_storage *client_addr)
{
    // Find an empty slot in the clients array
    const char *no_room_message;
    ssize_t     error_bytes;
    char        client_confirmation[BUFFER_SIZE];

    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        //        printf("%d: %d\n", i, clients[i].addr_len);
        if(clients[i].addr_len == 0)
        {
            ssize_t bytes_sent;
            // Found an empty slot, add the client
            clients[i].addr     = *client_addr;
            clients[i].addr_len = sizeof(struct sockaddr_storage);

            sprintf(client_confirmation, "Server: Successfully joined the game. You're %s", clients[i].username);

            bytes_sent = sendto(sockfd, client_confirmation, strlen(client_confirmation), 0, (const struct sockaddr *)client_addr, sizeof(struct sockaddr));

            if(bytes_sent == -1)
            {
                perror("sendto");
                return -1;
            }

            return i;    // Return the index of the added client
        }
    }
    // If no empty slot is found, send a message to the client and return -1 indicating failure
    no_room_message = "Server: No room available for new clients.";
    error_bytes     = sendto(sockfd, no_room_message, strlen(no_room_message), 0, (const struct sockaddr *)client_addr, sizeof(struct sockaddr));
    printf("No available space to add client\n");
    if(error_bytes == -1)
    {
        perror("sendto");
    }

    return -1;
}

static int get_client_index(int sockfd, const struct sockaddr_storage *client_addr)
{
    // Search for the client in the clients array
    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if(clients[i].addr_len != 0 && memcmp(client_addr, &clients[i].addr, sizeof(struct sockaddr_storage)) == 0)
        {
            // Client found, return their index
            return i;
        }
    }
    // Client not found, add them to the clients array
    return add_client(sockfd, client_addr);
}

static void remove_client(int index)
{
    if(index < 0 || index >= MAX_CLIENTS)
    {
        fprintf(stderr, "Invalid client index\n");
        return;
    }

    printf("Removing client at index %d\n", index);

    // Reset the client's address length to 0
    clients[index].addr_len = 0;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

static void sigint_handler(int signum)
{
    exit_flag = 1;
}

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

    if(sigaction(SIGINT, &sa, NULL) == -1)
    {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
}

void parse_arguments(int argc, char *argv[], char **address, char **port_str)
{
    // Parse command line arguments
    if(argc < 3)
    {
        fprintf(stderr, "Usage: %s <server_address> <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    *address  = argv[1];
    *port_str = argv[2];
}

static void handle_arguments(const char *binary_name, const char *ip_address, const char *port_str, in_port_t *port)
{
    if(ip_address == NULL)
    {
        usage(binary_name, EXIT_FAILURE, "The ip address is required.");
    }

    if(port_str == NULL)
    {
        usage(binary_name, EXIT_FAILURE, "The port is required.");
    }

    *port = parse_in_port_t(binary_name, port_str);
}

in_port_t parse_in_port_t(const char *binary_name, const char *str)
{
    char     *endptr;
    uintmax_t parsed_value;

    errno        = 0;
    parsed_value = strtoumax(str, &endptr, BASE_TEN);

    if(errno != 0)
    {
        perror("Error parsing in_port_t");
        exit(EXIT_FAILURE);
    }

    // Check if there are any non-numeric characters in the input string
    if(*endptr != '\0')
    {
        usage(binary_name, EXIT_FAILURE, "Invalid characters in input.");
    }

    // Check if the parsed value is within the valid range for in_port_t
    if(parsed_value > UINT16_MAX)
    {
        usage(binary_name, EXIT_FAILURE, "in_port_t value out of range.");
    }

    return (in_port_t)parsed_value;
}

_Noreturn static void usage(const char *program_name, int exit_code, const char *message)
{
    if(message)
    {
        fprintf(stderr, "%s\n", message);
    }

    fprintf(stderr, "Usage: %s [-h] <ip address> <port>\n", program_name);
    fputs("Options:\n", stderr);
    fputs("  -h  Display this help message\n", stderr);
    exit(exit_code);
}

static void convert_address(const char *address, struct sockaddr_storage *addr)
{
    memset(addr, 0, sizeof(*addr));

    if(inet_pton(AF_INET, address, &(((struct sockaddr_in *)addr)->sin_addr)) == 1)
    {
        addr->ss_family = AF_INET;
    }
    else if(inet_pton(AF_INET6, address, &(((struct sockaddr_in6 *)addr)->sin6_addr)) == 1)
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

    if(sockfd == -1)
    {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    return sockfd;
}

static void socket_bind(int sockfd, struct sockaddr_storage *addr, in_port_t port)
{
    char      addr_str[INET6_ADDRSTRLEN];
    socklen_t addr_len;
    void     *vaddr;
    in_port_t net_port;

    net_port = htons(port);

    if(addr->ss_family == AF_INET)
    {
        struct sockaddr_in *ipv4_addr;

        ipv4_addr           = (struct sockaddr_in *)addr;
        addr_len            = sizeof(*ipv4_addr);
        ipv4_addr->sin_port = net_port;
        vaddr               = (void *)&(((struct sockaddr_in *)addr)->sin_addr);
    }
    else if(addr->ss_family == AF_INET6)
    {
        struct sockaddr_in6 *ipv6_addr;

        ipv6_addr            = (struct sockaddr_in6 *)addr;
        addr_len             = sizeof(*ipv6_addr);
        ipv6_addr->sin6_port = net_port;
        vaddr                = (void *)&(((struct sockaddr_in6 *)addr)->sin6_addr);
    }
    else
    {
        fprintf(stderr, "Internal error: addr->ss_family must be AF_INET or AF_INET6, was: %d\n", addr->ss_family);
        exit(EXIT_FAILURE);
    }

    if(inet_ntop(addr->ss_family, vaddr, addr_str, sizeof(addr_str)) == NULL)
    {
        perror("inet_ntop");
        exit(EXIT_FAILURE);
    }

    printf("Binding to: %s:%u\n", addr_str, port);

    if(bind(sockfd, (struct sockaddr *)addr, addr_len) == -1)
    {
        perror("Binding failed");
        fprintf(stderr, "Error code: %d\n", errno);
        exit(EXIT_FAILURE);
    }

    printf("Bound to socket: %s:%u\n", addr_str, port);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

void handle_packet(int sockfd, const struct sockaddr_storage *client_addr, const char *buffer, size_t bytes)
{
    char client_host[NI_MAXHOST];    // Buffer to store client hostname
    char client_port[NI_MAXSERV];    // Buffer to store client port
    int  sender_index;

    // Get the human-readable representation of client address and port
    int ret = getnameinfo((const struct sockaddr *)client_addr, sizeof(struct sockaddr_storage), client_host, NI_MAXHOST, client_port, NI_MAXSERV, NI_NUMERICHOST | NI_NUMERICSERV);

    if(ret != 0)
    {
        fprintf(stderr, "getnameinfo: %s\n", gai_strerror(ret));
        return;
    }

    // Print client details
    // printf("Message: %s\n", buffer);

    // Check if the received message is "INIT"
    if(strcmp(buffer, "INIT") == 0)
    {
        int client_index;

        printf("Received 'INIT' message from %s:%s. Sending confirmation...\n", client_host, client_port);

        client_index = add_client(sockfd, client_addr);
        if(client_index == -1)
        {
            return;
        }

        return;
    }

    // Check if the received message is "QUIT"
    if(strcmp(buffer, "QUIT") == 0)
    {
        remove_client(get_client_index(sockfd, client_addr));
        return;
    }

    // Get the sender index
    sender_index = get_client_index(sockfd, client_addr);
    if(sender_index == -1)
    {
        return;
    }

    printf("Message: %s\n", buffer);

    // Broadcast the message to all clients except the sender
    broadcast(sockfd, buffer, sender_index);
}

#pragma GCC diagnostic pop

static void socket_close(int sockfd)
{
    if(close(sockfd) == -1)
    {
        perror("Error closing socket");
        exit(EXIT_FAILURE);
    }
}
