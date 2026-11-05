#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "cbot.h"
#include "cbot_telegram.h"
#include "cweb.h"

#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TG_DEFAULT_BASE_URL "https://api.telegram.org"
#define TG_DEFAULT_RETRY_MAX 3

/* ---------- JSON Parser ---------- */
typedef enum { TG_J_STRING, TG_J_NUMBER, TG_J_OBJECT, TG_J_ARRAY, TG_J_OTHER } TgJsonType;
typedef struct { const char *start; const char *end; TgJsonType type; } TgJsonValue;

static const char *skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static int parse_string(const char *p, const char **end) {
    if (*p++ != '"') return 0;
    while (*p) {
        if (*p == '"') { *end = p + 1; return 1; }
        if (*p == '\\') {
            p++;
            if (*p) p++;
        } else {
            p++;
        }
    }
    return 0;
}

static int parse_value(const char *p, TgJsonValue *out, int depth);

static int parse_object(const char *p, const char **end, int depth) {
    const char *cur = skip_ws(p + 1);
    if (*cur == '}') { *end = cur + 1; return 1; }
    while (*cur) {
        const char *key_end;
        if (!parse_string(cur, &key_end)) return 0;
        cur = skip_ws(key_end);
        if (*cur != ':') return 0;
        TgJsonValue val;
        if (!parse_value(cur + 1, &val, depth + 1)) return 0;
        cur = skip_ws(val.end);
        if (*cur == '}') { *end = cur + 1; return 1; }
        if (*cur != ',') return 0;
        cur = skip_ws(cur + 1);
    }
    return 0;
}

static int parse_array(const char *p, const char **end, int depth) {
    const char *cur = skip_ws(p + 1);
    if (*cur == ']') { *end = cur + 1; return 1; }
    while (*cur) {
        TgJsonValue val;
        if (!parse_value(cur, &val, depth + 1)) return 0;
        cur = skip_ws(val.end);
        if (*cur == ']') { *end = cur + 1; return 1; }
        if (*cur != ',') return 0;
        cur = skip_ws(cur + 1);
    }
    return 0;
}

static int parse_value(const char *p, TgJsonValue *out, int depth) {
    if (depth > 32) return 0;
    p = skip_ws(p);
    out->start = p;
    if (*p == '"') {
        out->type = TG_J_STRING;
        return parse_string(p, &out->end);
    }
    if (*p == '{') {
        out->type = TG_J_OBJECT;
        return parse_object(p, &out->end, depth);
    }
    if (*p == '[') {
        out->type = TG_J_ARRAY;
        return parse_array(p, &out->end, depth);
    }
    if (strncmp(p, "true", 4) == 0) { out->type = TG_J_OTHER; out->end = p + 4; return 1; }
    if (strncmp(p, "false", 5) == 0) { out->type = TG_J_OTHER; out->end = p + 5; return 1; }
    if (strncmp(p, "null", 4) == 0) { out->type = TG_J_OTHER; out->end = p + 4; return 1; }
    if (*p == '-' || isdigit((unsigned char)*p)) {
        const char *cur = p;
        if (*cur == '-') cur++;
        while (isdigit((unsigned char)*cur)) cur++;
        if (*cur == '.') {
            cur++;
            while (isdigit((unsigned char)*cur)) cur++;
        }
        if (*cur == 'e' || *cur == 'E') {
            cur++;
            if (*cur == '+' || *cur == '-') cur++;
            while (isdigit((unsigned char)*cur)) cur++;
        }
        out->type = TG_J_NUMBER;
        out->end = cur;
        return 1;
    }
    return 0;
}

