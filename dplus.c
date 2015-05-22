#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>

#include "dplus.h"
#include "lruhash.h"

#define HTTPDNS_DEFAULT_SERVER "119.29.29.29"
#define HTTPDNS_DEFAULT_PORT   80

#define CACHE_DEFAULT_MIN_TTL  90
#define PREFETCH_EXPIRY_ADD 60

#define HTTP_DEFAULT_DATA_SIZE 256

//calculate the prefetch TTL as 75% of original
#define PREFETCH_TTL_CALC(ttl) ((ttl) - (ttl)/4)

//dplus environment
static struct dp_env *dpe = NULL;

//max memory of dns cache
static size_t cache_maxmem = HASH_DEFAULT_MAXMEM;

//min cache ttl
static int min_ttl = CACHE_DEFAULT_MIN_TTL;

//http dns server and port
static char *serv_ip = HTTPDNS_DEFAULT_SERVER;
static int port = HTTPDNS_DEFAULT_PORT;

void dp_set_cache_mem(size_t maxmem)
{
    cache_maxmem = maxmem;
}

void dp_set_ttl(int ttl)
{
    min_ttl = ttl;
}

//djb2 hash function
static hashvalue_t hashfunc(const char *key, size_t klen) {
    hashvalue_t hash = 5381;
    while(klen--) {
        hash = ((hash << 5) + hash) + *key++; //hash * 33 + c
    }
    return hash;
}

static hashvalue_t query_info_hash(struct query_info *q)
{
    return hashfunc(q->node, strlen(q->node));
}

static size_t msgreply_sizefunc(void *k, void *d)
{
    struct msgreply_entry *q = (struct msgreply_entry *)k;
    struct reply_info *r = (struct reply_info *)d;
    size_t s = sizeof(struct msgreply_entry);
    s += strlen(q->key.node);
    s += sizeof(struct reply_info);
    s += sizeof(struct host_info);
    s += sizeof(char) * (r->host->h_length) * (r->host->addr_list_len);
    return s;
}

static int query_info_compare(void *k1, void *k2)
{
    struct query_info *q1 = (struct query_info *)k1;
    struct query_info *q2 = (struct query_info *)k2;
    return strcmp(q1->node, q2->node);
}

static void query_info_copy(struct query_info *d, struct query_info *s)
{
    memcpy(d, s, sizeof(*s));
    d->node = strdup(s->node);
}

static void query_info_clear(struct query_info *qinfo)
{
    free(qinfo->node);
}

void query_entry_delete(void *k)
{
    struct msgreply_entry *q = (struct msgreply_entry *)k;
    lock_basic_destroy(&q->entry.lock);
    query_info_clear(&q->key);
    free(q);
}

static void host_info_clear(struct host_info *host)
{
    int i;
    for(i=0; i<host->addr_list_len; i++) {
        if(host->h_addr_list[i]) {
            free(host->h_addr_list[i]);
        }
    }
    free(host->h_addr_list);
    free(host);
}

static void reply_info_delete(void *d)
{
    struct reply_info *r = (struct reply_info *)d;
    host_info_clear(r->host);
    free(r);
}

static struct msgreply_entry *query_info_entrysetup(struct query_info *q,
    struct reply_info *r, hashvalue_t h)
{
    struct msgreply_entry *e = (struct msgreply_entry *)malloc( 
        sizeof(struct msgreply_entry));
    if(!e) return NULL;
    query_info_copy(&e->key, q);
    e->entry.hash = h;
    e->entry.key = e;
    e->entry.data = r;
    lock_basic_init(&e->entry.lock);
    return e;
}

static void dns_cache_store_msg(struct query_info *qinfo, hashvalue_t hash,
    struct host_info *hi, time_t ttl)
{
    struct msgreply_entry *e;
    struct reply_info *rep;
    time_t now = time(NULL);
    rep = (struct reply_info *)malloc(sizeof(struct reply_info));
    if (rep == NULL) {
        fprintf(stderr, "malloc struct reply_info failed\n");
        return;
    }

    rep->host = hi;
    ttl = ttl < CACHE_DEFAULT_MIN_TTL ? CACHE_DEFAULT_MIN_TTL : ttl;
    rep->ttl = ttl + now;
    rep->prefetch_ttl = PREFETCH_TTL_CALC(ttl);

    if(!(e = query_info_entrysetup(qinfo, rep, hash))) {
        fprintf(stderr, "store_msg: malloc failed");
        reply_info_delete(rep);
        return;
    }
    lruhash_insert(dpe->cache, hash, &e->entry, rep);
}

