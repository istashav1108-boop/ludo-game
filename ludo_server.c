#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PORT 8080
#define MAX_ROOMS 64
#define MAX_PLAYERS 4
#define TOKENS_PER_PLAYER 4
#define TRACK_LEN 52
#define HOME_STRETCH 5
#define WIN_POS 57


static uint32_t rol32(uint32_t val, int bits) {
    return (val << bits) | (val >> (32 - bits));
}

static void sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    size_t new_len = len + 1;
    while (new_len % 64 != 56) new_len++;
    uint8_t *msg = calloc(new_len + 8, 1);
    memcpy(msg, data, len);
    msg[len] = 0x80;
    uint64_t bit_len = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) msg[new_len + 7 - i] = (bit_len >> (i * 8)) & 0xFF;

    for (size_t offset = 0; offset < new_len + 8; offset += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)msg[offset + i * 4] << 24) |
                   ((uint32_t)msg[offset + i * 4 + 1] << 16) |
                   ((uint32_t)msg[offset + i * 4 + 2] << 8) |
                   ((uint32_t)msg[offset + i * 4 + 3]);
        }
        for (int i = 16; i < 80; i++) {
            w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t temp = rol32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol32(b, 30); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    free(msg);
    uint32_t h[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (h[i] >> 24) & 0xFF;
        out[i * 4 + 1] = (h[i] >> 16) & 0xFF;
        out[i * 4 + 2] = (h[i] >> 8)  & 0xFF;
        out[i * 4 + 3] = h[i] & 0xFF;
    }
}

static void base64_encode(const uint8_t *in, size_t len, char *out) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, j = 0;
    while (i < len) {
        uint32_t a = i < len ? in[i++] : 0;
        uint32_t b = i < len ? in[i++] : 0;
        uint32_t c = i < len ? in[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        out[j++] = tbl[(triple >> 18) & 0x3F];
        out[j++] = tbl[(triple >> 12) & 0x3F];
        out[j++] = (i - 1 > len) ? '=' : tbl[(triple >> 6) & 0x3F];
        out[j++] = (i > len)     ? '=' : tbl[triple & 0x3F];
    }
    if (len % 3 == 1) { out[j - 2] = '='; out[j - 1] = '='; }
    else if (len % 3 == 2) { out[j - 1] = '='; }
    out[j] = '\0';
}

typedef struct {
    int active;
    int fd;
    char name[32];
    int color_idx; 
    int tokens[TOKENS_PER_PLAYER];
} Player;

typedef struct {
    int active;
    char code[8];
    pthread_mutex_t lock;
    Player players[MAX_PLAYERS];
    int turn;          
    int dice;         
    int rolled_this_turn;
    int winner;        
} Room;

static Room rooms[MAX_ROOMS];
static pthread_mutex_t rooms_lock = PTHREAD_MUTEX_INITIALIZER;
static const int START_SQUARE[4] = {0, 13, 26, 39};
static const int SAFE_SQUARES[8] = {0, 8, 13, 21, 26, 34, 39, 47};

static int is_safe_global(int gpos) {
    for (int i = 0; i < 8; i++) {
        if (SAFE_SQUARES[i] == gpos) return 1;
    }
    return 0;
}

static int to_global_pos(int color_idx, int rel_pos) {
    if (rel_pos < 0 || rel_pos > 50) return -1;
    return (START_SQUARE[color_idx] + rel_pos) % TRACK_LEN;
}

static int send_ws_text(int fd, const char *msg) {
    size_t len = strlen(msg);
    uint8_t header[10];
    size_t hlen = 0;
    header[0] = 0x81; 
    if (len <= 125) {
        header[1] = (uint8_t)len;
        hlen = 2;
    } else if (len <= 65535) {
        header[1] = 126;
        header[2] = (len >> 8) & 0xFF;
        header[3] = len & 0xFF;
        hlen = 4;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; i++) header[2 + i] = (len >> ((7 - i) * 8)) & 0xFF;
        hlen = 10;
    }
    if (write(fd, header, hlen) <= 0) return -1;
    if (write(fd, msg, len) <= 0) return -1;
    return 0;
}

