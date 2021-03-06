/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2015-2016 Treasure Data Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <stddef.h>

#include <fluent-bit/flb_info.h>
#include <fluent-bit/flb_macros.h>
#include <fluent-bit/flb_config.h>
#include <fluent-bit/flb_plugins.h>
#include <fluent-bit/flb_io_tls.h>
#include <fluent-bit/flb_kernel.h>

struct flb_service_config service_configs[] = {
    {FLB_CONF_STR_FLUSH,
     FLB_CONF_TYPE_INT,
     offsetof(struct flb_config, flush)},

    {FLB_CONF_STR_DAEMON,
     FLB_CONF_TYPE_BOOL,
     offsetof(struct flb_config, daemon)},

    {FLB_CONF_STR_LOGLEVEL,
     FLB_CONF_TYPE_STR,
     offsetof(struct flb_config, log)},

#ifdef FLB_HAVE_HTTP
    {FLB_CONF_STR_HTTP_MONITOR,
     FLB_CONF_TYPE_BOOL,
     offsetof(struct flb_config, http_server)},

    {FLB_CONF_STR_HTTP_PORT,
     FLB_CONF_TYPE_STR,
     offsetof(struct flb_config, http_port)},
#endif

#ifdef FLB_HAVE_BUFFERING
    {FLB_CONF_STR_BUF_PATH,
     FLB_CONF_TYPE_STR,
     offsetof(struct flb_config, buffer_path)},

    {FLB_CONF_STR_BUF_WORKERS,
     FLB_CONF_TYPE_INT,
     offsetof(struct flb_config, buffer_workers)},
#endif

    {NULL, FLB_CONF_TYPE_OTHER, 0} /* end of array */
};


struct flb_config *flb_config_init()
{
    struct flb_config *config;

    config = calloc(1, sizeof(struct flb_config));
    if (!config) {
        perror("malloc");
        return NULL;
    }

    /* Flush */
    config->flush        = FLB_CONFIG_FLUSH_SECS;
#ifdef FLB_HAVE_FLUSH_UCONTEXT
    config->flush_method = FLB_FLUSH_UCONTEXT;
#elif defined FLB_HAVE_FLUSH_PTHREADS
    config->flush_method = FLB_FLUSH_PTHREADS;
#endif
    config->daemon       = FLB_FALSE;
    config->init_time    = time(NULL);
    config->kernel       = flb_kernel_info();
    config->verbose      = 3;

#ifdef FLB_HAVE_HTTP
    config->http_server  = FLB_FALSE;
    config->http_port    = strdup(FLB_CONFIG_HTTP_PORT);
#endif

#ifdef FLB_HAVE_BUFFERING
    config->buffer_ctx     = NULL;
    config->buffer_path    = NULL;
    config->buffer_workers = 0;
#endif

    mk_list_init(&config->collectors);
    mk_list_init(&config->in_plugins);
    mk_list_init(&config->out_plugins);
    mk_list_init(&config->inputs);
    mk_list_init(&config->outputs);
    mk_list_init(&config->sched_requests);

    memset(&config->tasks_map, '\0', sizeof(config->tasks_map));

    /* Register plugins */
    flb_register_plugins(config);

    /* Ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    return config;
}

void flb_config_exit(struct flb_config *config)
{
    struct mk_list *tmp;
    struct mk_list *head;
    struct flb_input_collector *collector;

    if (config->log) {
        free(config->log);
    }

    if (config->kernel) {
        free(config->kernel->s_version.data);
        free(config->kernel);
    }

        /* release resources */
    if (config->ch_event.fd) {
        close(config->ch_event.fd);
    }

    /* Pipe */
    if (config->ch_data[0]) {
        close(config->ch_data[0]);
        close(config->ch_data[1]);
    }

    /* Channel manager */
    if (config->ch_manager[0] > 0) {
        close(config->ch_manager[0]);
        if (config->ch_manager[0] != config->ch_manager[1]) {
            close(config->ch_manager[1]);
        }
    }

