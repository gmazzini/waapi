// Gianluca Mazzini @2026- Version 1.23

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pwd.h>
#include <grp.h>
#include <math.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <png.h>
#include <ft2build.h>
#include <sqlite3.h>
#include FT_FREETYPE_H

#define WAAPI_CONFIG "/home/tools/mcp/work/waapi/config"
#define MAX_BODY 262144
#define MAX_OUTPUT 16384
#define MAX_URL 4096
#define MAX_FAMILY 32
#define MAX_ALIAS 64
#define MAX_LINE 4096
#define HTTP_TIMEOUT 12L
#define CONNECT_TIMEOUT 5L
#define WAAPI_USER "www-data"
#define PI 3.14159265358979323846

struct alias_entry {
    char *name;
    char *value;
};

struct config {
    char *phone_number_id;
    char *verify_token;
    char *app_secret;
    char *openweather_api_key;
    char *so_password;
    char *peso_access;
    char *token_file;
    char *auth_url;
    char *auth_key;
    char *stats_db;
    char *graph_version;
    char *cc_host;
    int cc_port;
    char *so_base_url;
    char *peso_url;
    char *display_url;
    char *rer_url;
    char *radio_url;
    char *font_file;
    char *family[MAX_FAMILY];
    int family_count;
    struct alias_entry alias[MAX_ALIAS];
    int alias_count;
};

struct memory {
    char *data;
    size_t len;
    size_t cap;
};

static void free_config(struct config *cfg);
static int load_config(const char *path, struct config *cfg);
static char *read_file_trim(const char *path);
static int appendf(char *buf, size_t size, size_t *used, const char *fmt, ...);
static size_t memory_write(void *ptr, size_t size, size_t nmemb, void *userdata);
static int http_get(const char *url, struct memory *mem, long *status);
static int http_post_json(const char *url, const char *token, const char *json, long *status);
static void strip_tags(char *s);
static int is_family(const struct config *cfg, const char *who);
static const char *resolve_alias(const struct config *cfg, const char *msg);
static int submit_auth_code(const struct config *cfg, const char *who, const char *msg);
static int verify_signature(const char *secret, const unsigned char *body, size_t body_len, const char *header);
static int constant_equal(const unsigned char *a, const unsigned char *b, size_t n);
static char *query_value(const char *query, const char *key);
static void url_decode(char *s);
static int send_whatsapp(const struct config *cfg, const char *who, const char *text, const char *image_url, const char *image_id);
static int upload_whatsapp_media(const struct config *cfg, const unsigned char *data, size_t size, char **media_id);

static int command_rer(const struct config *cfg, char *out, size_t out_size);
static int command_radio(const struct config *cfg, char *out, size_t out_size);
static int command_weather(const struct config *cfg, const char *arg, char *out, size_t out_size);
static int command_forecast(const struct config *cfg, const char *arg, char *out, size_t out_size);
static int command_pollution(const struct config *cfg, const char *arg, char *out, size_t out_size);
static int command_so(const struct config *cfg, const char *arg, char *out, size_t out_size);
static int command_cc(const struct config *cfg, const char *arg, char *out, size_t out_size);
static int command_peso(const struct config *cfg, const char *arg, char *out, size_t out_size);
static int command_display(const struct config *cfg, char *out, size_t out_size);
static int pollution_index(const double *limits, double value);
static char *make_url_arg(CURL *curl, const char *s);
static int build_random(char *out, size_t size);
static void cgi_status(int status, const char *text);
static int handle_get(const struct config *cfg);
static int handle_post(const struct config *cfg);
static int generate_clock(struct memory *png_data);
static int generate_word(const struct config *cfg, const char *word, struct memory *png_data);
static int stats_record(const struct config *cfg, const char *tool, int ok);
static int command_stats(const struct config *cfg, char *out, size_t out_size);
static const char *stats_tool(const struct config *cfg, const char *msg);
static int drop_privileges(void);

static int drop_privileges(void) {
    struct passwd *pw;

    if (geteuid() != 0) return 0;
    pw = getpwnam(WAAPI_USER);
    if (pw == NULL) return -1;
    if (initgroups(pw->pw_name, pw->pw_gid) != 0) return -1;
    if (setgid(pw->pw_gid) != 0) return -1;
    if (setuid(pw->pw_uid) != 0) return -1;
    return 0;
}

static char *dup_value(const char *s) {
    char *p;

    if (s == NULL) return NULL;
    p = strdup(s);
    return p;
}

static void set_value(char **dst, const char *value) {
    free(*dst);
    *dst = dup_value(value);
}

static char *trim(char *s) {
    char *end;

    while (*s != '\0' && isspace((unsigned char)*s)) s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

static void config_defaults(struct config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->cc_port = 55556;
    cfg->token_file = NULL;
    cfg->auth_url = dup_value("https://www.mazzini.org/auth");
    cfg->stats_db = dup_value("/home/tools/mcp/work/waapi/stats.db");
    cfg->graph_version = dup_value("v23.0");
    cfg->cc_host = dup_value("2001:678:1158:102:7a7b:8aff:fec3:b986");
    cfg->so_base_url = dup_value("http://home.mazzini.org:3333");
    cfg->peso_url = dup_value("https://google.mazzini.org/pesoGM.cgi");
    cfg->display_url = dup_value("https://music.mazzini.org/displayd?action=status");
    cfg->rer_url = dup_value("https://restdati.lepida.it/myout.php?istat=00008");
    cfg->radio_url = dup_value("https://radioacolori.net/?action=live&format=text");
    cfg->font_file = dup_value("/home/www/data/arial.ttf");
}

static int parse_config_line(struct config *cfg, char *line) {
    char *eq;
    char *key;
    char *value;
    char *sep;

    key = trim(line);
    if (*key == '\0' || *key == '#') return 0;
    eq = strchr(key, '=');
    if (eq == NULL) return -1;
    *eq = '\0';
    value = trim(eq + 1);
    key = trim(key);

    if (strcmp(key, "phone_number_id") == 0) set_value(&cfg->phone_number_id, value);
    else if (strcmp(key, "verify_token") == 0) set_value(&cfg->verify_token, value);
    else if (strcmp(key, "app_secret") == 0) set_value(&cfg->app_secret, value);
    else if (strcmp(key, "openweather_api_key") == 0) set_value(&cfg->openweather_api_key, value);
    else if (strcmp(key, "so_password") == 0) set_value(&cfg->so_password, value);
    else if (strcmp(key, "peso_access") == 0) set_value(&cfg->peso_access, value);
    else if (strcmp(key, "token_file") == 0) set_value(&cfg->token_file, value);
    else if (strcmp(key, "auth_url") == 0) set_value(&cfg->auth_url, value);
    else if (strcmp(key, "auth_key") == 0) set_value(&cfg->auth_key, value);
    else if (strcmp(key, "stats_db") == 0) set_value(&cfg->stats_db, value);
    else if (strcmp(key, "graph_version") == 0) set_value(&cfg->graph_version, value);
    else if (strcmp(key, "cc_host") == 0) set_value(&cfg->cc_host, value);
    else if (strcmp(key, "cc_port") == 0) cfg->cc_port = atoi(value);
    else if (strcmp(key, "so_base_url") == 0) set_value(&cfg->so_base_url, value);
    else if (strcmp(key, "peso_url") == 0) set_value(&cfg->peso_url, value);
    else if (strcmp(key, "display_url") == 0) set_value(&cfg->display_url, value);
    else if (strcmp(key, "rer_url") == 0) set_value(&cfg->rer_url, value);
    else if (strcmp(key, "radio_url") == 0) set_value(&cfg->radio_url, value);
    else if (strcmp(key, "font_file") == 0) set_value(&cfg->font_file, value);
    else if (strcmp(key, "family") == 0) {
        if (cfg->family_count >= MAX_FAMILY) return -1;
        cfg->family[cfg->family_count++] = dup_value(value);
    } else if (strcmp(key, "alias") == 0) {
        if (cfg->alias_count >= MAX_ALIAS) return -1;
        sep = strchr(value, '|');
        if (sep == NULL) return -1;
        *sep = '\0';
        cfg->alias[cfg->alias_count].name = dup_value(trim(value));
        cfg->alias[cfg->alias_count].value = dup_value(trim(sep + 1));
        cfg->alias_count++;
    }
    return 0;
}

static int load_config(const char *path, struct config *cfg) {
    FILE *fp;
    char line[MAX_LINE];
    int rc;

    config_defaults(cfg);
    fp = fopen(path, "r");
    if (fp == NULL) return -1;
    rc = 0;
    for (; fgets(line, sizeof(line), fp) != NULL;) {
        if (parse_config_line(cfg, line) != 0) {
            rc = -1;
            break;
        }
    }
    if (ferror(fp)) rc = -1;
    fclose(fp);
    return rc;
}

static void free_config(struct config *cfg) {
    int i;

    free(cfg->phone_number_id);
    free(cfg->verify_token);
    free(cfg->app_secret);
    free(cfg->openweather_api_key);
    free(cfg->so_password);
    free(cfg->peso_access);
    free(cfg->token_file);
    free(cfg->auth_url);
    free(cfg->auth_key);
    free(cfg->stats_db);
    free(cfg->graph_version);
    free(cfg->cc_host);
    free(cfg->so_base_url);
    free(cfg->peso_url);
    free(cfg->display_url);
    free(cfg->rer_url);
    free(cfg->radio_url);
    free(cfg->font_file);
    for (i = 0; i < cfg->family_count; i++) free(cfg->family[i]);
    for (i = 0; i < cfg->alias_count; i++) {
        free(cfg->alias[i].name);
        free(cfg->alias[i].value);
    }
}

static char *read_file_trim(const char *path) {
    FILE *fp;
    long len;
    char *buf;

    fp = fopen(path, "rb");
    if (fp == NULL) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    len = ftell(fp);
    if (len < 0 || len > 1024 * 1024 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    buf = malloc((size_t)len + 1);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    if (len > 0 && fread(buf, 1, (size_t)len, fp) != (size_t)len) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    buf[len] = '\0';
    trim(buf);
    return buf;
}

static int appendf(char *buf, size_t size, size_t *used, const char *fmt, ...) {
    va_list ap;
    int n;

    if (*used >= size) return -1;
    va_start(ap, fmt);
    n = vsnprintf(buf + *used, size - *used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= size - *used) return -1;
    *used += (size_t)n;
    return 0;
}

static int memory_reserve(struct memory *mem, size_t add) {
    size_t need;
    size_t cap;
    char *p;

    if (add > MAX_BODY || mem->len > MAX_BODY - add) return -1;
    need = mem->len + add + 1;
    if (need <= mem->cap) return 0;
    cap = mem->cap == 0 ? 4096 : mem->cap;
    for (; cap < need;) {
        if (cap > MAX_BODY / 2) {
            cap = MAX_BODY + 1;
            break;
        }
        cap *= 2;
    }
    p = realloc(mem->data, cap);
    if (p == NULL) return -1;
    mem->data = p;
    mem->cap = cap;
    return 0;
}

static size_t memory_write(void *ptr, size_t size, size_t nmemb, void *userdata) {
    struct memory *mem;
    size_t n;

    mem = userdata;
    if (nmemb != 0 && size > (size_t)-1 / nmemb) return 0;
    n = size * nmemb;
    if (memory_reserve(mem, n) != 0) return 0;
    memcpy(mem->data + mem->len, ptr, n);
    mem->len += n;
    mem->data[mem->len] = '\0';
    return n;
}

static int http_common(CURL *curl, struct memory *mem, long *status) {
    CURLcode rc;

    mem->data = NULL;
    mem->len = 0;
    mem->cap = 0;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, memory_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, mem);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, HTTP_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "waapi/1.18");
    rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) return -1;
    if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status) != CURLE_OK) return -1;
    return *status >= 200 && *status < 300 ? 0 : -1;
}

