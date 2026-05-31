#define _POSIX_C_SOURCE 200112L
#include "slsk.h"
#include "net.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <zlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>

static void print_usage(void) {
    fprintf(stderr, "soulc - minimalist soulseek client\n"
                    "Usage:\n"
                    "  soulc search <query>\n"
                    "  soulc get <username> <filepath> <size_in_bytes>\n");
    exit(EXIT_FAILURE);
}

static int read_string(const uint8_t *p, uint32_t len, uint32_t *off, char **out) {
    uint32_t n;

    if (len - *off < 4) return -1;
    n = get_u32_le(p + *off);
    *off += 4;
    if (n > len - *off) return -1;
    *out = malloc(n + 1);
    if (!*out) return -1;
    memcpy(*out, p + *off, n);
    (*out)[n] = '\0';
    *off += n;
    return 0;
}

static int skip_string(const uint8_t *p, uint32_t len, uint32_t *off) {
    uint32_t n;

    if (len - *off < 4) return -1;
    n = get_u32_le(p + *off);
    *off += 4;
    if (n > len - *off) return -1;
    *off += n;
    return 0;
}

static int inflate_msg(const uint8_t *in, uint32_t inlen, uint8_t **out, uint32_t *outlen) {
    unsigned long cap, n;
    uint8_t *buf;

    for (cap = 65536; cap <= 16 * 1024 * 1024; cap *= 2) {
        buf = malloc(cap);
        if (!buf) return -1;
        n = cap;
        if (uncompress(buf, &n, in, inlen) == Z_OK) {
            *out = buf;
            *outlen = (uint32_t)n;
            return 0;
        }
        free(buf);
    }
    return -1;
}

static void parse_result_list(const uint8_t *payload, uint32_t len, const char *user,
                              uint32_t *offset, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        char *filename = NULL;

        if (len - *offset < 1) break;
        (*offset)++;
        if (read_string(payload, len, offset, &filename) < 0) break;
        if (len - *offset < 8) { free(filename); break; }

        uint32_t size_low = get_u32_le(payload + *offset);
        uint32_t size_high = get_u32_le(payload + *offset + 4);
        uint64_t size = ((uint64_t)size_high << 32) | size_low;
        *offset += 8;

        if (skip_string(payload, len, offset) < 0) { free(filename); break; }
        if (len - *offset < 4) { free(filename); break; }
        uint32_t attr_count = get_u32_le(payload + *offset);
        *offset += 4;
        if ((uint64_t)attr_count * 8 > len - *offset) { free(filename); break; }
        *offset += attr_count * 8;

        printf("%s\t%" PRIu64 "\t%s\n", user, size, filename);
        free(filename);
    }
}

static void parse_search_reply(const uint8_t *payload, uint32_t len) {
    char *user = NULL;
    uint32_t offset = 0, result_count, private_count;

    if (read_string(payload, len, &offset, &user) < 0) return;
    if (len - offset < 8) { free(user); return; }
    offset += 4;
    result_count = get_u32_le(payload + offset);
    offset += 4;

    if (getenv("SLSK_DEBUG")) {
        fprintf(stderr, "[DEBUG] Parsed search reply from user '%s', result count: %u\n", user, result_count);
    }
    parse_result_list(payload, len, user, &offset, result_count);

    if (len - offset >= 13) {
        offset += 13;
        if (len - offset >= 4) {
            private_count = get_u32_le(payload + offset);
            offset += 4;
            if (getenv("SLSK_DEBUG")) {
                fprintf(stderr, "[DEBUG] Parsed private search reply from user '%s', result count: %u\n", user, private_count);
            }
            parse_result_list(payload, len, user, &offset, private_count);
        }
    }
    free(user);
}

static int handle_connect_to_peer(const uint8_t *payload, uint32_t len) {
    uint32_t offset = 0, ip, port, token;
    char *user = NULL, *type = NULL;
    char host[32], sport[16];
    int peer_fd = -1;

    if (read_string(payload, len, &offset, &user) < 0) return -1;
    if (read_string(payload, len, &offset, &type) < 0) { free(user); return -1; }
    if (len - offset < 12) { free(user); free(type); return -1; }

    ip = get_u32_le(payload + offset); offset += 4;
    port = get_u32_le(payload + offset); offset += 4;
    token = get_u32_le(payload + offset);

    snprintf(host, sizeof(host), "%u.%u.%u.%u",
             ip & 0xff, (ip >> 8) & 0xff, (ip >> 16) & 0xff, (ip >> 24) & 0xff);
    snprintf(sport, sizeof(sport), "%u", port);

    peer_fd = net_connect(host, sport);
    if (peer_fd >= 0 && slsk_send_pierce_fw(peer_fd, token) < 0) {
        if (getenv("SLSK_DEBUG")) {
            fprintf(stderr, "[DEBUG] Failed to send pierce firewall message to %s:%s\n", host, sport);
        }
        net_close(peer_fd);
        peer_fd = -1;
    }

    free(user);
    free(type);
    return peer_fd;
}