static int read_ws_frame(int fd, char *out, size_t max_len) {
    uint8_t h[2];
    ssize_t r = recv(fd, h, 2, MSG_WAITALL);
    if (r <= 0) return -1;
    uint8_t opcode = h[0] & 0x0F;
    if (opcode == 0x8) return -1; // Close frame
    int masked = (h[1] & 0x80) != 0;
    uint64_t payload_len = h[1] & 0x7F;
    if (payload_len == 126) {
        uint8_t ext[2];
        if (recv(fd, ext, 2, MSG_WAITALL) <= 0) return -1;
        payload_len = (ext[0] << 8) | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8];
        if (recv(fd, ext, 8, MSG_WAITALL) <= 0) return -1;
        payload_len = 0;
        for (int i = 0; i < 8; i++) payload_len = (payload_len << 8) | ext[i];
    }
    uint8_t mask[4] = {0};
    if (masked && recv(fd, mask, 4, MSG_WAITALL) <= 0) return -1;
    if (payload_len >= max_len) return -1;

    uint8_t *buf = malloc(payload_len + 1);
    if (payload_len > 0 && recv(fd, buf, payload_len, MSG_WAITALL) <= 0) {
        free(buf);
        return -1;
    }
    for (uint64_t i = 0; i < payload_len; i++) {
        out[i] = masked ? (buf[i] ^ mask[i % 4]) : buf[i];
    }
    out[payload_len] = '\0';
    free(buf);
    return 0;
}


static void broadcast_room_locked(Room *r, const char *event_msg) {
    char json[4096];
    int pos = snprintf(json, sizeof(json),
        "{\"type\":\"state\",\"room\":\"%s\",\"turn\":%d,\"dice\":%d,\"rolled\":%d,\"winner\":%d,\"msg\":\"%s\",\"players\":[",
        r->code, r->turn, r->dice, r->rolled_this_turn, r->winner, event_msg);

    int first = 1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!r->players[i].active) continue;
        if (!first) pos += snprintf(json + pos, sizeof(json) - pos, ",");
        first = 0;
        pos += snprintf(json + pos, sizeof(json) - pos,
            "{\"slot\":%d,\"name\":\"%s\",\"color\":%d,\"tokens\":[%d,%d,%d,%d]}",
            i, r->players[i].name, r->players[i].color_idx,
            r->players[i].tokens[0], r->players[i].tokens[1],
            r->players[i].tokens[2], r->players[i].tokens[3]);
    }
    snprintf(json + pos, sizeof(json) - pos, "]}");

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (r->players[i].active) send_ws_text(r->players[i].fd, json);
    }
}

static int find_next_player_locked(Room *r, int current) {
    for (int step = 1; step <= MAX_PLAYERS; step++) {
        int idx = (current + step) % MAX_PLAYERS;
        if (r->players[idx].active) return idx;
    }
    return current;
}

static int has_any_valid_move_locked(Room *r, Player *p) {
    if (!r->rolled_this_turn || r->dice == 0) return 0;
    for (int t = 0; t < TOKENS_PER_PLAYER; t++) {
        int pos = p->tokens[t];
        if (pos == -1 && r->dice == 6) return 1;
        if (pos >= 0 && pos + r->dice <= WIN_POS) return 1;
    }
    return 0;
}

static void handle_move_locked(Room *r, int player_slot, int token_idx) {
    Player *p = &r->players[player_slot];
    if (r->winner != -1 || r->turn != player_slot || !r->rolled_this_turn) return;
    if (token_idx < 0 || token_idx >= TOKENS_PER_PLAYER) return;

    int cur = p->tokens[token_idx];
    int next = -999;
    if (cur == -1) {
        if (r->dice == 6) next = 0;
    } else if (cur + r->dice <= WIN_POS) {
        next = cur + r->dice;
    }
    if (next == -999) return;

    p->tokens[token_idx] = next;
    char status[128];
    snprintf(status, sizeof(status), "%s moved token %d", p->name, token_idx + 1);

    int captured = 0;
    int gpos = to_global_pos(p->color_idx, next);
    if (gpos != -1 && !is_safe_global(gpos)) {
        for (int oi = 0; oi < MAX_PLAYERS; oi++) {
            if (oi == player_slot || !r->players[oi].active) continue;
            Player *op = &r->players[oi];
            for (int ot = 0; ot < TOKENS_PER_PLAYER; ot++) {
                int ogpos = to_global_pos(op->color_idx, op->tokens[ot]);
                if (ogpos == gpos) {
                    op->tokens[ot] = -1;
                    captured = 1;
                    snprintf(status, sizeof(status), "%s captured %s's token!", p->name, op->name);
                }
            }
        }
    }

    int all_home = 1;
    for (int t = 0; t < TOKENS_PER_PLAYER; t++) {
        if (p->tokens[t] != WIN_POS) { all_home = 0; break; }
    }
    if (all_home) {
        r->winner = p->color_idx;
        snprintf(status, sizeof(status), "%s wins the Ludo match!", p->name);
        broadcast_room_locked(r, status);
        return;
    }

    int extra_turn = (r->dice == 6) || captured || (next == WIN_POS);
    r->dice = 0;
    r->rolled_this_turn = 0;
    if (!extra_turn) {
        r->turn = find_next_player_locked(r, player_slot);
    }
    broadcast_room_locked(r, status);
}

