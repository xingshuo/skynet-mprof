#pragma once

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <string>
#include <map>
#include <unordered_map>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <execinfo.h>
#include <assert.h>

extern "C" {
#include "skynet.h"
#include "skynet_socket.h"
#include "skynet_server.h"
}

extern int mprof_notify_fd; // defined in malloc_hook.c
extern uint32_t mprof_svc_handle; // defined in malloc_hook.c

static const uint8_t MAX_FUNC_NAME_SIZE = 80;

static const int MAX_STACK_SIZE = 32;
static const int BUCK_HASH_SIZE = 179999;
static const int HEADER_LEN = 2;

struct bucket {
    int alloc_objs;
    int alloc_bytes;
    int free_objs;
    int free_bytes;

    int depth;
    void* stack[MAX_STACK_SIZE];
    uint64_t hash;
    size_t size;
    struct bucket* next;
    struct bucket* allnext;
};

class mprof_app {
public:
    mprof_app();
    ~mprof_app();
    void init_pipe();

    void handle_cmd(const char* msg, int sz);
    void handle_socket_msg(const struct skynet_socket_message* message);

    void build_func_symbol_table();
    void dump_mem_records(char* filename);

public:
    struct bucket* m_buckhash[BUCK_HASH_SIZE];
    struct bucket* m_bucklist;
    std::unordered_map<std::string, void*> m_func_name2Id;
    std::unordered_map<void*, std::string> m_func_Id2name;
    std::unordered_map<void*, struct bucket*> m_mem2buck;
    std::string m_recv_buf;
    struct skynet_context* m_ctx;
    int m_mem_prof_rate;
    int m_records_num;
    int m_bucks_num;
    int m_socketId;
};