static int http_get(const char *url, struct memory *mem, long *status) {
    CURL *curl;
    int rc;

    curl = curl_easy_init();
    if (curl == NULL) return -1;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    rc = http_common(curl, mem, status);
    curl_easy_cleanup(curl);
    return rc;
}

static int http_post_json(const char *url, const char *token, const char *json, long *status) {
    CURL *curl;
    struct curl_slist *headers;
    struct memory mem;
    char auth[2048];
    int rc;

    if (snprintf(auth, sizeof(auth), "Authorization: Bearer %s", token) >= (int)sizeof(auth)) return -1;
    curl = curl_easy_init();
    if (curl == NULL) return -1;
    headers = NULL;
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(json));
    rc = http_common(curl, &mem, status);
    free(mem.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return rc;
}

static void strip_tags(char *s) {
    char *src;
    char *dst;
    int tag;

    src = s;
    dst = s;
    tag = 0;
    for (; *src != '\0'; src++) {
        if (*src == '<') tag = 1;
        else if (*src == '>') tag = 0;
        else if (!tag) *dst++ = *src;
    }
    *dst = '\0';
}

static int is_family(const struct config *cfg, const char *who) {
    int i;

    for (i = 0; i < cfg->family_count; i++) {
        if (strcmp(cfg->family[i], who) == 0) return 1;
    }
    return 0;
}

static const char *resolve_alias(const struct config *cfg, const char *msg) {
    int i;

    for (i = 0; i < cfg->alias_count; i++) {
        if (strcmp(cfg->alias[i].name, msg) == 0) return cfg->alias[i].value;
    }
    return msg;
}

static int valid_phone(const char *s) {
    size_t i;
    size_t n;

    n = strlen(s);
    if (n < 6 || n > 20) return 0;
    for (i = 0; i < n; i++) if (!isdigit((unsigned char)s[i])) return 0;
    return 1;
}