static int hex_digit(char c) {
    if (isdigit((unsigned char)c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static int put_utf8(char *out, size_t size, size_t *used, int code) {
    int count = code < 0x80 ? 1 : (code < 0x800 ? 2 : (code < 0x10000 ? 3 : 4));
    if (*used + (size_t)count >= size) return 0;
    if (count == 1) {
        out[(*used)++] = (char)code;
    } else if (count == 2) {
        out[(*used)++] = (char)(0xc0 | (code >> 6));
        out[(*used)++] = (char)(0x80 | (code & 0x3f));
    } else if (count == 3) {
        out[(*used)++] = (char)(0xe0 | (code >> 12));
        out[(*used)++] = (char)(0x80 | ((code >> 6) & 0x3f));
        out[(*used)++] = (char)(0x80 | (code & 0x3f));
    } else {
        out[(*used)++] = (char)(0xf0 | (code >> 18));
        out[(*used)++] = (char)(0x80 | ((code >> 12) & 0x3f));
        out[(*used)++] = (char)(0x80 | ((code >> 6) & 0x3f));
        out[(*used)++] = (char)(0x80 | (code & 0x3f));
    }
    return 1;
}

static int decode_string(const TgJsonValue *val, char *out, size_t out_sz) {
    const char *p = val->start + 1;
    const char *last = val->end - 1;
    size_t used = 0;
    if (out_sz == 0) return 0;

    while (p < last) {
        char c = *p++;
        if (c != '\\') {
            if (used + 1 >= out_sz) return 0;
            out[used++] = c;
        } else {
            c = *p++;
            if (c == 'u') {
                if (p + 4 > last) return 0;
                int code = (hex_digit(p[0]) << 12) | (hex_digit(p[1]) << 8) |
                           (hex_digit(p[2]) << 4) | hex_digit(p[3]);
                p += 4;
                if (code >= 0xd800 && code <= 0xdbff && p + 6 <= last && p[0] == '\\' && p[1] == 'u') {
                    int low = (hex_digit(p[2]) << 12) | (hex_digit(p[3]) << 8) |
                              (hex_digit(p[4]) << 4) | hex_digit(p[5]);
                    p += 6;
                    if (low >= 0xdc00 && low <= 0xdfff) {
                        code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                    }
                }
                if (!put_utf8(out, out_sz, &used, code)) return 0;
            } else {
                if (used + 1 >= out_sz) return 0;
                switch (c) {
                    case '"': out[used++] = '"'; break;
                    case '\\': out[used++] = '\\'; break;
                    case '/': out[used++] = '/'; break;
                    case 'b': out[used++] = '\b'; break;
                    case 'f': out[used++] = '\f'; break;
                    case 'n': out[used++] = '\n'; break;
                    case 'r': out[used++] = '\r'; break;
                    case 't': out[used++] = '\t'; break;
                    default: out[used++] = c; break;
                }
            }
        }
    }
    out[used] = '\0';
    return 1;
}

static int find_member(const TgJsonValue *obj, const char *key, TgJsonValue *out) {
    const char *p = skip_ws(obj->start + 1);
    while (*p && *p != '}') {
        const char *k_end;
        if (!parse_string(p, &k_end)) return 0;
        TgJsonValue k_val = { .start = p, .end = k_end, .type = TG_J_STRING };
        char k_name[128];
        if (!decode_string(&k_val, k_name, sizeof(k_name))) return 0;
        p = skip_ws(k_end);
        if (*p != ':') return 0;
        p++;
        TgJsonValue v_val;
        if (!parse_value(p, &v_val, 0)) return 0;
        if (strcmp(k_name, key) == 0) {
            *out = v_val;
            return 1;
        }
        p = skip_ws(v_val.end);
        if (*p == '}') break;
        if (*p != ',') return 0;
        p = skip_ws(p + 1);
    }
    return 0;
}

static int find_item(const TgJsonValue *arr, int index, TgJsonValue *out) {
    const char *p = skip_ws(arr->start + 1);
    int cur_idx = 0;
    while (*p && *p != ']') {
        TgJsonValue item_val;
        if (!parse_value(p, &item_val, 0)) return 0;
        if (cur_idx == index) {
            *out = item_val;
            return 1;
        }
        cur_idx++;
        p = skip_ws(item_val.end);
        if (*p == ']') break;
        if (*p != ',') return 0;
        p = skip_ws(p + 1);
    }
    return 0;
}

static int tg_json_find(const char *json, const char *path, TgJsonValue *out) {
    TgJsonValue current;
    if (!parse_value(json, &current, 0)) return 0;

    while (*path) {
        if (current.type == TG_J_OBJECT) {
            char key[128];
            size_t used = 0;
            if (*path == '.') path++;
            while (*path && *path != '.' && *path != '[') {
                if (used + 1 >= sizeof(key)) return 0;
                key[used++] = *path++;
            }
            if (used == 0) return 0;
            key[used] = '\0';
            if (!find_member(&current, key, &current)) return 0;
        } else if (current.type == TG_J_ARRAY) {
            if (*path != '[') return 0;
            path++;
            int idx = 0;
            while (isdigit((unsigned char)*path)) {
                idx = idx * 10 + (*path - '0');
                path++;
            }
            if (*path != ']') return 0;
            path++;
            if (!find_item(&current, idx, &current)) return 0;
        } else {
            return 0;
        }
    }

    *out = current;
    return 1;
}

static int tg_json_get_string(const char *json, const char *path, char *out, size_t out_sz) {
    TgJsonValue v;
    if (!tg_json_find(json, path, &v) || v.type != TG_J_STRING) return 0;
    return decode_string(&v, out, out_sz);
}

static int tg_json_get_raw(const char *json, const char *path, char *out, size_t out_sz) {
    TgJsonValue v;
    if (!tg_json_find(json, path, &v)) return 0;
    if (v.type == TG_J_STRING) {
        return decode_string(&v, out, out_sz);
    }
    size_t len = (size_t)(v.end - v.start);
    if (len >= out_sz) return 0;
    memcpy(out, v.start, len);
    out[len] = '\0';
    return 1;
}

static int json_escape_string(const char *in, char *out, size_t out_sz) {
    if (!in || !out || out_sz == 0) return 0;
    size_t used = 0;
    while (*in) {
        unsigned char c = (unsigned char)*in++;
        if (c == '"') {
            if (used + 2 >= out_sz) return 0;
            out[used++] = '\\'; out[used++] = '"';
        } else if (c == '\\') {
            if (used + 2 >= out_sz) return 0;
            out[used++] = '\\'; out[used++] = '\\';
        } else if (c == '\n') {
            if (used + 2 >= out_sz) return 0;
            out[used++] = '\\'; out[used++] = 'n';
        } else if (c == '\r') {
            if (used + 2 >= out_sz) return 0;
            out[used++] = '\\'; out[used++] = 'r';
        } else if (c == '\t') {
            if (used + 2 >= out_sz) return 0;
            out[used++] = '\\'; out[used++] = 't';
        } else if (c < 0x20) {
            if (used + 6 >= out_sz) return 0;
            snprintf(out + used, 7, "\\u%04x", c);
            used += 6;
        } else {
            if (used + 1 >= out_sz) return 0;
            out[used++] = (char)c;
        }
    }
    out[used] = '\0';
    return 1;
}

/* ---------- Telegram Adapter Implementation ---------- */

typedef struct TelegramQueuedUpdate {
    CbotEvent event;
    char *raw_json;
    struct TelegramQueuedUpdate *next;
} TelegramQueuedUpdate;

typedef struct {
    char *token;
    char *base_url;
    int is_webhook;
    uint64_t last_offset;

    TelegramQueuedUpdate *q_head;
    TelegramQueuedUpdate *q_tail;
    int q_count;
    pthread_mutex_t q_lock;
    pthread_cond_t q_cond;

    TelegramQueuedUpdate *current_ev;

    CURL *curl;
} TelegramImpl;

static pthread_once_t g_curl_init_once = PTHREAD_ONCE_INIT;
static void init_curl_global(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

typedef struct {
    char *data;
    size_t size;
} MemoryBuffer;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    MemoryBuffer *mem = (MemoryBuffer *)userdata;
    char *ptr_new = realloc(mem->data, mem->size + total + 1);
    if (!ptr_new) return 0;
    mem->data = ptr_new;
    memcpy(mem->data + mem->size, ptr, total);
    mem->size += total;
    mem->data[mem->size] = '\0';
    return total;
}

static CbotStatus telegram_http_request(TelegramImpl *impl, const char *url, const char *post_json,
                                       int timeout_sec, MemoryBuffer *out_resp) {
    if (!impl || !url || !out_resp) return CBOT_ERR_INVALID_ARG;

    CURL *curl = curl_easy_init();
    if (!curl) return CBOT_ERR_NOMEM;

    struct curl_slist *headers = NULL;
    if (post_json) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_json);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out_resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)(timeout_sec > 0 ? timeout_sec : 30));
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cbot/0.1 (TelegramAdapter)");

    int retry_count = 0;
    CbotStatus result = CBOT_OK;

    while (retry_count <= TG_DEFAULT_RETRY_MAX) {
        free(out_resp->data);
        out_resp->data = NULL;
        out_resp->size = 0;

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            if (retry_count == TG_DEFAULT_RETRY_MAX) {
                result = CBOT_ERR_NETWORK;
                break;
            }
            retry_count++;
            usleep(200000 * retry_count);
            continue;
        }

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (http_code == 429) {
            int retry_after = 1;
            char retry_str[32];
            if (out_resp->data && tg_json_get_raw(out_resp->data, "parameters.retry_after", retry_str, sizeof(retry_str))) {
                retry_after = atoi(retry_str);
                if (retry_after <= 0) retry_after = 1;
            }
            if (retry_count == TG_DEFAULT_RETRY_MAX) {
                result = CBOT_ERR_RATE_LIMITED;
                break;
            }
            retry_count++;
            if (retry_after > 5) retry_after = 5;
            sleep((unsigned int)retry_after);
            continue;
        }

        if (http_code >= 500 && http_code <= 504) {
            if (retry_count == TG_DEFAULT_RETRY_MAX) {
                result = CBOT_ERR_NETWORK;
                break;
            }
            retry_count++;
            usleep(300000 * retry_count);
            continue;
        }

        if (http_code >= 200 && http_code < 300) {
            result = CBOT_OK;
            break;
        }

        result = CBOT_ERR_PROTOCOL;
        break;
    }

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}