static struct prefetch_stat_list *new_prefetch_list()
{
    struct prefetch_stat_list *prefetch_list;
    prefetch_list = (struct prefetch_stat_list *)malloc(
        sizeof(struct prefetch_stat_list));
    if(prefetch_list == NULL) {
        fprintf(stderr, "new_prefetch_list failed");
        exit(1);
    }

    lock_basic_init(&prefetch_list->lock);
    prefetch_list->head = NULL;

    return prefetch_list;
}

static struct prefetch_stat *new_prefetch_stat(struct query_info *qinfo)
{
    struct prefetch_stat *prefetch;
    prefetch = (struct prefetch_stat *)malloc(sizeof(struct prefetch_stat));
    if (prefetch == NULL) {
        fprintf(stderr, "malloc struct prefetch_stat failed\n");
        return NULL;
    }
    query_info_copy(&prefetch->qinfo, qinfo);
    prefetch->next = NULL;
    return prefetch;
}

static void free_prefetch_stat(struct prefetch_stat *prefetch)
{
    query_info_clear(&prefetch->qinfo);
    free(prefetch);
}

static void prefetch_list_destroy(struct prefetch_stat_list *list)
{
    struct prefetch_stat *s, *t;

    lock_basic_destroy(&list->lock);
    s = list->head;
    while(s) {
        t = s;
        s = s->next;
        free_prefetch_stat(t);
    }
    free(list);
}

static int prefetch_stat_exist(struct query_info *qinfo,
    struct prefetch_stat *s)
{
    while(s) {
        if (query_info_compare((void *)qinfo, (void *)(&s->qinfo)) == 0){
            return 1;
        }
        s = s->next;
    }
    return 0;
}

static struct prefetch_stat *prefetch_stat_insert(struct query_info *qinfo,
    struct prefetch_stat_list *list)
{
    struct prefetch_stat *s, *new_prefetch;
    int ret;

    lock_basic_lock(&list->lock);
    ret = prefetch_stat_exist(qinfo, list->head);
    if (ret) {
        lock_basic_unlock(&list->lock);
        return NULL;
    }

    new_prefetch = new_prefetch_stat(qinfo);
    if (new_prefetch == NULL) {
        lock_basic_unlock(&list->lock);
        return NULL;
    }
    s = list->head;
    if (s == NULL) {
        s = new_prefetch;
    } else {
        while(s->next)
            s = s->next;
        s->next = new_prefetch;
    }
    lock_basic_unlock(&list->lock);
    return new_prefetch;
}

static int prefetch_stat_delete(struct query_info *qinfo,
    struct prefetch_stat_list *list)
{
    struct prefetch_stat *s, *prev = NULL;
    lock_basic_lock(&list->lock);
    s = list->head;
    while(s) {
        if (query_info_compare((void *)qinfo, (void *)(&s->qinfo)) == 0){
            if (prev == NULL) {
                list->head = s->next;
            } else {
                prev->next = s->next;
            }
            lock_basic_unlock(&list->lock);
            free_prefetch_stat(s);
            return 1;
        }
        prev = s;
        s = s->next;
    }
    lock_basic_unlock(&list->lock);
    return 0;
}

struct prefetch_job_info {
    struct query_info qinfo;
    hashvalue_t hash;
};

static void *prefetch_job(void *arg)
{
    struct prefetch_job_info *tinfo = (struct prefetch_job_info *)arg;
    struct host_info *hi;
    time_t ttl = 0;
    hi = http_query(tinfo->qinfo.node, &ttl);
    if (hi == NULL) {
        prefetch_stat_delete(&tinfo->qinfo, dpe->prefetch_list);
        return NULL;
    }
    dns_cache_store_msg(&tinfo->qinfo, tinfo->hash, hi, ttl);
    prefetch_stat_delete(&tinfo->qinfo, dpe->prefetch_list);
    return NULL;
}

static void prefetch_new_query(struct query_info *qinfo, hashvalue_t hash)
{
    struct prefetch_job_info tinfo;
    pthread_t thread;
    pthread_attr_t attr;
    struct prefetch_stat *prefetch;

    prefetch = prefetch_stat_insert(qinfo, dpe->prefetch_list);
    if (prefetch == NULL) {
        return;
    }

    tinfo.qinfo = prefetch->qinfo;
    tinfo.hash = hash;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&thread, &attr, &prefetch_job, &tinfo);
    pthread_attr_destroy(&attr);
}

static int is_integer(const char *s)
{
    if (*s == '-' || *s == '+')
        s++;
    if (*s < '0' || '9' < *s)
        return 0;
    s++;
    while ('0' <= *s && *s <= '9')
        s++;
    return (*s == '\0');
}

