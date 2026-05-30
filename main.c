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

void print_usage(void) {
    printf("soulc - minimalist soulseek client\n");
    printf("Usage:\n");
    printf("  soulc search <query>\n");
    printf("  soulc get <username> <filepath> <size_in_bytes>\n");
    exit(1);
}

void parse_search_reply(const uint8_t *payload, uint32_t len) {
    if (len < 4) return;
    uint32_t user_len = get_u32_le(payload);
    if (user_len > len - 4) return;

    char *user = malloc(user_len + 1);
    memcpy(user, payload + 4, user_len);
    user[user_len] = '\0';

    uint32_t offset = 4 + user_len;

    if (offset + 4 > len) { free(user); return; }
    uint32_t ticket = get_u32_le(payload + offset);
    offset += 4;

    if (offset + 4 > len) { free(user); return; }
    uint32_t result_count = get_u32_le(payload + offset);
    offset += 4;

    for (uint32_t i = 0; i < result_count; i++) {
        if (len - offset < 1) break;
        uint8_t code = payload[offset++]; // usually 1

        if (len - offset < 4) break;
        uint32_t file_len = get_u32_le(payload + offset);
        offset += 4;

        if (file_len > len - offset) break;
        char *filename = malloc(file_len + 1);
        memcpy(filename, payload + offset, file_len);
        filename[file_len] = '\0';
        offset += file_len;

        if (len - offset < 8) { free(filename); break; }
        uint32_t size_low = get_u32_le(payload + offset);
        uint32_t size_high = get_u32_le(payload + offset + 4);
        uint64_t size = ((uint64_t)size_high << 32) | size_low;
        offset += 8;

        if (len - offset < 4) { free(filename); break; }
        // ext_len is often 0 or points to attributes (like bit rate)
        uint32_t ext_len = get_u32_le(payload + offset);
        offset += 4;

        // Skip attributes for now safely
        uint64_t attr_size = (uint64_t)ext_len * 8;
        if (attr_size > len - offset) {
            free(filename);
            break;
        }
        offset += (uint32_t)attr_size; // (type(4) + val(4)) * ext_len

        printf("%s\t%" PRIu64 "\t%s\n", user, size, filename);

        free(filename);
    }

    free(user);
}

void do_search(int fd, const char *query) {
    uint32_t ticket = (uint32_t)time(NULL);
    if (slsk_send_search(fd, query, ticket) < 0) {
        fprintf(stderr, "Failed to send search\n");
        return;
    }

    printf("Searching for '%s'. Wait ~10 seconds for results...\n", query);
    printf("USER\tSIZE_BYTES\tFILEPATH\n");

    time_t start = time(NULL);
    while (time(NULL) - start < 10) {
        if (net_wait(fd, 1000) > 0) {
            uint32_t msg_code;
            uint8_t *payload = NULL;
            uint32_t payload_len = 0;

            if (slsk_process_server_msg(fd, &msg_code, &payload, &payload_len) == 0) {
                if (getenv("SLSK_DEBUG")) {
                    fprintf(stderr, "[DEBUG] Server msg: code=%u, len=%u\n", msg_code, payload_len);
                }
                if (msg_code == SLSK_MSG_SEARCH_REPLY && payload != NULL && payload_len >= 4) {
                    uint32_t decompressed_len = get_u32_le(payload);
                    if (decompressed_len > 0 && decompressed_len < 10000000) { // Reasonable limit 10MB
                        uint8_t *uncompressed = malloc(decompressed_len);
                        if (uncompressed) {
                            unsigned long destLen = decompressed_len;
                            if (uncompress(uncompressed, &destLen, payload + 4, payload_len - 4) == Z_OK) {
                                parse_search_reply(uncompressed, destLen);
                            }
                            free(uncompressed);
                        }
                    } else {
                        // Fallback: try parsing as uncompressed if length makes no sense or is missing
                        parse_search_reply(payload, payload_len);
                    }
                }
                if (payload) free(payload);
            } else {
                fprintf(stderr, "Connection to server lost during search.\n");
                break;
            }
        }
    }
}

void do_get(int fd_server, const char *username, const char *filepath, uint64_t file_size) {
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
                if (getenv("SLSK_DEBUG")) {
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
                                    (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
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

    printf("Found peer %s at %s:%u. Connecting...\n", username, peer_ip, peer_port);

    char port_str[16];
    sprintf(port_str, "%u", peer_port);
    int fd_peer = net_connect(peer_ip, port_str);
    if (fd_peer < 0) {
        fprintf(stderr, "Failed to connect to peer\n");
        return;
    }

    // Simplistic P2P download phase (not fully robust for real network but follows standard)
    const char *my_username = getenv("SLSK_USER");
    slsk_send_peer_init(fd_peer, my_username, username, 0);
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
        printf("Downloading to %s (%" PRIu64 " bytes)...\n", base_name, file_size);
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
        if (total_read == file_size) {
            printf("Download complete.\n");
        }
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

    // Wait briefly for login reply (msg 1)
    if (net_wait(fd, 5000) > 0) {
        uint32_t msg_code;
        uint8_t *payload = NULL;
        uint32_t payload_len = 0;
        if (slsk_process_server_msg(fd, &msg_code, &payload, &payload_len) == 0) {
            if (getenv("SLSK_DEBUG")) {
                fprintf(stderr, "[DEBUG] Server msg: code=%u, len=%u\n", msg_code, payload_len);
            }
            if (msg_code == SLSK_MSG_LOGIN && payload != NULL && payload_len >= 1) {
                if (payload[0] != 1) {
                    fprintf(stderr, "Login failed. Check credentials.\n");
                    free(payload);
                    net_close(fd);
                    return 1;
                }
                if (getenv("SLSK_DEBUG")) {
                    fprintf(stderr, "[DEBUG] Login successful!\n");
                }
            }
            if (payload) free(payload);
        }
    }

    if (strcmp(argv[1], "search") == 0 && argc == 3) {
        do_search(fd, argv[2]);
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