static int submit_auth_code(const struct config *cfg, const char *who, const char *msg) {
    CURL *curl;
    CURLcode crc;
    struct curl_slist *headers;
    struct memory mem;
    cJSON *root;
    char code[6];
    char *json;
    long status;
    int i;
    int rc;

    if (!valid_phone(who)) return -1;
    if (strlen(msg) < 5) return 0;
    for (i = 0; i < 5; i++) {
        if (!isdigit((unsigned char)msg[i])) return 0;
        code[i] = msg[i];
    }
    code[5] = '\0';
    if (cfg->auth_url == NULL || strncmp(cfg->auth_url, "https://", 8) != 0 ||
        cfg->auth_key == NULL || *cfg->auth_key == '\0') return -1;

    root = cJSON_CreateObject();
    if (root == NULL) return -1;
    cJSON_AddStringToObject(root, "action", "incoming");
    cJSON_AddStringToObject(root, "method", "whatsapp");
    cJSON_AddStringToObject(root, "from", who);
    cJSON_AddStringToObject(root, "text", code);
    cJSON_AddStringToObject(root, "key", cfg->auth_key);
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) return -1;

    curl = curl_easy_init();
    if (curl == NULL) {
        free(json);
        return -1;
    }
    headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, cfg->auth_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(json));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, HTTP_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "waapi/1.18");
    mem.data = NULL;
    mem.len = 0;
    mem.cap = 0;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, memory_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mem);
    crc = curl_easy_perform(curl);
    status = 0;
    if (crc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    rc = crc == CURLE_OK && status >= 200 && status < 300 ? 1 : -1;
    free(mem.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(json);
    return rc;
}

static int hex_decode(const char *hex, unsigned char *out, size_t out_len) {
    size_t i;
    int hi;
    int lo;

    if (strlen(hex) != out_len * 2) return -1;
    for (i = 0; i < out_len; i++) {
        hi = isdigit((unsigned char)hex[i * 2]) ? hex[i * 2] - '0' : tolower((unsigned char)hex[i * 2]) - 'a' + 10;
        lo = isdigit((unsigned char)hex[i * 2 + 1]) ? hex[i * 2 + 1] - '0' : tolower((unsigned char)hex[i * 2 + 1]) - 'a' + 10;
        if (hi < 0 || hi > 15 || lo < 0 || lo > 15) return -1;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return 0;
}

static int verify_signature(const char *secret, const unsigned char *body, size_t body_len, const char *header) {
    EVP_MAC *mac;
    EVP_MAC_CTX *ctx;
    OSSL_PARAM params[2];
    unsigned char expected[32];
    unsigned char supplied[32];
    size_t out_len;
    char digest[] = "SHA256";
    int ok;

    if (secret == NULL || *secret == '\0') return 1;
    if (header == NULL || strncmp(header, "sha256=", 7) != 0) return 0;
    if (hex_decode(header + 7, supplied, sizeof(supplied)) != 0) return 0;
    mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
    if (mac == NULL) return 0;
    ctx = EVP_MAC_CTX_new(mac);
    EVP_MAC_free(mac);
    if (ctx == NULL) return 0;
    params[0] = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest, 0);
    params[1] = OSSL_PARAM_construct_end();
    ok = EVP_MAC_init(ctx, (const unsigned char *)secret, strlen(secret), params) == 1 &&
         EVP_MAC_update(ctx, body, body_len) == 1 &&
         EVP_MAC_final(ctx, expected, &out_len, sizeof(expected)) == 1 && out_len == sizeof(expected) &&
         constant_equal(expected, supplied, sizeof(expected));
    EVP_MAC_CTX_free(ctx);
    return ok;
}

static int constant_equal(const unsigned char *a, const unsigned char *b, size_t n) {
    unsigned char diff;
    size_t i;

    diff = 0;
    for (i = 0; i < n; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

static void url_decode(char *s) {
    char *src;
    char *dst;
    int hi;
    int lo;

    src = s;
    dst = s;
    for (; *src != '\0'; src++) {
        if (*src == '+') *dst++ = ' ';
        else if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            hi = isdigit((unsigned char)src[1]) ? src[1] - '0' : tolower((unsigned char)src[1]) - 'a' + 10;
            lo = isdigit((unsigned char)src[2]) ? src[2] - '0' : tolower((unsigned char)src[2]) - 'a' + 10;
            *dst++ = (char)((hi << 4) | lo);
            src += 2;
        } else *dst++ = *src;
    }
    *dst = '\0';
}

static char *query_value(const char *query, const char *key) {
    char *copy;
    char *part;
    char *save;
    char *eq;
    char *result;

    if (query == NULL) return NULL;
    copy = strdup(query);
    if (copy == NULL) return NULL;
    result = NULL;
    save = NULL;
    for (part = strtok_r(copy, "&", &save); part != NULL; part = strtok_r(NULL, "&", &save)) {
        eq = strchr(part, '=');
        if (eq == NULL) continue;
        *eq = '\0';
        url_decode(part);
        if (strcmp(part, key) == 0) {
            result = strdup(eq + 1);
            if (result != NULL) url_decode(result);
            break;
        }
    }
    free(copy);
    return result;
}

static int send_whatsapp(const struct config *cfg, const char *who, const char *text, const char *image_url, const char *image_id) {
    cJSON *root;
    cJSON *obj;
    char *json;
    char *token;
    char url[MAX_URL];
    long status;
    int rc;

    if (cfg->phone_number_id == NULL || *cfg->phone_number_id == '\0') return -1;
    token = read_file_trim(cfg->token_file);
    if (token == NULL || *token == '\0') {
        free(token);
        return -1;
    }
    root = cJSON_CreateObject();
    if (root == NULL) {
        free(token);
        return -1;
    }
    cJSON_AddStringToObject(root, "messaging_product", "whatsapp");
    cJSON_AddStringToObject(root, "to", who);
    if (text != NULL && *text != '\0') {
        cJSON_AddStringToObject(root, "type", "text");
        obj = cJSON_AddObjectToObject(root, "text");
        cJSON_AddStringToObject(obj, "body", text);
    } else {
        cJSON_AddStringToObject(root, "type", "image");
        obj = cJSON_AddObjectToObject(root, "image");
        if (image_id != NULL && *image_id != '\0') cJSON_AddStringToObject(obj, "id", image_id);
        else if (image_url != NULL && *image_url != '\0') cJSON_AddStringToObject(obj, "link", image_url);
        else {
            cJSON_Delete(root);
            free(token);
            return -1;
        }
    }
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        free(token);
        return -1;
    }
    if (snprintf(url, sizeof(url), "https://graph.facebook.com/%s/%s/messages", cfg->graph_version, cfg->phone_number_id) >= (int)sizeof(url)) {
        free(json);
        free(token);
        return -1;
    }
    rc = http_post_json(url, token, json, &status);
    free(json);
    free(token);
    return rc;
}

static int upload_whatsapp_media(const struct config *cfg, const unsigned char *data, size_t size, char **media_id) {
    CURL *curl;
    curl_mime *mime;
    curl_mimepart *part;
    struct curl_slist *headers;
    struct memory mem;
    cJSON *root;
    cJSON *item;
    char *token;
    char auth[2048];
    char url[MAX_URL];
    long status;
    int rc;

    *media_id = NULL;
    if (data == NULL || size == 0 || cfg->phone_number_id == NULL || *cfg->phone_number_id == '\0') return -1;
    token = read_file_trim(cfg->token_file);
    if (token == NULL || *token == '\0') {
        free(token);
        return -1;
    }
    if (snprintf(auth, sizeof(auth), "Authorization: Bearer %s", token) >= (int)sizeof(auth) ||
        snprintf(url, sizeof(url), "https://graph.facebook.com/%s/%s/media", cfg->graph_version, cfg->phone_number_id) >= (int)sizeof(url)) {
        free(token);
        return -1;
    }
    curl = curl_easy_init();
    if (curl == NULL) {
        free(token);
        return -1;
    }
    headers = NULL;
    mime = NULL;
    headers = curl_slist_append(headers, auth);
    mime = curl_mime_init(curl);
    if (headers == NULL || mime == NULL) {
        curl_slist_free_all(headers);
        curl_mime_free(mime);
        curl_easy_cleanup(curl);
        free(token);
        return -1;
    }
    part = curl_mime_addpart(mime);
    if (part == NULL || curl_mime_name(part, "messaging_product") != CURLE_OK ||
        curl_mime_data(part, "whatsapp", CURL_ZERO_TERMINATED) != CURLE_OK) {
        curl_mime_free(mime);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(token);
        return -1;
    }
    part = curl_mime_addpart(mime);
    if (part == NULL || curl_mime_name(part, "file") != CURLE_OK ||
        curl_mime_filename(part, "image.png") != CURLE_OK || curl_mime_type(part, "image/png") != CURLE_OK ||
        curl_mime_data(part, (const char *)data, size) != CURLE_OK) {
        curl_mime_free(mime);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(token);
        return -1;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    rc = http_common(curl, &mem, &status);
    if (rc == 0) {
        root = cJSON_Parse(mem.data);
        if (root != NULL) {
            item = cJSON_GetObjectItemCaseSensitive(root, "id");
            if (cJSON_IsString(item) && item->valuestring != NULL && *item->valuestring != '\0') *media_id = strdup(item->valuestring);
            cJSON_Delete(root);
        }
        if (*media_id == NULL) rc = -1;
    }
    free(mem.data);
    curl_mime_free(mime);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(token);
    return rc;
}

static int command_rer(const struct config *cfg, char *out, size_t out_size) {
    struct memory mem;
    cJSON *root;
    cJSON *regione;
    cJSON *item;
    long status;
    size_t used;

    if (http_get(cfg->rer_url, &mem, &status) != 0) {
        free(mem.data);
        return -1;
    }
    root = cJSON_Parse(mem.data);
    free(mem.data);
    if (root == NULL) return -1;
    regione = cJSON_GetObjectItemCaseSensitive(root, "regione");
    if (!cJSON_IsObject(regione)) {
        cJSON_Delete(root);
        return -1;
    }
    used = 0;
    cJSON_ArrayForEach(item, regione) {
        if (cJSON_IsString(item) && item->string != NULL) appendf(out, out_size, &used, "%s %s\n", item->string, item->valuestring);
        else if (cJSON_IsNumber(item) && item->string != NULL) appendf(out, out_size, &used, "%s %.15g\n", item->string, item->valuedouble);
    }
    cJSON_Delete(root);
    return used > 0 ? 0 : -1;
}

static int command_radio(const struct config *cfg, char *out, size_t out_size) {
    struct memory mem;
    long status;

    if (http_get(cfg->radio_url, &mem, &status) != 0) {
        free(mem.data);
        return -1;
    }
    strip_tags(mem.data);
    snprintf(out, out_size, "%.*s", (int)out_size - 1, mem.data);
    free(mem.data);
    return *out != '\0' ? 0 : -1;
}

static char *make_url_arg(CURL *curl, const char *s) {
    return curl_easy_escape(curl, s, 0);
}

static int command_weather(const struct config *cfg, const char *arg, char *out, size_t out_size) {
    CURL *curl;
    char *where;
    char url[MAX_URL];
    struct memory mem;
    cJSON *root;
    cJSON *sys;
    cJSON *weather;
    cJSON *main;
    cJSON *wind;
    cJSON *item;
    const char *name;
    const char *country;
    const char *desc;
    long status;

    if (cfg->openweather_api_key == NULL) return -1;
    curl = curl_easy_init();
    if (curl == NULL) return -1;
    where = make_url_arg(curl, *arg != '\0' ? arg : "bologna,it");
    if (where == NULL) {
        curl_easy_cleanup(curl);
        return -1;
    }
    snprintf(url, sizeof(url), "https://api.openweathermap.org/data/2.5/weather?q=%s&appid=%s&units=metric", where, cfg->openweather_api_key);
    curl_free(where);
    curl_easy_cleanup(curl);
    if (http_get(url, &mem, &status) != 0) {
        free(mem.data);
        return -1;
    }
    root = cJSON_Parse(mem.data);
    free(mem.data);
    if (root == NULL) return -1;
    item = cJSON_GetObjectItemCaseSensitive(root, "name");
    sys = cJSON_GetObjectItemCaseSensitive(root, "sys");
    weather = cJSON_GetObjectItemCaseSensitive(root, "weather");
    main = cJSON_GetObjectItemCaseSensitive(root, "main");
    wind = cJSON_GetObjectItemCaseSensitive(root, "wind");
    name = cJSON_IsString(item) ? item->valuestring : "?";
    item = cJSON_IsObject(sys) ? cJSON_GetObjectItemCaseSensitive(sys, "country") : NULL;
    country = cJSON_IsString(item) ? item->valuestring : "?";
    item = cJSON_IsArray(weather) ? cJSON_GetArrayItem(weather, 0) : NULL;
    item = cJSON_IsObject(item) ? cJSON_GetObjectItemCaseSensitive(item, "description") : NULL;
    desc = cJSON_IsString(item) ? item->valuestring : "?";
    if (!cJSON_IsObject(main) || !cJSON_IsObject(wind) ||
        !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(main, "temp")) ||
        !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(main, "feels_like")) ||
        !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(main, "pressure")) ||
        !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(main, "humidity")) ||
        !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(root, "visibility")) ||
        !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(wind, "speed"))) {
        cJSON_Delete(root);
        return -1;
    }
    snprintf(out, out_size, "%s,%s\n%s\nTemp:       %7.1f\nFeel:       %7.1f\nPressure:   %5d\nHumidity:   %5d\nVisibility: %5d\nWind:       %7.1f\n",
             name, country, desc,
             cJSON_GetObjectItemCaseSensitive(main, "temp")->valuedouble,
             cJSON_GetObjectItemCaseSensitive(main, "feels_like")->valuedouble,
             cJSON_GetObjectItemCaseSensitive(main, "pressure")->valueint,
             cJSON_GetObjectItemCaseSensitive(main, "humidity")->valueint,
             cJSON_GetObjectItemCaseSensitive(root, "visibility")->valueint,
             cJSON_GetObjectItemCaseSensitive(wind, "speed")->valuedouble);
    cJSON_Delete(root);
    return 0;
}

