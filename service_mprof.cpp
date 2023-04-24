#include "service_mprof.h"

mprof_app::mprof_app() {
    for (int i = 0; i < BUCK_HASH_SIZE; i++) {
        m_buckhash[i] = NULL;
    }
    m_ctx = NULL;
    m_recv_buf = "";
    m_mem_prof_rate = 0; // closed
    m_records_num = 0;
    m_bucks_num = 0;
    m_bucklist = NULL;
    m_socketId = -1;
}

mprof_app::~mprof_app() {
    struct bucket* pcurr = m_bucklist;
    while (pcurr) {
        void* tmp = pcurr;
        pcurr = pcurr->allnext;
        free(tmp);
    }
}

void mprof_app::init_pipe() {
    int temp[2];
    if (pipe(temp)) {
        skynet_error(m_ctx, "mprof: pipe create failed");
        return;
    }
    // set write fd
    mprof_notify_fd = temp[1];
    // bind read fd to skynet socket poll
    m_socketId = skynet_socket_bind(m_ctx, temp[0]);
    assert(m_socketId >= 0);
}

static inline int
read_size(const uint8_t * buffer) {
	int r = (int)buffer[0] << 8 | (int)buffer[1];
	return r;
}

void mprof_app::handle_socket_msg(const struct skynet_socket_message* message) {
    switch(message->type) {
    case SKYNET_SOCKET_TYPE_DATA: {
        assert(message->id == m_socketId);
        const char* buf = message->buffer;
        int sz = message->ud;
        m_recv_buf.append(buf, sz);
        while (true) {
            if (m_recv_buf.length() < HEADER_LEN) {
                break;
            }
            const char* rbuf = m_recv_buf.c_str();
            int pkg_len = read_size((const uint8_t*)rbuf);
            if ((int)m_recv_buf.length() < pkg_len + HEADER_LEN) {
                break;
            }
            std::string data = m_recv_buf.substr(HEADER_LEN, pkg_len);
            m_recv_buf = m_recv_buf.substr(pkg_len + HEADER_LEN);
            handle_cmd(data.c_str(), data.length());
        }
        break;
    }
    }
}