static int parse_single_telegram_update(const char *json_snippet, CbotEvent *out, char **raw_copy) {
    if (!json_snippet || !out) return 0;
    memset(out, 0, sizeof(*out));

    char id_str[64];
    if (tg_json_get_raw(json_snippet, "update_id", id_str, sizeof(id_str))) {
        out->event_id = (uint64_t)strtoull(id_str, NULL, 10);
    }

    out->platform = "telegram";

    TgJsonValue val;
    if (tg_json_find(json_snippet, "callback_query", &val)) {
        out->type = CBOT_EV_CALLBACK;

        char cb_data[512];
        if (tg_json_get_string(json_snippet, "callback_query.data", cb_data, sizeof(cb_data))) {
            out->callback_data = strdup(cb_data);
        }

        char user_id[64];
        if (tg_json_get_raw(json_snippet, "callback_query.from.id", user_id, sizeof(user_id))) {
            out->user_id = strdup(user_id);
        }

        char username[128];
        if (tg_json_get_string(json_snippet, "callback_query.from.username", username, sizeof(username))) {
            out->user_name = strdup(username);
        } else if (tg_json_get_string(json_snippet, "callback_query.from.first_name", username, sizeof(username))) {
            out->user_name = strdup(username);
        }

        char chat_id[64];
        if (tg_json_get_raw(json_snippet, "callback_query.message.chat.id", chat_id, sizeof(chat_id))) {
            out->chat_id = strdup(chat_id);
        }

        if (raw_copy) {
            *raw_copy = strdup(json_snippet);
            out->raw = *raw_copy;
        }
        return 1;
    }

    const char *msg_prefix = "message";
    if (!tg_json_find(json_snippet, "message", &val)) {
        if (tg_json_find(json_snippet, "channel_post", &val)) {
            msg_prefix = "channel_post";
        } else if (tg_json_find(json_snippet, "edited_message", &val)) {
            msg_prefix = "edited_message";
        } else {
            return 0;
        }
    }

    char path[128];
    snprintf(path, sizeof(path), "%s.chat.id", msg_prefix);
    char chat_id[64];
    if (tg_json_get_raw(json_snippet, path, chat_id, sizeof(chat_id))) {
        out->chat_id = strdup(chat_id);
    }

    snprintf(path, sizeof(path), "%s.from.id", msg_prefix);
    char user_id[64];
    if (tg_json_get_raw(json_snippet, path, user_id, sizeof(user_id))) {
        out->user_id = strdup(user_id);
    } else if (out->chat_id) {
        out->user_id = strdup(out->chat_id);
    }

    snprintf(path, sizeof(path), "%s.from.username", msg_prefix);
    char username[128];
    if (tg_json_get_string(json_snippet, path, username, sizeof(username))) {
        out->user_name = strdup(username);
    } else {
        snprintf(path, sizeof(path), "%s.from.first_name", msg_prefix);
        if (tg_json_get_string(json_snippet, path, username, sizeof(username))) {
            out->user_name = strdup(username);
        } else {
            snprintf(path, sizeof(path), "%s.chat.title", msg_prefix);
            if (tg_json_get_string(json_snippet, path, username, sizeof(username))) {
                out->user_name = strdup(username);
            }
        }
    }

    snprintf(path, sizeof(path), "%s.text", msg_prefix);
    char text_buf[4096];
    if (tg_json_get_string(json_snippet, path, text_buf, sizeof(text_buf))) {
        out->text = strdup(text_buf);
        if (text_buf[0] == '/') {
            out->type = CBOT_EV_COMMAND;
        } else {
            out->type = CBOT_EV_MESSAGE;
        }
    }

    if (raw_copy) {
        *raw_copy = strdup(json_snippet);
        out->raw = *raw_copy;
    }

    return 1;
}