static void handle_peer_msg(int peer_fd) {
    uint8_t hdr[4], *msg, *plain = NULL;
    uint32_t msg_len, code, plain_len;

    if (net_read_exact(peer_fd, hdr, 4) < 0) return;
    msg_len = get_u32_le(hdr);
    if (msg_len == 0 || msg_len > 16 * 1024 * 1024) return;

    msg = malloc(msg_len);
    if (!msg) return;
    if (net_read_exact(peer_fd, msg, msg_len) < 0) { free(msg); return; }

    if (msg[0] == PEER_INIT_PEER_INIT || msg[0] == PEER_INIT_PIERCE_FW) {
        free(msg);
        return;
    }
    if (msg_len < 4) { free(msg); return; }

    code = get_u32_le(msg);
    if (getenv("SLSK_DEBUG")) {
        fprintf(stderr, "[DEBUG] Peer msg: code=%u, len=%u\n", code, msg_len - 4);
    }
    if (code == PEER_MSG_FILE_SEARCH_RESPONSE) {
        if (inflate_msg(msg + 4, msg_len - 4, &plain, &plain_len) == 0) {
            parse_search_reply(plain, plain_len);
            free(plain);
        } else {
            parse_search_reply(msg + 4, msg_len - 4);
        }
    }
    free(msg);
}

static int search_timeout(void) {
    char *end;
    const char *env = getenv("SLSK_SEARCH_SECONDS");
    long n;

    if (!env || !*env) return 10;
    n = strtol(env, &end, 10);
    if (*end != '\0' || n < 1 || n > 600) return 10;
    return (int)n;
}

static void do_search(int fd, int listen_fd, const char *query) {
    uint32_t ticket = (uint32_t)time(NULL);
    struct pollfd pfds[64];
    int pfd_count = 1;
    int timeout = search_timeout();
    time_t start;

    if (slsk_send_search(fd, query, ticket) < 0) {
        fprintf(stderr, "Failed to send search\n");
        return;
    }
    if (getenv("SLSK_DEBUG")) {
        fprintf(stderr, "[DEBUG] Sent search query for '%s' (ticket: %u). Waiting up to %d seconds for responses...\n", query, ticket, timeout);
    }

    pfds[0].fd = fd;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;

    if (listen_fd >= 0) {
        pfds[pfd_count].fd = listen_fd;
        pfds[pfd_count].events = POLLIN;
        pfds[pfd_count].revents = 0;
        pfd_count++;
    }

    start = time(NULL);
    while (time(NULL) - start < timeout) {
        int first_peer = listen_fd >= 0 ? 2 : 1;
        int ret = poll(pfds, pfd_count, 1000);

        if (ret < 0) break;
        if (ret == 0) continue;

        if (pfds[0].revents & POLLIN) {
            uint32_t msg_code, payload_len = 0;
            uint8_t *payload = NULL;

            if (slsk_process_server_msg(fd, &msg_code, &payload, &payload_len) < 0) {
                fprintf(stderr, "Connection to server lost during search.\n");
                break;
            }

            if (getenv("SLSK_DEBUG") && msg_code != 0) {
                fprintf(stderr, "[DEBUG] search poll server msg: code=%u, len=%u\n", msg_code, payload_len);
            }

            if (msg_code == SLSK_MSG_CONNECT_TO_PEER && payload) {
                if (getenv("SLSK_DEBUG")) {
                    fprintf(stderr, "[DEBUG] Got SLSK_MSG_CONNECT_TO_PEER, attempting connection...\n");
                }
                int peer_fd = handle_connect_to_peer(payload, payload_len);
                if (peer_fd >= 0) {
                    if (pfd_count < 64) {
                        if (getenv("SLSK_DEBUG")) {
                            fprintf(stderr, "[DEBUG] Accepted ConnectToPeer request (fd: %d), total active poll fds: %d\n", peer_fd, pfd_count + 1);
                        }
                        pfds[pfd_count].fd = peer_fd;
                        pfds[pfd_count].events = POLLIN;
                        pfds[pfd_count].revents = 0;
                        pfd_count++;
                        if (getenv("SLSK_DEBUG")) {
                            fprintf(stderr, "[DEBUG] Accepted ConnectToPeer request, total active poll fds: %d\n", pfd_count);
                        }
                    } else {
                        net_close(peer_fd);
                        if (getenv("SLSK_DEBUG")) {
                            fprintf(stderr, "[DEBUG] Max connections reached. Closing new peer connection.\n");
                        }
                    }
                }
            }
            free(payload);
        }

        if (listen_fd >= 0 && (pfds[1].revents & POLLIN)) {
            int new_fd = net_accept(listen_fd);
            if (new_fd >= 0) {
                if (pfd_count < 64) {
                    pfds[pfd_count].fd = new_fd;
                    pfds[pfd_count].events = POLLIN;
                    pfds[pfd_count].revents = 0;
                    pfd_count++;
                } else {
                    net_close(new_fd);
                }
            }
        }

        for (int i = first_peer; i < pfd_count; i++) {
            if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                net_close(pfds[i].fd);
                pfds[i].fd = -1;
            } else if (pfds[i].revents & POLLIN) {
                handle_peer_msg(pfds[i].fd);
            }
        }

        int k = first_peer;
        for (int i = first_peer; i < pfd_count; i++)
            if (pfds[i].fd != -1) pfds[k++] = pfds[i];
        pfd_count = k;
    }

    for (int i = (listen_fd >= 0 ? 2 : 1); i < pfd_count; i++)
        if (pfds[i].fd >= 0) net_close(pfds[i].fd);
}