static int command_forecast(const struct config *cfg, const char *arg, char *out, size_t out_size) {
    CURL *curl;
    char *where;
    char url[MAX_URL];
    struct memory mem;
    cJSON *root;
    cJSON *city;
    cJSON *list;
    cJSON *v;
    cJSON *main;
    cJSON *weather;
    cJSON *item;
    const char *name;
    const char *country;
    const char *dt;
    const char *kind;
    long status;
    size_t used;

    if (cfg->openweather_api_key == NULL) return -1;
    curl = curl_easy_init();
    if (curl == NULL) return -1;
    where = make_url_arg(curl, *arg != '\0' ? arg : "bologna,it");
    if (where == NULL) {
        curl_easy_cleanup(curl);
        return -1;
    }
    snprintf(url, sizeof(url), "https://api.openweathermap.org/data/2.5/forecast?q=%s&appid=%s&units=metric", where, cfg->openweather_api_key);
    curl_free(where);
    curl_easy_cleanup(curl);
    if (http_get(url, &mem, &status) != 0) {
        free(mem.data);
        return -1;
    }
    root = cJSON_Parse(mem.data);
    free(mem.data);
    if (root == NULL) return -1;
    city = cJSON_GetObjectItemCaseSensitive(root, "city");
    list = cJSON_GetObjectItemCaseSensitive(root, "list");
    item = cJSON_IsObject(city) ? cJSON_GetObjectItemCaseSensitive(city, "name") : NULL;
    name = cJSON_IsString(item) ? item->valuestring : "?";
    item = cJSON_IsObject(city) ? cJSON_GetObjectItemCaseSensitive(city, "country") : NULL;
    country = cJSON_IsString(item) ? item->valuestring : "?";
    used = 0;
    appendf(out, out_size, &used, "%s,%s\n", name, country);
    cJSON_ArrayForEach(v, list) {
        item = cJSON_GetObjectItemCaseSensitive(v, "dt_txt");
        dt = cJSON_IsString(item) ? item->valuestring : "?";
        main = cJSON_GetObjectItemCaseSensitive(v, "main");
        weather = cJSON_GetObjectItemCaseSensitive(v, "weather");
        item = cJSON_IsArray(weather) ? cJSON_GetArrayItem(weather, 0) : NULL;
        item = cJSON_IsObject(item) ? cJSON_GetObjectItemCaseSensitive(item, "main") : NULL;
        kind = cJSON_IsString(item) ? item->valuestring : "?";
        item = cJSON_IsObject(main) ? cJSON_GetObjectItemCaseSensitive(main, "temp") : NULL;
        if (!cJSON_IsNumber(item)) continue;
        if (appendf(out, out_size, &used, "%.16s / %4.1f / %s\n", dt, item->valuedouble, kind) != 0) break;
    }
    cJSON_Delete(root);
    return used > 0 ? 0 : -1;
}

static int pollution_index(const double *limits, double value) {
    int i;

    for (i = 0; i < 5; i++) if (value < limits[i]) break;
    return i + 1;
}

static int command_pollution(const struct config *cfg, const char *arg, char *out, size_t out_size) {
    static const double o3_l[] = {50, 100, 130, 240, 380};
    static const double no2_l[] = {40, 90, 120, 230, 340};
    static const double so2_l[] = {100, 200, 350, 500, 750};
    static const double pm10_l[] = {20, 40, 50, 100, 150};
    static const double pm25_l[] = {10, 20, 25, 50, 75};
    CURL *curl;
    char *where;
    char url[MAX_URL];
    struct memory mem;
    cJSON *root;
    cJSON *geo;
    cJSON *item;
    cJSON *list;
    cJSON *components;
    const char *name;
    const char *country;
    double lat;
    double lon;
    double o3;
    double no2;
    double so2;
    double pm10;
    double pm25;
    int i1;
    int i2;
    int i3;
    int i4;
    int i5;
    int ei;
    long status;

    if (cfg->openweather_api_key == NULL) return -1;
    curl = curl_easy_init();
    if (curl == NULL) return -1;
    where = make_url_arg(curl, *arg != '\0' ? arg : "bologna,it");
    if (where == NULL) {
        curl_easy_cleanup(curl);
        return -1;
    }
    snprintf(url, sizeof(url), "https://api.openweathermap.org/geo/1.0/direct?q=%s&limit=1&appid=%s", where, cfg->openweather_api_key);
    curl_free(where);
    curl_easy_cleanup(curl);
    if (http_get(url, &mem, &status) != 0) {
        free(mem.data);
        return -1;
    }
    root = cJSON_Parse(mem.data);
    free(mem.data);
    if (root == NULL || !cJSON_IsArray(root) || cJSON_GetArraySize(root) < 1) {
        cJSON_Delete(root);
        return -1;
    }
    geo = cJSON_GetArrayItem(root, 0);
    item = cJSON_GetObjectItemCaseSensitive(geo, "name");
    name = cJSON_IsString(item) ? item->valuestring : "?";
    item = cJSON_GetObjectItemCaseSensitive(geo, "country");
    country = cJSON_IsString(item) ? item->valuestring : "?";
    item = cJSON_GetObjectItemCaseSensitive(geo, "lat");
    lat = cJSON_IsNumber(item) ? item->valuedouble : 0.0;
    item = cJSON_GetObjectItemCaseSensitive(geo, "lon");
    lon = cJSON_IsNumber(item) ? item->valuedouble : 0.0;
    snprintf(out, out_size, "%s,%s\n", name, country);
    cJSON_Delete(root);
    snprintf(url, sizeof(url), "https://api.openweathermap.org/data/2.5/air_pollution?lat=%.8f&lon=%.8f&appid=%s", lat, lon, cfg->openweather_api_key);
    if (http_get(url, &mem, &status) != 0) {
        free(mem.data);
        return -1;
    }
    root = cJSON_Parse(mem.data);
    free(mem.data);
    if (root == NULL) return -1;
    list = cJSON_GetObjectItemCaseSensitive(root, "list");
    item = cJSON_IsArray(list) ? cJSON_GetArrayItem(list, 0) : NULL;
    components = cJSON_IsObject(item) ? cJSON_GetObjectItemCaseSensitive(item, "components") : NULL;
    if (!cJSON_IsObject(components)) {
        cJSON_Delete(root);
        return -1;
    }
    if (!cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(components, "o3")) ||
        !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(components, "no2")) ||
        !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(components, "so2")) ||
        !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(components, "pm10")) ||
        !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(components, "pm2_5")) ||
        !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(components, "co")) ||
        !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(components, "no")) ||
        !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(components, "nh3"))) {
        cJSON_Delete(root);
        return -1;
    }