static void telegram_free_queued_update(TelegramQueuedUpdate *qu) {
    if (!qu) return;
    free((void *)qu->event.chat_id);
    free((void *)qu->event.user_id);
    free((void *)qu->event.user_name);
    free((void *)qu->event.text);
    free((void *)qu->event.command);
    free((void *)qu->event.args);
    free((void *)qu->event.callback_data);
    free(qu->raw_json);
    free(qu);
}

CbotStatus cbot_telegram_feed_update(CbotAdapter *a, const char *json_payload) {
    if (!a || !a->impl || !json_payload) return CBOT_ERR_INVALID_ARG;
    TelegramImpl *impl = (TelegramImpl *)a->impl;

    TelegramQueuedUpdate *qu = calloc(1, sizeof(*qu));
    if (!qu) return CBOT_ERR_NOMEM;

    if (!parse_single_telegram_update(json_payload, &qu->event, &qu->raw_json)) {
        free(qu);
        return CBOT_ERR_PROTOCOL;
    }

    pthread_mutex_lock(&impl->q_lock);
    if (qu->event.event_id >= impl->last_offset) {
        impl->last_offset = qu->event.event_id + 1;
    }

    if (impl->q_tail) {
        impl->q_tail->next = qu;
        impl->q_tail = qu;
    } else {
        impl->q_head = qu;
        impl->q_tail = qu;
    }
    impl->q_count++;

    pthread_cond_signal(&impl->q_cond);
    pthread_mutex_unlock(&impl->q_lock);

    return CBOT_OK;
}