static void do_get(int fd_server, const char *username, const char *filepath, uint64_t file_size) {
    if (slsk_send_get_peer_addr(fd_server, username) < 0) {
        fprintf(stderr, "Failed to send GetPeerAddress\n");
        return;
    }

    char peer_ip[64] = {0};
    uint32_t peer_port = 0;
    int found = 0;

    time_t start = time(NULL);
    while (time(NULL) - start < 5) {
        if (net_wait(fd_server, 1000) > 0) {
            uint32_t msg_code;
            uint8_t *payload = NULL;
            uint32_t payload_len = 0;

            if (slsk_process_server_msg(fd_server, &msg_code, &payload, &payload_len) == 0) {
                if (getenv("SLSK_DEBUG") && msg_code != 0) {
                    fprintf(stderr, "[DEBUG] Server msg: code=%u, len=%u\n", msg_code, payload_len);
                }
                if (msg_code == SLSK_MSG_GET_PEER_ADDR && payload != NULL && payload_len >= 4) {
                    uint32_t offset = 0;
                    uint32_t user_len = get_u32_le(payload);
                    offset += 4 + user_len;

                    if (offset + 4 <= payload_len) {
                        uint32_t ip = get_u32_le(payload + offset);
                        offset += 4;
                        if (offset + 4 <= payload_len) {
                            peer_port = get_u32_le(payload + offset);
                            sprintf(peer_ip, "%d.%d.%d.%d",
                                    ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
                            found = 1;
                        }
                    }
                }
                if (payload) free(payload);
                if (found) break;
            } else {
                fprintf(stderr, "Connection to server lost while resolving peer.\n");
                break;
            }
        }
    }

    if (!found) {
        fprintf(stderr, "Could not find peer %s (offline or firewalled)\n", username);
        return;
    }

    char port_str[16];
    sprintf(port_str, "%u", peer_port);
    int fd_peer = net_connect(peer_ip, port_str);
    if (fd_peer < 0) {
        fprintf(stderr, "Failed to connect to peer\n");
        return;
    }

    // Simplistic P2P download phase (not fully robust for real network but follows standard)
    const char *my_username = getenv("SLSK_USER");
    slsk_send_peer_init(fd_peer, my_username, "P", 0);
    slsk_send_transfer_request(fd_peer, filepath, file_size);

    // Wait for TransferReply message (or other peer messages)
    int transfer_reply_received = 0;

    // First read standard peer message header (length + code)
    uint8_t msg_hdr[8];
    while (!transfer_reply_received) {
        if (net_read_exact(fd_peer, msg_hdr, 8) < 0) {
            fprintf(stderr, "Connection to peer lost while waiting for transfer reply.\n");
            net_close(fd_peer);
            return;
        }

        uint32_t msg_len = get_u32_le(msg_hdr);
        uint32_t msg_code = get_u32_le(msg_hdr + 4);

        if (msg_len < 4) continue;
        uint32_t payload_len = msg_len - 4;

        uint8_t *payload = NULL;
        if (payload_len > 0) {
            payload = malloc(payload_len);
            if (!payload) {
                fprintf(stderr, "Memory allocation error\n");
                net_close(fd_peer);
                return;
            }

            if (net_read_exact(fd_peer, payload, payload_len) < 0) {
                free(payload);
                net_close(fd_peer);
                return;
            }
        }

        if (msg_code == PEER_MSG_TRANSFER_REP) {
            transfer_reply_received = 1;
            // payload contains: ticket(4) + allowed(4) + reason(str) ...
            if (payload_len >= 8) {
                uint32_t allowed = get_u32_le(payload + 4);
                if (allowed == 0) {
                    fprintf(stderr, "Transfer rejected by peer (possibly queued).\n");
                    free(payload);
                    net_close(fd_peer);
                    return;
                }
            }
        }
        free(payload);
    }

    // After a successful TransferReply, the peer immediately sends the file data.
    const char *base_name = strrchr(filepath, '\\');
    if (!base_name) base_name = strrchr(filepath, '/');
    if (!base_name) base_name = filepath;
    else base_name++;

    FILE *f = fopen(base_name, "wb");
    if (f) {

        char buf[4096];
        uint64_t total_read = 0;
        while (total_read < file_size) {
            size_t to_read = sizeof(buf);
            if (file_size - total_read < to_read) {
                to_read = file_size - total_read;
            }
            int n = recv(fd_peer, buf, to_read, 0);
            if (n <= 0) {
                fprintf(stderr, "Connection closed prematurely (read %" PRIu64 " of %" PRIu64 " bytes)\n", total_read, file_size);
                break;
            }
            fwrite(buf, 1, n, f);
            total_read += n;
        }
        fclose(f);
    } else {
        fprintf(stderr, "Failed to open local file for writing\n");
    }

    net_close(fd_peer);
}

int main(int argc, char **argv) {
    if (argc < 2) print_usage();

    const char *user = getenv("SLSK_USER");
    const char *pass = getenv("SLSK_PASS");
    if (!user || !pass) {
        fprintf(stderr, "Error: SLSK_USER and SLSK_PASS environment variables must be set.\n");
        return 1;
    }

    const char *server = getenv("SLSK_SERVER");
    if (!server) server = "server.slsknet.org";
    const char *port = getenv("SLSK_PORT");
    if (!port) port = "2242";

    const char *listen_port_str = getenv("SLSK_LISTEN_PORT");
    int listen_fd = -1;
    int listen_port = 0;
    if (listen_port_str) {
        listen_fd = net_listen(listen_port_str);
        if (listen_fd >= 0) listen_port = atoi(listen_port_str);
    }

    int fd = net_connect(server, port);
    if (fd < 0) {
        fprintf(stderr, "Failed to connect to server %s:%s\n", server, port);
        return 1;
    }

    if (slsk_send_login(fd, user, pass) < 0) {
        fprintf(stderr, "Failed to send login request\n");
        net_close(fd);
        return 1;
    }

    time_t login_start = time(NULL);
    int logged_in = 0;
    while (!logged_in && time(NULL) - login_start < 10) {
        if (net_wait(fd, 1000) <= 0) continue;

        uint32_t msg_code;
        uint8_t *payload = NULL;
        uint32_t payload_len = 0;

        if (slsk_process_server_msg(fd, &msg_code, &payload, &payload_len) < 0) {
            fprintf(stderr, "Connection to server lost during login.\n");
            net_close(fd);
            return 1;
        }
        if (getenv("SLSK_DEBUG") && msg_code != 0) {
            fprintf(stderr, "[DEBUG] Server msg: code=%u, len=%u\n", msg_code, payload_len);
        }
        if (msg_code == SLSK_MSG_LOGIN && payload != NULL && payload_len >= 1) {
            if (payload[0] != 1) {
                fprintf(stderr, "Login failed. Check credentials.\n");
                uint32_t offset = 1; // skip success boolean
                if (payload_len >= offset + 4) {
                    uint32_t reason_len = get_u32_le(payload + offset);
                    offset += 4;
                    if (payload_len >= offset + reason_len) {
                        fprintf(stderr, "Reason: %.*s\n", reason_len, payload + offset);
                    }
                }
                free(payload);
                net_close(fd);
                return 1;
            }
            logged_in = 1;
        }
        if (payload) free(payload);
    }
    if (!logged_in) { fprintf(stderr, "Login timeout.\n"); net_close(fd); return 1; }
    slsk_send_listen_port(fd, listen_port > 0 ? listen_port : 0);
    if (strcmp(argv[1], "search") == 0 && argc == 3) {
        do_search(fd, listen_fd, argv[2]);
    } else if (strcmp(argv[1], "get") == 0 && argc == 5) {
        char *endptr;
        uint64_t file_size = strtoull(argv[4], &endptr, 10);
        if (*endptr != '\0') {
            fprintf(stderr, "Invalid file size\n");
            return 1;
        }
        do_get(fd, argv[2], argv[3], file_size);
    } else {
        print_usage();
    }

    net_close(fd);
    return 0;
}
