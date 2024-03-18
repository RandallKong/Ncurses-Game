#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void           parse_arguments(int argc, char *argv[], char **address, char **port_str);
static void           handle_arguments(const char *binary_name, const char *address, const char *port_str, in_port_t *port);
static in_port_t      parse_in_port_t(const char *binary_name, const char *port_str);
_Noreturn static void usage(const char *program_name, int exit_code, const char *message);
static void           convert_address(const char *address, struct sockaddr_storage *addr, socklen_t *addr_len);
static int            socket_create(int domain, int type, int protocol);
static void           get_address_to_server(struct sockaddr_storage *addr, in_port_t port);
static void           socket_close(int sockfd);

static void send_init_message(int sockfd, const struct sockaddr *addr, socklen_t addr_len);
static void handle_input(int sockfd, struct sockaddr *addr, socklen_t addr_len);
static void read_from_keyboard(int sockfd, const struct sockaddr *addr, socklen_t addr_len);

// #define UNKNOWN_OPTION_MESSAGE_LEN 24
#define BUFFER_SIZE 1024
#define BASE_TEN 10

int main(int argc, char *argv[])
{
    char                   *address;
    char                   *port_str;
    in_port_t               port;
    int                     sockfd;
    struct sockaddr_storage addr;
    fd_set                  read_fds;

    socklen_t addr_len = sizeof(addr);

    address  = NULL;
    port_str = NULL;
    parse_arguments(argc, argv, &address, &port_str);
    handle_arguments(argv[0], address, port_str, &port);
    convert_address(address, &addr, &addr_len);
    sockfd = socket_create(addr.ss_family, SOCK_DGRAM, 0);
    get_address_to_server(&addr, port);

    send_init_message(sockfd, (const struct sockaddr *)&addr, addr_len);

    FD_ZERO(&read_fds);
    FD_SET(sockfd, &read_fds);
    FD_SET(STDIN_FILENO, &read_fds);

    while(1)
    {
        fd_set tmp_fds = read_fds;

        // Wait for activity on the socket or stdin
        if(select(sockfd + 1, &tmp_fds, NULL, NULL, NULL) == -1)
        {
            perror("select");
            break;
        }

        // Check for activity on the socket
        if(FD_ISSET(sockfd, &tmp_fds))
        {
            handle_input(sockfd, (struct sockaddr *)&addr, addr_len);
        }

        // Check for activity on stdin
        if(FD_ISSET(STDIN_FILENO, &tmp_fds))
        {
            read_from_keyboard(sockfd, (struct sockaddr *)&addr, addr_len);
        }
    }

    socket_close(sockfd);

    return EXIT_SUCCESS;
}

static void send_init_message(int sockfd, const struct sockaddr *addr, socklen_t addr_len)
{
    const char *init_message = "INIT";
    ssize_t     bytes_sent;

    // Send the "INIT" message over the socket
    bytes_sent = sendto(sockfd, init_message, strlen(init_message), 0, addr, addr_len);

    if(bytes_sent == -1)
    {
        perror("sendto");
        exit(EXIT_FAILURE);
    }

    printf("Sent INIT message\n");
}

static void handle_input(int sockfd, struct sockaddr *addr, socklen_t addr_len)
{
    char    input_buffer[BUFFER_SIZE];
    ssize_t bytes_received;

    // Receive message from the server
    bytes_received = recvfrom(sockfd, input_buffer, sizeof(input_buffer), 0, addr, &addr_len);

    if(bytes_received == -1)
    {
        perror("recvfrom");
        exit(EXIT_FAILURE);
    }
    else if(bytes_received == 0)
    {
        printf("Server closed connection\n");
        exit(EXIT_SUCCESS);
    }
    else
    {
        input_buffer[bytes_received] = '\0';
        printf("Received %zu bytes: \"%s\"\n", (size_t)bytes_received, input_buffer);
    }
}

void read_from_keyboard(int sockfd, const struct sockaddr *addr, socklen_t addr_len)
{
    char    input_buffer[BUFFER_SIZE];
    ssize_t bytes_sent;

    // Read input from the keyboard
    if(fgets(input_buffer, sizeof(input_buffer), stdin) == NULL)
    {
        // Error or EOF
        exit(EXIT_FAILURE);
    }

    // Remove newline character from the input_buffer
    input_buffer[strcspn(input_buffer, "\n")] = '\0';

    // Send the message over the socket
    bytes_sent = sendto(sockfd, input_buffer, strlen(input_buffer), 0, addr, addr_len);

    if(bytes_sent == -1)
    {
        perror("sendto");
        exit(EXIT_FAILURE);
    }

    printf("Sent %zu bytes: \"%s\"\n", (size_t)bytes_sent, input_buffer);
}

static void parse_arguments(int argc, char *argv[], char **address, char **port_str)
{
    // Parse command line arguments
    if(argc != 3)
    {
        fprintf(stderr, "Usage: %s <server_address> <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    *address  = argv[1];
    *port_str = argv[2];
}

static void handle_arguments(const char *binary_name, const char *address, const char *port_str, in_port_t *port)
{
    if(address == NULL)
    {
        usage(binary_name, EXIT_FAILURE, "The address is required.");
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

    fprintf(stderr, "Usage: %s [-h] <address> <port> <message>\n", program_name);
    fputs("Options:\n", stderr);
    fputs("  -h  Display this help message\n", stderr);
    exit(exit_code);
}

static void convert_address(const char *address, struct sockaddr_storage *addr, socklen_t *addr_len)
{
    memset(addr, 0, sizeof(*addr));

    if(inet_pton(AF_INET, address, &(((struct sockaddr_in *)addr)->sin_addr)) == 1)
    {
        addr->ss_family = AF_INET;
        *addr_len       = sizeof(struct sockaddr_in);
    }
    else if(inet_pton(AF_INET6, address, &(((struct sockaddr_in6 *)addr)->sin6_addr)) == 1)
    {
        addr->ss_family = AF_INET6;
        *addr_len       = sizeof(struct sockaddr_in6);
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

static void get_address_to_server(struct sockaddr_storage *addr, in_port_t port)
{
    if(addr->ss_family == AF_INET)
    {
        struct sockaddr_in *ipv4_addr;

        ipv4_addr             = (struct sockaddr_in *)addr;
        ipv4_addr->sin_family = AF_INET;
        ipv4_addr->sin_port   = htons(port);
    }
    else if(addr->ss_family == AF_INET6)
    {
        struct sockaddr_in6 *ipv6_addr;

        ipv6_addr              = (struct sockaddr_in6 *)addr;
        ipv6_addr->sin6_family = AF_INET6;
        ipv6_addr->sin6_port   = htons(port);
    }
}

static void socket_close(int sockfd)
{
    if(close(sockfd) == -1)
    {
        perror("Error closing socket");
        exit(EXIT_FAILURE);
    }
}