static CbotStatus telegram_fetch_updates(TelegramImpl *impl, int timeout_sec) {
    char url[512];
    pthread_mutex_lock(&impl->q_lock);
    uint64_t offset = impl->last_offset;
    pthread_mutex_unlock(&impl->q_lock);

    snprintf(url, sizeof(url), "%s/bot%s/getUpdates?offset=%llu&timeout=%d",
             impl->base_url, impl->token, (unsigned long long)offset, timeout_sec);

    MemoryBuffer resp = {0};
    CbotStatus status = telegram_http_request(impl, url, NULL, timeout_sec + 5, &resp);
    if (status != CBOT_OK) {
        free(resp.data);
        return status;
    }

    if (!resp.data) return CBOT_OK;

    TgJsonValue res_val;
    if (tg_json_find(resp.data, "result", &res_val) && res_val.type == TG_J_ARRAY) {
        int idx = 0;
        TgJsonValue item_val;
        while (1) {
            char path[32];
            snprintf(path, sizeof(path), "result[%d]", idx);
            if (!tg_json_find(resp.data, path, &item_val)) break;

            size_t snippet_len = (size_t)(item_val.end - item_val.start);
            char *snippet = malloc(snippet_len + 1);
            if (snippet) {
                memcpy(snippet, item_val.start, snippet_len);
                snippet[snippet_len] = '\0';
                TelegramQueuedUpdate *qu = calloc(1, sizeof(*qu));
                if (qu && parse_single_telegram_update(snippet, &qu->event, &qu->raw_json)) {
                    pthread_mutex_lock(&impl->q_lock);
                    if (qu->event.event_id >= impl->last_offset) {
                        impl->last_offset = qu->event.event_id + 1;
                    }
                    if (impl->q_tail) {
                        impl->q_tail->next = qu;
                        impl->q_tail = qu;
                    } else {
                        impl->q_head = qu;
                        impl->q_tail = qu;
                    }
                    impl->q_count++;
                    pthread_mutex_unlock(&impl->q_lock);
                } else {
                    free(qu);
                }
                free(snippet);
            }
            idx++;
        }
    }

    free(resp.data);
    return CBOT_OK;
}