void mprof_app::handle_cmd(const char* msg, int sz) {
    if (sz == 0) {
        skynet_error(m_ctx, "mprof: null msg");
        return;
    }
    char tmp[sz+1];
    memcpy(tmp, msg, sz);
    tmp[sz] = '\0';
    int cmdlen;
    for (cmdlen = 0; cmdlen < sz; cmdlen++) {
        if (tmp[cmdlen] == ' ') {
            break;
        }
    }

    if (memcmp(tmp, "start", cmdlen) == 0) {
        m_mem_prof_rate = 1; // only support full sampling now
        skynet_error(m_ctx, "mprof: open mem sampling");
    } else if (memcmp(tmp, "stop", cmdlen) == 0) {
        m_mem_prof_rate = 0;
        skynet_error(m_ctx, "mprof: stop mem sampling");
    } else if (memcmp(tmp, "malloc", cmdlen) == 0) {
        if (cmdlen == sz) {
            skynet_error(m_ctx, "mprof: malloc param error");
            return;
        }
        if (m_mem_prof_rate == 0) {
            return;
        }
        char* param = tmp + cmdlen + 1;
        void* ptr = NULL;
        memcpy(&ptr, param, sizeof(ptr));
        int offset = sizeof(ptr);
        size_t size;
        memcpy(&size, param + offset, sizeof(size));
        offset += sizeof(size);
        int stack_depth;
        memcpy(&stack_depth, param + offset, sizeof(stack_depth));
        offset += sizeof(stack_depth);
        stack_depth = (stack_depth > MAX_STACK_SIZE) ? MAX_STACK_SIZE : stack_depth;
        void* stack_trace[stack_depth];
        memcpy(stack_trace, param + offset, stack_depth * sizeof(void*));

        // Hash stack.
        uint64_t hash = 0;
        for (int i = 0; i < stack_depth; i++) {
            hash += (uint64_t)stack_trace[i];
            hash += hash << 10;
            hash ^= hash >> 6;
        }
        // hash in size
        hash += size;
        hash += hash << 10;
        hash ^= hash >> 6;
        // finalize
        hash += hash << 3;
        hash ^= hash >> 11;

        int slot = hash % BUCK_HASH_SIZE;
        struct bucket* b = NULL;
        struct bucket* pcurr = m_buckhash[slot];
        while (pcurr) {
            if (pcurr->hash == hash
            && pcurr->size == size
            && pcurr->depth == stack_depth
            && memcmp(pcurr->stack, stack_trace, sizeof(void*) * stack_depth) == 0) {
                b = pcurr;
                break;
            }
            pcurr = pcurr->next;
        }
        if (!b) {
            b = (struct bucket*)malloc(sizeof(struct bucket));
            b->alloc_objs = 0;
            b->alloc_bytes = 0;
            b->free_objs = 0;
            b->free_bytes = 0;

            b->depth = stack_depth;
            memcpy(b->stack, stack_trace, sizeof(void*) * stack_depth);
            b->hash = hash;
            b->size = size;

            b->next = m_buckhash[slot];
            m_buckhash[slot] = b;

            b->allnext = m_bucklist;
            m_bucklist = b;
            m_bucks_num++;
        }
        b->alloc_objs++;
        b->alloc_bytes += size;
        m_mem2buck[ptr] = b;
        m_records_num++;

    } else if (memcmp(tmp, "free", cmdlen) == 0) {
        if (cmdlen == sz) {
            skynet_error(m_ctx, "mprof: free param error");
            return;
        }
        char* param = tmp + cmdlen + 1;
        void* ptr = NULL;
        memcpy(&ptr, param, sizeof(ptr));
        int offset = sizeof(ptr);
        size_t size;
        memcpy(&size, param + offset, sizeof(size));
        struct bucket* b = m_mem2buck[ptr];
        if (b) {
            b->free_objs++;
            b->free_bytes += size;
        }
    } else if (memcmp(tmp, "dump", cmdlen) == 0) {
        if (cmdlen == sz) {
            skynet_error(m_ctx, "mprof: dump param error");
            return;
        }
        char* filename = tmp + cmdlen + 1;
        build_func_symbol_table();
        dump_mem_records(filename);
    } else {
        skynet_error(m_ctx, "mprof: unknown msg: %s", tmp);
    }
}

static inline void
put_bigendian_u32(uint8_t* buf, uint32_t v) {
    buf[0] = (uint8_t)(v >> 24);
    buf[1] = (uint8_t)(v >> 16);
    buf[2] = (uint8_t)(v >> 8);
    buf[3] = (uint8_t)v;
}

static inline void
put_bigendian_u64(uint8_t* buf, uint64_t v) {
    buf[0] = (uint8_t)(v >> 56);
    buf[1] = (uint8_t)(v >> 48);
    buf[2] = (uint8_t)(v >> 40);
    buf[3] = (uint8_t)(v >> 32);
    buf[4] = (uint8_t)(v >> 24);
    buf[5] = (uint8_t)(v >> 16);
    buf[6] = (uint8_t)(v >> 8);
    buf[7] = (uint8_t)v;
}

static void
write_file(int fd, const char* buf, size_t len) {
    while (len > 0) {
        ssize_t r = write(fd, buf, len);
        buf += r;
        len -= r;
    }
}

