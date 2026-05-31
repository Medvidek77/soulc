#define _POSIX_C_SOURCE 200112L
#include "slsk.h"
#include "utils.h"
#include "net.h"
#include "md5.h"
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <zlib.h>

size_t slsk_pack_string(uint8_t *buf, size_t buf_size, const char *str) {
    size_t len = strlen(str);
    if (buf_size < 4 + len) return 0;
    put_u32_le(buf, (uint32_t)len);
    memcpy(buf + 4, str, len);
    return 4 + len;
}

int slsk_send_login(int fd, const char *username, const char *password) {
    uint8_t buf[1024];
    size_t offset = 8; // skip length and code

    size_t n;
    // username
    n = slsk_pack_string(buf + offset, sizeof(buf) - offset, username);
    if (!n) return -1;
    offset += n;

    // password
    n = slsk_pack_string(buf + offset, sizeof(buf) - offset, password);
    if (!n) return -1;
    offset += n;

    if (sizeof(buf) - offset < 24) return -1;

    // version
    put_u32_le(buf + offset, 160);
    offset += 4;

    // md5 hash of user + pass
    MD5_CTX ctx;
    MD5_Init(&ctx);
    MD5_Update(&ctx, username, strlen(username));
    MD5_Update(&ctx, password, strlen(password));
    uint8_t digest[16];
    MD5_Final(digest, &ctx);

    memcpy(buf + offset, digest, 16);
    offset += 16;

    put_u32_le(buf + offset, 0); // version patch
    offset += 4;

    // Write header
    put_u32_le(buf, (uint32_t)(offset - 4));
    put_u32_le(buf + 4, SLSK_MSG_LOGIN);

    return net_write_exact(fd, buf, offset);
}

int slsk_send_listen_port(int fd, uint32_t port) {
    uint8_t buf[12];
    put_u32_le(buf, 8); // len
    put_u32_le(buf + 4, 2); // msg code 2 (SetListenPort)
    put_u32_le(buf + 8, port);
    return net_write_exact(fd, buf, 12);
}

int slsk_send_search(int fd, const char *query, uint32_t ticket) {
    uint8_t buf[1024];
    size_t offset = 8; // skip length and code

    if (sizeof(buf) - offset < 4) return -1;

    // ticket (random number for search id)
    put_u32_le(buf + offset, ticket);
    offset += 4;

    // search query
    size_t n = slsk_pack_string(buf + offset, sizeof(buf) - offset, query);
    if (!n) return -1;
    offset += n;

    // header
    put_u32_le(buf, (uint32_t)(offset - 4));
    put_u32_le(buf + 4, SLSK_MSG_SEARCH);

    return net_write_exact(fd, buf, offset);
}

int slsk_send_get_peer_addr(int fd, const char *username) {
    uint8_t buf[512];
    size_t offset = 8; // skip length and code

    size_t n = slsk_pack_string(buf + offset, sizeof(buf) - offset, username);
    if (!n) return -1;
    offset += n;

    put_u32_le(buf, (uint32_t)(offset - 4));
    put_u32_le(buf + 4, SLSK_MSG_GET_PEER_ADDR);

    return net_write_exact(fd, buf, offset);
}

int slsk_process_server_msg(int fd, uint32_t *out_msg_code, uint8_t **out_payload, uint32_t *out_len) {
    uint8_t header[8];

    // Server might send 4-byte keepalives (msg_len = 0). We must handle them correctly to stay in sync.
    // We read the first 4 bytes (length)
    if (net_read_exact(fd, header, 4) < 0) return -1;
    uint32_t msg_len = get_u32_le(header);

    if (msg_len == 0) {
        // keepalive, nothing to do. Return 0 for success but set payload length to 0, code to 0
        *out_msg_code = 0;
        *out_payload = NULL;
        *out_len = 0;
        return 0;
    }

    // Now read the 4 byte msg code
    if (net_read_exact(fd, header + 4, 4) < 0) return -1;
    uint32_t msg_code = get_u32_le(header + 4);

    if (msg_len < 4) return -1;
    uint32_t payload_len = msg_len - 4;

    uint8_t *payload = NULL;
    if (payload_len > 0) {
        payload = malloc(payload_len);
        if (!payload) return -1;
        if (net_read_exact(fd, payload, payload_len) < 0) {
            free(payload);
            return -1;
        }
    }

    *out_msg_code = msg_code;
    *out_payload = payload;
    *out_len = payload_len;

    return 0;
}

int slsk_send_peer_init(int fd, const char *my_username, const char *peer_username, uint32_t token) {
    uint8_t buf[512];
    size_t offset = 8;

    size_t n;
    n = slsk_pack_string(buf + offset, sizeof(buf) - offset, my_username);
    if (!n) return -1;
    offset += n;

    n = slsk_pack_string(buf + offset, sizeof(buf) - offset, "type"); // dummy type
    if (!n) return -1;
    offset += n;

    if (sizeof(buf) - offset < 4) return -1;
    put_u32_le(buf + offset, token);
    offset += 4;

    put_u32_le(buf, (uint32_t)(offset - 4));
    put_u32_le(buf + 4, PEER_MSG_PEER_INIT);

    return net_write_exact(fd, buf, offset);
}

int slsk_send_transfer_request(int fd, const char *filename, uint64_t filesize) {
    uint8_t buf[1024];
    size_t offset = 8;

    if (sizeof(buf) - offset < 8) return -1;

    put_u32_le(buf + offset, 0); // direction: download
    offset += 4;

    // ticket
    put_u32_le(buf + offset, 12345);
    offset += 4;

    size_t n = slsk_pack_string(buf + offset, sizeof(buf) - offset, filename);
    if (!n) return -1;
    offset += n;

    if (sizeof(buf) - offset < 8) return -1;
    // filesize
    put_u32_le(buf + offset, (uint32_t)(filesize & 0xFFFFFFFF));
    offset += 4;
    put_u32_le(buf + offset, (uint32_t)(filesize >> 32));
    offset += 4;

    put_u32_le(buf, (uint32_t)(offset - 4));
    put_u32_le(buf + 4, PEER_MSG_TRANSFER_REQ);

    return net_write_exact(fd, buf, offset);
}

void parse_peer_search_reply(const uint8_t *payload, uint32_t len, const char *peer_username) {
    if (len < 4) return;
    uint32_t offset = 0;

    // search_username
    uint32_t user_len = get_u32_le(payload + offset);
    offset += 4;
    if (user_len > len - offset) return;
    // We skip the search username itself since we just want the results
    offset += user_len;

    if (len - offset < 4) return;
    uint32_t token = get_u32_le(payload + offset);
    (void)token;
    offset += 4;

    if (len - offset < 4) return;
    uint32_t result_count = get_u32_le(payload + offset);
    offset += 4;

    for (uint32_t i = 0; i < result_count; i++) {
        if (len - offset < 1) break;
        uint8_t code = payload[offset++]; // usually 1
        (void)code;

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
        uint32_t ext_len = get_u32_le(payload + offset);
        offset += 4;

        uint64_t attr_size = (uint64_t)ext_len * 8;
        if (attr_size > len - offset) {
            free(filename);
            break;
        }
        offset += (uint32_t)attr_size;

        printf("%s\t%" PRIu64 "\t%s\n", peer_username, size, filename);
        free(filename);
    }
}