static int is_address(const char *s)
{
    unsigned char buf[sizeof(struct in6_addr)];
    int r;

    r = inet_pton(AF_INET, s, buf);
    if (r <= 0) {
        r = inet_pton(AF_INET6, s, buf);
        return (r > 0);
    }

    return 1;
}

//TODO: support IPV6
static struct addrinfo *malloc_addrinfo(int port, uint32_t addr,
    int socktype, int proto)
{
    struct addrinfo *ai;
    struct sockaddr_in *sa_in;
    size_t socklen;
    socklen = sizeof(struct sockaddr_in);

    ai = (struct addrinfo *)calloc(1, sizeof(struct addrinfo) + socklen);
    if(!ai)
        return NULL;

    ai->ai_socktype = socktype;
    ai->ai_protocol = proto;

    ai->ai_addr = (struct sockaddr *)(ai + 1);
    ai->ai_addrlen = socklen;
    ai->ai_addr->sa_family = ai->ai_family = AF_INET;

    sa_in = (struct sockaddr_in *)ai->ai_addr;
    sa_in->sin_port = port;
    sa_in->sin_addr.s_addr = addr;

    return ai;
}

static int fillin_addrinfo_res(struct addrinfo **res, struct host_info *hi,
    int port, int socktype, int proto)
{
    int i;
    struct addrinfo *cur, *prev = NULL;
    for (i = 0; i < hi->addr_list_len; i++) {
        struct in_addr *in = ((struct in_addr *)hi->h_addr_list[i]);
        cur = malloc_addrinfo(port, in->s_addr, socktype, proto);
        if (cur == NULL) {
            if (*res)
                freeaddrinfo(*res);
            return EAI_MEMORY;
        }
        if (prev)
            prev->ai_next = cur;
        else
            *res = cur;
        prev = cur;
    }

    return 0;
}

void dp_env_init()
{
    if(dpe != NULL)
        return;
    dpe = (struct dp_env*)calloc(1, sizeof(struct dp_env));
    if(!dpe) {
        fprintf(stderr, "dp_env_init: malloc failed");
        exit(1);
    }

    dpe->cache_maxmem = cache_maxmem;
    dpe->min_ttl = min_ttl;
    dpe->serv_ip = serv_ip;
    dpe->port = port;
    dpe->cache = lruhash_create(HASH_DEFAULT_ARRAY_SIZE, dpe->cache_maxmem,
        msgreply_sizefunc, query_info_compare,
        query_entry_delete, reply_info_delete);
    if(dpe->cache == NULL) {
        fprintf(stderr, "lruhash_create failed");
        exit(1);
    }
    dpe->prefetch_list = new_prefetch_list();
}

void dp_env_destroy()
{
    if(dpe == NULL)
        return;

    lruhash_delete(dpe->cache);
    prefetch_list_destroy(dpe->prefetch_list);
    free(dpe);
}

void dp_flush_cache(const char *node)
{
    hashvalue_t h;
    struct query_info qinfo;

    qinfo.node = (char *)node;
    h = query_info_hash(&qinfo);
    lruhash_remove(dpe->cache, h, &qinfo);
}

static int strchr_num(const char *str, char c)
{
    int count = 0;
    while(*str){
        if(*str++ == c){
            count++;
        }
    }
    return count;
}

struct host_info *http_query(const char *node, time_t *ttl)
{
    int i, ret, sockfd;
    struct host_info *hi;
    char http_data[HTTP_DEFAULT_DATA_SIZE];
    char *http_data_ptr = http_data;
    char *comma_ptr;

    sockfd = make_connection(dpe->serv_ip, dpe->port);
    if(sockfd < 0) {
        return NULL;
    }

    snprintf(http_data, HTTP_DEFAULT_DATA_SIZE, "/d?dn=%s&ttl=1", node);
    ret = make_request(sockfd, dpe->serv_ip, http_data);
    if(ret < 0){
        close(sockfd);
        return NULL;
    }

    ret = fetch_response(sockfd, http_data, HTTP_DEFAULT_DATA_SIZE);
    if (ret < 0) {
        close(sockfd);
        return NULL;
    }
    close(sockfd);

    comma_ptr = strchr(http_data, ',');
    if (comma_ptr != NULL) {
        sscanf(comma_ptr + 1, "%ld", ttl);
        *comma_ptr = '\0';
    } else {
        *ttl = 0;
    }

    hi = (struct host_info *)malloc(sizeof(struct host_info));
    if (hi == NULL) {
        fprintf(stderr, "malloc struct host_info failed\n");
        return NULL;
    }