static void handle_roll_locked(Room *r, int player_slot) {
    Player *p = &r->players[player_slot];
    if (r->winner != -1 || r->turn != player_slot || r->rolled_this_turn) return;

    r->dice = (rand() % 6) + 1;
    r->rolled_this_turn = 1;
    char status[128];
    snprintf(status, sizeof(status), "%s rolled a %d", p->name, r->dice);

    if (!has_any_valid_move_locked(r, p)) {
        snprintf(status, sizeof(status), "%s rolled %d (no valid moves)", p->name, r->dice);
        r->dice = 0;
        r->rolled_this_turn = 0;
        r->turn = find_next_player_locked(r, player_slot);
    }
    broadcast_room_locked(r, status);
}


typedef struct {
    int fd;
} ClientArg;

static void serve_static_file(int fd, const char *path, const char *mime) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        const char *not_found = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot Found";
        write(fd, not_found, strlen(not_found));
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *body = malloc(sz + 1);
    fread(body, 1, sz, f);
    body[sz] = '\0';
    fclose(f);

    char hdr[256];
    snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n",
        mime, sz);
    write(fd, hdr, strlen(hdr));
    write(fd, body, sz);
    free(body);
}

static void *client_thread(void *arg) {
    int fd = ((ClientArg *)arg)->fd;
    free(arg);

    char req[4096];
    ssize_t n = recv(fd, req, sizeof(req) - 1, 0);
    if (n <= 0) { close(fd); return NULL; }
    req[n] = '\0';

    if (strncmp(req, "GET /style.css", 14) == 0) {
        serve_static_file(fd, "style.css", "text/css");
        close(fd);
        return NULL;
    }
    if (strstr(req, "Upgrade: websocket") == NULL && strstr(req, "upgrade: websocket") == NULL) {
        serve_static_file(fd, "index.html", "text/html");
        close(fd);
        return NULL;
    }


    char *key_hdr = strstr(req, "Sec-WebSocket-Key:");
    if (!key_hdr) key_hdr = strstr(req, "sec-websocket-key:");
    if (!key_hdr) { close(fd); return NULL; }
    key_hdr += 18;
    while (*key_hdr == ' ') key_hdr++;
    char ws_key[128] = {0};
    int ki = 0;
    while (*key_hdr && *key_hdr != '\r' && *key_hdr != '\n' && ki < 120) {
        ws_key[ki++] = *key_hdr++;
    }
    char combined[256];
    snprintf(combined, sizeof(combined), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", ws_key);
    uint8_t hash[20];
    sha1((uint8_t *)combined, strlen(combined), hash);
    char accept_b64[64];
    base64_encode(hash, 20, accept_b64);

    char resp[512];
    snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept_b64);
    write(fd, resp, strlen(resp));

    int my_room = -1;
    int my_slot = -1;
    char msg[512];

    while (read_ws_frame(fd, msg, sizeof(msg)) == 0) {
        char name[32] = "Player", code[8] = {0};
        if (sscanf(msg, "CREATE %31s", name) == 1 || strncmp(msg, "CREATE", 6) == 0) {
            if (strncmp(msg, "CREATE ", 7) == 0) strncpy(name, msg + 7, 31);
            pthread_mutex_lock(&rooms_lock);
            for (int i = 0; i < MAX_ROOMS; i++) {
                if (!rooms[i].active) {
                    rooms[i].active = 1;
                    snprintf(rooms[i].code, sizeof(rooms[i].code), "%04d", rand() % 10000);
                    rooms[i].turn = 0;
                    rooms[i].dice = 0;
                    rooms[i].rolled_this_turn = 0;
                    rooms[i].winner = -1;
                    for (int p = 0; p < MAX_PLAYERS; p++) rooms[i].players[p].active = 0;
                    rooms[i].players[0].active = 1;
                    rooms[i].players[0].fd = fd;
                    strncpy(rooms[i].players[0].name, name, 31);
                    rooms[i].players[0].color_idx = 0;
                    for (int t = 0; t < TOKENS_PER_PLAYER; t++) rooms[i].players[0].tokens[t] = -1;
                    my_room = i;
                    my_slot = 0;
                    break;
                }
            }
            pthread_mutex_unlock(&rooms_lock);

            if (my_room != -1) {
                char joined[128];
                snprintf(joined, sizeof(joined), "{\"type\":\"joined\",\"room\":\"%s\",\"slot\":0,\"color\":0}", rooms[my_room].code);
                send_ws_text(fd, joined);
                pthread_mutex_lock(&rooms[my_room].lock);
                broadcast_room_locked(&rooms[my_room], "Room created. Waiting for players...");
                pthread_mutex_unlock(&rooms[my_room].lock);
            }
        } else if (sscanf(msg, "JOIN %7s %31s", code, name) >= 1) {
            pthread_mutex_lock(&rooms_lock);
            for (int i = 0; i < MAX_ROOMS; i++) {
                if (rooms[i].active && strcmp(rooms[i].code, code) == 0) {
                    for (int p = 0; p < MAX_PLAYERS; p++) {
                        if (!rooms[i].players[p].active) {
                            rooms[i].players[p].active = 1;
                            rooms[i].players[p].fd = fd;
                            strncpy(rooms[i].players[p].name, name[0] ? name : "Player", 31);
                            rooms[i].players[p].color_idx = p;
                            for (int t = 0; t < TOKENS_PER_PLAYER; t++) rooms[i].players[p].tokens[t] = -1;
                            my_room = i;
                            my_slot = p;
                            break;
                        }
                    }
                    break;
                }
            }
            pthread_mutex_unlock(&rooms_lock);

            if (my_room != -1) {
                char joined[128];
                snprintf(joined, sizeof(joined), "{\"type\":\"joined\",\"room\":\"%s\",\"slot\":%d,\"color\":%d}",
                         rooms[my_room].code, my_slot, my_slot);
                send_ws_text(fd, joined);
                char info[96];
                snprintf(info, sizeof(info), "%s joined room %s", rooms[my_room].players[my_slot].name, rooms[my_room].code);
                pthread_mutex_lock(&rooms[my_room].lock);
                broadcast_room_locked(&rooms[my_room], info);
                pthread_mutex_unlock(&rooms[my_room].lock);
            } else {
                send_ws_text(fd, "{\"type\":\"error\",\"msg\":\"Room not found or full\"}");
            }
        } else if (my_room != -1 && strcmp(msg, "ROLL") == 0) {
            pthread_mutex_lock(&rooms[my_room].lock);
            handle_roll_locked(&rooms[my_room], my_slot);
            pthread_mutex_unlock(&rooms[my_room].lock);
        } else if (my_room != -1 && strncmp(msg, "MOVE ", 5) == 0) {
            int tok = atoi(msg + 5);
            pthread_mutex_lock(&rooms[my_room].lock);
            handle_move_locked(&rooms[my_room], my_slot, tok);
            pthread_mutex_unlock(&rooms[my_room].lock);
        }
    }

    if (my_room != -1 && my_slot != -1) {
        pthread_mutex_lock(&rooms[my_room].lock);
        rooms[my_room].players[my_slot].active = 0;
        if (rooms[my_room].turn == my_slot) {
            rooms[my_room].rolled_this_turn = 0;
            rooms[my_room].dice = 0;
            rooms[my_room].turn = find_next_player_locked(&rooms[my_room], my_slot);
        }
        int any_left = 0;
        for (int i = 0; i < MAX_PLAYERS; i++) if (rooms[my_room].players[i].active) any_left = 1;
        if (!any_left) {
            rooms[my_room].active = 0;
        } else {
            broadcast_room_locked(&rooms[my_room], "A player disconnected");
        }
        pthread_mutex_unlock(&rooms[my_room].lock);
    }
    close(fd);
    return NULL;
}

int main(void) {
    srand((unsigned int)time(NULL));
    for (int i = 0; i < MAX_ROOMS; i++) pthread_mutex_init(&rooms[i].lock, NULL);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 64);
    printf("Ludo C Server listening on http://0.0.0.0:%d\n", PORT);

    while (1) {
        int cfd = accept(server_fd, NULL, NULL);
        if (cfd < 0) continue;
        ClientArg *carg = malloc(sizeof(ClientArg));
        carg->fd = cfd;
        pthread_t tid;
        pthread_create(&tid, NULL, client_thread, carg);
        pthread_detach(tid);
    }
    return 0;
}