#define V(name) cJSON_GetObjectItemCaseSensitive(components, name)->valuedouble
    o3 = V("o3"); no2 = V("no2"); so2 = V("so2"); pm10 = V("pm10"); pm25 = V("pm2_5");
    i1 = pollution_index(o3_l, o3); i2 = pollution_index(no2_l, no2); i3 = pollution_index(so2_l, so2);
    i4 = pollution_index(pm10_l, pm10); i5 = pollution_index(pm25_l, pm25);
    ei = i1; if (i2 > ei) ei = i2; if (i3 > ei) ei = i3; if (i4 > ei) ei = i4; if (i5 > ei) ei = i5;
    snprintf(out + strlen(out), out_size - strlen(out),
             "o3 %.1f AIQ=%d\nno2 %.1f AIQ=%d\nso2 %.1f AIQ=%d\npm10 %.1f AIQ=%d\npm2_5 %.1f AIQ=%d\nAIQ %d\nco %.1f\nno %.1f\nnh3 %.1f\n",
             o3, i1, no2, i2, so2, i3, pm10, i4, pm25, i5, ei, V("co"), V("no"), V("nh3"));
#undef V
    cJSON_Delete(root);
    return 0;
}

static int command_so(const struct config *cfg, const char *arg, char *out, size_t out_size) {
    CURL *curl;
    char *escaped;
    char url[MAX_URL];
    struct memory mem;
    long status;

    if (cfg->so_password == NULL || *cfg->so_password == '\0') return -1;
    curl = curl_easy_init();
    if (curl == NULL) return -1;
    escaped = make_url_arg(curl, arg);
    if (escaped == NULL) {
        curl_easy_cleanup(curl);
        return -1;
    }
    snprintf(url, sizeof(url), "%s/%s/%s", cfg->so_base_url, cfg->so_password, escaped);
    curl_free(escaped);
    curl_easy_cleanup(curl);
    if (http_get(url, &mem, &status) != 0) {
        free(mem.data);
        return -1;
    }
    strip_tags(mem.data);
    snprintf(out, out_size, "%.*s", (int)out_size - 1, mem.data);
    free(mem.data);
    return 0;
}