static CbotStatus telegram_poll(CbotAdapter *a, CbotEvent *out, int timeout_ms) {
    if (!a || !a->impl || !out) return CBOT_ERR_INVALID_ARG;
    TelegramImpl *impl = (TelegramImpl *)a->impl;

    pthread_mutex_lock(&impl->q_lock);

    if (impl->q_head == NULL) {
        if (impl->is_webhook) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += timeout_ms / 1000;
            ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += ts.tv_nsec / 1000000000L;
                ts.tv_nsec %= 1000000000L;
            }
            pthread_cond_timedwait(&impl->q_cond, &impl->q_lock, &ts);
        } else {
            pthread_mutex_unlock(&impl->q_lock);
            int poll_sec = timeout_ms / 1000;
            if (poll_sec <= 0) poll_sec = 1;
            telegram_fetch_updates(impl, poll_sec);
            pthread_mutex_lock(&impl->q_lock);
        }
    }

    if (impl->q_head != NULL) {
        TelegramQueuedUpdate *qu = impl->q_head;
        impl->q_head = qu->next;
        if (!impl->q_head) impl->q_tail = NULL;
        impl->q_count--;

        impl->current_ev = qu;
        *out = qu->event;
        pthread_mutex_unlock(&impl->q_lock);
        return CBOT_OK;
    }

    pthread_mutex_unlock(&impl->q_lock);
    return CBOT_OK;
}

static void telegram_release_event(CbotAdapter *a, CbotEvent *ev) {
    (void)ev;
    if (!a || !a->impl) return;
    TelegramImpl *impl = (TelegramImpl *)a->impl;

    pthread_mutex_lock(&impl->q_lock);
    if (impl->current_ev) {
        telegram_free_queued_update(impl->current_ev);
        impl->current_ev = NULL;
    }
    pthread_mutex_unlock(&impl->q_lock);
}