    //Only support IPV4
    hi->h_addrtype = AF_INET;
    hi->h_length = sizeof(struct in_addr);
    hi->addr_list_len = strchr_num(http_data, ';') + 1;
    hi->h_addr_list = (char **)calloc(hi->addr_list_len, sizeof(char *));
    if(hi->h_addr_list == NULL) {
        fprintf(stderr, "calloc addr_list failed\n");
        free(hi);
        return NULL;
    }

    for (i = 0; i < hi->addr_list_len; ++i) {
        char *addr;
        char *ipstr = http_data_ptr;
        char *semicolon = strchr(ipstr, ';');
        if (semicolon != NULL) {
            *semicolon = '\0';
            http_data_ptr = semicolon + 1;
        }

        addr = (char *)malloc(sizeof(struct in_addr));
        if (addr == NULL) {
            fprintf(stderr, "malloc struct in_addr failed\n");
            host_info_clear(hi);
            return NULL;
        }
        ret = inet_pton(AF_INET, ipstr, addr);
        if (ret <= 0) {
            fprintf(stderr, "invalid ipstr:%s\n", ipstr);
            host_info_clear(hi);
            return NULL;
        }

        hi->h_addr_list[i] = addr;
    }

    return hi;
}

void dp_freeaddrinfo(struct addrinfo *res)
{
    freeaddrinfo(res);
}

int dp_getaddrinfo(const char *node, const char *service,
    const struct addrinfo *hints, struct addrinfo **res)
{
    struct host_info *hi = NULL;
    int port = 0, socktype, proto, ret = 0;

    hashvalue_t h;
    struct lruhash_entry *e;
    struct query_info qinfo;
    time_t now = time(NULL);
    time_t ttl;

    if (node == NULL)
        return EAI_NONAME;

    //AI_NUMERICHOST not supported
    if(is_address(node) || (hints && (hints->ai_flags & AI_NUMERICHOST)))
        return EAI_BADFLAGS;

    if (hints && hints->ai_family != PF_INET
        && hints->ai_family != PF_UNSPEC
        && hints->ai_family != PF_INET6) {
        return EAI_FAMILY;
    }
    if (hints && hints->ai_socktype != SOCK_DGRAM
        && hints->ai_socktype != SOCK_STREAM
        && hints->ai_socktype != 0) {
        return EAI_SOCKTYPE;
    }

    socktype = (hints && hints->ai_socktype) ? hints->ai_socktype
        : SOCK_STREAM;
    if (hints && hints->ai_protocol)
        proto = hints->ai_protocol;
    else {
        switch (socktype) {
            case SOCK_DGRAM:
                proto = IPPROTO_UDP;
                break;
            case SOCK_STREAM:
                proto = IPPROTO_TCP;
                break;
            default:
                proto = 0;
                break;
        }
    }

    if (service != NULL && service[0] == '*' && service[1] == 0)
        service = NULL;

    if (service != NULL) {
        if (is_integer(service))
            port = htons(atoi(service));
        else {
            struct servent *servent;
            char *pe_proto;
            switch(socktype){
                case SOCK_DGRAM:
                    pe_proto = "udp";
                    break;
                case SOCK_STREAM:
                    pe_proto = "tcp";
                    break;
                default:
                    pe_proto = "tcp";
                    break;
            }
            servent = getservbyname(service, pe_proto);
            if(servent == NULL)
                return EAI_SERVICE;
            port = servent->s_port;
        }
    } else {
        port = htons(0);
    }

    qinfo.node = (char *)node;
    h = query_info_hash(&qinfo);
    e = lruhash_lookup(dpe->cache, h, &qinfo);
    if(e) {
        struct reply_info *repinfo = (struct reply_info*)e->data;
        time_t ttl = repinfo->ttl;
        time_t prefetch_ttl = repinfo->prefetch_ttl;
        if(ttl > now) {
            ret = fillin_addrinfo_res(res, repinfo->host,
                port, socktype, proto);
            lock_basic_unlock(&e->lock);

            //prefetch it if the prefetch TTL expired
            if(prefetch_ttl <= now)
                prefetch_new_query(&qinfo, h);
            }
            return ret;
        lock_basic_unlock(&e->lock);
    }

    hi = http_query(node, &ttl);
    if (hi == NULL) {
        return getaddrinfo(node, service, hints, res);
    }
    ret = fillin_addrinfo_res(res, hi, port, socktype, proto);

    dns_cache_store_msg(&qinfo, h, hi, ttl);
    return ret;
}