static int command_cc(const struct config *cfg, const char *arg, char *out, size_t out_size) {
    struct sockaddr_in6 addr;
    struct timeval tv;
    char buf[2001];
    char *next;
    char *end;
    size_t used;
    ssize_t n;
    int fd;

    if (strlen(arg) > 1024) return -1;
    fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons((unsigned short)cfg->cc_port);
    if (inet_pton(AF_INET6, cfg->cc_host, &addr.sin6_addr) != 1) {
        close(fd);
        return -1;
    }
    if (sendto(fd, arg, strlen(arg), 0, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    used = 0;
    for (;;) {
        n = recvfrom(fd, buf, sizeof(buf) - 1, 0, NULL, NULL);
        if (n < 0) break;
        buf[n] = '\0';
        end = strstr(buf, "<end>");
        next = strstr(buf, "<next>");
        if (end != NULL) {
            *end = '\0';
            appendf(out, out_size, &used, "%s", buf);
            break;
        }
        if (next != NULL) {
            memmove(next, next + 6, strlen(next + 6) + 1);
        }
        if (appendf(out, out_size, &used, "%s", buf) != 0) break;
    }
    close(fd);
    return used > 0 ? 0 : -1;
}

static int command_peso(const struct config *cfg, const char *arg, char *out, size_t out_size) {
    CURL *curl;
    char *escaped;
    char url[MAX_URL];
    struct memory mem;
    long status;
    int rc;

    if (cfg->peso_access == NULL || *cfg->peso_access == '\0') return -1;
    curl = curl_easy_init();
    if (curl == NULL) return -1;
    escaped = make_url_arg(curl, arg);
    if (escaped == NULL) {
        curl_easy_cleanup(curl);
        return -1;
    }
    snprintf(url, sizeof(url), "%s?%s,%s", cfg->peso_url, cfg->peso_access, escaped);
    curl_free(escaped);
    curl_easy_cleanup(curl);
    rc = http_get(url, &mem, &status);
    free(mem.data);
    if (rc != 0) return -1;
    snprintf(out, out_size, "Peso set to %s\n", arg);
    return 0;
}

static int command_display(const struct config *cfg, char *out, size_t out_size) {
    struct memory mem;
    long status;
    size_t used;

    if (cfg->display_url == NULL || *cfg->display_url == '\0') return -1;
    if (http_get(cfg->display_url, &mem, &status) != 0) {
        free(mem.data);
        return -1;
    }
    used = 0;
    if (appendf(out, out_size, &used, "```%s```", mem.data) != 0) {
        free(mem.data);
        return -1;
    }
    free(mem.data);
    return 0;
}

static int build_random(char *out, size_t size) {
    static const char hex[] = "0123456789abcdef";
    unsigned char raw[6];
    int i;

    if (size < 13 || RAND_bytes(raw, sizeof(raw)) != 1) return -1;
    for (i = 0; i < 6; i++) {
        out[i * 2] = hex[raw[i] >> 4];
        out[i * 2 + 1] = hex[raw[i] & 15];
    }
    out[12] = '\0';
    return 0;
}

static void cgi_status(int status, const char *text) {
    printf("Status: %d %s\r\nContent-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\n\r\n", status, text);
}

static int handle_get(const struct config *cfg) {
    const char *query;
    char *mode;
    char *token;
    char *challenge;
    int ok;

    query = getenv("QUERY_STRING");
    mode = query_value(query, "hub.mode");
    token = query_value(query, "hub.verify_token");
    challenge = query_value(query, "hub.challenge");
    ok = mode != NULL && token != NULL && challenge != NULL && strcmp(mode, "subscribe") == 0 &&
         cfg->verify_token != NULL && *cfg->verify_token != '\0' && strcmp(token, cfg->verify_token) == 0;
    if (ok) {
        cgi_status(200, "OK");
        printf("%s", challenge);
    } else {
        cgi_status(403, "Forbidden");
        printf("Forbidden\n");
    }
    free(mode);
    free(token);
    free(challenge);
    return ok ? 0 : 1;
}


static void image_pixel(unsigned char *image, int width, int height, int x, int y, unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    unsigned char *p;

    if (x < 0 || y < 0 || x >= width || y >= height) return;
    p = image + ((size_t)y * (size_t)width + (size_t)x) * 4;
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = a;
}

static void image_rect(unsigned char *image, int width, int height, int x1, int y1, int x2, int y2, unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    int x;
    int y;

    for (y = y1; y <= y2; y++) {
        for (x = x1; x <= x2; x++) image_pixel(image, width, height, x, y, r, g, b, a);
    }
}

static void image_circle(unsigned char *image, int width, int height, int cx, int cy, int radius, unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    int x;
    int y;

    for (y = -radius; y <= radius; y++) {
        for (x = -radius; x <= radius; x++) {
            if (x * x + y * y <= radius * radius) image_pixel(image, width, height, cx + x, cy + y, r, g, b, a);
        }
    }
}

static void image_line(unsigned char *image, int width, int height, int x1, int y1, int x2, int y2, int thickness, unsigned char r, unsigned char g, unsigned char b) {
    int dx;
    int dy;
    int steps;
    int i;
    int x;
    int y;

    dx = x2 - x1;
    dy = y2 - y1;
    steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    if (steps == 0) {
        image_circle(image, width, height, x1, y1, thickness / 2, r, g, b, 255);
        return;
    }
    for (i = 0; i <= steps; i++) {
        x = x1 + dx * i / steps;
        y = y1 + dy * i / steps;
        image_circle(image, width, height, x, y, thickness / 2, r, g, b, 255);
    }
}

static void png_memory_write(png_structp png, png_bytep data, png_size_t length) {
    struct memory *mem;

    mem = png_get_io_ptr(png);
    if (mem == NULL || memory_reserve(mem, (size_t)length) != 0) png_error(png, "PNG buffer overflow");
    memcpy(mem->data + mem->len, data, (size_t)length);
    mem->len += (size_t)length;
    mem->data[mem->len] = '\0';
}

static void png_memory_flush(png_structp png) {
    (void)png;
}

static int encode_png(unsigned char *image, int width, int height, struct memory *mem) {
    png_structp png;
    png_infop info;
    png_bytep *rows;
    int y;

    mem->data = NULL;
    mem->len = 0;
    mem->cap = 0;
    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png == NULL) return -1;
    info = png_create_info_struct(png);
    if (info == NULL) {
        png_destroy_write_struct(&png, NULL);
        return -1;
    }
    rows = malloc((size_t)height * sizeof(*rows));
    if (rows == NULL) {
        png_destroy_write_struct(&png, &info);
        return -1;
    }
    for (y = 0; y < height; y++) rows[y] = image + (size_t)y * (size_t)width * 4;
    if (setjmp(png_jmpbuf(png))) {
        free(rows);
        free(mem->data);
        mem->data = NULL;
        mem->len = 0;
        mem->cap = 0;
        png_destroy_write_struct(&png, &info);
        return -1;
    }
    png_set_write_fn(png, mem, png_memory_write, png_memory_flush);
    png_set_IHDR(png, info, (png_uint_32)width, (png_uint_32)height, 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    png_write_image(png, rows);
    png_write_end(png, info);
    free(rows);
    png_destroy_write_struct(&png, &info);
    return 0;
}

static int generate_clock(struct memory *png_data) {
    unsigned char *image;
    time_t now;
    struct tm tmv;
    double angle;
    double dx;
    double dy;
    double scale;
    int width;
    int center;
    int margin;
    int i;
    int x;
    int y;
    int hx;
    int hy;
    int mx;
    int my;
    int rc;

    width = 400;
    center = width / 2;
    margin = 20;
    image = calloc((size_t)width * (size_t)width, 4);
    if (image == NULL) return -1;
    image_rect(image, width, width, margin, margin, width - margin, width - margin, 255, 255, 255, 255);
    for (i = margin; i <= width - margin; i++) {
        image_pixel(image, width, width, i, margin, 0, 0, 0, 255);
        image_pixel(image, width, width, i, width - margin, 0, 0, 0, 255);
        image_pixel(image, width, width, margin, i, 0, 0, 0, 255);
        image_pixel(image, width, width, width - margin, i, 0, 0, 0, 255);
    }
    for (i = 0; i < 12; i++) {
        angle = ((double)i * 30.0 - 90.0) * PI / 180.0;
        dx = cos(angle);
        dy = sin(angle);
        scale = (double)(center - margin) / (fabs(dx) > fabs(dy) ? fabs(dx) : fabs(dy));
        x = center + (int)(dx * scale);
        y = center + (int)(dy * scale);
        image_circle(image, width, width, x, y, 6, 0, 0, 0, 255);
    }
    setenv("TZ", "Europe/Rome", 1);
    tzset();
    now = time(NULL);
    localtime_r(&now, &tmv);
    angle = (((double)(tmv.tm_hour % 12) + (double)tmv.tm_min / 60.0) * 30.0 - 90.0) * PI / 180.0;
    dx = cos(angle);
    dy = sin(angle);
    scale = (double)(center - margin) / (fabs(dx) > fabs(dy) ? fabs(dx) : fabs(dy)) * 0.70;
    hx = center + (int)(dx * scale);
    hy = center + (int)(dy * scale);
    angle = ((double)tmv.tm_min * 6.0 - 90.0) * PI / 180.0;
    dx = cos(angle);
    dy = sin(angle);
    scale = (double)(center - margin) / (fabs(dx) > fabs(dy) ? fabs(dx) : fabs(dy)) * 0.90;
    mx = center + (int)(dx * scale);
    my = center + (int)(dy * scale);
    image_line(image, width, width, center, center, hx, hy, 12, 0, 90, 180);
    image_line(image, width, width, center, center, mx, my, 5, 100, 100, 100);
    image_circle(image, width, width, center, center, 5, 0, 0, 0, 255);
    rc = encode_png(image, width, width, png_data);
    free(image);
    return rc;
}

static unsigned long utf8_next(const unsigned char **src) {
    const unsigned char *p;
    unsigned long c;

    p = *src;
    if (*p < 0x80) {
        *src = p + 1;
        return *p;
    }
    if ((*p & 0xe0) == 0xc0 && p[1] != '\0' && (p[1] & 0xc0) == 0x80) {
        c = ((unsigned long)(p[0] & 0x1f) << 6) | (unsigned long)(p[1] & 0x3f);
        *src = p + 2;
        return c;
    }
    if ((*p & 0xf0) == 0xe0 && p[1] != '\0' && p[2] != '\0' && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
        c = ((unsigned long)(p[0] & 0x0f) << 12) | ((unsigned long)(p[1] & 0x3f) << 6) | (unsigned long)(p[2] & 0x3f);
        *src = p + 3;
        return c;
    }
    if ((*p & 0xf8) == 0xf0 && p[1] != '\0' && p[2] != '\0' && p[3] != '\0' && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80) {
        c = ((unsigned long)(p[0] & 0x07) << 18) | ((unsigned long)(p[1] & 0x3f) << 12) |
            ((unsigned long)(p[2] & 0x3f) << 6) | (unsigned long)(p[3] & 0x3f);
        *src = p + 4;
        return c;
    }
    *src = p + 1;
    return '?';
}

static int generate_word(const struct config *cfg, const char *word, struct memory *png_data) {
    FT_Library library;
    FT_Face face;
    FT_GlyphSlot glyph;
    const unsigned char *p;
    unsigned char *mask;
    unsigned char *image;
    unsigned char value;
    unsigned long codepoint;
    unsigned int seed;
    int width;
    int height;
    int text_width;
    int ascender;
    int descender;
    int baseline;
    int pen_x;
    int gx;
    int gy;
    int x;
    int y;
    int bx;
    int by;
    int pitch;
    int offset;
    int ox;
    int oy;
    int gray;
    int rc;

    if (strlen(word) > 256 || cfg->font_file == NULL || *cfg->font_file == '\0') {
        return -1;
    }
    if (FT_Init_FreeType(&library) != 0) {
        return -1;
    }
    if (FT_New_Face(library, cfg->font_file, 0, &face) != 0 || FT_Set_Pixel_Sizes(face, 0, 100) != 0) {
        FT_Done_FreeType(library);
        return -1;
    }
    width = 600;
    height = 300;
    mask = calloc((size_t)width * (size_t)height, 1);
    image = malloc((size_t)width * (size_t)height * 4);
    if (mask == NULL || image == NULL) {
        free(mask);
        free(image);
        FT_Done_Face(face);
        FT_Done_FreeType(library);
        return -1;
    }
    memset(image, 255, (size_t)width * (size_t)height * 4);
    text_width = 0;
    p = (const unsigned char *)word;
    for (; *p != '\0';) {
        codepoint = utf8_next(&p);
        if (FT_Load_Char(face, codepoint, FT_LOAD_DEFAULT) == 0) text_width += (int)(face->glyph->advance.x >> 6);
    }
    ascender = (int)(face->size->metrics.ascender >> 6);
    descender = (int)(-(face->size->metrics.descender >> 6));
    baseline = height / 2 + (ascender - descender) / 2;
    pen_x = (width - text_width) / 2;
    p = (const unsigned char *)word;
    for (; *p != '\0';) {
        codepoint = utf8_next(&p);
        if (FT_Load_Char(face, codepoint, FT_LOAD_RENDER) != 0) continue;
        glyph = face->glyph;
        gx = pen_x + glyph->bitmap_left;
        gy = baseline - glyph->bitmap_top;
        pitch = glyph->bitmap.pitch;
        for (by = 0; by < (int)glyph->bitmap.rows; by++) {
            for (bx = 0; bx < (int)glyph->bitmap.width; bx++) {
                x = gx + bx;
                y = gy + by;
                if (x < 0 || x >= width || y < 0 || y >= height) continue;
                value = glyph->bitmap.buffer[by * pitch + bx];
                if (value > mask[y * width + x]) mask[y * width + x] = value;
            }
        }
        pen_x += (int)(glyph->advance.x >> 6);
    }
    if (RAND_bytes((unsigned char *)&seed, sizeof(seed)) != 1) seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
    for (y = 0; y < height; y += 4) {
        for (x = 0; x < width; x += 4) {
            offset = (255 - (int)mask[y * width + x]) * 2 / 255;
            ox = offset == 0 ? 0 : (int)(rand_r(&seed) % (unsigned int)(offset * 2 + 1)) - offset;
            oy = offset == 0 ? 0 : (int)(rand_r(&seed) % (unsigned int)(offset * 2 + 1)) - offset;
            gray = (((x + y) / 4) % 2 == 0) ? 120 : 130;
            image_pixel(image, width, height, x + ox, y + oy, (unsigned char)gray, (unsigned char)gray, (unsigned char)gray, 255);
        }
    }
    rc = encode_png(image, width, height, png_data);
    free(mask);
    free(image);
    FT_Done_Face(face);
    FT_Done_FreeType(library);
    return rc;
}

static int stats_open(const struct config *cfg, sqlite3 **db) {
    const char *schema;
    int rc;

    *db = NULL;
    if (cfg->stats_db == NULL || *cfg->stats_db == '\0') return -1;
    rc = sqlite3_open_v2(cfg->stats_db, db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        if (*db != NULL) sqlite3_close(*db);
        *db = NULL;
        return -1;
    }
    sqlite3_busy_timeout(*db, 1000);
    if (sqlite3_exec(*db, "PRAGMA journal_mode=MEMORY", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(*db);
        *db = NULL;
        return -1;
    }
    schema = "CREATE TABLE IF NOT EXISTS stats ("
             "day TEXT NOT NULL,"
             "tool TEXT NOT NULL,"
             "ok INTEGER NOT NULL DEFAULT 0,"
             "fail INTEGER NOT NULL DEFAULT 0,"
             "last_use INTEGER NOT NULL,"
             "PRIMARY KEY(day,tool)) WITHOUT ROWID";
    if (sqlite3_exec(*db, schema, NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(*db);
        *db = NULL;
        return -1;
    }
    return 0;
}

static int stats_day(char *day, size_t size) {
    time_t now;
    struct tm tmv;

    now = time(NULL);
    if (localtime_r(&now, &tmv) == NULL) return -1;
    return strftime(day, size, "%Y-%m-%d", &tmv) > 0 ? 0 : -1;
}

static int stats_record(const struct config *cfg, const char *tool, int ok) {
    sqlite3 *db;
    sqlite3_stmt *stmt;
    const char *sql;
    char day[11];
    int rc;

    if (tool == NULL || *tool == '\0' || stats_day(day, sizeof(day)) != 0) return -1;
    if (stats_open(cfg, &db) != 0) return -1;
    sql = "INSERT INTO stats(day,tool,ok,fail,last_use) VALUES(?1,?2,?3,?4,?5) "
          "ON CONFLICT(day,tool) DO UPDATE SET "
          "ok=stats.ok+excluded.ok,fail=stats.fail+excluded.fail,last_use=excluded.last_use";
    stmt = NULL;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, day, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, tool, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 3, ok ? 1 : 0);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt, 4, ok ? 0 : 1);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 5, (sqlite3_int64)time(NULL));
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int command_stats(const struct config *cfg, char *out, size_t out_size) {
    sqlite3 *db;
    sqlite3_stmt *stmt;
    const char *sql;
    const unsigned char *tool;
    char day[11];
    size_t used;
    sqlite3_int64 today;
    sqlite3_int64 total;
    sqlite3_int64 fail;
    int rc;

    if (stats_day(day, sizeof(day)) != 0 || stats_open(cfg, &db) != 0) return -1;
    sql = "SELECT tool,"
          "SUM(CASE WHEN day=?1 THEN ok+fail ELSE 0 END),"
          "SUM(ok+fail),SUM(fail) "
          "FROM stats GROUP BY tool ORDER BY SUM(ok+fail) DESC,tool";
    stmt = NULL;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, day, -1, SQLITE_TRANSIENT);
    used = 0;
    if (rc == SQLITE_OK) appendf(out, out_size, &used, "Stats oggi / totale / errori\n");
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    for (; rc == SQLITE_ROW; rc = sqlite3_step(stmt)) {
        tool = sqlite3_column_text(stmt, 0);
        today = sqlite3_column_int64(stmt, 1);
        total = sqlite3_column_int64(stmt, 2);
        fail = sqlite3_column_int64(stmt, 3);
        if (tool != NULL && appendf(out, out_size, &used, "%-12s %lld / %lld / %lld\n", (const char *)tool,
                                   (long long)today, (long long)total, (long long)fail) != 0) {
            rc = SQLITE_TOOBIG;
            break;
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc == SQLITE_DONE ? 0 : -1;
}

static const char *stats_tool(const struct config *cfg, const char *msg) {
    const char *resolved;
    const char *end;
    size_t len;

    resolved = resolve_alias(cfg, msg);
    if (resolved == NULL || *resolved != '/') return NULL;
    end = strchr(resolved, ' ');
    len = end == NULL ? strlen(resolved) : (size_t)(end - resolved);
    if (len == 4 && strncmp(resolved, "/pic", 4) == 0) return "/pic";
    if (len == 6 && strncmp(resolved, "/clock", 6) == 0) return "/clock";
    if (len == 5 && strncmp(resolved, "/word", 5) == 0) return "/word";
    if (len == 4 && strncmp(resolved, "/rer", 4) == 0) return "/rer";
    if (len == 6 && strncmp(resolved, "/radio", 6) == 0) return "/radio";
    if (len == 3 && strncmp(resolved, "/we", 3) == 0) return "/we";
    if (len == 3 && strncmp(resolved, "/fo", 3) == 0) return "/fo";
    if (len == 3 && strncmp(resolved, "/po", 3) == 0) return "/po";
    if (len == 3 && strncmp(resolved, "/so", 3) == 0) return "/so";
    if (len == 3 && strncmp(resolved, "/cc", 3) == 0) return "/cc";
    if (len == 5 && strncmp(resolved, "/peso", 5) == 0) return "/peso";
    if (len == 8 && strncmp(resolved, "/display", 8) == 0) return "/display";
    if (len == 6 && strncmp(resolved, "/stats", 6) == 0) return "/stats";
    if (len == 5 && strncmp(resolved, "/help", 5) == 0) return "/help";
    return NULL;
}

static int process_command(const struct config *cfg, const char *who, const char *msg, char *out, size_t out_size, char *image, size_t image_size, struct memory *media) {
    const char *resolved;
    const char *arg;
    size_t cmd_len;
    int family;
    char random[13];

    resolved = resolve_alias(cfg, msg);
    if (*resolved != '/') return 0;
    arg = strchr(resolved, ' ');
    if (arg == NULL) {
        cmd_len = strlen(resolved);
        arg = resolved + cmd_len;
    } else {
        cmd_len = (size_t)(arg - resolved);
        arg++;
    }
    family = is_family(cfg, who);
    if (cmd_len == 4 && strncmp(resolved, "/pic", 4) == 0) {
        if (build_random(random, sizeof(random)) != 0) return -1;
        snprintf(image, image_size, "https://picsum.photos/400/300?rrr=%s", random);
    } else if (cmd_len == 6 && strncmp(resolved, "/clock", 6) == 0) {
        return generate_clock(media) == 0 ? 1 : -1;
    } else if (cmd_len == 5 && strncmp(resolved, "/word", 5) == 0) {
        return generate_word(cfg, arg, media) == 0 ? 1 : -1;
    } else if (cmd_len == 4 && strncmp(resolved, "/rer", 4) == 0) return command_rer(cfg, out, out_size) == 0 ? 1 : -1;
    else if (cmd_len == 6 && strncmp(resolved, "/radio", 6) == 0) return command_radio(cfg, out, out_size) == 0 ? 1 : -1;
    else if (cmd_len == 3 && strncmp(resolved, "/we", 3) == 0) return command_weather(cfg, arg, out, out_size) == 0 ? 1 : -1;
    else if (cmd_len == 3 && strncmp(resolved, "/fo", 3) == 0) return command_forecast(cfg, arg, out, out_size) == 0 ? 1 : -1;
    else if (cmd_len == 3 && strncmp(resolved, "/po", 3) == 0) return command_pollution(cfg, arg, out, out_size) == 0 ? 1 : -1;
    else if (family && cmd_len == 3 && strncmp(resolved, "/so", 3) == 0) return command_so(cfg, arg, out, out_size) == 0 ? 1 : -1;
    else if (family && cmd_len == 3 && strncmp(resolved, "/cc", 3) == 0) return command_cc(cfg, arg, out, out_size) == 0 ? 1 : -1;
    else if (family && cmd_len == 5 && strncmp(resolved, "/peso", 5) == 0) return command_peso(cfg, arg, out, out_size) == 0 ? 1 : -1;
    else if (family && cmd_len == 8 && strncmp(resolved, "/display", 8) == 0) return command_display(cfg, out, out_size) == 0 ? 1 : -1;
    else if (family && cmd_len == 6 && strncmp(resolved, "/stats", 6) == 0) return command_stats(cfg, out, out_size) == 0 ? 1 : -1;
    else if (cmd_len == 5 && strncmp(resolved, "/help", 5) == 0) {
        snprintf(out, out_size,
                 "/pic immagine random\n/clock immagine ora\n/word xx immagine maschera testo xx\n/rer dati RER\n/radio lista radioacolori\n/we xx tempo atmosferico\n/fo xx previsioni\n/po xx inquinamento\n%s/help questo help\n",
                 family ? "FAMILY /cc xx corte connessa\nFAMILY /so xx sostegno\nFAMILY /peso xx set perso GM\nFAMILY /display\nFAMILY /stats\n" : "");
    }
    return (*out != '\0' || *image != '\0') ? 1 : 0;
}

static int handle_post(const struct config *cfg) {
    const char *cl;
    const char *sig;
    char *end;
    unsigned char *body;
    long len;
    size_t got;
    cJSON *root;
    cJSON *entry;
    cJSON *change;
    cJSON *value;
    cJSON *messages;
    cJSON *message;
    cJSON *item;
    cJSON *text;
    const char *who;
    const char *type;
    const char *msg;
    const char *tool;
    char out[MAX_OUTPUT];
    char image[MAX_URL];
    struct memory media;
    char *media_id;
    int rc;
    int auth_rc;
    int send_rc;

    cl = getenv("CONTENT_LENGTH");
    if (cl == NULL || *cl == '\0') {
        cgi_status(400, "Bad Request");
        printf("Bad Request\n");
        return 1;
    }
    errno = 0;
    len = strtol(cl, &end, 10);
    if (errno != 0 || *end != '\0' || len < 0 || len > MAX_BODY) {
        cgi_status(413, "Payload Too Large");
        printf("Payload Too Large\n");
        return 1;
    }
    body = malloc((size_t)len + 1);
    if (body == NULL) {
        cgi_status(500, "Internal Server Error");
        return 1;
    }
    got = fread(body, 1, (size_t)len, stdin);
    if (got != (size_t)len) {
        free(body);
        cgi_status(400, "Bad Request");
        return 1;
    }
    body[len] = '\0';
    sig = getenv("HTTP_X_HUB_SIGNATURE_256");
    if (!verify_signature(cfg->app_secret, body, (size_t)len, sig)) {
        free(body);
        cgi_status(401, "Unauthorized");
        printf("Invalid signature\n");
        return 1;
    }
    root = cJSON_ParseWithLength((const char *)body, (size_t)len);
    free(body);
    if (root == NULL) {
        cgi_status(400, "Bad Request");
        printf("Invalid JSON\n");
        return 1;
    }
    cgi_status(200, "OK");
    printf("EVENT_RECEIVED");
    fflush(stdout);
    entry = cJSON_GetObjectItemCaseSensitive(root, "entry");
    entry = cJSON_IsArray(entry) ? cJSON_GetArrayItem(entry, 0) : NULL;
    change = cJSON_IsObject(entry) ? cJSON_GetObjectItemCaseSensitive(entry, "changes") : NULL;
    change = cJSON_IsArray(change) ? cJSON_GetArrayItem(change, 0) : NULL;
    value = cJSON_IsObject(change) ? cJSON_GetObjectItemCaseSensitive(change, "value") : NULL;
    messages = cJSON_IsObject(value) ? cJSON_GetObjectItemCaseSensitive(value, "messages") : NULL;
    message = cJSON_IsArray(messages) ? cJSON_GetArrayItem(messages, 0) : NULL;
    if (!cJSON_IsObject(message)) {
        cJSON_Delete(root);
        return 0;
    }
    item = cJSON_GetObjectItemCaseSensitive(message, "from");
    who = cJSON_IsString(item) ? item->valuestring : NULL;
    item = cJSON_GetObjectItemCaseSensitive(message, "type");
    type = cJSON_IsString(item) ? item->valuestring : NULL;
    if (who == NULL || !valid_phone(who) || type == NULL || strcmp(type, "text") != 0) {
        cJSON_Delete(root);
        return 0;
    }
    text = cJSON_GetObjectItemCaseSensitive(message, "text");
    item = cJSON_IsObject(text) ? cJSON_GetObjectItemCaseSensitive(text, "body") : NULL;
    msg = cJSON_IsString(item) ? item->valuestring : NULL;
    if (msg == NULL || strlen(msg) > 4096) {
        cJSON_Delete(root);
        return 0;
    }
    auth_rc = submit_auth_code(cfg, who, msg);
    if (auth_rc != 0) stats_record(cfg, "auth-wa", auth_rc > 0);
    tool = stats_tool(cfg, msg);
    out[0] = '\0';
    image[0] = '\0';
    media.data = NULL;
    media.len = 0;
    media.cap = 0;
    rc = process_command(cfg, who, msg, out, sizeof(out), image, sizeof(image), &media);
    send_rc = -1;
    if (rc > 0 && media.len > 0) {
        media_id = NULL;
        if (upload_whatsapp_media(cfg, (const unsigned char *)media.data, media.len, &media_id) == 0)
            send_rc = send_whatsapp(cfg, who, NULL, NULL, media_id);
        free(media_id);
    } else if (rc > 0) send_rc = send_whatsapp(cfg, who, out[0] != '\0' ? out : NULL, image[0] != '\0' ? image : NULL, NULL);
    if (tool != NULL && rc != 0) stats_record(cfg, tool, rc > 0 && send_rc == 0);
    free(media.data);
    cJSON_Delete(root);
    return 0;
}

int main(void) {
    struct config cfg;
    const char *method;
    const char *path;
    int rc;

    signal(SIGPIPE, SIG_IGN);
    path = getenv("WAAPI_CONFIG");
    if (path == NULL || *path == '\0') path = WAAPI_CONFIG;
    if (load_config(path, &cfg) != 0) {
        cgi_status(500, "Internal Server Error");
        printf("Configuration unavailable\n");
        free_config(&cfg);
        return 1;
    }
    if (drop_privileges() != 0) {
        cgi_status(500, "Internal Server Error");
        printf("Privilege setup failed\n");
        free_config(&cfg);
        return 1;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        cgi_status(500, "Internal Server Error");
        free_config(&cfg);
        return 1;
    }
    method = getenv("REQUEST_METHOD");
    path = getenv("SCRIPT_NAME");
    if (method == NULL || path == NULL) {
        fprintf(stderr, "waapi: CGI request environment is incomplete\n");
        rc = 1;
    } else if (strcmp(path, "/webhook") == 0 && strcmp(method, "GET") == 0) rc = handle_get(&cfg);
    else if (strcmp(path, "/webhook") == 0 && strcmp(method, "POST") == 0) rc = handle_post(&cfg);
    else {
        cgi_status(404, "Not Found");
        printf("Not Found\n");
        rc = 1;
    }
    curl_global_cleanup();
    free_config(&cfg);
    return rc;
}