static CbotStatus telegram_send_text(CbotAdapter *a, const char *chat_id, const char *text) {
    if (!a || !a->impl || !chat_id || !text) return CBOT_ERR_INVALID_ARG;
    TelegramImpl *impl = (TelegramImpl *)a->impl;

    size_t text_len = strlen(text);
    size_t esc_size = text_len * 6 + 1;
    char *escaped_text = malloc(esc_size);
    if (!escaped_text) return CBOT_ERR_NOMEM;
    json_escape_string(text, escaped_text, esc_size);

    char esc_chat[128];
    json_escape_string(chat_id, esc_chat, sizeof(esc_chat));

    size_t post_size = strlen(escaped_text) + strlen(esc_chat) + 64;
    char *post_json = malloc(post_size);
    if (!post_json) {
        free(escaped_text);
        return CBOT_ERR_NOMEM;
    }
    snprintf(post_json, post_size, "{\"chat_id\":\"%s\",\"text\":\"%s\"}", esc_chat, escaped_text);
    free(escaped_text);

    char url[512];
    snprintf(url, sizeof(url), "%s/bot%s/sendMessage", impl->base_url, impl->token);

    MemoryBuffer resp = {0};
    CbotStatus status = telegram_http_request(impl, url, post_json, 15, &resp);
    free(post_json);

    if (status == CBOT_OK && resp.data) {
        char ok_val[16];
        if (tg_json_get_raw(resp.data, "ok", ok_val, sizeof(ok_val))) {
            if (strcmp(ok_val, "true") != 0) {
                status = CBOT_ERR_PROTOCOL;
            }
        }
    }

    free(resp.data);
    return status;
}

static void telegram_destroy(CbotAdapter *a) {
    if (!a) return;
    if (a->impl) {
        TelegramImpl *impl = (TelegramImpl *)a->impl;
        pthread_mutex_lock(&impl->q_lock);
        TelegramQueuedUpdate *cur = impl->q_head;
        while (cur) {
            TelegramQueuedUpdate *next = cur->next;
            telegram_free_queued_update(cur);
            cur = next;
        }
        if (impl->current_ev) {
            telegram_free_queued_update(impl->current_ev);
        }
        pthread_mutex_unlock(&impl->q_lock);

        pthread_mutex_destroy(&impl->q_lock);
        pthread_cond_destroy(&impl->q_cond);

        free(impl->token);
        free(impl->base_url);
        free(impl);
    }
    free(a);
}

static const CbotAdapterVTable telegram_vtable = {
    .name = "telegram",
    .poll = telegram_poll,
    .send_text = telegram_send_text,
    .release_event = telegram_release_event,
    .destroy = telegram_destroy
};

static CbotAdapter *create_telegram_adapter(const char *token, int is_webhook) {
    if (!token) return NULL;

    pthread_once(&g_curl_init_once, init_curl_global);

    CbotAdapter *a = calloc(1, sizeof(*a));
    if (!a) return NULL;

    TelegramImpl *impl = calloc(1, sizeof(*impl));
    if (!impl) {
        free(a);
        return NULL;
    }

    impl->token = strdup(token);
    impl->base_url = strdup(TG_DEFAULT_BASE_URL);
    impl->is_webhook = is_webhook;
    impl->last_offset = 0;

    pthread_mutex_init(&impl->q_lock, NULL);
    pthread_cond_init(&impl->q_cond, NULL);

    a->vt = &telegram_vtable;
    a->impl = impl;
    return a;
}

CbotAdapter *cbot_adapter_telegram(const char *token) {
    return create_telegram_adapter(token, 0);
}

CbotAdapter *cbot_adapter_telegram_webhook(const char *token) {
    return create_telegram_adapter(token, 1);
}

void cbot_telegram_set_base_url(CbotAdapter *a, const char *base_url) {
    if (!a || !a->impl || !base_url) return;
    TelegramImpl *impl = (TelegramImpl *)a->impl;
    free(impl->base_url);
    impl->base_url = strdup(base_url);
}