void mprof_app::dump_mem_records(char* filename) {
    if (m_bucklist == NULL) {
        skynet_error(m_ctx, "mprof: no mem records");
        return;
    }
    int fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd <= 0) {
        skynet_error(m_ctx, "mprof: open dump file failed, %s", filename);
        return;
    }
    // calc func info len
    int func_num = 0;
    int func_info_len = 4; // total func num
    for (auto iter = m_func_name2Id.begin(); iter != m_func_name2Id.end(); iter++) {
        func_num++;
        const std::string& name = iter->first;
        int name_len = name.length();
        name_len = (name_len > MAX_FUNC_NAME_SIZE) ? MAX_FUNC_NAME_SIZE : name_len;
        func_info_len += (name_len + 1); //func name
        func_info_len += 8; //func Id
    }
    // calc samples data len
    int samples_len = 0;
    struct bucket* pcurr = m_bucklist;
    while (pcurr) {
        // alloc_objs + alloc_bytes + free_objs + free_bytes + depth + stack[:depth]
        samples_len += (20 + pcurr->depth * sizeof(uint64_t));
        pcurr = pcurr->allnext;
    }

    int total_len = 4 + func_info_len + samples_len;
    uint8_t* buf = (uint8_t*)malloc(total_len);
    // write profile data len
    put_bigendian_u32(buf, func_info_len + samples_len);
    // write func info
    int offset = 4;
    put_bigendian_u32(buf+offset, func_num);
    offset += 4;
    for (auto iter = m_func_name2Id.begin(); iter != m_func_name2Id.end(); iter++) {
        const std::string& name = iter->first;
        int name_len = name.length();
        name_len = (name_len > MAX_FUNC_NAME_SIZE) ? MAX_FUNC_NAME_SIZE : name_len;
        buf[offset] = name_len;
        offset += 1;
        memcpy(buf+offset, name.c_str(), name_len);
        offset += name_len;

        uint64_t Id = (uint64_t)iter->second;
        put_bigendian_u64(buf+offset, Id);
        offset += 8;
    }
    // write samples data
    pcurr = m_bucklist;
    while (pcurr) {
        put_bigendian_u32(buf+offset, pcurr->alloc_objs);
        offset += 4;
        put_bigendian_u32(buf+offset, pcurr->alloc_bytes);
        offset += 4;
        put_bigendian_u32(buf+offset, pcurr->free_objs);
        offset += 4;
        put_bigendian_u32(buf+offset, pcurr->free_bytes);
        offset += 4;
        put_bigendian_u32(buf+offset, pcurr->depth);
        offset += 4;
        for (int i = 0; i < pcurr->depth; i++) {
            put_bigendian_u64(buf+offset, (uint64_t)pcurr->stack[i]);
            offset += 8;
        }

        pcurr = pcurr->allnext;
    }

    write_file(fd, (const char*)buf, total_len);
    close(fd);
    skynet_error(m_ctx, "mprof: save %d records, %d buckets, %d nodes to %s", m_records_num, m_bucks_num, func_num, filename);
}

void mprof_app::build_func_symbol_table() {
    struct bucket* pcurr = m_bucklist;
    while (pcurr) {
        int backtrace = 0;
        for (int i = 0; i < pcurr->depth; i++) {
            void* pc = pcurr->stack[i];
            if (m_func_Id2name.find(pc) == m_func_Id2name.end()) {
                backtrace = 1;
                break;
            }
        }
        if (backtrace) {
            char** stack_strings = (char **)backtrace_symbols(pcurr->stack, pcurr->depth);
            if (stack_strings == NULL) {
                skynet_error(m_ctx, "mprof: memory is not enough when dump stack trace");
                abort();
            }
            for (int i = 0; i < pcurr->depth; i++) {
                void* pc = pcurr->stack[i];
                if (m_func_Id2name.find(pc) == m_func_Id2name.end()) {
                    std::string name = stack_strings[i];
                    m_func_Id2name[pc] = name;
                    m_func_name2Id[name] = pc;
                }
            }
            free(stack_strings);
        }

        pcurr = pcurr->allnext;
    }
}

extern "C" int
_cb(struct skynet_context * ctx, void * ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
    mprof_app* app = (mprof_app*)ud;
    switch(type) {
    case PTYPE_TEXT:
        app->handle_cmd((const char*)msg, sz);
        break;
    case PTYPE_SOCKET:
        app->handle_socket_msg((const struct skynet_socket_message*)msg);
        break;
    }
    return 0;
}

extern "C" mprof_app*
mprof_create(void) {
    mprof_app* app = new mprof_app();
    return app;
}

extern "C" void
mprof_release(mprof_app* app) {
    delete app;
}

extern "C" int
mprof_init(mprof_app* app, struct skynet_context* ctx, char* parm) {
    mprof_svc_handle = skynet_context_handle(ctx);
    app->m_ctx = ctx;
    app->init_pipe();
    skynet_callback(ctx, app, _cb);
    skynet_error(ctx, "mprof: service init done");
    return 0;
}