    /* Channel notifications */
    if (config->ch_notif[0] > 0) {
        close(config->ch_notif[0]);
        if (config->ch_notif[0] != config->ch_notif[1]) {
            close(config->ch_notif[1]);
        }
    }

    /* Collectors */
    mk_list_foreach_safe(head, tmp, &config->collectors) {
        collector = mk_list_entry(head, struct flb_input_collector, _head);
        mk_event_del(config->evl, &collector->event);

        if (collector->type == FLB_COLLECT_TIME) {
            close(collector->fd_timer);
        }

        mk_list_del(&collector->_head);
        free(collector);
    }

    /* Event flush */
    mk_event_del(config->evl, &config->event_flush);
    close(config->flush_fd);

#ifdef FLB_HAVE_HTTP
    if (config->http_port) {
        free(config->http_port);
    }

    if (config->http_server) {
        free(config->http_server);
    }
#endif

#ifdef FLB_HAVE_STATS
    flb_stats_exit(config);
#endif

#ifdef FLB_HAVE_BUFFERING
    free(config->buffer_path);
#endif

    mk_event_loop_destroy(config->evl);
    free(config);
}

char *flb_config_prop_get(char *key, struct mk_list *list)
{
    struct mk_list *head;
    struct flb_config_prop *p;

    mk_list_foreach(head, list) {
        p = mk_list_entry(head, struct flb_config_prop, _head);
        if (strcasecmp(key, p->key) == 0) {
            return p->val;
        }
    }

    return NULL;
}

static inline int prop_key_check(char *key, char *kv, int k_len)
{
    size_t len;

    len = strnlen(key,256);
    if (strncasecmp(key, kv, k_len) == 0 && len == k_len) {
        return 0;
    }
    return -1;
}

static int set_log_level(struct flb_config *config, char *v_str)
{
    if (v_str != NULL && config->log != NULL) {
        if (strcasecmp(v_str, "error") == 0) {
            config->log->level = 1;
        }
        else if (strcasecmp(v_str, "warning") == 0) {
            config->log->level = 2;
        }
        else if (strcasecmp(v_str, "info") == 0) {
            config->log->level = 3;
        }
        else if (strcasecmp(v_str, "debug") == 0) {
            config->log->level = 4;
        }
        else if (strcasecmp(v_str, "trace") == 0) {
            config->log->level = 5;
        }
        else {
            return -1;
        }
    }else{
        flb_error("Not set log level");
    }
    return 0;
}

static inline int atobool(char*v)
{
    return  (strncasecmp("true", v, 256) == 0)
        ? FLB_TRUE
        : FLB_FALSE;
}   

int flb_config_set_property(struct flb_config *config,
                            char *k, char *v)
{
    int i=0;
    int ret = -1;
    int*  i_val;
    char* s_val;
    size_t len = strnlen(k, 256);
    char* key = service_configs[0].key;

    while( key != NULL ){
        if ( prop_key_check(key, k,len) == 0) {
            if ( !strncasecmp(key, FLB_CONF_STR_LOGLEVEL ,256) ) {
                ret = set_log_level(config, v);
            }else{
                ret = 0;
                switch(service_configs[i].type){
                case FLB_CONF_TYPE_INT:
                    i_val  = (int*)((char*)config + service_configs[i].offset);
                    *i_val = atoi(v);
                    break;

                case FLB_CONF_TYPE_BOOL:
                    i_val = (int*)((char*)config+service_configs[i].offset);
                    *i_val = atobool(v);
                    break;

                case FLB_CONF_TYPE_STR:
                    s_val = (char*)config+service_configs[i].offset;
                    s_val = strdup(v);
                    break;

                default:
                    ret = -1;
                }
            }

            if (ret < 0) {
                flb_error("config parameter error");
                return -1;
            }
            return 0;
        }
        key = service_configs[++i].key;
    }
    return 0;
}
