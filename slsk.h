#ifndef SLSK_H
#define SLSK_H

#include <stdint.h>
#include <stddef.h>

#define SLSK_MSG_LOGIN 1
#define SLSK_MSG_SEARCH 26
#define SLSK_MSG_SEARCH_REPLY 9
#define SLSK_MSG_GET_PEER_ADDR 71

#define PEER_MSG_PIERCE_FW 18
#define PEER_MSG_PEER_INIT 1
#define PEER_MSG_TRANSFER_REQ 40
#define PEER_MSG_TRANSFER_REP 41

typedef struct {
    char *username;
    char *filename;
    uint64_t size;
    uint32_t bitrate;
} SlskSearchResult;

/* Pack a string into a buffer. Returns number of bytes written.
   buf_size is the remaining space in the buffer. Returns 0 if no space. */
size_t slsk_pack_string(uint8_t *buf, size_t buf_size, const char *str);

/* Send a login message to the server */
int slsk_send_login(int fd, const char *username, const char *password);

/* Send a search query to the server */
int slsk_send_search(int fd, const char *query, uint32_t ticket);

/* Send a GetPeerAddress message */
int slsk_send_get_peer_addr(int fd, const char *username);

/* Receive and process a message from the server.
   Returns message code on success, -1 on error.
   If it's a search reply, it parses and prints it. */
int slsk_process_server_msg(int fd, uint32_t *out_msg_code, uint8_t **out_payload, uint32_t *out_len);

/* Send PeerInit to a peer */
int slsk_send_peer_init(int fd, const char *my_username, const char *peer_username, uint32_t token);

/* Send TransferRequest to a peer */
int slsk_send_transfer_request(int fd, const char *filename, uint64_t filesize);

#endif