CbotStatus cbot_telegram_set_webhook(const char *token, const char *webhook_url) {
    if (!token || !webhook_url) return CBOT_ERR_INVALID_ARG;
    pthread_once(&g_curl_init_once, init_curl_global);

    TelegramImpl temp_impl = {
        .token = (char *)token,
        .base_url = TG_DEFAULT_BASE_URL
    };

    char esc_url[512];
    json_escape_string(webhook_url, esc_url, sizeof(esc_url));

    char body[1024];
    snprintf(body, sizeof(body), "{\"url\":\"%s\"}", esc_url);

    char url[1024];
    snprintf(url, sizeof(url), "%s/bot%s/setWebhook", temp_impl.base_url, token);

    MemoryBuffer resp = {0};
    CbotStatus s = telegram_http_request(&temp_impl, url, body, 15, &resp);
    free(resp.data);
    return s;
}

CbotStatus cbot_telegram_delete_webhook(const char *token) {
    if (!token) return CBOT_ERR_INVALID_ARG;
    pthread_once(&g_curl_init_once, init_curl_global);

    TelegramImpl temp_impl = {
        .token = (char *)token,
        .base_url = TG_DEFAULT_BASE_URL
    };

    char url[1024];
    snprintf(url, sizeof(url), "%s/bot%s/deleteWebhook", temp_impl.base_url, token);

    MemoryBuffer resp = {0};
    CbotStatus s = telegram_http_request(&temp_impl, url, "{}", 15, &resp);
    free(resp.data);
    return s;
}

/* ---------- cweb Integration ---------- */
typedef struct {
    CwebApp *app;
    char path[256];
    CbotAdapter *adapter;
} TelegramCwebRoute;

#define MAX_CWEB_TG_ROUTES 16
static TelegramCwebRoute g_tg_cweb_routes[MAX_CWEB_TG_ROUTES];
static int g_num_tg_cweb_routes = 0;
static pthread_mutex_t g_tg_cweb_lock = PTHREAD_MUTEX_INITIALIZER;

static int telegram_cweb_handler(CwebRequest *request, CwebResponse *response) {
    CbotAdapter *matched_adapter = NULL;

    pthread_mutex_lock(&g_tg_cweb_lock);
    for (int i = 0; i < g_num_tg_cweb_routes; i++) {
        if (g_tg_cweb_routes[i].app == request->app &&
            strcmp(g_tg_cweb_routes[i].path, request->path) == 0) {
            matched_adapter = g_tg_cweb_routes[i].adapter;
            break;
        }
    }
    pthread_mutex_unlock(&g_tg_cweb_lock);

    if (matched_adapter && request->body[0] != '\0') {
        cbot_telegram_feed_update(matched_adapter, request->body);
    }

    cweb_json(response, 200, "{\"ok\":true}");
    return 200;
}

int cbot_telegram_attach_cweb(CbotAdapter *adapter, struct CwebApp *app, const char *path) {
    if (!adapter || !app || !path) return -1;

    pthread_mutex_lock(&g_tg_cweb_lock);
    if (g_num_tg_cweb_routes >= MAX_CWEB_TG_ROUTES) {
        pthread_mutex_unlock(&g_tg_cweb_lock);
        return -1;
    }

    g_tg_cweb_routes[g_num_tg_cweb_routes].app = app;
    strncpy(g_tg_cweb_routes[g_num_tg_cweb_routes].path, path, sizeof(g_tg_cweb_routes[0].path) - 1);
    g_tg_cweb_routes[g_num_tg_cweb_routes].adapter = adapter;
    g_num_tg_cweb_routes++;
    pthread_mutex_unlock(&g_tg_cweb_lock);

    if (!cweb_add_route(app, "POST", path, telegram_cweb_handler)) {
        return -1;
    }
    return 0;
}
