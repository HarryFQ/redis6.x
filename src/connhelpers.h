
/*
 * Copyright (c) 2019, Redis Labs
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __REDIS_CONNHELPERS_H
#define __REDIS_CONNHELPERS_H

#include "connection.h"

/* These are helper functions that are common to different connection
 * implementations (currently sockets in connection.c and TLS in tls.c).
 * (这些是不同连接实现(当前是connect .c中的套接字和ls.c中的tl)常见的辅助函数。)
 * Currently helpers implement the mechanisms for invoking connection
 * handlers and tracking connection references, to allow safe destruction
 * of connections from within a handler.
 * (当前的helper实现了调用连接处理程序和跟踪连接引用的机制，以允许从处理程序内安全地销毁连接。)
 */

/* Incremenet connection references.
 *
 * Inside a connection handler, we guarantee refs >= 1 so it is always
 * safe to connClose().
 *（Incremenet连接引用。在连接处理程序中，我们保证refs >= 1，所以使用connClose()总是安全的。）
 *
 * In other cases where we don't want to prematurely lose the connection,
 * it can go beyond 1 as well; currently it is only done by connAccept().
 * (在其他情况下，我们不想过早地失去联系，它也可以超过1;目前仅由connAccept()完成。)
 */
/**
 * 连接辅助类
 * @param conn
 */
static inline void connIncrRefs(connection *conn) {
    conn->refs++;
}

/* Decrement connection references.
 *
 * Note that this is not intended to provide any automatic free logic!
 * callHandler() takes care of that for the common flows, and anywhere an
 * explicit connIncrRefs() is used, the caller is expected to take care of
 * that.
 */

static inline void connDecrRefs(connection *conn) {
    conn->refs--;
}

static inline int connHasRefs(connection *conn) {
    return conn->refs;
}

/* Helper for connection implementations to call handlers:
 * 1. Increment refs to protect the connection.
 * 2. Execute the handler (if set).
 * 3. Decrement refs and perform deferred close, if refs==0.
 */
static inline int callHandler(connection *conn, ConnectionCallbackFunc handler) {
    connIncrRefs(conn);
    if (handler) handler(conn);
    connDecrRefs(conn);
    if (conn->flags & CONN_FLAG_CLOSE_SCHEDULED) {
        if (!connHasRefs(conn)) connClose(conn);
        return 0;
    }
    return 1;
}

#endif  /* __REDIS_CONNHELPERS_H */
