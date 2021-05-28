/*
 * Copyright (c) 2009-2016, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "server.h"
#include "cluster.h"
#include "slowlog.h"
#include "bio.h"
#include "latency.h"
#include "atomicvar.h"

#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <locale.h>
#include <sys/socket.h>
/**
 * redis的主文件
 */
/* Our shared "common" objects */

struct sharedObjectsStruct shared;

/* Global vars that are actually used as constants. The following double
 * values are used for double on-disk serialization, and are initialized
 * at runtime to avoid strange compiler optimizations. */

double R_Zero, R_PosInf, R_NegInf, R_Nan;

/*================================= Globals ================================= */

/* Global vars */
struct redisServer server; /* Server global state */
volatile unsigned long lru_clock; /* Server global current LRU time. */

/* Our command table.
 *
 * Every entry is composed of the following fields:
 *
 * name:        A string representing the command name.
 *
 * function:    Pointer to the C function implementing the command.
 *
 * arity:       Number of arguments, it is possible to use -N to say >= N
 *
 * sflags:      Command flags as string. See below for a table of flags.
 *
 * flags:       Flags as bitmask. Computed by Redis using the 'sflags' field.
 *
 * get_keys_proc: An optional function to get key arguments from a command.
 *                This is only used when the following three fields are not
 *                enough to specify what arguments are keys.
 *
 * first_key_index: First argument that is a key
 *
 * last_key_index: Last argument that is a key
 *
 * key_step:    Step to get all the keys from first to last argument.
 *              For instance in MSET the step is two since arguments
 *              are key,val,key,val,...
 *
 * microseconds: Microseconds of total execution time for this command.
 *
 * calls:       Total number of calls of this command.
 *
 * id:          Command bit identifier for ACLs or other goals.
 *
 * The flags, microseconds and calls fields are computed by Redis and should
 * always be set to zero.
 *
 * Command flags are expressed using space separated strings, that are turned
 * into actual flags by the populateCommandTable() function.
 *
 * This is the meaning of the flags:
 *
 * write:       Write command (may modify the key space).
 *
 * read-only:   All the non special commands just reading from keys without
 *              changing the content, or returning other informations like
 *              the TIME command. Special commands such administrative commands
 *              or transaction related commands (multi, exec, discard, ...)
 *              are not flagged as read-only commands, since they affect the
 *              server or the connection in other ways.
 *
 * use-memory:  May increase memory usage once called. Don't allow if out
 *              of memory.
 *
 * admin:       Administrative command, like SAVE or SHUTDOWN.
 *
 * pub-sub:     Pub/Sub related command.
 *
 * no-script:   Command not allowed in scripts.
 *
 * random:      Random command. Command is not deterministic, that is, the same
 *              command with the same arguments, with the same key space, may
 *              have different results. For instance SPOP and RANDOMKEY are
 *              two random commands.
 *
 * to-sort:     Sort command output array if called from script, so that the
 *              output is deterministic. When this flag is used (not always
 *              possible), then the "random" flag is not needed.
 *
 * ok-loading:  Allow the command while loading the database.
 *
 * ok-stale:    Allow the command while a slave has stale data but is not
 *              allowed to serve this data. Normally no command is accepted
 *              in this condition but just a few.
 *
 * no-monitor:  Do not automatically propagate the command on MONITOR.
 *
 * no-slowlog:  Do not automatically propagate the command to the slowlog.
 *
 * cluster-asking: Perform an implicit ASKING for this command, so the
 *              command will be accepted in cluster mode if the slot is marked
 *              as 'importing'.
 *
 * fast:        Fast command: O(1) or O(log(N)) command that should never
 *              delay its execution as long as the kernel scheduler is giving
 *              us time. Note that commands that may trigger a DEL as a side
 *              effect (like SET) are not fast commands.
 *
 * The following additional flags are only used in order to put commands
 * in a specific ACL category. Commands can have multiple ACL categories.
 *
 * @keyspace, @read, @write, @set, @sortedset, @list, @hash, @string, @bitmap,
 * @hyperloglog, @stream, @admin, @fast, @slow, @pubsub, @blocking, @dangerous,
 * @connection, @transaction, @scripting, @geo.
 *
 * Note that:
 *
 * 1) The read-only flag implies the @read ACL category.
 * 2) The write flag implies the @write ACL category.
 * 3) The fast flag implies the @fast ACL category.
 * 4) The admin flag implies the @admin and @dangerous ACL category.
 * 5) The pub-sub flag implies the @pubsub ACL category.
 * 6) The lack of fast flag implies the @slow ACL category.
 * 7) The non obvious "keyspace" category includes the commands
 *    that interact with keys without having anything to do with
 *    specific data structures, such as: DEL, RENAME, MOVE, SELECT,
 *    TYPE, EXPIRE*, PEXPIRE*, TTL, PTTL, ...
 */

struct redisCommand redisCommandTable[] = {
        {"module",               moduleCommand,              -2,
                "admin no-script",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"get",                  getCommand,                 2,
                "read-only fast @string",
                0, NULL,               1, 1,  1, 0, 0, 0},

        /* Note that we can't flag set as fast, since it may perform an
     * implicit DEL of a large key. */
        {"set",                  setCommand,                 -3,
                "write use-memory @string",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"setnx",                setnxCommand,               3,
                "write use-memory fast @string",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"setex",                setexCommand,               4,
                "write use-memory @string",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"psetex",               psetexCommand,              4,
                "write use-memory @string",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"append",               appendCommand,              3,
                "write use-memory fast @string",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"strlen",               strlenCommand,              2,
                "read-only fast @string",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"del",                  delCommand,                 -2,
                "write @keyspace",
                0, NULL,               1, -1, 1, 0, 0, 0},

        {"unlink",               unlinkCommand,              -2,
                "write fast @keyspace",
                0, NULL,               1, -1, 1, 0, 0, 0},

        {"exists",               existsCommand,              -2,
                "read-only fast @keyspace",
                0, NULL,               1, -1, 1, 0, 0, 0},

        {"setbit",               setbitCommand,              4,
                "write use-memory @bitmap",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"getbit",               getbitCommand,              3,
                "read-only fast @bitmap",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"bitfield",             bitfieldCommand,            -2,
                "write use-memory @bitmap",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"bitfield_ro",          bitfieldroCommand,          -2,
                "read-only fast @bitmap",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"setrange",             setrangeCommand,            4,
                "write use-memory @string",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"getrange",             getrangeCommand,            4,
                "read-only @string",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"substr",               getrangeCommand,            4,
                "read-only @string",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"incr",                 incrCommand,                2,
                "write use-memory fast @string",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"decr",                 decrCommand,                2,
                "write use-memory fast @string",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"mget",                 mgetCommand,                -2,
                "read-only fast @string",
                0, NULL,               1, -1, 1, 0, 0, 0},

        {"rpush",                rpushCommand,               -3,
                "write use-memory fast @list",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"lpush",                lpushCommand,               -3,
                "write use-memory fast @list",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"rpushx",               rpushxCommand,              -3,
                "write use-memory fast @list",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"lpushx",               lpushxCommand,              -3,
                "write use-memory fast @list",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"linsert",              linsertCommand,             5,
                "write use-memory @list",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"rpop",                 rpopCommand,                2,
                "write fast @list",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"lpop",                 lpopCommand,                2,
                "write fast @list",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"brpop",                brpopCommand,               -3,
                "write no-script @list @blocking",
                0, NULL,               1, -2, 1, 0, 0, 0},

        {"brpoplpush",           brpoplpushCommand,          4,
                "write use-memory no-script @list @blocking",
                0, NULL,               1, 2,  1, 0, 0, 0},

        {"blpop",                blpopCommand,               -3,
                "write no-script @list @blocking",
                0, NULL,               1, -2, 1, 0, 0, 0},

        {"llen",                 llenCommand,                2,
                "read-only fast @list",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"lindex",               lindexCommand,              3,
                "read-only @list",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"lset",                 lsetCommand,                4,
                "write use-memory @list",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"lrange",               lrangeCommand,              4,
                "read-only @list",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"ltrim",                ltrimCommand,               4,
                "write @list",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"lpos",                 lposCommand,                -3,
                "read-only @list",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"lrem",                 lremCommand,                4,
                "write @list",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"rpoplpush",            rpoplpushCommand,           3,
                "write use-memory @list",
                0, NULL,               1, 2,  1, 0, 0, 0},

        {"sadd",                 saddCommand,                -3,
                "write use-memory fast @set",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"srem",                 sremCommand,                -3,
                "write fast @set",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"smove",                smoveCommand,               4,
                "write fast @set",
                0, NULL,               1, 2,  1, 0, 0, 0},

        {"sismember",            sismemberCommand,           3,
                "read-only fast @set",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"scard",                scardCommand,               2,
                "read-only fast @set",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"spop",                 spopCommand,                -2,
                "write random fast @set",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"srandmember",          srandmemberCommand,         -2,
                "read-only random @set",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"sinter",               sinterCommand,              -2,
                "read-only to-sort @set",
                0, NULL,               1, -1, 1, 0, 0, 0},

        {"sinterstore",          sinterstoreCommand,         -3,
                "write use-memory @set",
                0, NULL,               1, -1, 1, 0, 0, 0},

        {"sunion",               sunionCommand,              -2,
                "read-only to-sort @set",
                0, NULL,               1, -1, 1, 0, 0, 0},

        {"sunionstore",          sunionstoreCommand,         -3,
                "write use-memory @set",
                0, NULL,               1, -1, 1, 0, 0, 0},

        {"sdiff",                sdiffCommand,               -2,
                "read-only to-sort @set",
                0, NULL,               1, -1, 1, 0, 0, 0},

        {"sdiffstore",           sdiffstoreCommand,          -3,
                "write use-memory @set",
                0, NULL,               1, -1, 1, 0, 0, 0},

        {"smembers",             sinterCommand,              2,
                "read-only to-sort @set",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"sscan",                sscanCommand,               -3,
                "read-only random @set",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"zadd",                 zaddCommand,                -4,
                "write use-memory fast @sortedset",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"zincrby",              zincrbyCommand,             4,
                "write use-memory fast @sortedset",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"zrem",                 zremCommand,                -3,
                "write fast @sortedset",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"zremrangebyscore",     zremrangebyscoreCommand,    4,
                "write @sortedset",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"zremrangebyrank",      zremrangebyrankCommand,     4,
                "write @sortedset",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"zremrangebylex",       zremrangebylexCommand,      4,
                "write @sortedset",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"zunionstore",          zunionstoreCommand,         -4,
                "write use-memory @sortedset",
                0, zunionInterGetKeys, 0, 0,  0, 0, 0, 0},

        {"zinterstore",          zinterstoreCommand,         -4,
                "write use-memory @sortedset",
                0, zunionInterGetKeys, 0, 0,  0, 0, 0, 0},

        {"zrange",               zrangeCommand,              -4,
                "read-only @sortedset",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"zrangebyscore",        zrangebyscoreCommand,       -4,
                "read-only @sortedset",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"zrevrangebyscore",     zrevrangebyscoreCommand,    -4,
                "read-only @sortedset",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"zrangebylex",          zrangebylexCommand,         -4,
                "read-only @sortedset",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"zrevrangebylex",       zrevrangebylexCommand,      -4,
                "read-only @sortedset",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"zcount",               zcountCommand,              4,
                "read-only fast @sortedset",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"zlexcount",            zlexcountCommand,           4,
                "read-only fast @sortedset",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"zrevrange",            zrevrangeCommand,           -4,
                "read-only @sortedset",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"zcard",                zcardCommand,               2,
                "read-only fast @sortedset",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"zscore",               zscoreCommand,              3,
                "read-only fast @sortedset",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"zrank",                zrankCommand,               3,
                "read-only fast @sortedset",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"zrevrank",             zrevrankCommand,            3,
                "read-only fast @sortedset",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"zscan",                zscanCommand,               -3,
                "read-only random @sortedset",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"zpopmin",              zpopminCommand,             -2,
                "write fast @sortedset",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"zpopmax",              zpopmaxCommand,             -2,
                "write fast @sortedset",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"bzpopmin",             bzpopminCommand,            -3,
                "write no-script fast @sortedset @blocking",
                0, NULL,               1, -2, 1, 0, 0, 0},

        {"bzpopmax",             bzpopmaxCommand,            -3,
                "write no-script fast @sortedset @blocking",
                0, NULL,               1, -2, 1, 0, 0, 0},

        {"hset",                 hsetCommand,                -4,
                "write use-memory fast @hash",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"hsetnx",               hsetnxCommand,              4,
                "write use-memory fast @hash",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"hget",                 hgetCommand,                3,
                "read-only fast @hash",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"hmset",                hsetCommand,                -4,
                "write use-memory fast @hash",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"hmget",                hmgetCommand,               -3,
                "read-only fast @hash",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"hincrby",              hincrbyCommand,             4,
                "write use-memory fast @hash",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"hincrbyfloat",         hincrbyfloatCommand,        4,
                "write use-memory fast @hash",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"hdel",                 hdelCommand,                -3,
                "write fast @hash",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"hlen",                 hlenCommand,                2,
                "read-only fast @hash",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"hstrlen",              hstrlenCommand,             3,
                "read-only fast @hash",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"hkeys",                hkeysCommand,               2,
                "read-only to-sort @hash",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"hvals",                hvalsCommand,               2,
                "read-only to-sort @hash",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"hgetall",              hgetallCommand,             2,
                "read-only random @hash",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"hexists",              hexistsCommand,             3,
                "read-only fast @hash",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"hscan",                hscanCommand,               -3,
                "read-only random @hash",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"incrby",               incrbyCommand,              3,
                "write use-memory fast @string",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"decrby",               decrbyCommand,              3,
                "write use-memory fast @string",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"incrbyfloat",          incrbyfloatCommand,         3,
                "write use-memory fast @string",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"getset",               getsetCommand,              3,
                "write use-memory fast @string",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"mset",                 msetCommand,                -3,
                "write use-memory @string",
                0, NULL,               1, -1, 2, 0, 0, 0},

        {"msetnx",               msetnxCommand,              -3,
                "write use-memory @string",
                0, NULL,               1, -1, 2, 0, 0, 0},

        {"randomkey",            randomkeyCommand,           1,
                "read-only random @keyspace",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"select",               selectCommand,              2,
                "ok-loading fast ok-stale @keyspace",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"swapdb",               swapdbCommand,              3,
                "write fast @keyspace @dangerous",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"move",                 moveCommand,                3,
                "write fast @keyspace",
                0, NULL,               1, 1,  1, 0, 0, 0},

        /* Like for SET, we can't mark rename as a fast command because
     * overwriting the target key may result in an implicit slow DEL. */
        {"rename",               renameCommand,              3,
                "write @keyspace",
                0, NULL,               1, 2,  1, 0, 0, 0},

        {"renamenx",             renamenxCommand,            3,
                "write fast @keyspace",
                0, NULL,               1, 2,  1, 0, 0, 0},

        {"expire",               expireCommand,              3,
                "write fast @keyspace",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"expireat",             expireatCommand,            3,
                "write fast @keyspace",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"pexpire",              pexpireCommand,             3,
                "write fast @keyspace",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"pexpireat",            pexpireatCommand,           3,
                "write fast @keyspace",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"keys",                 keysCommand,                2,
                "read-only to-sort @keyspace @dangerous",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"scan",                 scanCommand,                -2,
                "read-only random @keyspace",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"dbsize",               dbsizeCommand,              1,
                "read-only fast @keyspace",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"auth",                 authCommand,                -2,
                "no-auth no-script ok-loading ok-stale fast no-monitor no-slowlog @connection",
                0, NULL,               0, 0,  0, 0, 0, 0},

        /* We don't allow PING during loading since in Redis PING is used as
     * failure detection, and a loading server is considered to be
     * not available. */
        {"ping",                 pingCommand,                -1,
                "ok-stale fast @connection",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"echo",                 echoCommand,                2,
                "read-only fast @connection",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"save",                 saveCommand,                1,
                "admin no-script",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"bgsave",               bgsaveCommand,              -1,
                "admin no-script",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"bgrewriteaof",         bgrewriteaofCommand,        1,
                "admin no-script",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"shutdown",             shutdownCommand,            -1,
                "admin no-script ok-loading ok-stale",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"lastsave",             lastsaveCommand,            1,
                "read-only random fast ok-loading ok-stale @admin @dangerous",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"type",                 typeCommand,                2,
                "read-only fast @keyspace",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"multi",                multiCommand,               1,
                "no-script fast ok-loading ok-stale @transaction",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"exec",                 execCommand,                1,
                "no-script no-monitor no-slowlog ok-loading ok-stale @transaction",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"discard",              discardCommand,             1,
                "no-script fast ok-loading ok-stale @transaction",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"sync",                 syncCommand,                1,
                "admin no-script",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"psync",                syncCommand,                3,
                "admin no-script",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"replconf",             replconfCommand,            -1,
                "admin no-script ok-loading ok-stale",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"flushdb",              flushdbCommand,             -1,
                "write @keyspace @dangerous",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"flushall",             flushallCommand,            -1,
                "write @keyspace @dangerous",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"sort",                 sortCommand,                -2,
                "write use-memory @list @set @sortedset @dangerous",
                0, sortGetKeys,        1, 1,  1, 0, 0, 0},

        {"info",                 infoCommand,                -1,
                "ok-loading ok-stale random @dangerous",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"monitor",              monitorCommand,             1,
                "admin no-script ok-loading ok-stale",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"ttl",                  ttlCommand,                 2,
                "read-only fast random @keyspace",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"touch",                touchCommand,               -2,
                "read-only fast @keyspace",
                0, NULL,               1, -1, 1, 0, 0, 0},

        {"pttl",                 pttlCommand,                2,
                "read-only fast random @keyspace",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"persist",              persistCommand,             2,
                "write fast @keyspace",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"slaveof",              replicaofCommand,           3,
                "admin no-script ok-stale",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"replicaof",            replicaofCommand,           3,
                "admin no-script ok-stale",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"role",                 roleCommand,                1,
                "ok-loading ok-stale no-script fast read-only @dangerous",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"debug",                debugCommand,               -2,
                "admin no-script ok-loading ok-stale",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"config",               configCommand,              -2,
                "admin ok-loading ok-stale no-script",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"subscribe",            subscribeCommand,           -2,
                "pub-sub no-script ok-loading ok-stale",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"unsubscribe",          unsubscribeCommand,         -1,
                "pub-sub no-script ok-loading ok-stale",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"psubscribe",           psubscribeCommand,          -2,
                "pub-sub no-script ok-loading ok-stale",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"punsubscribe",         punsubscribeCommand,        -1,
                "pub-sub no-script ok-loading ok-stale",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"publish",              publishCommand,             3,
                "pub-sub ok-loading ok-stale fast",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"pubsub",               pubsubCommand,              -2,
                "pub-sub ok-loading ok-stale random",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"watch",                watchCommand,               -2,
                "no-script fast ok-loading ok-stale @transaction",
                0, NULL,               1, -1, 1, 0, 0, 0},

        {"unwatch",              unwatchCommand,             1,
                "no-script fast ok-loading ok-stale @transaction",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"cluster",              clusterCommand,             -2,
                "admin ok-stale random",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"restore",              restoreCommand,             -4,
                "write use-memory @keyspace @dangerous",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"restore-asking",       restoreCommand,             -4,
                "write use-memory cluster-asking @keyspace @dangerous",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"migrate",              migrateCommand,             -6,
                "write random @keyspace @dangerous",
                0, migrateGetKeys,     0, 0,  0, 0, 0, 0},

        {"asking",               askingCommand,              1,
                "fast @keyspace",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"readonly",             readonlyCommand,            1,
                "fast @keyspace",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"readwrite",            readwriteCommand,           1,
                "fast @keyspace",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"dump",                 dumpCommand,                2,
                "read-only random @keyspace",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"object",               objectCommand,              -2,
                "read-only random @keyspace",
                0, NULL,               2, 2,  1, 0, 0, 0},

        {"memory",               memoryCommand,              -2,
                "random read-only",
                0, memoryGetKeys,      0, 0,  0, 0, 0, 0},

        {"client",               clientCommand,              -2,
                "admin no-script random ok-loading ok-stale @connection",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"hello",                helloCommand,               -2,
                "no-auth no-script fast no-monitor ok-loading ok-stale no-slowlog @connection",
                0, NULL,               0, 0,  0, 0, 0, 0},

        /* EVAL can modify the dataset, however it is not flagged as a write
     * command since we do the check while running commands from Lua. */
        {"eval",                 evalCommand,                -3,
                "no-script @scripting",
                0, evalGetKeys,        0, 0,  0, 0, 0, 0},

        {"evalsha",              evalShaCommand,             -3,
                "no-script @scripting",
                0, evalGetKeys,        0, 0,  0, 0, 0, 0},

        {"slowlog",              slowlogCommand,             -2,
                "admin random ok-loading ok-stale",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"script",               scriptCommand,              -2,
                "no-script @scripting",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"time",                 timeCommand,                1,
                "read-only random fast ok-loading ok-stale",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"bitop",                bitopCommand,               -4,
                "write use-memory @bitmap",
                0, NULL,               2, -1, 1, 0, 0, 0},

        {"bitcount",             bitcountCommand,            -2,
                "read-only @bitmap",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"bitpos",               bitposCommand,              -3,
                "read-only @bitmap",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"wait",                 waitCommand,                3,
                "no-script @keyspace",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"command",              commandCommand,             -1,
                "ok-loading ok-stale random @connection",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"geoadd",               geoaddCommand,              -5,
                "write use-memory @geo",
                0, NULL,               1, 1,  1, 0, 0, 0},

        /* GEORADIUS has store options that may write. */
        {"georadius",            georadiusCommand,           -6,
                "write @geo",
                0, georadiusGetKeys,   1, 1,  1, 0, 0, 0},

        {"georadius_ro",         georadiusroCommand,         -6,
                "read-only @geo",
                0, georadiusGetKeys,   1, 1,  1, 0, 0, 0},

        {"georadiusbymember",    georadiusbymemberCommand,   -5,
                "write @geo",
                0, georadiusGetKeys,   1, 1,  1, 0, 0, 0},

        {"georadiusbymember_ro", georadiusbymemberroCommand, -5,
                "read-only @geo",
                0, georadiusGetKeys,   1, 1,  1, 0, 0, 0},

        {"geohash",              geohashCommand,             -2,
                "read-only @geo",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"geopos",               geoposCommand,              -2,
                "read-only @geo",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"geodist",              geodistCommand,             -4,
                "read-only @geo",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"pfselftest",           pfselftestCommand,          1,
                "admin @hyperloglog",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"pfadd",                pfaddCommand,               -2,
                "write use-memory fast @hyperloglog",
                0, NULL,               1, 1,  1, 0, 0, 0},

        /* Technically speaking PFCOUNT may change the key since it changes the
     * final bytes in the HyperLogLog representation. However in this case
     * we claim that the representation, even if accessible, is an internal
     * affair, and the command is semantically read only. */
        {"pfcount",              pfcountCommand,             -2,
                "read-only @hyperloglog",
                0, NULL,               1, -1, 1, 0, 0, 0},

        {"pfmerge",              pfmergeCommand,             -2,
                "write use-memory @hyperloglog",
                0, NULL,               1, -1, 1, 0, 0, 0},

        {"pfdebug",              pfdebugCommand,             -3,
                "admin write",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"xadd",                 xaddCommand,                -5,
                "write use-memory fast random @stream",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"xrange",               xrangeCommand,              -4,
                "read-only @stream",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"xrevrange",            xrevrangeCommand,           -4,
                "read-only @stream",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"xlen",                 xlenCommand,                2,
                "read-only fast @stream",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"xread",                xreadCommand,               -4,
                "read-only @stream @blocking",
                0, xreadGetKeys,       1, 1,  1, 0, 0, 0},

        {"xreadgroup",           xreadCommand,               -7,
                "write @stream @blocking",
                0, xreadGetKeys,       1, 1,  1, 0, 0, 0},

        {"xgroup",               xgroupCommand,              -2,
                "write use-memory @stream",
                0, NULL,               2, 2,  1, 0, 0, 0},

        {"xsetid",               xsetidCommand,              3,
                "write use-memory fast @stream",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"xack",                 xackCommand,                -4,
                "write fast random @stream",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"xpending",             xpendingCommand,            -3,
                "read-only random @stream",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"xclaim",               xclaimCommand,              -6,
                "write random fast @stream",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"xinfo",                xinfoCommand,               -2,
                "read-only random @stream",
                0, NULL,               2, 2,  1, 0, 0, 0},

        {"xdel",                 xdelCommand,                -3,
                "write fast @stream",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"xtrim",                xtrimCommand,               -2,
                "write random @stream",
                0, NULL,               1, 1,  1, 0, 0, 0},

        {"post",                 securityWarningCommand,     -1,
                "ok-loading ok-stale read-only",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"host:",                securityWarningCommand,     -1,
                "ok-loading ok-stale read-only",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"latency",              latencyCommand,             -2,
                "admin no-script ok-loading ok-stale",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"lolwut",               lolwutCommand,              -1,
                "read-only fast",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"acl",                  aclCommand,                 -2,
                "admin no-script no-slowlog ok-loading ok-stale",
                0, NULL,               0, 0,  0, 0, 0, 0},

        {"stralgo",              stralgoCommand,             -2,
                "read-only @string",
                0, lcsGetKeys,         0, 0,  0, 0, 0, 0}
};

/*============================ Utility functions ============================ */

/* We use a private localtime implementation which is fork-safe. The logging
 * function of Redis may be called from other threads. */
void nolocks_localtime(struct tm *tmp, time_t t, time_t tz, int dst);

/* Low level logging. To use only for very big messages, otherwise
 * serverLog() is to prefer. */
void serverLogRaw(int level, const char *msg) {
    const int syslogLevelMap[] = {LOG_DEBUG, LOG_INFO, LOG_NOTICE, LOG_WARNING};
    const char *c = ".-*#";
    FILE *fp;
    char buf[64];
    int rawmode = (level & LL_RAW);
    int log_to_stdout = server.logfile[0] == '\0';

    level &= 0xff; /* clear flags */
    if (level < server.verbosity) return;

    fp = log_to_stdout ? stdout : fopen(server.logfile, "a");
    if (!fp) return;

    if (rawmode) {
        fprintf(fp, "%s", msg);
    } else {
        int off;
        struct timeval tv;
        int role_char;
        pid_t pid = getpid();

        gettimeofday(&tv, NULL);
        struct tm tm;
        nolocks_localtime(&tm, tv.tv_sec, server.timezone, server.daylight_active);
        off = strftime(buf, sizeof(buf), "%d %b %Y %H:%M:%S.", &tm);
        snprintf(buf + off, sizeof(buf) - off, "%03d", (int) tv.tv_usec / 1000);
        if (server.sentinel_mode) {
            role_char = 'X'; /* Sentinel. */
        } else if (pid != server.pid) {
            role_char = 'C'; /* RDB / AOF writing child. */
        } else {
            role_char = (server.masterhost ? 'S' : 'M'); /* Slave or Master. */
        }
        fprintf(fp, "%d:%c %s %c %s\n",
                (int) getpid(), role_char, buf, c[level], msg);
    }
    fflush(fp);

    if (!log_to_stdout) fclose(fp);
    if (server.syslog_enabled) syslog(syslogLevelMap[level], "%s", msg);
}

/* Like serverLogRaw() but with printf-alike support. This is the function that
 * is used across the code. The raw version is only used in order to dump
 * the INFO output on crash. */
void serverLog(int level, const char *fmt, ...) {
    va_list ap;
    char msg[LOG_MAX_LEN];

    if ((level & 0xff) < server.verbosity) return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    serverLogRaw(level, msg);
}

/* Log a fixed message without printf-alike capabilities, in a way that is
 * safe to call from a signal handler.
 *
 * We actually use this only for signals that are not fatal from the point
 * of view of Redis. Signals that are going to kill the server anyway and
 * where we need printf-alike features are served by serverLog(). */
void serverLogFromHandler(int level, const char *msg) {
    int fd;
    int log_to_stdout = server.logfile[0] == '\0';
    char buf[64];

    if ((level & 0xff) < server.verbosity || (log_to_stdout && server.daemonize))
        return;
    fd = log_to_stdout ? STDOUT_FILENO :
         open(server.logfile, O_APPEND | O_CREAT | O_WRONLY, 0644);
    if (fd == -1) return;
    ll2string(buf, sizeof(buf), getpid());
    if (write(fd, buf, strlen(buf)) == -1) goto err;
    if (write(fd, ":signal-handler (", 17) == -1) goto err;
    ll2string(buf, sizeof(buf), time(NULL));
    if (write(fd, buf, strlen(buf)) == -1) goto err;
    if (write(fd, ") ", 2) == -1) goto err;
    if (write(fd, msg, strlen(msg)) == -1) goto err;
    if (write(fd, "\n", 1) == -1) goto err;
    err:
    if (!log_to_stdout) close(fd);
}

/* Return the UNIX time in microseconds */
long long ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long) tv.tv_sec) * 1000000;
    ust += tv.tv_usec;
    return ust;
}

/* Return the UNIX time in milliseconds */
mstime_t mstime(void) {
    return ustime() / 1000;
}

/* After an RDB dump or AOF rewrite we exit from children using _exit() instead of
 * exit(), because the latter may interact with the same file objects used by
 * the parent process. However if we are testing the coverage normal exit() is
 * used in order to obtain the right coverage information. */
void exitFromChild(int retcode) {
#ifdef COVERAGE_TEST
    exit(retcode);
#else
    _exit(retcode);
#endif
}

/*====================== Hash table type implementation  ==================== */

/* This is a hash table type that uses the SDS dynamic strings library as
 * keys and redis objects as values (objects can hold SDS strings,
 * lists, sets). */

void dictVanillaFree(void *privdata, void *val) {
    DICT_NOTUSED(privdata);
    zfree(val);
}

void dictListDestructor(void *privdata, void *val) {
    DICT_NOTUSED(privdata);
    listRelease((list *) val);
}

int dictSdsKeyCompare(void *privdata, const void *key1,
                      const void *key2) {
    int l1, l2;
    DICT_NOTUSED(privdata);

    l1 = sdslen((sds) key1);
    l2 = sdslen((sds) key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

/* A case insensitive version used for the command lookup table and other
 * places where case insensitive non binary-safe comparison is needed. */
int dictSdsKeyCaseCompare(void *privdata, const void *key1,
                          const void *key2) {
    DICT_NOTUSED(privdata);

    return strcasecmp(key1, key2) == 0;
}

void dictObjectDestructor(void *privdata, void *val) {
    DICT_NOTUSED(privdata);

    if (val == NULL) return; /* Lazy freeing will set value to NULL. */
    decrRefCount(val);
}

void dictSdsDestructor(void *privdata, void *val) {
    DICT_NOTUSED(privdata);

    sdsfree(val);
}

int dictObjKeyCompare(void *privdata, const void *key1,
                      const void *key2) {
    const robj *o1 = key1, *o2 = key2;
    return dictSdsKeyCompare(privdata, o1->ptr, o2->ptr);
}

uint64_t dictObjHash(const void *key) {
    const robj *o = key;
    return dictGenHashFunction(o->ptr, sdslen((sds) o->ptr));
}

uint64_t dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char *) key, sdslen((char *) key));
}

uint64_t dictSdsCaseHash(const void *key) {
    return dictGenCaseHashFunction((unsigned char *) key, sdslen((char *) key));
}

int dictEncObjKeyCompare(void *privdata, const void *key1,
                         const void *key2) {
    robj *o1 = (robj *) key1, *o2 = (robj *) key2;
    int cmp;

    if (o1->encoding == OBJ_ENCODING_INT &&
        o2->encoding == OBJ_ENCODING_INT)
        return o1->ptr == o2->ptr;

    /* Due to OBJ_STATIC_REFCOUNT, we avoid calling getDecodedObject() without
     * good reasons, because it would incrRefCount() the object, which
     * is invalid. So we check to make sure dictFind() works with static
     * objects as well. */
    if (o1->refcount != OBJ_STATIC_REFCOUNT) o1 = getDecodedObject(o1);
    if (o2->refcount != OBJ_STATIC_REFCOUNT) o2 = getDecodedObject(o2);
    cmp = dictSdsKeyCompare(privdata, o1->ptr, o2->ptr);
    if (o1->refcount != OBJ_STATIC_REFCOUNT) decrRefCount(o1);
    if (o2->refcount != OBJ_STATIC_REFCOUNT) decrRefCount(o2);
    return cmp;
}

uint64_t dictEncObjHash(const void *key) {
    robj *o = (robj *) key;

    if (sdsEncodedObject(o)) {
        return dictGenHashFunction(o->ptr, sdslen((sds) o->ptr));
    } else {
        if (o->encoding == OBJ_ENCODING_INT) {
            char buf[32];
            int len;

            len = ll2string(buf, 32, (long) o->ptr);
            return dictGenHashFunction((unsigned char *) buf, len);
        } else {
            uint64_t hash;

            o = getDecodedObject(o);
            hash = dictGenHashFunction(o->ptr, sdslen((sds) o->ptr));
            decrRefCount(o);
            return hash;
        }
    }
}

/* Generic hash table type where keys are Redis Objects, Values
 * dummy pointers. */
dictType objectKeyPointerValueDictType = {
        dictEncObjHash,            /* hash function */
        NULL,                      /* key dup */
        NULL,                      /* val dup */
        dictEncObjKeyCompare,      /* key compare */
        dictObjectDestructor,      /* key destructor */
        NULL                       /* val destructor */
};

/* Like objectKeyPointerValueDictType(), but values can be destroyed, if
 * not NULL, calling zfree(). */
dictType objectKeyHeapPointerValueDictType = {
        dictEncObjHash,            /* hash function */
        NULL,                      /* key dup */
        NULL,                      /* val dup */
        dictEncObjKeyCompare,      /* key compare */
        dictObjectDestructor,      /* key destructor */
        dictVanillaFree            /* val destructor */
};

/* Set dictionary type. Keys are SDS strings, values are ot used. */
dictType setDictType = {
        dictSdsHash,               /* hash function */
        NULL,                      /* key dup */
        NULL,                      /* val dup */
        dictSdsKeyCompare,         /* key compare */
        dictSdsDestructor,         /* key destructor */
        NULL                       /* val destructor */
};

/* Sorted sets hash (note: a skiplist is used in addition to the hash table) */
dictType zsetDictType = {
        dictSdsHash,               /* hash function */
        NULL,                      /* key dup */
        NULL,                      /* val dup */
        dictSdsKeyCompare,         /* key compare */
        NULL,                      /* Note: SDS string shared & freed by skiplist */
        NULL                       /* val destructor */
};

/* Db->dict, keys are sds strings, vals are Redis objects. */
dictType dbDictType = {
        dictSdsHash,                /*当前字典键散列函数 hash function */
        NULL,                       /* key dup */
        NULL,                       /* val dup */
        dictSdsKeyCompare,          /* 当前字典键的比较函数 key compare */
        dictSdsDestructor,          /* 当前字典键析构函数 key destructor */
        dictObjectDestructor   /* 当前字典对象析构函数 val destructor */
};

/* server.lua_scripts sha (as sds string) -> scripts (as robj) cache. */
dictType shaScriptObjectDictType = {
        dictSdsCaseHash,            /* hash function */
        NULL,                       /* key dup */
        NULL,                       /* val dup */
        dictSdsKeyCaseCompare,      /* key compare */
        dictSdsDestructor,          /* key destructor */
        dictObjectDestructor        /* val destructor */
};

/* Db->expires */
dictType keyptrDictType = {
        dictSdsHash,                /* hash function */
        NULL,                       /* key dup */
        NULL,                       /* val dup */
        dictSdsKeyCompare,          /* key compare */
        NULL,                       /* key destructor */
        NULL                        /* val destructor */
};

/* Command table. sds string -> command struct pointer. */
dictType commandTableDictType = {
        dictSdsCaseHash,            /* hash function */
        NULL,                       /* key dup */
        NULL,                       /* val dup */
        dictSdsKeyCaseCompare,      /* key compare */
        dictSdsDestructor,          /* key destructor */
        NULL                        /* val destructor */
};

/* Hash type hash table (note that small hashes are represented with ziplists) */
dictType hashDictType = {
        dictSdsHash,                /* hash function */
        NULL,                       /* key dup */
        NULL,                       /* val dup */
        dictSdsKeyCompare,          /* key compare */
        dictSdsDestructor,          /* key destructor */
        dictSdsDestructor           /* val destructor */
};

/* Keylist hash table type has unencoded redis objects as keys and
 * lists as values. It's used for blocking operations (BLPOP) and to
 * map swapped keys to a list of clients waiting for this keys to be loaded. */
dictType keylistDictType = {
        dictObjHash,                /* hash function */
        NULL,                       /* key dup */
        NULL,                       /* val dup */
        dictObjKeyCompare,          /* key compare */
        dictObjectDestructor,       /* key destructor */
        dictListDestructor          /* val destructor */
};

/* Cluster nodes hash table, mapping nodes addresses 1.2.3.4:6379 to
 * clusterNode structures. */
dictType clusterNodesDictType = {
        dictSdsHash,                /* hash function */
        NULL,                       /* key dup */
        NULL,                       /* val dup */
        dictSdsKeyCompare,          /* key compare */
        dictSdsDestructor,          /* key destructor */
        NULL                        /* val destructor */
};

/* Cluster re-addition blacklist. This maps node IDs to the time
 * we can re-add this node. The goal is to avoid readding a removed
 * node for some time. */
dictType clusterNodesBlackListDictType = {
        dictSdsCaseHash,            /* hash function */
        NULL,                       /* key dup */
        NULL,                       /* val dup */
        dictSdsKeyCaseCompare,      /* key compare */
        dictSdsDestructor,          /* key destructor */
        NULL                        /* val destructor */
};

/* Cluster re-addition blacklist. This maps node IDs to the time
 * we can re-add this node. The goal is to avoid readding a removed
 * node for some time. */
dictType modulesDictType = {
        dictSdsCaseHash,            /* hash function */
        NULL,                       /* key dup */
        NULL,                       /* val dup */
        dictSdsKeyCaseCompare,      /* key compare */
        dictSdsDestructor,          /* key destructor */
        NULL                        /* val destructor */
};

/* Migrate cache dict type. */
dictType migrateCacheDictType = {
        dictSdsHash,                /* hash function */
        NULL,                       /* key dup */
        NULL,                       /* val dup */
        dictSdsKeyCompare,          /* key compare */
        dictSdsDestructor,          /* key destructor */
        NULL                        /* val destructor */
};

/* Replication cached script dict (server.repl_scriptcache_dict).
 * Keys are sds SHA1 strings, while values are not used at all in the current
 * implementation. */
dictType replScriptCacheDictType = {
        dictSdsCaseHash,            /* hash function */
        NULL,                       /* key dup */
        NULL,                       /* val dup */
        dictSdsKeyCaseCompare,      /* key compare */
        dictSdsDestructor,          /* key destructor */
        NULL                        /* val destructor */
};

int htNeedsResize(dict *dict) {
    long long size, used;

    size = dictSlots(dict);
    used = dictSize(dict);
    return (size > DICT_HT_INITIAL_SIZE &&
            (used * 100 / size < HASHTABLE_MIN_FILL));
}

/* If the percentage of used slots in the HT reaches HASHTABLE_MIN_FILL
 * we resize the hash table to save memory */
void tryResizeHashTables(int dbid) {
    if (htNeedsResize(server.db[dbid].dict))
        dictResize(server.db[dbid].dict);
    if (htNeedsResize(server.db[dbid].expires))
        dictResize(server.db[dbid].expires);
}

/* Our hash table implementation performs rehashing incrementally while
 * we write/read from the hash table. Still if the server is idle, the hash
 * table will use two tables for a long time. So we try to use 1 millisecond
 * of CPU time at every call of this function to perform some rehahsing.
 *
 * The function returns 1 if some rehashing was performed, otherwise 0
 * is returned. */
int incrementallyRehash(int dbid) {
    // 函数分别检查检查key空间和过期key空间.遍历每个数据库，进行rehash操作，每次调用只rehash一个。
    /* Keys dictionary */
    if (dictIsRehashing(server.db[dbid].dict)) {
        dictRehashMilliseconds(server.db[dbid].dict, 1);
        return 1; /* already used our millisecond for this loop... */
    }
    /* Expires */
    if (dictIsRehashing(server.db[dbid].expires)) {
        dictRehashMilliseconds(server.db[dbid].expires, 1);
        return 1; /* already used our millisecond for this loop... */
    }
    return 0;
}

/* This function is called once a background process of some kind terminates,
 * as we want to avoid resizing the hash tables when there is a child in order
 * to play well with copy-on-write (otherwise when a resize happens lots of
 * memory pages are copied). The goal of this function is to update the ability
 * for dict.c to resize the hash tables accordingly to the fact we have o not
 * running childs. */
void updateDictResizePolicy(void) {
    if (!hasActiveChildProcess())
        dictEnableResize();
    else
        dictDisableResize();
}

/* Return true if there are no active children processes doing RDB saving,
 * AOF rewriting, or some side process spawned by a loaded module. */
int hasActiveChildProcess() {
    return server.rdb_child_pid != -1 ||
           server.aof_child_pid != -1 ||
           server.module_child_pid != -1;
}

/* Return true if this instance has persistence completely turned off:
 * both RDB and AOF are disabled. */
int allPersistenceDisabled(void) {
    return server.saveparamslen == 0 && server.aof_state == AOF_OFF;
}

/* ======================= Cron: called every 100 ms ======================== */

/* Add a sample to the operations per second array of samples. */
void trackInstantaneousMetric(int metric, long long current_reading) {
    long long t = mstime() - server.inst_metric[metric].last_sample_time;
    long long ops = current_reading -
                    server.inst_metric[metric].last_sample_count;
    long long ops_sec;

    ops_sec = t > 0 ? (ops * 1000 / t) : 0;

    server.inst_metric[metric].samples[server.inst_metric[metric].idx] =
            ops_sec;
    server.inst_metric[metric].idx++;
    server.inst_metric[metric].idx %= STATS_METRIC_SAMPLES;
    server.inst_metric[metric].last_sample_time = mstime();
    server.inst_metric[metric].last_sample_count = current_reading;
}

/* Return the mean of all the samples. */
long long getInstantaneousMetric(int metric) {
    int j;
    long long sum = 0;

    for (j = 0; j < STATS_METRIC_SAMPLES; j++)
        sum += server.inst_metric[metric].samples[j];
    return sum / STATS_METRIC_SAMPLES;
}

/* The client query buffer is an sds.c string that can end with a lot of
 * free space not used, this function reclaims space if needed.
 *
 * The function always returns 0 as it never terminates the client. */
int clientsCronResizeQueryBuffer(client *c) {
    size_t querybuf_size = sdsAllocSize(c->querybuf);
    time_t idletime = server.unixtime - c->lastinteraction;

    /* There are two conditions to resize the query buffer:
     * 1) Query buffer is > BIG_ARG and too big for latest peak.
     * 2) Query buffer is > BIG_ARG and client is idle. */
    if (querybuf_size > PROTO_MBULK_BIG_ARG &&
        ((querybuf_size / (c->querybuf_peak + 1)) > 2 ||
         idletime > 2)) {
        /* Only resize the query buffer if it is actually wasting
         * at least a few kbytes. */
        if (sdsavail(c->querybuf) > 1024 * 4) {
            c->querybuf = sdsRemoveFreeSpace(c->querybuf);
        }
    }
    /* Reset the peak again to capture the peak memory usage in the next
     * cycle. */
    c->querybuf_peak = 0;

    /* Clients representing masters also use a "pending query buffer" that
     * is the yet not applied part of the stream we are reading. Such buffer
     * also needs resizing from time to time, otherwise after a very large
     * transfer (a huge value or a big MIGRATE operation) it will keep using
     * a lot of memory. */
    if (c->flags & CLIENT_MASTER) {
        /* There are two conditions to resize the pending query buffer:
         * 1) Pending Query buffer is > LIMIT_PENDING_QUERYBUF.
         * 2) Used length is smaller than pending_querybuf_size/2 */
        size_t pending_querybuf_size = sdsAllocSize(c->pending_querybuf);
        if (pending_querybuf_size > LIMIT_PENDING_QUERYBUF &&
            sdslen(c->pending_querybuf) < (pending_querybuf_size / 2)) {
            c->pending_querybuf = sdsRemoveFreeSpace(c->pending_querybuf);
        }
    }
    return 0;
}

/* This function is used in order to track clients using the biggest amount
 * of memory in the latest few seconds. This way we can provide such information
 * in the INFO output (clients section), without having to do an O(N) scan for
 * all the clients.
 *
 * This is how it works. We have an array of CLIENTS_PEAK_MEM_USAGE_SLOTS slots
 * where we track, for each, the biggest client output and input buffers we
 * saw in that slot. Every slot correspond to one of the latest seconds, since
 * the array is indexed by doing UNIXTIME % CLIENTS_PEAK_MEM_USAGE_SLOTS.
 *
 * When we want to know what was recently the peak memory usage, we just scan
 * such few slots searching for the maximum value. */
#define CLIENTS_PEAK_MEM_USAGE_SLOTS 8
size_t ClientsPeakMemInput[CLIENTS_PEAK_MEM_USAGE_SLOTS];
size_t ClientsPeakMemOutput[CLIENTS_PEAK_MEM_USAGE_SLOTS];

int clientsCronTrackExpansiveClients(client *c) {
    size_t in_usage = sdsAllocSize(c->querybuf);
    size_t out_usage = getClientOutputBufferMemoryUsage(c);
    int i = server.unixtime % CLIENTS_PEAK_MEM_USAGE_SLOTS;
    int zeroidx = (i + 1) % CLIENTS_PEAK_MEM_USAGE_SLOTS;

    /* Always zero the next sample, so that when we switch to that second, we'll
     * only register samples that are greater in that second without considering
     * the history of such slot.
     *
     * Note: our index may jump to any random position if serverCron() is not
     * called for some reason with the normal frequency, for instance because
     * some slow command is called taking multiple seconds to execute. In that
     * case our array may end containing data which is potentially older
     * than CLIENTS_PEAK_MEM_USAGE_SLOTS seconds: however this is not a problem
     * since here we want just to track if "recently" there were very expansive
     * clients from the POV of memory usage. */
    ClientsPeakMemInput[zeroidx] = 0;
    ClientsPeakMemOutput[zeroidx] = 0;

    /* Track the biggest values observed so far in this slot. */
    if (in_usage > ClientsPeakMemInput[i]) ClientsPeakMemInput[i] = in_usage;
    if (out_usage > ClientsPeakMemOutput[i]) ClientsPeakMemOutput[i] = out_usage;

    return 0; /* This function never terminates the client. */
}

/* Iterating all the clients in getMemoryOverheadData() is too slow and
 * in turn would make the INFO command too slow. So we perform this
 * computation incrementally and track the (not instantaneous but updated
 * to the second) total memory used by clients using clinetsCron() in
 * a more incremental way (depending on server.hz). */
int clientsCronTrackClientsMemUsage(client *c) {
    size_t mem = 0;
    int type = getClientType(c);
    mem += getClientOutputBufferMemoryUsage(c);
    mem += sdsAllocSize(c->querybuf);
    mem += sizeof(client);
    /* Now that we have the memory used by the client, remove the old
     * value from the old categoty, and add it back. */
    server.stat_clients_type_memory[c->client_cron_last_memory_type] -=
            c->client_cron_last_memory_usage;
    server.stat_clients_type_memory[type] += mem;
    /* Remember what we added and where, to remove it next time. */
    c->client_cron_last_memory_usage = mem;
    c->client_cron_last_memory_type = type;
    return 0;
}

/* Return the max samples in the memory usage of clients tracked by
 * the function clientsCronTrackExpansiveClients(). */
void getExpansiveClientsInfo(size_t *in_usage, size_t *out_usage) {
    size_t i = 0, o = 0;
    for (int j = 0; j < CLIENTS_PEAK_MEM_USAGE_SLOTS; j++) {
        if (ClientsPeakMemInput[j] > i) i = ClientsPeakMemInput[j];
        if (ClientsPeakMemOutput[j] > o) o = ClientsPeakMemOutput[j];
    }
    *in_usage = i;
    *out_usage = o;
}

/* This function is called by serverCron() and is used in order to perform
 * operations on clients that are important to perform constantly. For instance
 * we use this function in order to disconnect clients after a timeout, including
 * clients blocked in some blocking command with a non-zero timeout.
 *
 * The function makes some effort to process all the clients every second, even
 * if this cannot be strictly guaranteed, since serverCron() may be called with
 * an actual frequency lower than server.hz in case of latency events like slow
 * commands.
 *
 * It is very important for this function, and the functions it calls, to be
 * very fast: sometimes Redis has tens of hundreds of connected clients, and the
 * default server.hz value is 10, so sometimes here we need to process thousands
 * of clients per second, turning this function into a source of latency.
 */
#define CLIENTS_CRON_MIN_ITERATIONS 5

void clientsCron(void) {
    /* Try to process at least numclients/server.hz of clients
     * per call. Since normally (if there are no big latency events) this
     * function is called server.hz times per second, in the average case we
     * process all the clients in 1 second. */
    int numclients = listLength(server.clients);
    int iterations = numclients / server.hz;
    mstime_t now = mstime();

    /* Process at least a few clients while we are at it, even if we need
     * to process less than CLIENTS_CRON_MIN_ITERATIONS to meet our contract
     * of processing each client once per second. */
    if (iterations < CLIENTS_CRON_MIN_ITERATIONS)
        iterations = (numclients < CLIENTS_CRON_MIN_ITERATIONS) ?
                     numclients : CLIENTS_CRON_MIN_ITERATIONS;

    while (listLength(server.clients) && iterations--) {
        client *c;
        listNode *head;

        /* Rotate the list, take the current head, process.
         * This way if the client must be removed from the list it's the
         * first element and we don't incur into O(N) computation. */
        // 将当前处理的客户端调到表头，这样在要删除客户端时，复杂度就是O(1)而不是O(N)
        listRotateTailToHead(server.clients);
        head = listFirst(server.clients);
        c = listNodeValue(head);
        /* The following functions do different service checks on the client.
         * The protocol is that they return non-zero if the client was
         * terminated. */
        // 检查客户端是否超时，如果是的话删除它的连接如果客户端正因 BLPOP/BRPOP/BLPOPRPUSH 阻塞，那么检查阻塞是否超时，是的话就退出阻塞状态
        if (clientsCronHandleTimeout(c, now)) continue;
        // 释放客户端查询缓存多余的空间
        if (clientsCronResizeQueryBuffer(c)) continue;
        if (clientsCronTrackExpansiveClients(c)) continue;
        if (clientsCronTrackClientsMemUsage(c)) continue;
    }
}

/* This function handles 'background' operations we are required to do
 * incrementally in Redis databases, such as active key expiring, resizing,
 * rehashing.(这个函数处理Redis数据库中递增的“后台”操作，如活动键过期，调整大小，重散列。)
 * databasesCron()函数主要用于在后台做一些需要逐渐实现的操作，比如key的过期，哈希重置等操作。
 * */
void databasesCron(void) {
    /* Expire keys by random sampling. Not required for slaves
     * as master will synthesize DELs for us. */
    if (server.active_expire_enabled) {
        if (iAmMaster()) {
            /**
             * activeExpireCycle()这个函数主要是用于过期值的处理,这个函数只有在active_expire_enabled这个配置为true，而且本库是master的的情况下执行，
             * slave会等候由master传递的DEL消息，保证master-slave在过期值处理上的一致。redis是随机抽取过期值的，所以master和slave可能抽取不同的值，
             * 故通过DEL消息实现同步，同时这种expire机制也是不可靠的expire，即key超时后有可能不会被删除。
             */
            activeExpireCycle(ACTIVE_EXPIRE_CYCLE_SLOW);
        } else {
            expireSlaveKeys();
        }
    }

    /* Defrag keys gradually. */
    activeDefragCycle();

    /* Perform hash tables rehashing if needed, but only if there are no
     * other processes saving the DB on disk. Otherwise rehashing is bad
     * as will cause a lot of copy-on-write of memory pages.
     * （在需要时执行哈希表重散列，但仅当没有其他进程将数据库保存在磁盘上时才执行。否则，重散列是不好的，因为它会导致大量内存页的写时复制。）
     * */
    // 如果没有其进程进行DB存储的时候，才能进行hash重置，因为这会引起很多的写实复制内存页。
    if (!hasActiveChildProcess()) {
        /* We use global counters so if we stop the computation at a given
         * DB we'll be able to start from the successive in the next
         * cron loop iteration. */
        static unsigned int resize_db = 0;
        static unsigned int rehash_db = 0;
        int dbs_per_call = CRON_DBS_PER_CALL;
        int j;

        /* Don't test more DBs than we have.
         * 不能超过数据库数目，默认为16。
         * */
        if (dbs_per_call > server.dbnum) dbs_per_call = server.dbnum;

        /* Resize */
        for (j = 0; j < dbs_per_call; j++) {
            tryResizeHashTables(resize_db % server.dbnum);
            resize_db++;
        }

        /* Rehash */
        if (server.activerehashing) {
            // 遍历每个数据库都进行Resize操作
            for (j = 0; j < dbs_per_call; j++) {
                // 函数分别检查检查key空间和过期key空间
                int work_done = incrementallyRehash(rehash_db);
                if (work_done) {
                    /* If the function did some work, stop here, we'll do
                     * more at the next cron loop. */
                    break;
                } else {
                    /* If this db didn't need rehash, we'll try the next one. */
                    rehash_db++;
                    rehash_db %= server.dbnum;
                }
            }
        }
    }
}

/* We take a cached value of the unix time in the global state because with
 * virtual memory and aging there is to store the current time in objects at
 * every object access, and accuracy is not needed. To access a global var is
 * a lot faster than calling time(NULL).
 * (我们在全局状态中获取unix时间的缓存值，因为使用虚拟内存和老化，在每次对象访问时都要将当前时间存储在对象中，因此不需要准确性。
 * 访问一个全局变量比调用time(NULL)要快得多.)
 * This function should be fast because it is called at every command execution
 * in call(), so it is possible to decide if to update the daylight saving
 * info or not using the 'update_daylight_info' argument. Normally we update
 * such info only when calling this function from serverCron() but not when
 * calling it from call().
 * (这个函数应该很快，因为它在call()中每次执行命令时都会被调用，所以可以决定是否更新日光节约信息或不使用'update_daylight_info'参数。通常我们只在从serverCron()
 * 调用这个函数时更新这些信息，而不是在从call()调用它时更新。)
 * */
void updateCachedTime(int update_daylight_info) {
    // 将UNIX 时间保存在服务器状态中，减少对time(NULL) 的调用，获取一个全局变量快于一个调用time(NULL)。
    server.ustime = ustime();
    // 记录当前时间的毫秒数
    server.mstime = server.ustime / 1000;
    server.unixtime = server.mstime / 1000;

    /* To get information about daylight saving time, we need to call
     * localtime_r and cache the result. However calling localtime_r in this
     * context is safe since we will never fork() while here, in the main
     * thread. The logging function will call a thread safe version of
     * localtime that has no locks.
     * (为了获得关于夏令时的信息，我们需要调用localtime_r并缓存结果。但是，在这个上下文中调用localtime_r是安全的，因为在主线程中我们永远不会使用fork()。日志函数将调用没有锁的localtime的线程安全版本。)
     * */
    if (update_daylight_info) {
        struct tm tm;
        time_t ut = server.unixtime;
        localtime_r(&ut, &tm);
        server.daylight_active = tm.tm_isdst;
    }
}

void checkChildrenDone(void) {
    /* 如果有aof或者rdb在后台进行，则等待对应的退出。注意，这里用了WNOHANG，所以不会阻塞在wait3 */
    int statloc;
    pid_t pid;

    /* If we have a diskless rdb child (note that we support only one concurrent
     * child), we want to avoid collecting it's exit status and acting on it
     * as long as we didn't finish to drain the pipe, since then we're at risk
     * of starting a new fork and a new pipe before we're done with the previous
     * one. (如果我们有一个无盘rdb的节点(注意,我们只支持一个并发的节点),我们希望避免收集的退出状态和作用于它,只要我们没有完成排水管道,从那时起,我们开始一个新的叉的风险和新的管之前我们完成了前一个。)
     **/
    if (server.rdb_child_pid != -1 && server.rdb_pipe_conns)
        return;

    /* wait3返回非0值，要么是子进程退出，要么是出错 */
    if ((pid = wait3(&statloc, WNOHANG, NULL)) != 0) {
        int exitcode = WEXITSTATUS(statloc);
        int bysignal = 0;

        if (WIFSIGNALED(statloc)) bysignal = WTERMSIG(statloc);

        /* sigKillChildHandler catches the signal and calls exit(), but we  (sigKillChildHandler捕获信号并调用exit()，但我们)
         * must make sure not to flag lastbgsave_status, etc incorrectly.  (必须确保lastbgsave_status等没有被错误标记。)
         * We could directly terminate the child process via SIGUSR1  (我们可以通过SIGUSR1直接终止子进程)
         * without handling it, but in this case Valgrind will log an  (但在本例中，Valgrind将记录一个恼人的错误。)
         * annoying error. */
        if (exitcode == SERVER_CHILD_NOERROR_RETVAL) {
            bysignal = SIGUSR1;
            exitcode = 1;
        }
        /* 如果是出错，在log中记录这次错误
         * 如果是rdb任务退出，调用backgroundSaveDoneHandler进行收尾工作
         * 如果是aof任务退出，调用backgroundRewriteDoneHandler进行收尾工作
         */
        if (pid == -1) {
            serverLog(LL_WARNING, "wait3() returned an error: %s. "
                                  "rdb_child_pid = %d, aof_child_pid = %d, module_child_pid = %d",
                      strerror(errno),
                      (int) server.rdb_child_pid,
                      (int) server.aof_child_pid,
                      (int) server.module_child_pid);
        } else if (pid == server.rdb_child_pid) {
            backgroundSaveDoneHandler(exitcode, bysignal);
            if (!bysignal && exitcode == 0) receiveChildInfo();
        } else if (pid == server.aof_child_pid) {
            backgroundRewriteDoneHandler(exitcode, bysignal);
            if (!bysignal && exitcode == 0) receiveChildInfo();
        } else if (pid == server.module_child_pid) {
            ModuleForkDoneHandler(exitcode, bysignal);
            if (!bysignal && exitcode == 0) receiveChildInfo();
        } else {
            if (!ldbRemoveChild(pid)) {
                serverLog(LL_WARNING,
                          "Warning, detected child with unmatched pid: %ld",
                          (long) pid);
            }
        }
        /* 如果当前有rdb/aof任务在处理，则将dict_can_resize设置为0(表示不允许进行resize)，否则，设置为1 */
        updateDictResizePolicy();
        closeChildInfoPipe();
    }
}

/* This is our timer interrupt, called server.hz times per second.
 * Here is where we do a number of things that need to be done asynchronously. (这是我们的定时器中断，称为服务器。赫兹每秒。在这里，我们需要异步地完成许多事情)
 * For instance:
 *
 * - Active expired keys collection (it is also performed in a lazy way on
 *   lookup). (活动的过期键收集(它也以惰性方式执行查找)。)
 * - Software watchdog.
 * - Update some statistic.
 * - Incremental rehashing of the DBs hash tables. (DBs哈希表的增量重哈希.)
 * - Triggering BGSAVE / AOF rewrite, and handling of terminated children. (触发BGSAVE / AOF重写，并处理终止的子节点。)
 * - Clients timeout of different kinds. (不同类型的客户超时。)
 * - Replication reconnection. (复制重新连接。)
 * - Many more...
 *
 * Everything directly called here will be called server.hz times per second,
 * so in order to throttle execution of things we want to do less frequently
 * a macro is used: run_with_period(milliseconds) { .... }
 */
/**
 * fixme serverCron是一个主循环事件的回调处理函数，redis每个循环都会执行该函数。实现了Redis服务所有的定时任务，进行周期指向
 * @param eventLoop
 * @param id
 * @param clientData
 * @return
 */
int serverCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    int j;
    // 目的是为了去掉编译器对未使用的局部变量的警告。
    UNUSED(eventLoop);
    UNUSED(id);
    UNUSED(clientData);

    /* Software watchdog: deliver the SIGALRM that will reach the signal
     * handler if we don't return here fast enough. (如果我们不能足够快的返回这里，发送信号处理器。)
     * */
    /* 用SIGALRM信号触发watchdog的处理过程，具体的函数为watchdogSignalHandler */
    if (server.watchdog_period) watchdogScheduleSignal(server.watchdog_period);

    /* Update the time cache. */
    /* 更新server.unixtime和server.mstime */
    updateCachedTime(1);

    server.hz = server.config_hz;
    /* Adapt the server.hz value to the number of configured clients. If we have
     * many clients, we want to call serverCron() with an higher frequency. (调整服务器。配置的客户端数量的hz值。如果我们有很多客户机，我们希望以更高的频率调用serverCron()。)
     * */
    /* 每100ms更新一次统计量，包括这段时间内的commands, net_input_bytes, net_output_bytes */
    if (server.dynamic_hz) {
        while (listLength(server.clients) / server.hz >
               MAX_CLIENTS_PER_CLOCK_TICK) {
            server.hz *= 2;
            if (server.hz > CONFIG_MAX_HZ) {
                server.hz = CONFIG_MAX_HZ;
                break;
            }
        }
    }
    //100毫秒周期执行
    run_with_period(100) {
        trackInstantaneousMetric(STATS_METRIC_COMMAND, server.stat_numcommands);
        trackInstantaneousMetric(STATS_METRIC_NET_INPUT,
                                 server.stat_net_input_bytes);
        trackInstantaneousMetric(STATS_METRIC_NET_OUTPUT,
                                 server.stat_net_output_bytes);
    }

    /* We have just LRU_BITS bits per object for LRU information.
     * So we use an (eventually wrapping) LRU clock.
     *
     * Note that even if the counter wraps it's not a big problem,
     * everything will still work but some object will appear younger
     * to Redis. However for this to happen a given object should never be
     * touched for all the time needed to the counter to wrap, which is
     * not likely.
     *
     * Note that you can change the resolution altering the
     * LRU_CLOCK_RESOLUTION define. */
    /* 根据server.lruclock的定义，getLRUClock返回的是当前时间换算成秒数的低23位
     * 后续在执行lru淘汰策略时，作为比较的基准值。redis默认的时间精度是10s(#defineREDIS_LRU_CLOCK_RESOLUTION 10)，保存lruclock的变量共有22bit。换算成总的时间为1.5year
     * （每隔1.5年循环一次）
     * */
    server.lruclock = getLRUClock();

    /* Record the max memory used since the server was started. (记录自服务器启动以来使用的最大内存.)*/
    if (zmalloc_used_memory() > server.stat_peak_memory)
        server.stat_peak_memory = zmalloc_used_memory();

    run_with_period(100) {
        /* Sample the RSS and other metrics here since this is a relatively slow call. (这里示例RSS和其他度量，因为这是一个相对缓慢的调用。)
         * We must sample the zmalloc_used at the same time we take the rss, otherwise  (我们必须在获取rss的同时对zmalloc_used进行采样，否则)
         * the frag ratio calculate may be off (ratio of two samples at different times)  (计算的压裂比可能有偏差(两个样品在不同时间的比率))
         **/
        server.cron_malloc_stats.process_rss = zmalloc_get_rss();
        server.cron_malloc_stats.zmalloc_used = zmalloc_used_memory();
        /* Sampling the allcator info can be slow too.
         * The fragmentation ratio it'll show is potentically more accurate
         * it excludes other RSS pages such as: shared libraries, LUA and other non-zmalloc
         * allocations, and allocator reserved pages that can be pursed (all not actual frag) */
        zmalloc_get_allocator_info(&server.cron_malloc_stats.allocator_allocated,
                                   &server.cron_malloc_stats.allocator_active,
                                   &server.cron_malloc_stats.allocator_resident);
        /* in case the allocator isn't providing these stats, fake them so that
         * fragmention info still shows some (inaccurate metrics) */
        if (!server.cron_malloc_stats.allocator_resident) {
            /* LUA memory isn't part of zmalloc_used, but it is part of the process RSS,
             * so we must desuct it in order to be able to calculate correct
             * "allocator fragmentation" ratio */
            size_t lua_memory = lua_gc(server.lua, LUA_GCCOUNT, 0) * 1024LL;
            server.cron_malloc_stats.allocator_resident = server.cron_malloc_stats.process_rss - lua_memory;
        }
        if (!server.cron_malloc_stats.allocator_active)
            server.cron_malloc_stats.allocator_active = server.cron_malloc_stats.allocator_resident;
        if (!server.cron_malloc_stats.allocator_allocated)
            server.cron_malloc_stats.allocator_allocated = server.cron_malloc_stats.zmalloc_used;
    }

    /* We received a SIGTERM, shutting down here in a safe way, as it is
     * not ok doing so inside the signal handler. (我们接收到一个SIGTERM，在这里以一种安全的方式关闭，因为在信号处理程序中这样做是不行的。即：如果收到了SIGTERM信号，尝试退出)  */
    if (server.shutdown_asap) {
        if (prepareForShutdown(SHUTDOWN_NOFLAGS) == C_OK) exit(0);
        serverLog(LL_WARNING,
                  "SIGTERM received but errors trying to shut down the server, check the logs for more information");
        server.shutdown_asap = 0;
    }

    /* Show some info about non-empty databases */
    /* 每5秒输出一次非空databases的信息（使用的key数目、设置过期的key数目、以及当前的hashtabale的槽位数。）到log当中 */
    run_with_period(5000) {
        for (j = 0; j < server.dbnum; j++) {
            long long size, used, vkeys;

            size = dictSlots(server.db[j].dict);
            used = dictSize(server.db[j].dict);
            vkeys = dictSize(server.db[j].expires);
            if (used || vkeys) {
                serverLog(LL_VERBOSE, "DB %d: %lld keys (%lld volatile) in %lld slots HT.", j, used, vkeys, size);
                /* dictPrintStats(server.dict); */
            }
        }
    }

    /* Show information about connected clients */
    /* 如果不是sentinel模式，则每5秒输出一个connected的client的信息到log */
    if (!server.sentinel_mode) {
        run_with_period(5000) {
            serverLog(LL_DEBUG,
                      "%lu clients connected (%lu replicas), %zu bytes in use",
                      listLength(server.clients) - listLength(server.slaves),
                      listLength(server.slaves),
                      zmalloc_used_memory());
        }
    }

    /* We need to do a few operations on clients asynchronously. */
    /* 清理空闲的客户端或者释放query buffer中未被使用的空间
     * 次调用clientCron例程，这是一个对server.clients列表进行处理的过程。再每次执行clientCron时，会对 server.clients进行迭代，
     * 并且保证 1/(REDIS_HZ*10)ofclients per call。也就是每次执行clientCron，如果clients过多，clientCron不会遍历所有clients，而是遍历一部分 clients，
     * 但是保证每个clients都会在一定时间内得到处理。处理过程主要是检测client连接是否idle超时，或者block超时，然后 会调解每个client的缓冲区大小。
     * */
    clientsCron();

    /* Handle background operations on Redis databases. */
    /* databases的处理，rehash就在这里 */
    databasesCron();

    /* Start a scheduled AOF rewrite if this was requested by the user while
     * a BGSAVE was in progress. (如果用户在此期间，执行BGREWRITEAOF 命令的话，在后台执行AOF重写。。) */
    /* 如果开启了aof_rewrite的调度并且当前没有在background执行rdb/aof的操作，则进行background的aof操作 */
    if (!hasActiveChildProcess() &&
        server.aof_rewrite_scheduled) {
        rewriteAppendOnlyFileBackground();
    }

    /* Check if a background saving or AOF rewrite in progress terminated. (检查后台保存或AOF重写是否已终止。) */
    if (hasActiveChildProcess() || ldbPendingChildren()) {
        checkChildrenDone();
    } else {
        /* If there is not a background saving/rewrite in progress check if
         * we have to save/rewrite now. */
        /* 当前没有rdb/aof任务在执行，这里来判断是否要开启新的rdb/aof任务 */
        for (j = 0; j < server.saveparamslen; j++) {
            struct saveparam *sp = server.saveparams + j;

            /* Save if we reached the given amount of changes, (如果我们达到了给定的变化量，)
             * the given amount of seconds, and if the latest bgsave was (给定的秒数，以及最新的bgsave是否为)
             * successful or if, in case of an error, at least  (成功或如果，万一出现错误，至少)
             * CONFIG_BGSAVE_RETRY_DELAY seconds already elapsed. (CONFIG_BGSAVE_RETRY_DELAY秒数已经过去。) */
            if (server.dirty >= sp->changes &&
                server.unixtime - server.lastsave > sp->seconds &&
                (server.unixtime - server.lastbgsave_try >
                 CONFIG_BGSAVE_RETRY_DELAY ||
                 server.lastbgsave_status == C_OK)) {
                serverLog(LL_NOTICE, "%d changes in %d seconds. Saving...",
                          sp->changes, (int) sp->seconds);
                rdbSaveInfo rsi, *rsiptr;
                rsiptr = rdbPopulateSaveInfo(&rsi);
                rdbSaveBackground(server.rdb_filename, rsiptr);
                break;
            }
        }

        /* Trigger an AOF rewrite if needed. 如果需要则触发一个aof 写磁盘 */
        if (server.aof_state == AOF_ON &&
            !hasActiveChildProcess() &&
            server.aof_rewrite_perc &&
            server.aof_current_size > server.aof_rewrite_min_size) {
            long long base = server.aof_rewrite_base_size ?
                             server.aof_rewrite_base_size : 1;
            long long growth = (server.aof_current_size * 100 / base) - 100;
            if (growth >= server.aof_rewrite_perc) {
                serverLog(LL_NOTICE, "Starting automatic rewriting of AOF on %lld%% growth", growth);
                rewriteAppendOnlyFileBackground();
            }
        }
    }


    /* AOF postponed flush: Try at every cron cycle if the slow fsync
     * completed. */
    /* 如果开启了aof_flush_postponed_start，则在每次serverCron流程里都将server.aof_buf写入磁盘文件。
     * PS, server.aof_buf是从上一次写aof文件到目前为止所执行过的命令集合，所以是append only file
     */
    if (server.aof_flush_postponed_start) flushAppendOnlyFile(0);

    /* AOF write errors: in this case we have a buffer to flush as well and
     * clear the AOF error in case of success to make the DB writable again,
     * however to try every second is enough in case of 'hz' is set to
     * an higher frequency. (AOF写错误:在这种情况下，我们有一个缓冲区，以刷新和清除AOF错误，在成功的情况下，使DB写入再次，然而，尝试每秒钟是足够的情况下，hz被设置为更高的频率)*/
    /* 每一秒检查一次上一轮aof的写入是否发生了错误，如果有错误则尝试重新写一次 */
    run_with_period(1000) {
        if (server.aof_last_write_status == C_ERR)
            flushAppendOnlyFile(0);
    }

    /* Clear the paused clients flag if needed. */
    /* clients被paused时，会相应地记录一个超时的时间，如果那个时间已经到来，则给client打上REDIS_UNBLOCKED标记(slave的client不处理)，并加到server.unblocked_clients上 */
    clientsArePaused(); /* Don't check return value, just use the side effect.*/

    /* Replication cron function -- used to reconnect to master,
     * detect transfer failures, start background RDB transfers and so forth. (Replication cron function -- used to reconnect to master,detect transfer failures, start background RDB transfers and so forth.) */
    /* 每秒执行一次replicationCron(),这个函数用来master重连和传输失败检查*/
    run_with_period(1000) replicationCron();

    /* Run the Redis Cluster cron. */
    /* 每100ms执行一次clusterCron */
    run_with_period(100) {
        if (server.cluster_enabled) clusterCron();
    }

    /* Run the Sentinel timer if we are in sentinel mode. */
    /* 如果再监视模式下，则每100ms运行一次监视Time */
    if (server.sentinel_mode) sentinelTimer();

    /* Cleanup expired MIGRATE cached sockets. */
    /* 每1秒清理一次server.migrate_cached_sockets链表上的超时sockets */
    run_with_period(1000) {
        migrateCloseTimedoutSockets();
    }

    /* Stop the I/O threads if we don't have enough pending work. */
    /*没有任务(读写)时就停止线程*/
    stopThreadedIOIfNeeded();

    /* Resize tracking keys table if needed. This is also done at every
     * command execution, but we want to be sure that if the last command
     * executed changes the value via CONFIG SET, the server will perform
     * the operation even if completely idle.
     * (如果需要，调整跟踪键表的大小。这也是在每个命令执行时执行的，但是我们想要确保，如果最后执行的命令通过配置设置更改了值，服务器将执行操作，即使完全空闲.)
     * */
    if (server.tracking_clients) trackingLimitUsedSlots();

    /* Start a scheduled BGSAVE if the corresponding flag is set. This is  (如果设置了相应的标志，则启动一个预定的BGSAVE)
     * useful when we are forced to postpone a BGSAVE because an AOF  (当我们因为故障而不得不推迟一项任务时，它很有用)
     * rewrite is in progress.  (重写正在进行中。)
     *
     * Note: this code must be after the replicationCron() call above so  (注意:此代码必须在上面的replicationCron()调用之后执行)
     * make sure when refactoring this file to keep this order. This is useful (确保在重构这个文件时保持这个顺序。这是非常有用的)
     * because we want to give priority to RDB savings for replication.  (因为我们希望优先考虑用于复制的RDB节省。) */
    if (!hasActiveChildProcess() &&
        server.rdb_bgsave_scheduled &&
        (server.unixtime - server.lastbgsave_try > CONFIG_BGSAVE_RETRY_DELAY ||
         server.lastbgsave_status == C_OK)) {
        rdbSaveInfo rsi, *rsiptr;
        rsiptr = rdbPopulateSaveInfo(&rsi);
        if (rdbSaveBackground(server.rdb_filename, rsiptr) == C_OK)
            server.rdb_bgsave_scheduled = 0;
    }

    /* Fire the cron loop modules event. */
    RedisModuleCronLoopV1 ei = {REDISMODULE_CRON_LOOP_VERSION, server.hz};
    moduleFireServerEvent(REDISMODULE_EVENT_CRON_LOOP,
                          0,
                          &ei);
    /* serverCron执行次数 */
    server.cronloops++;
    /* 返回下一次执行serverCron的间隔 */
    return 1000 / server.hz;
}

extern int ProcessingEventsWhileBlocked;

/* This function gets called every time Redis is entering the
 * main loop of the event driven library, that is, before to sleep
 * for ready file descriptors.(这个函数在每次Redis进入事件驱动库的主循环时被调用，也就是说，在休眠准备好的文件描述符之前。)
 *
 * Note: This function is (currently) called from two functions: （注意:该函数(当前)由两个函数调用:）
 * 1. aeMain - The main server loop (1. aeMain -主服务器循环)
 * 2. processEventsWhileBlocked - Process clients during RDB/AOF load (2. processeventswhile在RDB/AOF负载期间阻塞进程客户端)
 *
 * If it was called from processEventsWhileBlocked we don't want
 * to perform all actions (For example, we don't want to expire
 * keys), but we do need to perform some actions.（如果它是从processEventsWhileBlocked调用的，我们不想执行所有的操作(例如，我们不想让键过期)，但是我们确实需要执行一些操作。）
 *
 * The most important is freeClientsInAsyncFreeQueue but we also
 * call some other low-risk functions. (最重要的是freeClientsInAsyncFreeQueue，但我们也调用一些其他低风险函数。)*/
void beforeSleep(struct aeEventLoop *eventLoop) {
    UNUSED(eventLoop);

    /* Just call a subset of vital functions in case we are re-entering
     * the event loop from processEventsWhileBlocked(). Note that in this
     * case we keep track of the number of events we are processing, since
     * processEventsWhileBlocked() wants to stop ASAP if there are no longer
     * events to handle. （只需调用重要函数的子集，以防我们从processEventsWhileBlocked()重新进入事件循环。注意，在本例中，我们跟踪正在处理的事件数量，因为processeventswhile()希望在不再有需要处理的事件时尽快停止）*/
    if (ProcessingEventsWhileBlocked) {
        uint64_t processed = 0;
        processed += handleClientsWithPendingReadsUsingThreads();
        processed += tlsProcessPendingData();
        processed += handleClientsWithPendingWrites();
        processed += freeClientsInAsyncFreeQueue();
        server.events_processed_while_blocked += processed;
        return;
    }

    /* Handle precise timeouts of blocked clients.(处理阻塞客户端的精确超时。) */
    handleBlockedClientsTimeout();

    /* We should handle pending reads clients ASAP after event loop. （我们应该在事件循环后尽快处理等待的读取客户端。）*/
    handleClientsWithPendingReadsUsingThreads();

    /* Handle TLS pending data. (must be done before flushAppendOnlyFile) 在刷盘之前停止*/
    tlsProcessPendingData();

    /* If tls still has pending unread data don't sleep at all. */
    aeSetDontWait(server.el, tlsHasPendingData());

    /* Call the Redis Cluster before sleep function. Note that this function
     * may change the state of Redis Cluster (from ok to fail or vice versa),
     * so it's a good idea to call it before serving the unblocked clients
     * later in this function.(调用Redis集群before sleep函数。注意，这个函数可能会改变Redis集群的状态(从ok到fail或反之)，所以在这个函数中服务未阻塞的客户端之前调用它是一个好主意。我们应该在事件循环后尽快处理等待的读取客户端。) */
    if (server.cluster_enabled) clusterBeforeSleep();

    /* Run a fast expire cycle (the called function will return
     * ASAP if a fast cycle is not needed). (处理过期键)*/
    if (server.active_expire_enabled && server.masterhost == NULL)
        activeExpireCycle(ACTIVE_EXPIRE_CYCLE_FAST);

    /* Unblock all the clients blocked for synchronous replication
     * in WAIT.(解除在等待中为同步复制而阻塞的所有客户机的阻塞。唤醒因为复制而阻塞的客户端) */
    if (listLength(server.clients_waiting_acks))
        processClientsWaitingReplicas();

    /* Check if there are clients unblocked by modules that implement
     * blocking commands. */
    if (moduleCount()) moduleHandleBlockedClients();

    /* Try to process pending commands for clients that were just unblocked. */
    if (listLength(server.unblocked_clients))
        processUnblockedClients();

    /* Send all the slaves an ACK request if at least one client blocked
     * during the previous event loop iteration. Note that we do this after
     * processUnblockedClients(), so if there are multiple pipelined WAITs
     * and the just unblocked WAIT gets blocked again, we don't have to wait
     * a server cron cycle in absence of other event loop events. See #6623. */
    if (server.get_ack_from_slaves) {
        robj *argv[3];

        argv[0] = createStringObject("REPLCONF", 8);
        argv[1] = createStringObject("GETACK", 6);
        argv[2] = createStringObject("*", 1); /* Not used argument. */
        replicationFeedSlaves(server.slaves, server.slaveseldb, argv, 3);
        decrRefCount(argv[0]);
        decrRefCount(argv[1]);
        decrRefCount(argv[2]);
        server.get_ack_from_slaves = 0;
    }

    /* Send the invalidation messages to clients participating to the
     * client side caching protocol in broadcasting (BCAST) mode. (以广播(BCAST)模式将无效消息发送到参与客户端缓存协议的客户端。解除在等待中为同步复制而阻塞的所有客户机的阻塞。)*/
    trackingBroadcastInvalidationMessages();

    /* Write the AOF buffer on disk (aof 刷盘)*/
    flushAppendOnlyFile(0);

    /* Handle writes with pending output buffers. (挂起输出缓冲区的句柄写操作。)*/
    handleClientsWithPendingWritesUsingThreads();

    /* Close clients that need to be closed asynchronous 异步关闭客户端 */
    freeClientsInAsyncFreeQueue();

    /* Before we are going to sleep, let the threads access the dataset by
     * releasing the GIL. Redis main thread will not touch anything at this
     * time. */
    if (moduleCount()) moduleReleaseGIL();
}

/* This function is called immadiately after the event loop multiplexing
 * API returned, and the control is going to soon return to Redis by invoking
 * the different events callbacks. */
void afterSleep(struct aeEventLoop *eventLoop) {
    UNUSED(eventLoop);

    if (!ProcessingEventsWhileBlocked) {
        if (moduleCount()) moduleAcquireGIL();
    }
}

/* =========================== Server initialization ======================== */

void createSharedObjects(void) {
    int j;

    shared.crlf = createObject(OBJ_STRING, sdsnew("\r\n"));
    shared.ok = createObject(OBJ_STRING, sdsnew("+OK\r\n"));
    shared.err = createObject(OBJ_STRING, sdsnew("-ERR\r\n"));
    shared.emptybulk = createObject(OBJ_STRING, sdsnew("$0\r\n\r\n"));
    shared.czero = createObject(OBJ_STRING, sdsnew(":0\r\n"));
    shared.cone = createObject(OBJ_STRING, sdsnew(":1\r\n"));
    shared.emptyarray = createObject(OBJ_STRING, sdsnew("*0\r\n"));
    shared.pong = createObject(OBJ_STRING, sdsnew("+PONG\r\n"));
    shared.queued = createObject(OBJ_STRING, sdsnew("+QUEUED\r\n"));
    shared.emptyscan = createObject(OBJ_STRING, sdsnew("*2\r\n$1\r\n0\r\n*0\r\n"));
    shared.wrongtypeerr = createObject(OBJ_STRING, sdsnew(
            "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"));
    shared.nokeyerr = createObject(OBJ_STRING, sdsnew(
            "-ERR no such key\r\n"));
    shared.syntaxerr = createObject(OBJ_STRING, sdsnew(
            "-ERR syntax error\r\n"));
    shared.sameobjecterr = createObject(OBJ_STRING, sdsnew(
            "-ERR source and destination objects are the same\r\n"));
    shared.outofrangeerr = createObject(OBJ_STRING, sdsnew(
            "-ERR index out of range\r\n"));
    shared.noscripterr = createObject(OBJ_STRING, sdsnew(
            "-NOSCRIPT No matching script. Please use EVAL.\r\n"));
    shared.loadingerr = createObject(OBJ_STRING, sdsnew(
            "-LOADING Redis is loading the dataset in memory\r\n"));
    shared.slowscripterr = createObject(OBJ_STRING, sdsnew(
            "-BUSY Redis is busy running a script. You can only call SCRIPT KILL or SHUTDOWN NOSAVE.\r\n"));
    shared.masterdownerr = createObject(OBJ_STRING, sdsnew(
            "-MASTERDOWN Link with MASTER is down and replica-serve-stale-data is set to 'no'.\r\n"));
    shared.bgsaveerr = createObject(OBJ_STRING, sdsnew(
            "-MISCONF Redis is configured to save RDB snapshots, but it is currently not able to persist on disk. Commands that may modify the data set are disabled, because this instance is configured to report errors during writes if RDB snapshotting fails (stop-writes-on-bgsave-error option). Please check the Redis logs for details about the RDB error.\r\n"));
    shared.roslaveerr = createObject(OBJ_STRING, sdsnew(
            "-READONLY You can't write against a read only replica.\r\n"));
    shared.noautherr = createObject(OBJ_STRING, sdsnew(
            "-NOAUTH Authentication required.\r\n"));
    shared.oomerr = createObject(OBJ_STRING, sdsnew(
            "-OOM command not allowed when used memory > 'maxmemory'.\r\n"));
    shared.execaborterr = createObject(OBJ_STRING, sdsnew(
            "-EXECABORT Transaction discarded because of previous errors.\r\n"));
    shared.noreplicaserr = createObject(OBJ_STRING, sdsnew(
            "-NOREPLICAS Not enough good replicas to write.\r\n"));
    shared.busykeyerr = createObject(OBJ_STRING, sdsnew(
            "-BUSYKEY Target key name already exists.\r\n"));
    shared.space = createObject(OBJ_STRING, sdsnew(" "));
    shared.colon = createObject(OBJ_STRING, sdsnew(":"));
    shared.plus = createObject(OBJ_STRING, sdsnew("+"));

    /* The shared NULL depends on the protocol version. */
    shared.null[0] = NULL;
    shared.null[1] = NULL;
    shared.null[2] = createObject(OBJ_STRING, sdsnew("$-1\r\n"));
    shared.null[3] = createObject(OBJ_STRING, sdsnew("_\r\n"));

    shared.nullarray[0] = NULL;
    shared.nullarray[1] = NULL;
    shared.nullarray[2] = createObject(OBJ_STRING, sdsnew("*-1\r\n"));
    shared.nullarray[3] = createObject(OBJ_STRING, sdsnew("_\r\n"));

    shared.emptymap[0] = NULL;
    shared.emptymap[1] = NULL;
    shared.emptymap[2] = createObject(OBJ_STRING, sdsnew("*0\r\n"));
    shared.emptymap[3] = createObject(OBJ_STRING, sdsnew("%0\r\n"));

    shared.emptyset[0] = NULL;
    shared.emptyset[1] = NULL;
    shared.emptyset[2] = createObject(OBJ_STRING, sdsnew("*0\r\n"));
    shared.emptyset[3] = createObject(OBJ_STRING, sdsnew("~0\r\n"));

    for (j = 0; j < PROTO_SHARED_SELECT_CMDS; j++) {
        char dictid_str[64];
        int dictid_len;

        dictid_len = ll2string(dictid_str, sizeof(dictid_str), j);
        shared.select[j] = createObject(OBJ_STRING,
                                        sdscatprintf(sdsempty(),
                                                     "*2\r\n$6\r\nSELECT\r\n$%d\r\n%s\r\n",
                                                     dictid_len, dictid_str));
    }
    shared.messagebulk = createStringObject("$7\r\nmessage\r\n", 13);
    shared.pmessagebulk = createStringObject("$8\r\npmessage\r\n", 14);
    shared.subscribebulk = createStringObject("$9\r\nsubscribe\r\n", 15);
    shared.unsubscribebulk = createStringObject("$11\r\nunsubscribe\r\n", 18);
    shared.psubscribebulk = createStringObject("$10\r\npsubscribe\r\n", 17);
    shared.punsubscribebulk = createStringObject("$12\r\npunsubscribe\r\n", 19);
    shared.del = createStringObject("DEL", 3);
    shared.unlink = createStringObject("UNLINK", 6);
    shared.rpop = createStringObject("RPOP", 4);
    shared.lpop = createStringObject("LPOP", 4);
    shared.lpush = createStringObject("LPUSH", 5);
    shared.rpoplpush = createStringObject("RPOPLPUSH", 9);
    shared.zpopmin = createStringObject("ZPOPMIN", 7);
    shared.zpopmax = createStringObject("ZPOPMAX", 7);
    shared.multi = createStringObject("MULTI", 5);
    shared.exec = createStringObject("EXEC", 4);
    for (j = 0; j < OBJ_SHARED_INTEGERS; j++) {
        shared.integers[j] =
                makeObjectShared(createObject(OBJ_STRING, (void *) (long) j));
        shared.integers[j]->encoding = OBJ_ENCODING_INT;
    }
    for (j = 0; j < OBJ_SHARED_BULKHDR_LEN; j++) {
        shared.mbulkhdr[j] = createObject(OBJ_STRING,
                                          sdscatprintf(sdsempty(), "*%d\r\n", j));
        shared.bulkhdr[j] = createObject(OBJ_STRING,
                                         sdscatprintf(sdsempty(), "$%d\r\n", j));
    }
    /* The following two shared objects, minstring and maxstrings, are not
     * actually used for their value but as a special object meaning
     * respectively the minimum possible string and the maximum possible
     * string in string comparisons for the ZRANGEBYLEX command. */
    shared.minstring = sdsnew("minstring");
    shared.maxstring = sdsnew("maxstring");
}

/**
 * Server是一个redisServer类型的全局变量，也是redis最重要的一个全局变量，大部分的服务器配置和一些重要状态都保存在这里。
 * initServerConfig()函数是初始化Server的部分成员变量。
 */
void initServerConfig(void) {
    int j;
    /**
     * 设置缓存时间，使用时直接获取避免每次创建
     */
    updateCachedTime(1);
    getRandomHexChars(server.runid, CONFIG_RUN_ID_SIZE);
    server.runid[CONFIG_RUN_ID_SIZE] = '\0';
    changeReplicationId();// 每次启动在初始化时更改当前实例的切片ID，因此从节点重启时关系到psync 操作
    clearReplicationId2();// 与全量同步时的offset 相关的
    server.hz = CONFIG_DEFAULT_HZ; /* Initialize it ASAP, even if it may get
                                      updated later after loading the config.(尽快初始化它，即使它可能在稍后加载配置后被更新。)
                                      This value may be used before the server
                                      is initialized.(该值可以在服务器初始化之前使用。) */
    server.timezone = getTimeZone(); /* Initialized by tzset(). */
    server.configfile = NULL;
    server.executable = NULL;
    // 判断当前执行环境是多少位。
    server.arch_bits = (sizeof(long) == 8) ? 64 : 32;
    server.bindaddr_count = 0;
    server.unixsocketperm = CONFIG_DEFAULT_UNIX_SOCKET_PERM;
    server.ipfd_count = 0;// 在ipfd 数组中使用的槽 数量
    server.tlsfd_count = 0;
    server.sofd = -1;//套接字描述符
    server.active_expire_enabled = 1;
    // 客户端查询缓存最大长度，默认1G
    server.client_max_querybuf_len = PROTO_MAX_QUERYBUF_LEN;
    // 这个存储的是redis服务器端程序从配置文件中读取的持久化参数
    server.saveparams = NULL;
    server.loading = 0;
    server.logfile = zstrdup(CONFIG_DEFAULT_LOGFILE);
    server.aof_state = AOF_OFF;
    server.aof_rewrite_base_size = 0;
    server.aof_rewrite_scheduled = 0;
    server.aof_flush_sleep = 0;
    server.aof_last_fsync = time(NULL);
    server.aof_rewrite_time_last = -1;
    server.aof_rewrite_time_start = -1;
    server.aof_lastbgrewrite_status = C_OK;
    server.aof_delayed_fsync = 0;
    server.aof_fd = -1;
    server.aof_selected_db = -1; /* Make sure the first time will not match */
    server.aof_flush_postponed_start = 0;
    server.pidfile = NULL;
    server.active_defrag_running = 0;
    server.notify_keyspace_events = 0;
    server.blocked_clients = 0;
    memset(server.blocked_clients_by_type, 0,
           sizeof(server.blocked_clients_by_type)); // linux 申请新内存空间之前的初始化
    server.shutdown_asap = 0;
    server.cluster_configfile = zstrdup(CONFIG_DEFAULT_CLUSTER_CONFIG_FILE);
    server.cluster_module_flags = CLUSTER_MODULE_FLAG_NONE;
    server.migrate_cached_sockets = dictCreate(&migrateCacheDictType, NULL);
    server.next_client_id = 1; /* Client IDs, start from 1 .*/
    server.loading_process_events_interval_bytes = (1024 * 1024 * 2);

    // redis实现LRU算法所需的，每个redis object都带有一个lruclock，用来从内存中移除空闲的对象。
    server.lruclock = getLRUClock();
    resetServerSaveParams();

    appendServerSaveParams(60 * 60, 1);  /* save after 1 hour and 1 change */
    appendServerSaveParams(300, 100);  /* save after 5 minutes and 100 changes */
    appendServerSaveParams(60, 10000); /* save after 1 minute and 10000 changes */

    /* Replication related(复制相关) */
    server.masterauth = NULL;
    server.masterhost = NULL;
    server.masterport = 6379;
    server.master = NULL;
    server.cached_master = NULL;
    server.master_initial_offset = -1;
    server.repl_state = REPL_STATE_NONE;
    server.repl_transfer_tmpfile = NULL;
    server.repl_transfer_fd = -1;
    server.repl_transfer_s = NULL;
    server.repl_syncio_timeout = CONFIG_REPL_SYNCIO_TIMEOUT;
    server.repl_down_since = 0; /* Never connected, repl is down since EVER. */
    server.master_repl_offset = 0;

    /* Replication partial resync backlog */
    server.repl_backlog = NULL;
    server.repl_backlog_histlen = 0;
    server.repl_backlog_idx = 0;
    server.repl_backlog_off = 0;
    server.repl_no_slaves_since = time(NULL);

    /* Client output buffer limits :3种，普通，事件，订阅*/
    for (j = 0; j < CLIENT_TYPE_OBUF_COUNT; j++)
        server.client_obuf_limits[j] = clientBufferLimitsDefaults[j];

    /* Linux OOM Score config */
    for (j = 0; j < CONFIG_OOM_COUNT; j++)
        server.oom_score_adj_values[j] = configOOMScoreAdjValuesDefaults[j];

    /* Double constants initialization */
    R_Zero = 0.0;
    R_PosInf = 1.0 / R_Zero;
    R_NegInf = -1.0 / R_Zero;
    R_Nan = R_Zero / R_Zero;

    /* Command table -- we initiialize it here as it is part of the
     * initial configuration, since command names may be changed via
     * redis.conf using the rename-command directive. */
    //redis命令的字符数组
    server.commands = dictCreate(&commandTableDictType, NULL);
    server.orig_commands = dictCreate(&commandTableDictType, NULL);
    populateCommandTable();
    server.delCommand = lookupCommandByCString("del");
    server.multiCommand = lookupCommandByCString("multi");
    server.lpushCommand = lookupCommandByCString("lpush");
    server.lpopCommand = lookupCommandByCString("lpop");
    server.rpopCommand = lookupCommandByCString("rpop");
    server.zpopminCommand = lookupCommandByCString("zpopmin");
    server.zpopmaxCommand = lookupCommandByCString("zpopmax");
    server.sremCommand = lookupCommandByCString("srem");
    server.execCommand = lookupCommandByCString("exec");
    server.expireCommand = lookupCommandByCString("expire");
    server.pexpireCommand = lookupCommandByCString("pexpire");
    server.xclaimCommand = lookupCommandByCString("xclaim");
    server.xgroupCommand = lookupCommandByCString("xgroup");
    server.rpoplpushCommand = lookupCommandByCString("rpoplpush");

    /* Debugging */
    server.assert_failed = "<no assertion failed>";
    server.assert_file = "<no file>";
    server.assert_line = 0;
    server.bug_report_start = 0;
    server.watchdog_period = 0;

    /* By default we want scripts to be always replicated by effects
     * (single commands executed by the script), and not by sending the
     * script to the slave / AOF. This is the new way starting from
     * Redis 5. However it is possible to revert it via redis.conf. */
    server.lua_always_replicate_commands = 1;

    initConfigValues();
}

extern char **environ;

/* Restart the server, executing the same executable that started this
 * instance, with the same arguments and configuration file.
 *
 * The function is designed to directly call execve() so that the new
 * server instance will retain the PID of the previous one.
 *
 * The list of flags, that may be bitwise ORed together, alter the
 * behavior of this function:
 *
 * RESTART_SERVER_NONE              No flags.
 * RESTART_SERVER_GRACEFULLY        Do a proper shutdown before restarting.
 * RESTART_SERVER_CONFIG_REWRITE    Rewrite the config file before restarting.
 *
 * On success the function does not return, because the process turns into
 * a different process. On error C_ERR is returned. */
int restartServer(int flags, mstime_t delay) {
    int j;

    /* Check if we still have accesses to the executable that started this
     * server instance. */
    if (access(server.executable, X_OK) == -1) {
        serverLog(LL_WARNING, "Can't restart: this process has no "
                              "permissions to execute %s", server.executable);
        return C_ERR;
    }

    /* Config rewriting. */
    if (flags & RESTART_SERVER_CONFIG_REWRITE &&
        server.configfile &&
        rewriteConfig(server.configfile, 0) == -1) {
        serverLog(LL_WARNING, "Can't restart: configuration rewrite process "
                              "failed");
        return C_ERR;
    }

    /* Perform a proper shutdown. */
    if (flags & RESTART_SERVER_GRACEFULLY &&
        prepareForShutdown(SHUTDOWN_NOFLAGS) != C_OK) {
        serverLog(LL_WARNING, "Can't restart: error preparing for shutdown");
        return C_ERR;
    }

    /* Close all file descriptors, with the exception of stdin, stdout, strerr
     * which are useful if we restart a Redis server which is not daemonized. */
    for (j = 3; j < (int) server.maxclients + 1024; j++) {
        /* Test the descriptor validity before closing it, otherwise
         * Valgrind issues a warning on close(). */
        if (fcntl(j, F_GETFD) != -1) close(j);
    }

    /* Execute the server with the original command line. */
    if (delay) usleep(delay * 1000);
    zfree(server.exec_argv[0]);
    server.exec_argv[0] = zstrdup(server.executable);
    execve(server.executable, server.exec_argv, environ);

    /* If an error occurred here, there is nothing we can do, but exit. */
    _exit(1);

    return C_ERR; /* Never reached. */
}

static void readOOMScoreAdj(void) {
#ifdef HAVE_PROC_OOM_SCORE_ADJ
                                                                                                                            char buf[64];
    int fd = open("/proc/self/oom_score_adj", O_RDONLY);

    if (fd < 0) return;
    if (read(fd, buf, sizeof(buf)) > 0)
        server.oom_score_adj_base = atoi(buf);
    close(fd);
#endif
}

/* This function will configure the current process's oom_score_adj according
 * to user specified configuration. This is currently implemented on Linux
 * only.
 *
 * A process_class value of -1 implies OOM_CONFIG_MASTER or OOM_CONFIG_REPLICA,
 * depending on current role.
 */
int setOOMScoreAdj(int process_class) {

    if (!server.oom_score_adj) return C_OK;
    if (process_class == -1)
        process_class = (server.masterhost ? CONFIG_OOM_REPLICA : CONFIG_OOM_MASTER);

    serverAssert(process_class >= 0 && process_class < CONFIG_OOM_COUNT);

#ifdef HAVE_PROC_OOM_SCORE_ADJ
                                                                                                                            int fd;
    int val;
    char buf[64];

    val = server.oom_score_adj_base + server.oom_score_adj_values[process_class];
    if (val > 1000) val = 1000;
    if (val < -1000) val = -1000;

    snprintf(buf, sizeof(buf) - 1, "%d\n", val);

    fd = open("/proc/self/oom_score_adj", O_WRONLY);
    if (fd < 0 || write(fd, buf, strlen(buf)) < 0) {
        serverLog(LOG_WARNING, "Unable to write oom_score_adj: %s", strerror(errno));
        if (fd != -1) close(fd);
        return C_ERR;
    }

    close(fd);
    return C_OK;
#else
    /* Unsupported */
    return C_ERR;
#endif
}

/* This function will try to raise the max number of open files accordingly to
 * the configured max number of clients. It also reserves a number of file
 * descriptors (CONFIG_MIN_RESERVED_FDS) for extra operations of
 * persistence, listening sockets, log files and so forth.
 *(此函数将尝试将打开的文件的最大数量相应地提高到配置的最大客户端数量。它还保留了一些文件描述符(CONFIG_MIN_RESERVED_FDS)，用于额外的持久性操作、监听套接字、日志文件等等。)
 * If it will not be possible to set the limit accordingly to the configured
 * max number of clients, the function will do the reverse setting
 * server.maxclients to the value that we can actually handle. (如果不能根据所配置的最大客户端数量相应地设置限制，该函数将反向设置服务器。maxclients到我们可以实际操作的值。将命令字符串标志描述转换为一组实际的标志。)*/
void adjustOpenFilesLimit(void) {
    rlim_t maxfiles = server.maxclients + CONFIG_MIN_RESERVED_FDS;
    struct rlimit limit;

    if (getrlimit(RLIMIT_NOFILE, &limit) == -1) {
        serverLog(LL_WARNING,
                  "Unable to obtain the current NOFILE limit (%s), assuming 1024 and setting the max clients configuration accordingly.",
                  strerror(errno));
        server.maxclients = 1024 - CONFIG_MIN_RESERVED_FDS;
    } else {
        rlim_t oldlimit = limit.rlim_cur;

        /* Set the max number of files if the current limit is not enough
         * for our needs. */
        if (oldlimit < maxfiles) {
            rlim_t bestlimit;
            int setrlimit_error = 0;

            /* Try to set the file limit to match 'maxfiles' or at least
             * to the higher value supported less than maxfiles. */
            bestlimit = maxfiles;
            while (bestlimit > oldlimit) {
                rlim_t decr_step = 16;

                limit.rlim_cur = bestlimit;
                limit.rlim_max = bestlimit;
                if (setrlimit(RLIMIT_NOFILE, &limit) != -1) break;
                setrlimit_error = errno;

                /* We failed to set file limit to 'bestlimit'. Try with a
                 * smaller limit decrementing by a few FDs per iteration. */
                if (bestlimit < decr_step) break;
                bestlimit -= decr_step;
            }

            /* Assume that the limit we get initially is still valid if
             * our last try was even lower. */
            if (bestlimit < oldlimit) bestlimit = oldlimit;

            if (bestlimit < maxfiles) {
                unsigned int old_maxclients = server.maxclients;
                server.maxclients = bestlimit - CONFIG_MIN_RESERVED_FDS;
                /* maxclients is unsigned so may overflow: in order
                 * to check if maxclients is now logically less than 1
                 * we test indirectly via bestlimit. */
                if (bestlimit <= CONFIG_MIN_RESERVED_FDS) {
                    serverLog(LL_WARNING, "Your current 'ulimit -n' "
                                          "of %llu is not enough for the server to start. "
                                          "Please increase your open file limit to at least "
                                          "%llu. Exiting.",
                              (unsigned long long) oldlimit,
                              (unsigned long long) maxfiles);
                    exit(1);
                }
                serverLog(LL_WARNING, "You requested maxclients of %d "
                                      "requiring at least %llu max file descriptors.",
                          old_maxclients,
                          (unsigned long long) maxfiles);
                serverLog(LL_WARNING, "Server can't set maximum open files "
                                      "to %llu because of OS error: %s.",
                          (unsigned long long) maxfiles, strerror(setrlimit_error));
                serverLog(LL_WARNING, "Current maximum open files is %llu. "
                                      "maxclients has been reduced to %d to compensate for "
                                      "low ulimit. "
                                      "If you need higher maxclients increase 'ulimit -n'.",
                          (unsigned long long) bestlimit, server.maxclients);
            } else {
                serverLog(LL_NOTICE, "Increased maximum number of open files "
                                     "to %llu (it was originally set to %llu).",
                          (unsigned long long) maxfiles,
                          (unsigned long long) oldlimit);
            }
        }
    }
}

/* Check that server.tcp_backlog can be actually enforced in Linux according
 * to the value of /proc/sys/net/core/somaxconn, or warn about it. */
void checkTcpBacklogSettings(void) {
#ifdef HAVE_PROC_SOMAXCONN
                                                                                                                            FILE *fp = fopen("/proc/sys/net/core/somaxconn","r");
    char buf[1024];
    if (!fp) return;
    if (fgets(buf,sizeof(buf),fp) != NULL) {
        int somaxconn = atoi(buf);
        if (somaxconn > 0 && somaxconn < server.tcp_backlog) {
            serverLog(LL_WARNING,"WARNING: The TCP backlog setting of %d cannot be enforced because /proc/sys/net/core/somaxconn is set to the lower value of %d.", server.tcp_backlog, somaxconn);
        }
    }
    fclose(fp);
#endif
}

/**
 * Initialize a set of file descriptors to listen to the specified 'port'
 * binding the addresses specified in the Redis server configuration.
 *
 * The listening file descriptors are stored in the integer array 'fds'
 * and their number is set in '*count'.
 *
 * The addresses to bind are specified in the global server.bindaddr array
 * and their number is server.bindaddr_count. If the server configuration
 * contains no specific addresses to bind, this function will try to
 * bind * (all addresses) for both the IPv4 and IPv6 protocols.
 *
 * On success the function returns C_OK.
 *
 * On error the function returns C_ERR. For the function to be on
 * error, at least one of the server.bindaddr addresses was
 * impossible to bind, or no bind addresses were specified in the server
 * configuration but the function is not able to bind * for at least
 * one of the IPv4 or IPv6 protocols.
 * 初始化一组文件描述符来监听指定的“端口”绑定Redis服务器配置中指定的地址。侦听的文件描述符存储在整数数组“fds”中，它们的数量设置在“count”中。要绑定的地址在全局服务器中指定。bindaddr数组，其编号为server.bindaddr_count。如果服务器配置不包含要绑定的特定地址，此函数将尝试为IPv4和IPv6协议绑定(所有地址)。
 * @param port 端口
 * @param fds 数组指向所有socket文件描述符
 * @param count 存储socket数量
 **/
int listenToPort(int port, int *fds, int *count) {
    int j;
    // 输入的port表示用户配置的端口号，server结构体的bindaddr_count字段存储用户配置的IP地址数目,bindaddr字段存储用户配置的所有IP地址
    /* Force binding of 0.0.0.0 if no bind address is specified, always
     * entering the loop if j == 0. */
    //如果没有指定 IP ，设置为 NULL
    if (server.bindaddr_count == 0) server.bindaddr[0] = NULL;
    //遍历所有需要bind的 ip
    for (j = 0; j < server.bindaddr_count || j == 0; j++) {
        //如果为空，用户没有指定，使用默认值，即绑定IPV4 又 绑定 IPV6
        if (server.bindaddr[j] == NULL) {
            int unsupported = 0;
            /* Bind * for both IPv6 and IPv4, we enter here only if
             * server.bindaddr_count == 0. */
            //创建socket并启动监听，文件描述符存储在fds数组作为返回参数（IPV6）
            // TODO aneTcpServer实现了 socket 的创建，绑定，监听流程
            fds[*count] = anetTcp6Server(server.neterr, port, NULL,
                                         server.tcp_backlog);
            if (fds[*count] != ANET_ERR) {
                //TODO 设置socket 为非阻塞,anetNonBlock通过系统调用fcntl设置socket为非阻塞模式。
                anetNonBlock(NULL, fds[*count]);
                (*count)++;
            } else if (errno == EAFNOSUPPORT) {
                unsupported++;
                serverLog(LL_WARNING, "Not listening to IPv6: unsupported");
            }

            if (*count == 1 || unsupported) {
                /* Bind the IPv4 address as well. */
                //创建socket并启动监听，文件描述符存储在fds数组作为返回参数（IPV4）
                fds[*count] = anetTcpServer(server.neterr, port, NULL,
                                            server.tcp_backlog);
                if (fds[*count] != ANET_ERR) {
                    //设置socket 为非阻塞
                    anetNonBlock(NULL, fds[*count]);
                    (*count)++;
                } else if (errno == EAFNOSUPPORT) {
                    unsupported++;
                    serverLog(LL_WARNING, "Not listening to IPv4: unsupported");
                }
            }
            /* Exit the loop if we were able to bind * on IPv4 and IPv6,
             * otherwise fds[*count] will be ANET_ERR and we'll print an
             * error and return to the caller with an error. */
            if (*count + unsupported == 2) break;
        } else if (strchr(server.bindaddr[j], ':')) {
            /* Bind IPv6 address. */
            //IP里面有带 : 肯定是IPV6
            //创建socket并启动监听，文件描述符存储在fds数组作为返回参数（IPV6）
            fds[*count] = anetTcp6Server(server.neterr, port, server.bindaddr[j],
                                         server.tcp_backlog);
        } else {
            /* Bind IPv4 address. */
            //如果没有 : 而且还有 IP 那就是 IPV4
            /* Bind IPv4 address. */
            //创建socket并启动监听，文件描述符存储在fds数组作为返回参数（IPV4）
            fds[*count] = anetTcpServer(server.neterr, port, server.bindaddr[j],
                                        server.tcp_backlog);
        }
        if (fds[*count] == ANET_ERR) {
            serverLog(LL_WARNING,
                      "Could not create server TCP listening socket %s:%d: %s",
                      server.bindaddr[j] ? server.bindaddr[j] : "*",
                      port, server.neterr);
            if (errno == ENOPROTOOPT || errno == EPROTONOSUPPORT ||
                errno == ESOCKTNOSUPPORT || errno == EPFNOSUPPORT ||
                errno == EAFNOSUPPORT || errno == EADDRNOTAVAIL)
                continue;
            return C_ERR;
        }
        //设置socket 为非阻塞
        anetNonBlock(NULL, fds[*count]);
        (*count)++;
    }
    return C_OK;
}

/* Resets the stats that we expose via INFO or other means that we want
 * to reset via CONFIG RESETSTAT. The function is also used in order to
 * initialize these fields in initServer() at server startup.
 * 重置我们通过INFO或其他方式公开的统计数据，而我们希望通过配置RESETSTAT重置这些数据。该函数还用于在服务器启动时初始化initServer()中的这些字段。
 * */
void resetServerStats(void) {
    int j;
    // 命令数
    server.stat_numcommands = 0;
    // 连接数
    server.stat_numconnections = 0;
    // 过期键
    server.stat_expiredkeys = 0;
    server.stat_expired_stale_perc = 0;
    server.stat_expired_time_cap_reached_count = 0;
    server.stat_expire_cycle_time_used = 0;
    server.stat_evictedkeys = 0;
    server.stat_keyspace_misses = 0;
    server.stat_keyspace_hits = 0;
    server.stat_active_defrag_hits = 0;
    server.stat_active_defrag_misses = 0;
    server.stat_active_defrag_key_hits = 0;
    server.stat_active_defrag_key_misses = 0;
    server.stat_active_defrag_scanned = 0;
    server.stat_fork_time = 0;
    server.stat_fork_rate = 0;
    server.stat_rejected_conn = 0;
    server.stat_sync_full = 0;
    server.stat_sync_partial_ok = 0;
    server.stat_sync_partial_err = 0;
    server.stat_io_reads_processed = 0;
    server.stat_total_reads_processed = 0;
    server.stat_io_writes_processed = 0;
    server.stat_total_writes_processed = 0;
    for (j = 0; j < STATS_METRIC_COUNT; j++) {
        server.inst_metric[j].idx = 0;
        server.inst_metric[j].last_sample_time = mstime();
        server.inst_metric[j].last_sample_count = 0;
        memset(server.inst_metric[j].samples, 0,
               sizeof(server.inst_metric[j].samples));
    }
    server.stat_net_input_bytes = 0;
    server.stat_net_output_bytes = 0;
    server.stat_unexpected_error_replies = 0;
    server.aof_delayed_fsync = 0;
}

void initServer(void) {
    int j;

    // 让系统忽略SIGHUP信号(控制终端信号)，作为守护进程运行，不会有控制终端，所以忽略掉SIGHUP信号。
    signal(SIGHUP, SIG_IGN);
    // 让系统忽略SIGHUP信号(控制终端信号)，SIGPIPE信号是在写管道发现读进程终止时产生的信号，向已经终止的SOCK_STREAM套接字写入也会产生此信号。redis作为server，不可避免的会遇到各种各样的client，client意外终止导致产生的信号也应该在server启动后忽略掉。
    signal(SIGPIPE, SIG_IGN);
    /**
     * setupSignalHandlers函数处理的信号分两类：
     *  1）SIGTERM。SIGTERM是kill命令发送的系统默认终止信号。也就是我们在试图结束server时会触发的信号。对这类信号，redis���没有立即终止进程，其处理行为是，
     *   设置一个server.shutdown_asap，然后在下一次执行serverCron时，调用prepareForShutdown做清理工作，然后再退出程序。这样可以有效的避免盲目的kill程序导致数据丢失，
      *  2）SIGSEGV、SIGBUS、SIGFPE、SIGILL。这几个信号分别为无效内存引用（即我们常说的段错误），实现定义的硬件故障，算术运算错误（如除0）以及执行非法硬件指令。
     *   这类是非常严重的错误，redis的处理是通过sigsegvHandler，记录出错时的现场、执行必要的清理工作，然后kill自身。除上面提到的7个信号意外，redis不再处理任何其他信号，
     *   均保留默认操作。
     */
    setupSignalHandlers();

    // 如果服务器配置中启动了日志，那么就打开系统日志。openlog属于linux系统函数，如果不知道用法自行搜索。
    if (server.syslog_enabled) {
        openlog(server.syslog_ident, LOG_PID | LOG_NDELAY | LOG_NOWAIT,
                server.syslog_facility);
    }

    /* Initialization after setting defaults from the config system. */
    server.aof_state = server.aof_enabled ? AOF_ON : AOF_OFF;
    server.hz = server.config_hz;
    server.pid = getpid();
    // 设置当前处理的客户端为空
    server.current_client = NULL;
    server.fixed_time_expire = 0;
    // 客户端链表，用adlist.c的listCreate()创建了链表(实际分配头结点内存)
    server.clients = listCreate();
    server.clients_index = raxNew();
    // 要被关闭的客户端链表，
    server.clients_to_close = listCreate();
    // slave节点链表
    server.slaves = listCreate();
    // monitor客户端链表
    server.monitors = listCreate();
    server.clients_pending_write = listCreate();
    server.clients_pending_read = listCreate();
    server.clients_timeout_table = raxNew();
    server.slaveseldb = -1; /* Force to emit the first SELECT command. */
    // 被取消阻塞的客户端链表
    server.unblocked_clients = listCreate();
    // 已就绪key链表
    server.ready_keys = listCreate();
    server.clients_waiting_acks = listCreate();
    server.get_ack_from_slaves = 0;
    server.clients_paused = 0;
    server.events_processed_while_blocked = 0;
    server.system_memory_size = zmalloc_get_memory_size();

    if ((server.tls_port || server.tls_replication || server.tls_cluster)
        && tlsConfigure(&server.tls_ctx_config) == C_ERR) {
        serverLog(LL_WARNING, "Failed to configure TLS. Check logs for more info.");
        exit(1);
    }
    /**
     * TODO 初始化共享对象,主要是设置redis.c里的全局对象structsharedObjectsStructshared的属性赋初值。Redis出于性能的考虑，把一些server执行过程中经常用到的对象构造出来，
     * 放到内存中，使用到的时候直接从这里取，避免临时申请的开销。比如“+OK”反馈，错误反馈，1~10000的整数对象等等。
     */
    createSharedObjects();
    /**
     * TODO 获取最大打开文件数目，根据这个打开文件最大数，适当调整同时支持的客户端数server.maxclients 变量。
     */
    adjustOpenFilesLimit();
    /**
     * TODO 创建事件循环aeCreateEventLoop ()函数先创建一个结构aeEventLoop的指针变量eventLoop，并从堆中申请内存，把这个结构的一些成员赋初始值，
     * 创建aeApiState结构指针变量state，并创建epool，并把fd赋给state->epfd，最后把state赋给eventLoop->apidata。
     */
    server.el = aeCreateEventLoop(server.maxclients + CONFIG_FDSET_INCR);
    if (server.el == NULL) {
        serverLog(LL_WARNING,
                  "Failed creating the event loop. Error message: '%s'",
                  strerror(errno));
        exit(1);
    }
    // 根据服务器配置分配数据库内存
    server.db = zmalloc(sizeof(redisDb) * server.dbnum);

    /* TODO Open the TCP listening socket for the user commands. */
    if (server.port != 0 &&
        listenToPort(server.port, server.ipfd, &server.ipfd_count) == C_ERR)
        exit(1);
    if (server.tls_port != 0 &&
        listenToPort(server.tls_port, server.tlsfd, &server.tlsfd_count) == C_ERR)
        exit(1);

    /* Open the listening Unix domain socket. */
    //如果server.port设置了值，根据srver配置，创建SOCK_STREAM套接字，并监听server.port这个端口, 如果创建失败，则结束服务器程序。
    if (server.unixsocket != NULL) {
        unlink(server.unixsocket); /* don't care if this fails */
        server.sofd = anetUnixServer(server.neterr, server.unixsocket,
                                     server.unixsocketperm, server.tcp_backlog);
        if (server.sofd == ANET_ERR) {
            serverLog(LL_WARNING, "Opening Unix socket: %s", server.neterr);
            // 建立unix socket(本地无名套接字)，流程同上。如果配置两种连接方式都没设置，服务器程序也会退出。
            exit(1);
        }
        anetNonBlock(NULL, server.sofd);
    }

    /* Abort if there are no listening sockets at all. */
    if (server.ipfd_count == 0 && server.tlsfd_count == 0 && server.sofd < 0) {
        serverLog(LL_WARNING, "Configured to not listen anywhere, exiting.");
        exit(1);
    }

    /* Create the Redis databases, and initialize other internal state.
     * 根据配置初始化数据库
     * */
    for (j = 0; j < server.dbnum; j++) {
        // key 哈希
        server.db[j].dict = dictCreate(&dbDictType, NULL);
        // 过期key hash，存储会过期的key以及相应过期时间，即一对(key,time_t)的kv组合，也是一个。
        server.db[j].expires = dictCreate(&keyptrDictType, NULL);
        server.db[j].expires_cursor = 0;
        // 阻塞键hash
        server.db[j].blocking_keys = dictCreate(&keylistDictType, NULL);
        // 收到push命令的阻塞键hash
        server.db[j].ready_keys = dictCreate(&objectKeyPointerValueDictType, NULL);
        // 被WATCH命令监视的键hash
        server.db[j].watched_keys = dictCreate(&keylistDictType, NULL);
        // 数据库ID，数据库的id是从0开始递增。
        server.db[j].id = j;
        server.db[j].avg_ttl = 0;
        server.db[j].defrag_later = listCreate();
        listSetFreeMethod(server.db[j].defrag_later, (void (*)(void *)) sdsfree);
    }
    // 初始化lru
    evictionPoolAlloc(); /* Initialize the LRU keys pool. */
    // 订阅频道的hash，用来记录所有订阅的client。
    server.pubsub_channels = dictCreate(&keylistDictType, NULL);
    // 初始化订阅-发布模式列表
    server.pubsub_patterns = listCreate();
    server.pubsub_patterns_dict = dictCreate(&keylistDictType, NULL);
    // 发布-订阅函数的释放函数指针
    listSetFreeMethod(server.pubsub_patterns, freePubsubPattern);
    // 匹配函数
    listSetMatchMethod(server.pubsub_patterns, listMatchPubsubPattern);
    // CRON执行计数
    server.cronloops = 0;
    // BGSAVE 执行指示变量
    server.rdb_child_pid = -1;
    // BGREWRITEAOF 执行指示变量
    server.aof_child_pid = -1;
    //
    server.module_child_pid = -1;
    //
    server.rdb_child_type = RDB_CHILD_TYPE_NONE;
    server.rdb_pipe_conns = NULL;
    server.rdb_pipe_numconns = 0;
    server.rdb_pipe_numconns_writing = 0;
    server.rdb_pipe_buff = NULL;
    server.rdb_pipe_bufflen = 0;
    server.rdb_bgsave_scheduled = 0;
    server.child_info_pipe[0] = -1;
    server.child_info_pipe[1] = -1;
    server.child_info_data.magic = 0;
    aofRewriteBufferReset();
    //
    server.aof_buf = sdsempty();
    // 最后一次成功保存的时间
    server.lastsave = time(NULL); /* At startup we consider the DB saved. */
    server.lastbgsave_try = 0;    /* At startup we never tried to BGSAVE. */
    // 结束 SAVE 的时间
    server.rdb_save_time_last = -1;
    // 开始 SAVE 的时间
    server.rdb_save_time_start = -1;
    // 用来后续计算server维护的数据是否有更新，如果有，需要记录aof和通知replication.
    server.dirty = 0;
    resetServerStats();
    /* A few stats we don't want to reset: server startup time, and peak mem. */
    server.stat_starttime = time(NULL);
    server.stat_peak_memory = 0;
    server.stat_rdb_cow_bytes = 0;
    server.stat_aof_cow_bytes = 0;
    server.stat_module_cow_bytes = 0;
    for (int j = 0; j < CLIENT_TYPE_COUNT; j++)
        server.stat_clients_type_memory[j] = 0;
    server.cron_malloc_stats.zmalloc_used = 0;
    server.cron_malloc_stats.process_rss = 0;
    server.cron_malloc_stats.allocator_allocated = 0;
    server.cron_malloc_stats.allocator_active = 0;
    server.cron_malloc_stats.allocator_resident = 0;
    server.lastbgsave_status = C_OK;
    server.aof_last_write_status = C_OK;
    server.aof_last_write_errno = 0;
    server.repl_good_slaves_count = 0;

    /* Create the timer callback, this is our way to process many background
     * operations incrementally, like clients timeout, eviction of unaccessed
     * expired keys and so forth. */
    // TODO【非常重要】创建一个ae定时事件，加到server.el->timeEventHead的头部，并将serverCron设置为这个定时事件的处理函数。这是redis的核心循环，该过程是serverCron，每秒调用次数由一个叫REDIS_HZ的宏决定，默认是每10微秒超时，即每10微秒该ae时间事件处理过程serverCron会被过期调用
    if (aeCreateTimeEvent(server.el, 1, serverCron, NULL, NULL) == AE_ERR) {
        serverPanic("Can't create event loop timers.");
        exit(1);
    }

    /* Create an event handler for accepting new connections in TCP and Unix
     * domain sockets. */
    /**
     * TODO 创建ae文件事件，对redis的TCP或者unixsocket端口进行监听，使用相应的处理函数注册。每次得到clients连接后，都会触发ae文件事件，异步接收命令。
     * 如果server.ipfd(tcp/ip文件描述,类似于windows下的handle)有值,也即tcp/ip套接字创建成功的情况下,创建网络事件。在有连接请求进来后，acceptTcpHandler将会被调用，
     * 该函数调用accept接收连接，然后用accept函数返回的文件描述符创建一个client桩（一个redisClient对象），在server端代表连接进来的真正client。在创建client桩的时候，
     * 会将返回的这个描述符同样添加进事件监控列表，监控READABLE事件，事件发生代表着客户端发送数据过来，此时调readQueryFromClient接收客户端的query。
     * 对IP socket 创建文件事件.
     *
     *
     * server结构体的ipfd_count字段存储创建的监听socket数目，ipfd数组存储的是所有监听socket文件描述符，需要遍历所有的监听socket，为其创建对应的文件事件。
     * 可以看到监听事件的处理函数为acceptTcpHandler（后面的指令处理和这个函数有关），实现了socket连接请求的accept，以及客户端对象的创建。
     */
    for (j = 0; j < server.ipfd_count; j++) {
        if (aeCreateFileEvent(server.el, server.ipfd[j], AE_READABLE,
                              acceptTcpHandler, NULL) == AE_ERR) {
            serverPanic(
                    "Unrecoverable error creating server.ipfd file event.");
        }
    }
    //对 tls 创建文件事件
    for (j = 0; j < server.tlsfd_count; j++) {
        if (aeCreateFileEvent(server.el, server.tlsfd[j], AE_READABLE,
                              acceptTLSHandler, NULL) == AE_ERR) {
            serverPanic(
                    "Unrecoverable error creating server.tlsfd file event.");
        }
    }
    if (server.sofd > 0 && aeCreateFileEvent(server.el, server.sofd, AE_READABLE,
                                             acceptUnixHandler, NULL) == AE_ERR)
        serverPanic("Unrecoverable error creating server.sofd file event.");


    /* Register a readable event for the pipe used to awake the event loop
     * when a blocked client in a module needs attention. */
    if (aeCreateFileEvent(server.el, server.module_blocked_pipe[0], AE_READABLE,
                          moduleBlockedClientPipeReadable, NULL) == AE_ERR) {
        serverPanic(
                "Error registering the readable event for the module "
                "blocked clients subsystem.");
    }

    /* Register before and after sleep handlers (note this needs to be done
     * before loading persistence since it is used by processEventsWhileBlocked. （在睡眠处理程序之前和之后注册(注意这需要在加载持久性之前完成，因为它被processeventswhile阻塞使用。）
     * TODO 增加sleep 前执行逻辑，和sleep后 执行逻辑
     * */
    aeSetBeforeSleepProc(server.el, beforeSleep);
    aeSetAfterSleepProc(server.el, afterSleep);

    /* Open the AOF file if needed. */
    // 如果server设置了aof模式做持久化，将会打开或创建对应的打开或创建 AOF 文件，保存相关的描述符。
    if (server.aof_state == AOF_ON) {
        server.aof_fd = open(server.aof_filename,
                             O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (server.aof_fd == -1) {
            serverLog(LL_WARNING, "Can't open the append-only file: %s",
                      strerror(errno));
            exit(1);
        }
    }

    /* 32 bit instances are limited to 4GB of address space, so if there is
     * no explicit limit in the user provided configuration we set a limit
     * at 3 GB using maxmemory with 'noeviction' policy'. This avoids
     * useless crashes of the Redis instance for out of memory. */
    // 设置内存限制。32位系统，如果没有显式内存设置，默认设置为3G，并把maxmemory_policy设置为REDIS_MAXMEMORY_NO_EVICTION，在程序达到最大内存限制后，
    // 拒绝后续会增大内存使用的客户端执行的命令。
    if (server.arch_bits == 32 && server.maxmemory == 0) {
        serverLog(LL_WARNING,
                  "Warning: 32 bit instance detected but no memory limit set. Setting 3 GB maxmemory limit with 'noeviction' policy now.");
        server.maxmemory = 3072LL * (1024 * 1024); /* 3 GB */
        server.maxmemory_policy = MAXMEMORY_NO_EVICTION;
    }
    // TODO 如果集群模式已打开，那么初始化集群
    if (server.cluster_enabled) {
        clusterInit();
    }
    // 初始化脚本系统。Redis的脚本系统用的是lua语言。
    replicationScriptCacheInit();
    scriptingInit(1);
    // 初始化slowlog。slowlog是redis提供的进行query分析的工具。它将执行时间长的命令统一以list形式保存在内存之中，使用者可以通过slowlog命令查看这些慢query，从而分析系统瓶颈。
    slowlogInit();
    // 初始化监控
    latencyMonitorInit();
}

/* Some steps in server initialization need to be done last (after modules
 * are loaded).
 * Specifically, creation of threads due to a race bug in ld.so, in which
 * Thread Local Storage initialization collides with dlopen call.
 * see: https://sourceware.org/bugzilla/show_bug.cgi?id=19329 */
void InitServerLast() {
    bioInit();
    initThreadedIO();
    set_jemalloc_bg_thread(server.jemalloc_bg_thread);
    server.initial_memory_usage = zmalloc_used_memory();
}

/* Parse the flags string description 'strflags' and set them to the
 * command 'c'. If the flags are all valid C_OK is returned, otherwise
 * C_ERR is returned (yet the recognized flags are set in the command). */
int populateCommandTableParseFlags(struct redisCommand *c, char *strflags) {
    int argc;
    sds *argv;

    /* Split the line into arguments for processing. */
    argv = sdssplitargs(strflags, &argc);
    if (argv == NULL) return C_ERR;

    for (int j = 0; j < argc; j++) {
        char *flag = argv[j];
        if (!strcasecmp(flag, "write")) {
            c->flags |= CMD_WRITE | CMD_CATEGORY_WRITE;
        } else if (!strcasecmp(flag, "read-only")) {
            c->flags |= CMD_READONLY | CMD_CATEGORY_READ;
        } else if (!strcasecmp(flag, "use-memory")) {
            c->flags |= CMD_DENYOOM;
        } else if (!strcasecmp(flag, "admin")) {
            c->flags |= CMD_ADMIN | CMD_CATEGORY_ADMIN | CMD_CATEGORY_DANGEROUS;
        } else if (!strcasecmp(flag, "pub-sub")) {
            c->flags |= CMD_PUBSUB | CMD_CATEGORY_PUBSUB;
        } else if (!strcasecmp(flag, "no-script")) {
            c->flags |= CMD_NOSCRIPT;
        } else if (!strcasecmp(flag, "random")) {
            c->flags |= CMD_RANDOM;
        } else if (!strcasecmp(flag, "to-sort")) {
            c->flags |= CMD_SORT_FOR_SCRIPT;
        } else if (!strcasecmp(flag, "ok-loading")) {
            c->flags |= CMD_LOADING;
        } else if (!strcasecmp(flag, "ok-stale")) {
            c->flags |= CMD_STALE;
        } else if (!strcasecmp(flag, "no-monitor")) {
            c->flags |= CMD_SKIP_MONITOR;
        } else if (!strcasecmp(flag, "no-slowlog")) {
            c->flags |= CMD_SKIP_SLOWLOG;
        } else if (!strcasecmp(flag, "cluster-asking")) {
            c->flags |= CMD_ASKING;
        } else if (!strcasecmp(flag, "fast")) {
            c->flags |= CMD_FAST | CMD_CATEGORY_FAST;
        } else if (!strcasecmp(flag, "no-auth")) {
            c->flags |= CMD_NO_AUTH;
        } else {
            /* Parse ACL categories here if the flag name starts with @. */
            uint64_t catflag;
            if (flag[0] == '@' &&
                (catflag = ACLGetCommandCategoryFlagByName(flag + 1)) != 0) {
                c->flags |= catflag;
            } else {
                sdsfreesplitres(argv, argc);
                return C_ERR;
            }
        }
    }
    /* If it's not @fast is @slow in this binary world. */
    if (!(c->flags & CMD_CATEGORY_FAST)) c->flags |= CMD_CATEGORY_SLOW;

    sdsfreesplitres(argv, argc);
    return C_OK;
}

/* Populates the Redis Command Table starting from the hard coded list
 * we have on top of redis.c file. */
void populateCommandTable(void) {
    int j;
    int numcommands = sizeof(redisCommandTable) / sizeof(struct redisCommand);

    for (j = 0; j < numcommands; j++) {
        struct redisCommand *c = redisCommandTable + j;
        int retval1, retval2;

        /* Translate the command string flags description into an actual
         * set of flags. */
        if (populateCommandTableParseFlags(c, c->sflags) == C_ERR)
            serverPanic("Unsupported command flag");

        c->id = ACLGetCommandID(c->name); /* Assign the ID used for ACL. */
        retval1 = dictAdd(server.commands, sdsnew(c->name), c);
        /* Populate an additional dictionary that will be unaffected
         * by rename-command statements in redis.conf. */
        retval2 = dictAdd(server.orig_commands, sdsnew(c->name), c);
        serverAssert(retval1 == DICT_OK && retval2 == DICT_OK);
    }
}

void resetCommandTableStats(void) {
    struct redisCommand *c;
    dictEntry *de;
    dictIterator *di;

    di = dictGetSafeIterator(server.commands);
    while ((de = dictNext(di)) != NULL) {
        c = (struct redisCommand *) dictGetVal(de);
        c->microseconds = 0;
        c->calls = 0;
    }
    dictReleaseIterator(di);

}

/* ========================== Redis OP Array API ============================ */

void redisOpArrayInit(redisOpArray *oa) {
    oa->ops = NULL;
    oa->numops = 0;
}

int redisOpArrayAppend(redisOpArray *oa, struct redisCommand *cmd, int dbid,
                       robj **argv, int argc, int target) {
    redisOp *op;

    oa->ops = zrealloc(oa->ops, sizeof(redisOp) * (oa->numops + 1));
    op = oa->ops + oa->numops;
    op->cmd = cmd;
    op->dbid = dbid;
    op->argv = argv;
    op->argc = argc;
    op->target = target;
    oa->numops++;
    return oa->numops;
}

void redisOpArrayFree(redisOpArray *oa) {
    while (oa->numops) {
        int j;
        redisOp *op;

        oa->numops--;
        op = oa->ops + oa->numops;
        for (j = 0; j < op->argc; j++)
            decrRefCount(op->argv[j]);
        zfree(op->argv);
    }
    zfree(oa->ops);
}

/* ====================== Commands lookup and execution ===================== */

struct redisCommand *lookupCommand(sds name) {
    return dictFetchValue(server.commands, name);
}

struct redisCommand *lookupCommandByCString(char *s) {
    struct redisCommand *cmd;
    sds name = sdsnew(s);

    cmd = dictFetchValue(server.commands, name);
    sdsfree(name);
    return cmd;
}

/* Lookup the command in the current table, if not found also check in
 * the original table containing the original command names unaffected by
 * redis.conf rename-command statement.
 *
 * This is used by functions rewriting the argument vector such as
 * rewriteClientCommandVector() in order to set client->cmd pointer
 * correctly even if the command was renamed. */
struct redisCommand *lookupCommandOrOriginal(sds name) {
    struct redisCommand *cmd = dictFetchValue(server.commands, name);

    if (!cmd) cmd = dictFetchValue(server.orig_commands, name);
    return cmd;
}

/* Propagate the specified command (in the context of the specified database id)
 * to AOF and Slaves.
 *
 * flags are an xor between:
 * + PROPAGATE_NONE (no propagation of command at all)
 * + PROPAGATE_AOF (propagate into the AOF file if is enabled)
 * + PROPAGATE_REPL (propagate into the replication link)
 *
 * This should not be used inside commands implementation since it will not
 * wrap the resulting commands in MULTI/EXEC. Use instead alsoPropagate(),
 * preventCommandPropagation(), forceCommandPropagation().
 *
 * However for functions that need to (also) propagate out of the context of a
 * command execution, for example when serving a blocked client, you
 * want to use propagate().
 */
void propagate(struct redisCommand *cmd, int dbid, robj **argv, int argc,
               int flags) {
    if (server.aof_state != AOF_OFF && flags & PROPAGATE_AOF)
        feedAppendOnlyFile(cmd, dbid, argv, argc);
    if (flags & PROPAGATE_REPL)
        replicationFeedSlaves(server.slaves, dbid, argv, argc);
}

/* Used inside commands to schedule the propagation of additional commands
 * after the current command is propagated to AOF / Replication.
 *
 * 'cmd' must be a pointer to the Redis command to replicate, dbid is the
 * database ID the command should be propagated into.
 * Arguments of the command to propagte are passed as an array of redis
 * objects pointers of len 'argc', using the 'argv' vector.
 *
 * The function does not take a reference to the passed 'argv' vector,
 * so it is up to the caller to release the passed argv (but it is usually
 * stack allocated).  The function autoamtically increments ref count of
 * passed objects, so the caller does not need to. */
void alsoPropagate(struct redisCommand *cmd, int dbid, robj **argv, int argc,
                   int target) {
    robj **argvcopy;
    int j;

    if (server.loading) return; /* No propagation during loading. */

    argvcopy = zmalloc(sizeof(robj *) * argc);
    for (j = 0; j < argc; j++) {
        argvcopy[j] = argv[j];
        incrRefCount(argv[j]);
    }
    redisOpArrayAppend(&server.also_propagate, cmd, dbid, argvcopy, argc, target);
}

/* It is possible to call the function forceCommandPropagation() inside a
 * Redis command implementation in order to to force the propagation of a
 * specific command execution into AOF / Replication. */
void forceCommandPropagation(client *c, int flags) {
    if (flags & PROPAGATE_REPL) c->flags |= CLIENT_FORCE_REPL;
    if (flags & PROPAGATE_AOF) c->flags |= CLIENT_FORCE_AOF;
}

/* Avoid that the executed command is propagated at all. This way we
 * are free to just propagate what we want using the alsoPropagate()
 * API. */
void preventCommandPropagation(client *c) {
    c->flags |= CLIENT_PREVENT_PROP;
}

/* AOF specific version of preventCommandPropagation(). */
void preventCommandAOF(client *c) {
    c->flags |= CLIENT_PREVENT_AOF_PROP;
}

/* Replication specific version of preventCommandPropagation(). */
void preventCommandReplication(client *c) {
    c->flags |= CLIENT_PREVENT_REPL_PROP;
}

/* Call() is the core of Redis execution of a command.
 *
 * The following flags can be passed:
 * CMD_CALL_NONE        No flags.
 * CMD_CALL_SLOWLOG     Check command speed and log in the slow log if needed.
 * CMD_CALL_STATS       Populate command stats.
 * CMD_CALL_PROPAGATE_AOF   Append command to AOF if it modified the dataset
 *                          or if the client flags are forcing propagation.
 * CMD_CALL_PROPAGATE_REPL  Send command to slaves if it modified the dataset
 *                          or if the client flags are forcing propagation.
 * CMD_CALL_PROPAGATE   Alias for PROPAGATE_AOF|PROPAGATE_REPL.
 * CMD_CALL_FULL        Alias for SLOWLOG|STATS|PROPAGATE.
 *
 * The exact propagation behavior depends on the client flags.
 * Specifically:
 *
 * 1. If the client flags CLIENT_FORCE_AOF or CLIENT_FORCE_REPL are set
 *    and assuming the corresponding CMD_CALL_PROPAGATE_AOF/REPL is set
 *    in the call flags, then the command is propagated even if the
 *    dataset was not affected by the command.
 * 2. If the client flags CLIENT_PREVENT_REPL_PROP or CLIENT_PREVENT_AOF_PROP
 *    are set, the propagation into AOF or to slaves is not performed even
 *    if the command modified the dataset.
 *
 * Note that regardless of the client flags, if CMD_CALL_PROPAGATE_AOF
 * or CMD_CALL_PROPAGATE_REPL are not set, then respectively AOF or
 * slaves propagation will never occur.
 *
 * Client flags are modified by the implementation of a given command
 * using the following API:
 *
 * forceCommandPropagation(client *c, int flags);
 * preventCommandPropagation(client *c);
 * preventCommandAOF(client *c);
 * preventCommandReplication(client *c);
 *
 */
void call(client *c, int flags) {
    long long dirty;
    ustime_t start, duration;
    int client_old_flags = c->flags;
    struct redisCommand *real_cmd = c->cmd;

    server.fixed_time_expire++;

    /* Send the command to clients in MONITOR mode if applicable.
     * Administrative commands are considered too dangerous to be shown. */
    if (listLength(server.monitors) &&
        !server.loading &&
        !(c->cmd->flags & (CMD_SKIP_MONITOR | CMD_ADMIN))) {
        replicationFeedMonitors(c, server.monitors, c->db->id, c->argv, c->argc);
    }

    /* Initialization: clear the flags that must be set by the command on
     * demand, and initialize the array for additional commands propagation. */
    c->flags &= ~(CLIENT_FORCE_AOF | CLIENT_FORCE_REPL | CLIENT_PREVENT_PROP);
    redisOpArray prev_also_propagate = server.also_propagate;
    redisOpArrayInit(&server.also_propagate);

    /* Call the command.执行命令 */
    dirty = server.dirty;
    updateCachedTime(0);
    //计时
    start = server.ustime;
    //执行命令  redisCommandProc
    c->cmd->proc(c);
    duration = ustime() - start;
    dirty = server.dirty - dirty;
    if (dirty < 0) dirty = 0;

    /* When EVAL is called loading the AOF we don't want commands called
     * from Lua to go into the slowlog or to populate statistics. */
    if (server.loading && c->flags & CLIENT_LUA)
        flags &= ~(CMD_CALL_SLOWLOG | CMD_CALL_STATS);

    /* If the caller is Lua, we want to force the EVAL caller to propagate
     * the script if the command flag or client flag are forcing the
     * propagation. */
    if (c->flags & CLIENT_LUA && server.lua_caller) {
        if (c->flags & CLIENT_FORCE_REPL)
            server.lua_caller->flags |= CLIENT_FORCE_REPL;
        if (c->flags & CLIENT_FORCE_AOF)
            server.lua_caller->flags |= CLIENT_FORCE_AOF;
    }

    /* Log the command into the Slow log if needed, and populate the
     * per-command statistics that we show in INFO commandstats. */
    if (flags & CMD_CALL_SLOWLOG && !(c->cmd->flags & CMD_SKIP_SLOWLOG)) {
        char *latency_event = (c->cmd->flags & CMD_FAST) ?
                              "fast-command" : "command";
        //AOF持久化相关
        latencyAddSampleIfNeeded(latency_event, duration / 1000);
        //记录慢查询日志
        slowlogPushEntryIfNeeded(c, c->argv, c->argc, duration);
    }

    if (flags & CMD_CALL_STATS) {
        /* use the real command that was executed (cmd and lastamc) may be
         * different, in case of MULTI-EXEC or re-written commands such as
         * EXPIRE, GEOADD, etc. */
        real_cmd->microseconds += duration;
        real_cmd->calls++;
    }

    /* Propagate the command into the AOF and replication link */
    if (flags & CMD_CALL_PROPAGATE &&
        (c->flags & CLIENT_PREVENT_PROP) != CLIENT_PREVENT_PROP) {
        int propagate_flags = PROPAGATE_NONE;

        /* Check if the command operated changes in the data set. If so
         * set for replication / AOF propagation. */
        if (dirty) propagate_flags |= (PROPAGATE_AOF | PROPAGATE_REPL);

        /* If the client forced AOF / replication of the command, set
         * the flags regardless of the command effects on the data set. */
        if (c->flags & CLIENT_FORCE_REPL) propagate_flags |= PROPAGATE_REPL;
        if (c->flags & CLIENT_FORCE_AOF) propagate_flags |= PROPAGATE_AOF;

        /* However prevent AOF / replication propagation if the command
         * implementations called preventCommandPropagation() or similar,
         * or if we don't have the call() flags to do so. */
        if (c->flags & CLIENT_PREVENT_REPL_PROP ||
            !(flags & CMD_CALL_PROPAGATE_REPL))
            propagate_flags &= ~PROPAGATE_REPL;
        if (c->flags & CLIENT_PREVENT_AOF_PROP ||
            !(flags & CMD_CALL_PROPAGATE_AOF))
            propagate_flags &= ~PROPAGATE_AOF;

        /* Call propagate() only if at least one of AOF / replication
         * propagation is needed. Note that modules commands handle replication
         * in an explicit way, so we never replicate them automatically. */
        if (propagate_flags != PROPAGATE_NONE && !(c->cmd->flags & CMD_MODULE))
            propagate(c->cmd, c->db->id, c->argv, c->argc, propagate_flags);
    }

    /* Restore the old replication flags, since call() can be executed
     * recursively. */
    c->flags &= ~(CLIENT_FORCE_AOF | CLIENT_FORCE_REPL | CLIENT_PREVENT_PROP);
    c->flags |= client_old_flags &
                (CLIENT_FORCE_AOF | CLIENT_FORCE_REPL | CLIENT_PREVENT_PROP);

    /* Handle the alsoPropagate() API to handle commands that want to propagate
     * multiple separated commands. Note that alsoPropagate() is not affected
     * by CLIENT_PREVENT_PROP flag. */
    if (server.also_propagate.numops) {
        int j;
        redisOp *rop;

        if (flags & CMD_CALL_PROPAGATE) {
            int multi_emitted = 0;
            /* Wrap the commands in server.also_propagate array,
             * but don't wrap it if we are already in MULTI context,
             * in case the nested MULTI/EXEC.
             *
             * And if the array contains only one command, no need to
             * wrap it, since the single command is atomic. */
            if (server.also_propagate.numops > 1 &&
                !(c->cmd->flags & CMD_MODULE) &&
                !(c->flags & CLIENT_MULTI) &&
                !(flags & CMD_CALL_NOWRAP)) {
                execCommandPropagateMulti(c);
                multi_emitted = 1;
            }

            for (j = 0; j < server.also_propagate.numops; j++) {
                rop = &server.also_propagate.ops[j];
                int target = rop->target;
                /* Whatever the command wish is, we honor the call() flags. */
                if (!(flags & CMD_CALL_PROPAGATE_AOF)) target &= ~PROPAGATE_AOF;
                if (!(flags & CMD_CALL_PROPAGATE_REPL)) target &= ~PROPAGATE_REPL;
                if (target)
                    propagate(rop->cmd, rop->dbid, rop->argv, rop->argc, target);
            }

            if (multi_emitted) {
                execCommandPropagateExec(c);
            }
        }
        redisOpArrayFree(&server.also_propagate);
    }
    server.also_propagate = prev_also_propagate;

    /* If the client has keys tracking enabled for client side caching,
     * make sure to remember the keys it fetched via this command. */
    if (c->cmd->flags & CMD_READONLY) {
        client *caller = (c->flags & CLIENT_LUA && server.lua_caller) ?
                         server.lua_caller : c;
        if (caller->flags & CLIENT_TRACKING &&
            !(caller->flags & CLIENT_TRACKING_BCAST)) {
            trackingRememberKeys(caller);
        }
    }

    server.fixed_time_expire--;
    server.stat_numcommands++;
}

/* Used when a command that is ready for execution needs to be rejected, due to
 * varios pre-execution checks. it returns the appropriate error to the client.
 * If there's a transaction is flags it as dirty, and if the command is EXEC,
 * it aborts the transaction.
 * Note: 'reply' is expected to end with \r\n */
void rejectCommand(client *c, robj *reply) {
    flagTransaction(c);
    if (c->cmd && c->cmd->proc == execCommand) {
        execCommandAbort(c, reply->ptr);
    } else {
        /* using addReplyError* rather than addReply so that the error can be logged. */
        addReplyErrorObject(c, reply);
    }
}

void rejectCommandFormat(client *c, const char *fmt, ...) {
    //如果找不到返回错误
    flagTransaction(c);
    va_list ap;
    va_start(ap, fmt);
    sds s = sdscatvprintf(sdsempty(), fmt, ap);
    va_end(ap);
    /* Make sure there are no newlines in the string, otherwise invalid protocol
     * is emitted (The args come from the user, they may contain any character). */
    sdsmapchars(s, "\r\n", "  ", 2);
    if (c->cmd && c->cmd->proc == execCommand) {
        execCommandAbort(c, s);
    } else {
        addReplyErrorSds(c, s);
    }
    sdsfree(s);
}

/* If this function gets called we already read a whole
 * command, arguments are in the client argv/argc fields.
 * processCommand() execute the command or prepare the
 * server for a bulk read from the client.
 *
 * If C_OK is returned the client is still alive and valid and
 * other operations can be performed by the caller. Otherwise
 * if C_ERR is returned the client was destroyed (i.e. after QUIT). */
int processCommand(client *c) {
    moduleCallCommandFilters(c);

    /* The QUIT command is handled separately. Normal command procs will
     * go through checking for replication and QUIT will cause trouble
     * when FORCE_REPLICATION is enabled and would be implemented in
     * a regular command proc.
     * 如果是quit命令直接返回并且关闭客户端*/
    if (!strcasecmp(c->argv[0]->ptr, "quit")) {
        addReply(c, shared.ok);
        c->flags |= CLIENT_CLOSE_AFTER_REPLY;
        return C_ERR;
    }

    /* Now lookup the command and check ASAP about trivial error conditions
     * such as wrong arity, bad command name and so forth.
     * 如果命令不存在或者参数错误返回错误 */
    c->cmd = c->lastcmd = lookupCommand(c->argv[0]->ptr);
    if (!c->cmd) {
        sds args = sdsempty();
        int i;
        for (i = 1; i < c->argc && sdslen(args) < 128; i++)
            args = sdscatprintf(args, "`%.*s`, ", 128 - (int) sdslen(args), (char *) c->argv[i]->ptr);
        // 如果找不到返回错误
        rejectCommandFormat(c, "unknown command `%s`, with args beginning with: %s",
                            (char *) c->argv[0]->ptr, args);
        sdsfree(args);
        return C_OK;
    } else if ((c->cmd->arity > 0 && c->cmd->arity != c->argc) ||
               (c->argc < -c->cmd->arity)) {
        // 如果找不到返回错误
        rejectCommandFormat(c, "wrong number of arguments for '%s' command",
                            c->cmd->name);
        return C_OK;
    }

    int is_write_command = (c->cmd->flags & CMD_WRITE) ||
                           (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_WRITE));
    int is_denyoom_command = (c->cmd->flags & CMD_DENYOOM) ||
                             (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_DENYOOM));
    int is_denystale_command = !(c->cmd->flags & CMD_STALE) ||
                               (c->cmd->proc == execCommand && (c->mstate.cmd_inv_flags & CMD_STALE));
    int is_denyloading_command = !(c->cmd->flags & CMD_LOADING) ||
                                 (c->cmd->proc == execCommand && (c->mstate.cmd_inv_flags & CMD_LOADING));

    /* Check if the user is authenticated. This check is skipped in case
     * the default user is flagged as "nopass" and is active.检查用户是否经过身份验证。如果默认用户被标记为“nopass”并处于活动状态，则跳过此检查。 */
    int auth_required = (!(DefaultUser->flags & USER_FLAG_NOPASS) ||
                         (DefaultUser->flags & USER_FLAG_DISABLED)) &&
                        !c->authenticated;
    //需要认证的情况下只能使用 auth  和 hello
    if (auth_required) {
        /* AUTH and HELLO and no auth modules are valid even in
         * non-authenticated state. */
        if (!(c->cmd->flags & CMD_NO_AUTH)) {
            rejectCommand(c, shared.noautherr);
            return C_OK;
        }
    }

    /* Check if the user can run this command according to the current
     * ACLs. */
    int acl_keypos;
    int acl_retval = ACLCheckCommandPerm(c, &acl_keypos);
    if (acl_retval != ACL_OK) {
        addACLLogEntry(c, acl_retval, acl_keypos, NULL);
        if (acl_retval == ACL_DENIED_CMD)
            rejectCommandFormat(c,
                                "-NOPERM this user has no permissions to run "
                                "the '%s' command or its subcommand", c->cmd->name);
        else
            rejectCommandFormat(c,
                                "-NOPERM this user has no permissions to access "
                                "one of the keys used as arguments");
        return C_OK;
    }

    /* If cluster is enabled perform the cluster redirection here.
     * However we don't perform the redirection if:
     * 1) The sender of this command is our master.
     * 2) The command has no key arguments.
     *如果启用了集群，在这里执行集群重定向。然而，再下面两个条件下我们不执行重定向:
     *  1)发出这条命令的人是master。
     *  2)命令没有关键参数。
     *  TODO cluster 模式下重定向
     **/
    if (server.cluster_enabled &&
        !(c->flags & CLIENT_MASTER) &&
        !(c->flags & CLIENT_LUA &&
          server.lua_caller->flags & CLIENT_MASTER) &&
        !(c->cmd->getkeys_proc == NULL && c->cmd->firstkey == 0 &&
          c->cmd->proc != execCommand)) {
        int hashslot;
        int error_code;
        clusterNode *n = getNodeByQuery(c, c->cmd, c->argv, c->argc,
                                        &hashslot, &error_code);
        if (n == NULL || n != server.cluster->myself) {
            if (c->cmd->proc == execCommand) {
                discardTransaction(c);
            } else {
                flagTransaction(c);
            }
            clusterRedirectClient(c, n, hashslot, error_code);
            return C_OK;
        }
    }

    /* Handle the maxmemory directive.
     *
     * Note that we do not want to reclaim memory if we are here re-entering
     * the event loop since there is a busy Lua script running in timeout
     * condition, to avoid mixing the propagation of scripts with the
     * propagation of DELs due to eviction. */
    // 拒绝执行带有m标识的命令，到内存到达上限的时候的内存保护机制
    if (server.maxmemory && !server.lua_timedout) {
        //先调用freeMemoryIfNeededAndSafe进行一次内存释放
        int out_of_memory = freeMemoryIfNeededAndSafe() == C_ERR;
        /* freeMemoryIfNeeded may flush slave output buffers. This may result
         * into a slave, that may be the active client, to be freed. */
        //释放内存可能会清空主从同步slave的缓冲区，这可能会导致释放一个活跃的slave客户端
        if (server.current_client == NULL) return C_ERR;

        int reject_cmd_on_oom = is_denyoom_command;
        /* If client is in MULTI/EXEC context, queuing may consume an unlimited
         * amount of memory, so we want to stop that.
         * However, we never want to reject DISCARD, or even EXEC (unless it
         * contains denied commands, in which case is_denyoom_command is already
         * set. */
        //当内存释放也不能解决内存问题的时候，客户端试图执行命令在OOM的情况下被拒绝
        // 或者客户端处于MULTI/EXEC的上下文中
        if (c->flags & CLIENT_MULTI &&
            c->cmd->proc != execCommand &&
            c->cmd->proc != discardCommand) {
            reject_cmd_on_oom = 1;
        }

        if (out_of_memory && reject_cmd_on_oom) {
            //回复的内容OOM
            rejectCommand(c, shared.oomerr);
            return C_OK;
        }

        /* Save out_of_memory result at script start, otherwise if we check OOM
         * untill first write within script, memory used by lua stack and
         * arguments might interfere. */
        if (c->cmd->proc == evalCommand || c->cmd->proc == evalShaCommand) {
            server.lua_oom = out_of_memory;
        }
    }

    /* Make sure to use a reasonable amount of memory for client side
     * caching metadata. 请确保为客户端缓存元数据使用合理的内存*/
    if (server.tracking_clients) trackingLimitUsedSlots();

    /* Don't accept write commands if there are problems persisting on disk
     * and if this is a master instance. 持久化校验*/
    int deny_write_type = writeCommandsDeniedByDiskError();
    if (deny_write_type != DISK_ERROR_TYPE_NONE &&
        server.masterhost == NULL &&
        (is_write_command || c->cmd->proc == pingCommand)) {
        if (deny_write_type == DISK_ERROR_TYPE_RDB)
            rejectCommand(c, shared.bgsaveerr);
        else
            rejectCommandFormat(c,
                                "-MISCONF Errors writing to the AOF file: %s",
                                strerror(server.aof_last_write_errno));
        return C_OK;
    }

    /* Don't accept write commands if there are not enough good slaves and
     * user configured the min-slaves-to-write option.主从复制校验 */
    if (server.masterhost == NULL &&
        server.repl_min_slaves_to_write &&
        server.repl_min_slaves_max_lag &&
        is_write_command &&
        server.repl_good_slaves_count < server.repl_min_slaves_to_write) {
        rejectCommand(c, shared.noreplicaserr);
        return C_OK;
    }

    /* Don't accept write commands if this is a read only slave. But
     * accept write commands if this is our master. */
    if (server.masterhost && server.repl_slave_ro &&
        !(c->flags & CLIENT_MASTER) &&
        is_write_command) {
        rejectCommand(c, shared.roslaveerr);
        return C_OK;
    }

    /* Only allow a subset of commands in the context of Pub/Sub if the
     * connection is in RESP2 mode. With RESP3 there are no limits. */
    if ((c->flags & CLIENT_PUBSUB && c->resp == 2) &&
        c->cmd->proc != pingCommand &&
        c->cmd->proc != subscribeCommand &&
        c->cmd->proc != unsubscribeCommand &&
        c->cmd->proc != psubscribeCommand &&
        c->cmd->proc != punsubscribeCommand) {
        rejectCommandFormat(c,
                            "Can't execute '%s': only (P)SUBSCRIBE / "
                            "(P)UNSUBSCRIBE / PING / QUIT are allowed in this context",
                            c->cmd->name);
        return C_OK;
    }

    /* Only allow commands with flag "t", such as INFO, SLAVEOF and so on,
     * when slave-serve-stale-data is no and we are a slave with a broken
     * link with master. */
    if (server.masterhost && server.repl_state != REPL_STATE_CONNECTED &&
        server.repl_serve_stale_data == 0 &&
        is_denystale_command) {
        rejectCommand(c, shared.masterdownerr);
        return C_OK;
    }

    /* Loading DB? Return an error if the command has not the
     * CMD_LOADING flag. */
    if (server.loading && is_denyloading_command) {
        rejectCommand(c, shared.loadingerr);
        return C_OK;
    }

    /* Lua script too slow? Only allow a limited number of commands.
     * Note that we need to allow the transactions commands, otherwise clients
     * sending a transaction with pipelining without error checking, may have
     * the MULTI plus a few initial commands refused, then the timeout
     * condition resolves, and the bottom-half of the transaction gets
     * executed, see Github PR #7022. */
    if (server.lua_timedout &&
        c->cmd->proc != authCommand &&
        c->cmd->proc != helloCommand &&
        c->cmd->proc != replconfCommand &&
        c->cmd->proc != multiCommand &&
        c->cmd->proc != discardCommand &&
        c->cmd->proc != watchCommand &&
        c->cmd->proc != unwatchCommand &&
        !(c->cmd->proc == shutdownCommand &&
          c->argc == 2 &&
          tolower(((char *) c->argv[1]->ptr)[0]) == 'n') &&
        !(c->cmd->proc == scriptCommand &&
          c->argc == 2 &&
          tolower(((char *) c->argv[1]->ptr)[0]) == 'k')) {
        rejectCommand(c, shared.slowscripterr);
        return C_OK;
    }

    /* Exec the command 执行命令
     * 执行命令，前面已经把找到的命令放到了client 的cmd里面了
     * 如果当前开启事务，命令会被添加到commands队列中去
     * 这里也发现 exec multi watch discard的命令是不用进入队列的，因为需要直接执行
     * */
    if (c->flags & CLIENT_MULTI &&
        c->cmd->proc != execCommand && c->cmd->proc != discardCommand &&
        c->cmd->proc != multiCommand && c->cmd->proc != watchCommand) {
        //将命令添加到待执行队列中,证明Redis会使用事务的方式执行指令
        queueMultiCommand(c);
        addReply(c, shared.queued);
    } else {
        // 不进入队列的直接执行
        // 最终执行命令是在call中调用的，在call中会执行命令，并且计时，如果指令执行时间过长，会作为慢查询记录到日志中去。执行完成后如果有必要还需要更新统计信息，
        // 记录慢查询日志，AOF持久化该命令请求，传播命令请求给所有的从服务器等。
        call(c, CMD_CALL_FULL);
        c->woff = server.master_repl_offset;
        if (listLength(server.ready_keys))
            handleClientsBlockedOnKeys();
    }
    return C_OK;
}

/*================================== Shutdown =============================== */

/* Close listening sockets. Also unlink the unix domain socket if
 * unlink_unix_socket is non-zero. */
void closeListeningSockets(int unlink_unix_socket) {
    int j;

    for (j = 0; j < server.ipfd_count; j++) close(server.ipfd[j]);
    for (j = 0; j < server.tlsfd_count; j++) close(server.tlsfd[j]);
    if (server.sofd != -1) close(server.sofd);
    if (server.cluster_enabled)
        for (j = 0; j < server.cfd_count; j++) close(server.cfd[j]);
    if (unlink_unix_socket && server.unixsocket) {
        serverLog(LL_NOTICE, "Removing the unix socket file.");
        unlink(server.unixsocket); /* don't care if this fails */
    }
}

int prepareForShutdown(int flags) {
    /* When SHUTDOWN is called while the server is loading a dataset in
     * memory we need to make sure no attempt is performed to save
     * the dataset on shutdown (otherwise it could overwrite the current DB
     * with half-read data).
     *
     * Also when in Sentinel mode clear the SAVE flag and force NOSAVE. */
    if (server.loading || server.sentinel_mode)
        flags = (flags & ~SHUTDOWN_SAVE) | SHUTDOWN_NOSAVE;

    int save = flags & SHUTDOWN_SAVE;
    int nosave = flags & SHUTDOWN_NOSAVE;

    serverLog(LL_WARNING, "User requested shutdown...");
    if (server.supervised_mode == SUPERVISED_SYSTEMD)
        redisCommunicateSystemd("STOPPING=1\n");

    /* Kill all the Lua debugger forked sessions. */
    ldbKillForkedSessions();

    /* Kill the saving child if there is a background saving in progress.
       We want to avoid race conditions, for instance our saving child may
       overwrite the synchronous saving did by SHUTDOWN. */
    if (server.rdb_child_pid != -1) {
        serverLog(LL_WARNING, "There is a child saving an .rdb. Killing it!");
        killRDBChild();
    }

    /* Kill module child if there is one. */
    if (server.module_child_pid != -1) {
        serverLog(LL_WARNING, "There is a module fork child. Killing it!");
        TerminateModuleForkChild(server.module_child_pid, 0);
    }

    if (server.aof_state != AOF_OFF) {
        /* Kill the AOF saving child as the AOF we already have may be longer
         * but contains the full dataset anyway. */
        if (server.aof_child_pid != -1) {
            /* If we have AOF enabled but haven't written the AOF yet, don't
             * shutdown or else the dataset will be lost. */
            if (server.aof_state == AOF_WAIT_REWRITE) {
                serverLog(LL_WARNING, "Writing initial AOF, can't exit.");
                return C_ERR;
            }
            serverLog(LL_WARNING,
                      "There is a child rewriting the AOF. Killing it!");
            killAppendOnlyChild();
        }
        /* Append only file: flush buffers and fsync() the AOF at exit */
        serverLog(LL_NOTICE, "Calling fsync() on the AOF file.");
        flushAppendOnlyFile(1);
        redis_fsync(server.aof_fd);
    }

    /* Create a new RDB file before exiting. */
    if ((server.saveparamslen > 0 && !nosave) || save) {
        serverLog(LL_NOTICE, "Saving the final RDB snapshot before exiting.");
        if (server.supervised_mode == SUPERVISED_SYSTEMD)
            redisCommunicateSystemd("STATUS=Saving the final RDB snapshot\n");
        /* Snapshotting. Perform a SYNC SAVE and exit */
        rdbSaveInfo rsi, *rsiptr;
        rsiptr = rdbPopulateSaveInfo(&rsi);
        if (rdbSave(server.rdb_filename, rsiptr) != C_OK) {
            /* Ooops.. error saving! The best we can do is to continue
             * operating. Note that if there was a background saving process,
             * in the next cron() Redis will be notified that the background
             * saving aborted, handling special stuff like slaves pending for
             * synchronization... */
            serverLog(LL_WARNING, "Error trying to save the DB, can't exit.");
            if (server.supervised_mode == SUPERVISED_SYSTEMD)
                redisCommunicateSystemd("STATUS=Error trying to save the DB, can't exit.\n");
            return C_ERR;
        }
    }

    /* Fire the shutdown modules event. */
    moduleFireServerEvent(REDISMODULE_EVENT_SHUTDOWN, 0, NULL);

    /* Remove the pid file if possible and needed. */
    if (server.daemonize || server.pidfile) {
        serverLog(LL_NOTICE, "Removing the pid file.");
        unlink(server.pidfile);
    }

    /* Best effort flush of slave output buffers, so that we hopefully
     * send them pending writes. */
    flushSlavesOutputBuffers();

    /* Close the listening sockets. Apparently this allows faster restarts. */
    closeListeningSockets(1);
    serverLog(LL_WARNING, "%s is now ready to exit, bye bye...",
              server.sentinel_mode ? "Sentinel" : "Redis");
    return C_OK;
}

/*================================== Commands =============================== */

/* Sometimes Redis cannot accept write commands because there is a perstence
 * error with the RDB or AOF file, and Redis is configured in order to stop
 * accepting writes in such situation. This function returns if such a
 * condition is active, and the type of the condition.
 *
 * Function return values:
 *
 * DISK_ERROR_TYPE_NONE:    No problems, we can accept writes.
 * DISK_ERROR_TYPE_AOF:     Don't accept writes: AOF errors.
 * DISK_ERROR_TYPE_RDB:     Don't accept writes: RDB errors.
 */
int writeCommandsDeniedByDiskError(void) {
    if (server.stop_writes_on_bgsave_err &&
        server.saveparamslen > 0 &&
        server.lastbgsave_status == C_ERR) {
        return DISK_ERROR_TYPE_RDB;
    } else if (server.aof_state != AOF_OFF &&
               server.aof_last_write_status == C_ERR) {
        return DISK_ERROR_TYPE_AOF;
    } else {
        return DISK_ERROR_TYPE_NONE;
    }
}

/* The PING command. It works in a different way if the client is in
 * in Pub/Sub mode. */
void pingCommand(client *c) {
    /* The command takes zero or one arguments. */
    if (c->argc > 2) {
        addReplyErrorFormat(c, "wrong number of arguments for '%s' command",
                            c->cmd->name);
        return;
    }

    if (c->flags & CLIENT_PUBSUB && c->resp == 2) {
        addReply(c, shared.mbulkhdr[2]);
        addReplyBulkCBuffer(c, "pong", 4);
        if (c->argc == 1)
            addReplyBulkCBuffer(c, "", 0);
        else
            addReplyBulk(c, c->argv[1]);
    } else {
        if (c->argc == 1)
            addReply(c, shared.pong);
        else
            addReplyBulk(c, c->argv[1]);
    }
}

void echoCommand(client *c) {
    addReplyBulk(c, c->argv[1]);
}

void timeCommand(client *c) {
    struct timeval tv;

    /* gettimeofday() can only fail if &tv is a bad address so we
     * don't check for errors. */
    gettimeofday(&tv, NULL);
    addReplyArrayLen(c, 2);
    addReplyBulkLongLong(c, tv.tv_sec);
    addReplyBulkLongLong(c, tv.tv_usec);
}

/* Helper function for addReplyCommand() to output flags. */
int addReplyCommandFlag(client *c, struct redisCommand *cmd, int f, char *reply) {
    if (cmd->flags & f) {
        addReplyStatus(c, reply);
        return 1;
    }
    return 0;
}

/* Output the representation of a Redis command. Used by the COMMAND command. */
void addReplyCommand(client *c, struct redisCommand *cmd) {
    if (!cmd) {
        addReplyNull(c);
    } else {
        /* We are adding: command name, arg count, flags, first, last, offset, categories */
        addReplyArrayLen(c, 7);
        addReplyBulkCString(c, cmd->name);
        addReplyLongLong(c, cmd->arity);

        int flagcount = 0;
        void *flaglen = addReplyDeferredLen(c);
        flagcount += addReplyCommandFlag(c, cmd, CMD_WRITE, "write");
        flagcount += addReplyCommandFlag(c, cmd, CMD_READONLY, "readonly");
        flagcount += addReplyCommandFlag(c, cmd, CMD_DENYOOM, "denyoom");
        flagcount += addReplyCommandFlag(c, cmd, CMD_ADMIN, "admin");
        flagcount += addReplyCommandFlag(c, cmd, CMD_PUBSUB, "pubsub");
        flagcount += addReplyCommandFlag(c, cmd, CMD_NOSCRIPT, "noscript");
        flagcount += addReplyCommandFlag(c, cmd, CMD_RANDOM, "random");
        flagcount += addReplyCommandFlag(c, cmd, CMD_SORT_FOR_SCRIPT, "sort_for_script");
        flagcount += addReplyCommandFlag(c, cmd, CMD_LOADING, "loading");
        flagcount += addReplyCommandFlag(c, cmd, CMD_STALE, "stale");
        flagcount += addReplyCommandFlag(c, cmd, CMD_SKIP_MONITOR, "skip_monitor");
        flagcount += addReplyCommandFlag(c, cmd, CMD_SKIP_SLOWLOG, "skip_slowlog");
        flagcount += addReplyCommandFlag(c, cmd, CMD_ASKING, "asking");
        flagcount += addReplyCommandFlag(c, cmd, CMD_FAST, "fast");
        flagcount += addReplyCommandFlag(c, cmd, CMD_NO_AUTH, "no_auth");
        if ((cmd->getkeys_proc && !(cmd->flags & CMD_MODULE)) ||
            cmd->flags & CMD_MODULE_GETKEYS) {
            addReplyStatus(c, "movablekeys");
            flagcount += 1;
        }
        setDeferredSetLen(c, flaglen, flagcount);

        addReplyLongLong(c, cmd->firstkey);
        addReplyLongLong(c, cmd->lastkey);
        addReplyLongLong(c, cmd->keystep);

        addReplyCommandCategories(c, cmd);
    }
}

/* COMMAND <subcommand> <args> */
void commandCommand(client *c) {
    dictIterator *di;
    dictEntry *de;

    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr, "help")) {
        const char *help[] = {
                "(no subcommand) -- Return details about all Redis commands.",
                "COUNT -- Return the total number of commands in this Redis server.",
                "GETKEYS <full-command> -- Return the keys from a full Redis command.",
                "INFO [command-name ...] -- Return details about multiple Redis commands.",
                NULL
        };
        addReplyHelp(c, help);
    } else if (c->argc == 1) {
        addReplyArrayLen(c, dictSize(server.commands));
        di = dictGetIterator(server.commands);
        while ((de = dictNext(di)) != NULL) {
            addReplyCommand(c, dictGetVal(de));
        }
        dictReleaseIterator(di);
    } else if (!strcasecmp(c->argv[1]->ptr, "info")) {
        int i;
        addReplyArrayLen(c, c->argc - 2);
        for (i = 2; i < c->argc; i++) {
            addReplyCommand(c, dictFetchValue(server.commands, c->argv[i]->ptr));
        }
    } else if (!strcasecmp(c->argv[1]->ptr, "count") && c->argc == 2) {
        addReplyLongLong(c, dictSize(server.commands));
    } else if (!strcasecmp(c->argv[1]->ptr, "getkeys") && c->argc >= 3) {
        struct redisCommand *cmd = lookupCommand(c->argv[2]->ptr);
        int *keys, numkeys, j;

        if (!cmd) {
            addReplyError(c, "Invalid command specified");
            return;
        } else if (cmd->getkeys_proc == NULL && cmd->firstkey == 0) {
            addReplyError(c, "The command has no key arguments");
            return;
        } else if ((cmd->arity > 0 && cmd->arity != c->argc - 2) ||
                   ((c->argc - 2) < -cmd->arity)) {
            addReplyError(c, "Invalid number of arguments specified for command");
            return;
        }

        keys = getKeysFromCommand(cmd, c->argv + 2, c->argc - 2, &numkeys);
        if (!keys) {
            addReplyError(c, "Invalid arguments specified for command");
        } else {
            addReplyArrayLen(c, numkeys);
            for (j = 0; j < numkeys; j++) addReplyBulk(c, c->argv[keys[j] + 2]);
            getKeysFreeResult(keys);
        }
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

/* Convert an amount of bytes into a human readable string in the form
 * of 100B, 2G, 100M, 4K, and so forth. */
void bytesToHuman(char *s, unsigned long long n) {
    double d;

    if (n < 1024) {
        /* Bytes */
        sprintf(s, "%lluB", n);
    } else if (n < (1024 * 1024)) {
        d = (double) n / (1024);
        sprintf(s, "%.2fK", d);
    } else if (n < (1024LL * 1024 * 1024)) {
        d = (double) n / (1024 * 1024);
        sprintf(s, "%.2fM", d);
    } else if (n < (1024LL * 1024 * 1024 * 1024)) {
        d = (double) n / (1024LL * 1024 * 1024);
        sprintf(s, "%.2fG", d);
    } else if (n < (1024LL * 1024 * 1024 * 1024 * 1024)) {
        d = (double) n / (1024LL * 1024 * 1024 * 1024);
        sprintf(s, "%.2fT", d);
    } else if (n < (1024LL * 1024 * 1024 * 1024 * 1024 * 1024)) {
        d = (double) n / (1024LL * 1024 * 1024 * 1024 * 1024);
        sprintf(s, "%.2fP", d);
    } else {
        /* Let's hope we never need this */
        sprintf(s, "%lluB", n);
    }
}

/* Create the string returned by the INFO command. This is decoupled
 * by the INFO command itself as we need to report the same information
 * on memory corruption problems. */
sds genRedisInfoString(const char *section) {
    sds info = sdsempty();
    time_t uptime = server.unixtime - server.stat_starttime;
    int j;
    struct rusage self_ru, c_ru;
    int allsections = 0, defsections = 0, everything = 0, modules = 0;
    int sections = 0;

    if (section == NULL) section = "default";
    allsections = strcasecmp(section, "all") == 0;
    defsections = strcasecmp(section, "default") == 0;
    everything = strcasecmp(section, "everything") == 0;
    modules = strcasecmp(section, "modules") == 0;
    if (everything) allsections = 1;

    getrusage(RUSAGE_SELF, &self_ru);
    getrusage(RUSAGE_CHILDREN, &c_ru);

    /* Server */
    if (allsections || defsections || !strcasecmp(section, "server")) {
        static int call_uname = 1;
        static struct utsname name;
        char *mode;

        if (server.cluster_enabled) mode = "cluster";
        else if (server.sentinel_mode) mode = "sentinel";
        else mode = "standalone";

        if (sections++) info = sdscat(info, "\r\n");

        if (call_uname) {
            /* Uname can be slow and is always the same output. Cache it. */
            uname(&name);
            call_uname = 0;
        }

        info = sdscatfmt(info,
                         "# Server\r\n"
                         "redis_version:%s\r\n"
                         "redis_git_sha1:%s\r\n"
                         "redis_git_dirty:%i\r\n"
                         "redis_build_id:%s\r\n"
                         "redis_mode:%s\r\n"
                         "os:%s %s %s\r\n"
                         "arch_bits:%i\r\n"
                         "multiplexing_api:%s\r\n"
                         "atomicvar_api:%s\r\n"
                         "gcc_version:%i.%i.%i\r\n"
                         "process_id:%I\r\n"
                         "run_id:%s\r\n"
                         "tcp_port:%i\r\n"
                         "uptime_in_seconds:%I\r\n"
                         "uptime_in_days:%I\r\n"
                         "hz:%i\r\n"
                         "configured_hz:%i\r\n"
                         "lru_clock:%u\r\n"
                         "executable:%s\r\n"
                         "config_file:%s\r\n"
                         "io_threads_active:%i\r\n",
                         REDIS_VERSION,
                         redisGitSHA1(),
                         strtol(redisGitDirty(), NULL, 10) > 0,
                         redisBuildIdString(),
                         mode,
                         name.sysname, name.release, name.machine,
                         server.arch_bits,
                         aeGetApiName(),
                         REDIS_ATOMIC_API,
#ifdef __GNUC__
                         __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__,
#else
                0,0,0,
#endif
                         (int64_t) getpid(),
                         server.runid,
                         server.port ? server.port : server.tls_port,
                         (int64_t) uptime,
                         (int64_t) (uptime / (3600 * 24)),
                         server.hz,
                         server.config_hz,
                         server.lruclock,
                         server.executable ? server.executable : "",
                         server.configfile ? server.configfile : "",
                         server.io_threads_active);
    }

    /* Clients */
    if (allsections || defsections || !strcasecmp(section, "clients")) {
        size_t maxin, maxout;
        getExpansiveClientsInfo(&maxin, &maxout);
        if (sections++) info = sdscat(info, "\r\n");
        info = sdscatprintf(info,
                            "# Clients\r\n"
                            "connected_clients:%lu\r\n"
                            "client_recent_max_input_buffer:%zu\r\n"
                            "client_recent_max_output_buffer:%zu\r\n"
                            "blocked_clients:%d\r\n"
                            "tracking_clients:%d\r\n"
                            "clients_in_timeout_table:%llu\r\n",
                            listLength(server.clients) - listLength(server.slaves),
                            maxin, maxout,
                            server.blocked_clients,
                            server.tracking_clients,
                            (unsigned long long) raxSize(server.clients_timeout_table));
    }

    /* Memory */
    if (allsections || defsections || !strcasecmp(section, "memory")) {
        char hmem[64];
        char peak_hmem[64];
        char total_system_hmem[64];
        char used_memory_lua_hmem[64];
        char used_memory_scripts_hmem[64];
        char used_memory_rss_hmem[64];
        char maxmemory_hmem[64];
        size_t zmalloc_used = zmalloc_used_memory();
        size_t total_system_mem = server.system_memory_size;
        const char *evict_policy = evictPolicyToString();
        long long memory_lua = server.lua ? (long long) lua_gc(server.lua, LUA_GCCOUNT, 0) * 1024 : 0;
        struct redisMemOverhead *mh = getMemoryOverheadData();

        /* Peak memory is updated from time to time by serverCron() so it
         * may happen that the instantaneous value is slightly bigger than
         * the peak value. This may confuse users, so we update the peak
         * if found smaller than the current memory usage. */
        if (zmalloc_used > server.stat_peak_memory)
            server.stat_peak_memory = zmalloc_used;

        bytesToHuman(hmem, zmalloc_used);
        bytesToHuman(peak_hmem, server.stat_peak_memory);
        bytesToHuman(total_system_hmem, total_system_mem);
        bytesToHuman(used_memory_lua_hmem, memory_lua);
        bytesToHuman(used_memory_scripts_hmem, mh->lua_caches);
        bytesToHuman(used_memory_rss_hmem, server.cron_malloc_stats.process_rss);
        bytesToHuman(maxmemory_hmem, server.maxmemory);

        if (sections++) info = sdscat(info, "\r\n");
        info = sdscatprintf(info,
                            "# Memory\r\n"
                            "used_memory:%zu\r\n"
                            "used_memory_human:%s\r\n"
                            "used_memory_rss:%zu\r\n"
                            "used_memory_rss_human:%s\r\n"
                            "used_memory_peak:%zu\r\n"
                            "used_memory_peak_human:%s\r\n"
                            "used_memory_peak_perc:%.2f%%\r\n"
                            "used_memory_overhead:%zu\r\n"
                            "used_memory_startup:%zu\r\n"
                            "used_memory_dataset:%zu\r\n"
                            "used_memory_dataset_perc:%.2f%%\r\n"
                            "allocator_allocated:%zu\r\n"
                            "allocator_active:%zu\r\n"
                            "allocator_resident:%zu\r\n"
                            "total_system_memory:%lu\r\n"
                            "total_system_memory_human:%s\r\n"
                            "used_memory_lua:%lld\r\n"
                            "used_memory_lua_human:%s\r\n"
                            "used_memory_scripts:%lld\r\n"
                            "used_memory_scripts_human:%s\r\n"
                            "number_of_cached_scripts:%lu\r\n"
                            "maxmemory:%lld\r\n"
                            "maxmemory_human:%s\r\n"
                            "maxmemory_policy:%s\r\n"
                            "allocator_frag_ratio:%.2f\r\n"
                            "allocator_frag_bytes:%zu\r\n"
                            "allocator_rss_ratio:%.2f\r\n"
                            "allocator_rss_bytes:%zd\r\n"
                            "rss_overhead_ratio:%.2f\r\n"
                            "rss_overhead_bytes:%zd\r\n"
                            "mem_fragmentation_ratio:%.2f\r\n"
                            "mem_fragmentation_bytes:%zd\r\n"
                            "mem_not_counted_for_evict:%zu\r\n"
                            "mem_replication_backlog:%zu\r\n"
                            "mem_clients_slaves:%zu\r\n"
                            "mem_clients_normal:%zu\r\n"
                            "mem_aof_buffer:%zu\r\n"
                            "mem_allocator:%s\r\n"
                            "active_defrag_running:%d\r\n"
                            "lazyfree_pending_objects:%zu\r\n",
                            zmalloc_used,
                            hmem,
                            server.cron_malloc_stats.process_rss,
                            used_memory_rss_hmem,
                            server.stat_peak_memory,
                            peak_hmem,
                            mh->peak_perc,
                            mh->overhead_total,
                            mh->startup_allocated,
                            mh->dataset,
                            mh->dataset_perc,
                            server.cron_malloc_stats.allocator_allocated,
                            server.cron_malloc_stats.allocator_active,
                            server.cron_malloc_stats.allocator_resident,
                            (unsigned long) total_system_mem,
                            total_system_hmem,
                            memory_lua,
                            used_memory_lua_hmem,
                            (long long) mh->lua_caches,
                            used_memory_scripts_hmem,
                            dictSize(server.lua_scripts),
                            server.maxmemory,
                            maxmemory_hmem,
                            evict_policy,
                            mh->allocator_frag,
                            mh->allocator_frag_bytes,
                            mh->allocator_rss,
                            mh->allocator_rss_bytes,
                            mh->rss_extra,
                            mh->rss_extra_bytes,
                            mh->total_frag,       /* This is the total RSS overhead, including
                                     fragmentation, but not just it. This field
                                     (and the next one) is named like that just
                                     for backward compatibility. */
                            mh->total_frag_bytes,
                            freeMemoryGetNotCountedMemory(),
                            mh->repl_backlog,
                            mh->clients_slaves,
                            mh->clients_normal,
                            mh->aof_buffer,
                            ZMALLOC_LIB,
                            server.active_defrag_running,
                            lazyfreeGetPendingObjectsCount()
        );
        freeMemoryOverheadData(mh);
    }

    /* Persistence */
    if (allsections || defsections || !strcasecmp(section, "persistence")) {
        if (sections++) info = sdscat(info, "\r\n");
        info = sdscatprintf(info,
                            "# Persistence\r\n"
                            "loading:%d\r\n"
                            "rdb_changes_since_last_save:%lld\r\n"
                            "rdb_bgsave_in_progress:%d\r\n"
                            "rdb_last_save_time:%jd\r\n"
                            "rdb_last_bgsave_status:%s\r\n"
                            "rdb_last_bgsave_time_sec:%jd\r\n"
                            "rdb_current_bgsave_time_sec:%jd\r\n"
                            "rdb_last_cow_size:%zu\r\n"
                            "aof_enabled:%d\r\n"
                            "aof_rewrite_in_progress:%d\r\n"
                            "aof_rewrite_scheduled:%d\r\n"
                            "aof_last_rewrite_time_sec:%jd\r\n"
                            "aof_current_rewrite_time_sec:%jd\r\n"
                            "aof_last_bgrewrite_status:%s\r\n"
                            "aof_last_write_status:%s\r\n"
                            "aof_last_cow_size:%zu\r\n"
                            "module_fork_in_progress:%d\r\n"
                            "module_fork_last_cow_size:%zu\r\n",
                            server.loading,
                            server.dirty,
                            server.rdb_child_pid != -1,
                            (intmax_t) server.lastsave,
                            (server.lastbgsave_status == C_OK) ? "ok" : "err",
                            (intmax_t) server.rdb_save_time_last,
                            (intmax_t) ((server.rdb_child_pid == -1) ?
                                        -1 : time(NULL) - server.rdb_save_time_start),
                            server.stat_rdb_cow_bytes,
                            server.aof_state != AOF_OFF,
                            server.aof_child_pid != -1,
                            server.aof_rewrite_scheduled,
                            (intmax_t) server.aof_rewrite_time_last,
                            (intmax_t) ((server.aof_child_pid == -1) ?
                                        -1 : time(NULL) - server.aof_rewrite_time_start),
                            (server.aof_lastbgrewrite_status == C_OK) ? "ok" : "err",
                            (server.aof_last_write_status == C_OK) ? "ok" : "err",
                            server.stat_aof_cow_bytes,
                            server.module_child_pid != -1,
                            server.stat_module_cow_bytes);

        if (server.aof_enabled) {
            info = sdscatprintf(info,
                                "aof_current_size:%lld\r\n"
                                "aof_base_size:%lld\r\n"
                                "aof_pending_rewrite:%d\r\n"
                                "aof_buffer_length:%zu\r\n"
                                "aof_rewrite_buffer_length:%lu\r\n"
                                "aof_pending_bio_fsync:%llu\r\n"
                                "aof_delayed_fsync:%lu\r\n",
                                (long long) server.aof_current_size,
                                (long long) server.aof_rewrite_base_size,
                                server.aof_rewrite_scheduled,
                                sdslen(server.aof_buf),
                                aofRewriteBufferSize(),
                                bioPendingJobsOfType(BIO_AOF_FSYNC),
                                server.aof_delayed_fsync);
        }

        if (server.loading) {
            double perc;
            time_t eta, elapsed;
            off_t remaining_bytes = server.loading_total_bytes -
                                    server.loading_loaded_bytes;

            perc = ((double) server.loading_loaded_bytes /
                    (server.loading_total_bytes + 1)) * 100;

            elapsed = time(NULL) - server.loading_start_time;
            if (elapsed == 0) {
                eta = 1; /* A fake 1 second figure if we don't have
                            enough info */
            } else {
                eta = (elapsed * remaining_bytes) / (server.loading_loaded_bytes + 1);
            }

            info = sdscatprintf(info,
                                "loading_start_time:%jd\r\n"
                                "loading_total_bytes:%llu\r\n"
                                "loading_loaded_bytes:%llu\r\n"
                                "loading_loaded_perc:%.2f\r\n"
                                "loading_eta_seconds:%jd\r\n",
                                (intmax_t) server.loading_start_time,
                                (unsigned long long) server.loading_total_bytes,
                                (unsigned long long) server.loading_loaded_bytes,
                                perc,
                                (intmax_t) eta
            );
        }
    }

    /* Stats */
    if (allsections || defsections || !strcasecmp(section, "stats")) {
        if (sections++) info = sdscat(info, "\r\n");
        info = sdscatprintf(info,
                            "# Stats\r\n"
                            "total_connections_received:%lld\r\n"
                            "total_commands_processed:%lld\r\n"
                            "instantaneous_ops_per_sec:%lld\r\n"
                            "total_net_input_bytes:%lld\r\n"
                            "total_net_output_bytes:%lld\r\n"
                            "instantaneous_input_kbps:%.2f\r\n"
                            "instantaneous_output_kbps:%.2f\r\n"
                            "rejected_connections:%lld\r\n"
                            "sync_full:%lld\r\n"
                            "sync_partial_ok:%lld\r\n"
                            "sync_partial_err:%lld\r\n"
                            "expired_keys:%lld\r\n"
                            "expired_stale_perc:%.2f\r\n"
                            "expired_time_cap_reached_count:%lld\r\n"
                            "expire_cycle_cpu_milliseconds:%lld\r\n"
                            "evicted_keys:%lld\r\n"
                            "keyspace_hits:%lld\r\n"
                            "keyspace_misses:%lld\r\n"
                            "pubsub_channels:%ld\r\n"
                            "pubsub_patterns:%lu\r\n"
                            "latest_fork_usec:%lld\r\n"
                            "migrate_cached_sockets:%ld\r\n"
                            "slave_expires_tracked_keys:%zu\r\n"
                            "active_defrag_hits:%lld\r\n"
                            "active_defrag_misses:%lld\r\n"
                            "active_defrag_key_hits:%lld\r\n"
                            "active_defrag_key_misses:%lld\r\n"
                            "tracking_total_keys:%lld\r\n"
                            "tracking_total_items:%lld\r\n"
                            "tracking_total_prefixes:%lld\r\n"
                            "unexpected_error_replies:%lld\r\n"
                            "total_reads_processed:%lld\r\n"
                            "total_writes_processed:%lld\r\n"
                            "io_threaded_reads_processed:%lld\r\n"
                            "io_threaded_writes_processed:%lld\r\n",
                            server.stat_numconnections,
                            server.stat_numcommands,
                            getInstantaneousMetric(STATS_METRIC_COMMAND),
                            server.stat_net_input_bytes,
                            server.stat_net_output_bytes,
                            (float) getInstantaneousMetric(STATS_METRIC_NET_INPUT) / 1024,
                            (float) getInstantaneousMetric(STATS_METRIC_NET_OUTPUT) / 1024,
                            server.stat_rejected_conn,
                            server.stat_sync_full,
                            server.stat_sync_partial_ok,
                            server.stat_sync_partial_err,
                            server.stat_expiredkeys,
                            server.stat_expired_stale_perc * 100,
                            server.stat_expired_time_cap_reached_count,
                            server.stat_expire_cycle_time_used / 1000,
                            server.stat_evictedkeys,
                            server.stat_keyspace_hits,
                            server.stat_keyspace_misses,
                            dictSize(server.pubsub_channels),
                            listLength(server.pubsub_patterns),
                            server.stat_fork_time,
                            dictSize(server.migrate_cached_sockets),
                            getSlaveKeyWithExpireCount(),
                            server.stat_active_defrag_hits,
                            server.stat_active_defrag_misses,
                            server.stat_active_defrag_key_hits,
                            server.stat_active_defrag_key_misses,
                            (unsigned long long) trackingGetTotalKeys(),
                            (unsigned long long) trackingGetTotalItems(),
                            (unsigned long long) trackingGetTotalPrefixes(),
                            server.stat_unexpected_error_replies,
                            server.stat_total_reads_processed,
                            server.stat_total_writes_processed,
                            server.stat_io_reads_processed,
                            server.stat_io_writes_processed);
    }

    /* Replication */
    if (allsections || defsections || !strcasecmp(section, "replication")) {
        if (sections++) info = sdscat(info, "\r\n");
        info = sdscatprintf(info,
                            "# Replication\r\n"
                            "role:%s\r\n",
                            server.masterhost == NULL ? "master" : "slave");
        if (server.masterhost) {
            long long slave_repl_offset = 1;

            if (server.master)
                slave_repl_offset = server.master->reploff;
            else if (server.cached_master)
                slave_repl_offset = server.cached_master->reploff;

            info = sdscatprintf(info,
                                "master_host:%s\r\n"
                                "master_port:%d\r\n"
                                "master_link_status:%s\r\n"
                                "master_last_io_seconds_ago:%d\r\n"
                                "master_sync_in_progress:%d\r\n"
                                "slave_repl_offset:%lld\r\n", server.masterhost,
                                server.masterport,
                                (server.repl_state == REPL_STATE_CONNECTED) ?
                                "up" : "down",
                                server.master ?
                                ((int) (server.unixtime - server.master->lastinteraction)) : -1,
                                server.repl_state == REPL_STATE_TRANSFER,
                                slave_repl_offset
            );

            if (server.repl_state == REPL_STATE_TRANSFER) {
                info = sdscatprintf(info,
                                    "master_sync_left_bytes:%lld\r\n"
                                    "master_sync_last_io_seconds_ago:%d\r\n", (long long)
                                            (server.repl_transfer_size - server.repl_transfer_read),
                                    (int) (server.unixtime - server.repl_transfer_lastio)
                );
            }

            if (server.repl_state != REPL_STATE_CONNECTED) {
                info = sdscatprintf(info,
                                    "master_link_down_since_seconds:%jd\r\n",
                                    (intmax_t) (server.unixtime - server.repl_down_since));
            }
            info = sdscatprintf(info,
                                "slave_priority:%d\r\n"
                                "slave_read_only:%d\r\n",
                                server.slave_priority,
                                server.repl_slave_ro);
        }

        info = sdscatprintf(info,
                            "connected_slaves:%lu\r\n",
                            listLength(server.slaves));

        /* If min-slaves-to-write is active, write the number of slaves
         * currently considered 'good'. */
        if (server.repl_min_slaves_to_write &&
            server.repl_min_slaves_max_lag) {
            info = sdscatprintf(info,
                                "min_slaves_good_slaves:%d\r\n",
                                server.repl_good_slaves_count);
        }

        if (listLength(server.slaves)) {
            int slaveid = 0;
            listNode *ln;
            listIter li;

            listRewind(server.slaves, &li);
            while ((ln = listNext(&li))) {
                client *slave = listNodeValue(ln);
                char *state = NULL;
                char ip[NET_IP_STR_LEN], *slaveip = slave->slave_ip;
                int port;
                long lag = 0;

                if (slaveip[0] == '\0') {
                    if (connPeerToString(slave->conn, ip, sizeof(ip), &port) == -1)
                        continue;
                    slaveip = ip;
                }
                switch (slave->replstate) {
                    case SLAVE_STATE_WAIT_BGSAVE_START:
                    case SLAVE_STATE_WAIT_BGSAVE_END:
                        state = "wait_bgsave";
                        break;
                    case SLAVE_STATE_SEND_BULK:
                        state = "send_bulk";
                        break;
                    case SLAVE_STATE_ONLINE:
                        state = "online";
                        break;
                }
                if (state == NULL) continue;
                if (slave->replstate == SLAVE_STATE_ONLINE)
                    lag = time(NULL) - slave->repl_ack_time;

                info = sdscatprintf(info,
                                    "slave%d:ip=%s,port=%d,state=%s,"
                                    "offset=%lld,lag=%ld\r\n",
                                    slaveid, slaveip, slave->slave_listening_port, state,
                                    slave->repl_ack_off, lag);
                slaveid++;
            }
        }
        info = sdscatprintf(info,
                            "master_replid:%s\r\n"
                            "master_replid2:%s\r\n"
                            "master_repl_offset:%lld\r\n"
                            "second_repl_offset:%lld\r\n"
                            "repl_backlog_active:%d\r\n"
                            "repl_backlog_size:%lld\r\n"
                            "repl_backlog_first_byte_offset:%lld\r\n"
                            "repl_backlog_histlen:%lld\r\n",
                            server.replid,
                            server.replid2,
                            server.master_repl_offset,
                            server.second_replid_offset,
                            server.repl_backlog != NULL,
                            server.repl_backlog_size,
                            server.repl_backlog_off,
                            server.repl_backlog_histlen);
    }

    /* CPU */
    if (allsections || defsections || !strcasecmp(section, "cpu")) {
        if (sections++) info = sdscat(info, "\r\n");
        info = sdscatprintf(info,
                            "# CPU\r\n"
                            "used_cpu_sys:%ld.%06ld\r\n"
                            "used_cpu_user:%ld.%06ld\r\n"
                            "used_cpu_sys_children:%ld.%06ld\r\n"
                            "used_cpu_user_children:%ld.%06ld\r\n",
                            (long) self_ru.ru_stime.tv_sec, (long) self_ru.ru_stime.tv_usec,
                            (long) self_ru.ru_utime.tv_sec, (long) self_ru.ru_utime.tv_usec,
                            (long) c_ru.ru_stime.tv_sec, (long) c_ru.ru_stime.tv_usec,
                            (long) c_ru.ru_utime.tv_sec, (long) c_ru.ru_utime.tv_usec);
    }

    /* Modules */
    if (allsections || defsections || !strcasecmp(section, "modules")) {
        if (sections++) info = sdscat(info, "\r\n");
        info = sdscatprintf(info, "# Modules\r\n");
        info = genModulesInfoString(info);
    }

    /* Command statistics */
    if (allsections || !strcasecmp(section, "commandstats")) {
        if (sections++) info = sdscat(info, "\r\n");
        info = sdscatprintf(info, "# Commandstats\r\n");

        struct redisCommand *c;
        dictEntry *de;
        dictIterator *di;
        di = dictGetSafeIterator(server.commands);
        while ((de = dictNext(di)) != NULL) {
            c = (struct redisCommand *) dictGetVal(de);
            if (!c->calls) continue;
            info = sdscatprintf(info,
                                "cmdstat_%s:calls=%lld,usec=%lld,usec_per_call=%.2f\r\n",
                                c->name, c->calls, c->microseconds,
                                (c->calls == 0) ? 0 : ((float) c->microseconds / c->calls));
        }
        dictReleaseIterator(di);
    }

    /* Cluster */
    if (allsections || defsections || !strcasecmp(section, "cluster")) {
        if (sections++) info = sdscat(info, "\r\n");
        info = sdscatprintf(info,
                            "# Cluster\r\n"
                            "cluster_enabled:%d\r\n",
                            server.cluster_enabled);
    }

    /* Key space */
    if (allsections || defsections || !strcasecmp(section, "keyspace")) {
        if (sections++) info = sdscat(info, "\r\n");
        info = sdscatprintf(info, "# Keyspace\r\n");
        for (j = 0; j < server.dbnum; j++) {
            long long keys, vkeys;

            keys = dictSize(server.db[j].dict);
            vkeys = dictSize(server.db[j].expires);
            if (keys || vkeys) {
                info = sdscatprintf(info,
                                    "db%d:keys=%lld,expires=%lld,avg_ttl=%lld\r\n",
                                    j, keys, vkeys, server.db[j].avg_ttl);
            }
        }
    }

    /* Get info from modules.
     * if user asked for "everything" or "modules", or a specific section
     * that's not found yet. */
    if (everything || modules ||
        (!allsections && !defsections && sections == 0)) {
        info = modulesCollectInfo(info,
                                  everything || modules ? NULL : section,
                                  0, /* not a crash report */
                                  sections);
    }
    return info;
}

void infoCommand(client *c) {
    char *section = c->argc == 2 ? c->argv[1]->ptr : "default";

    if (c->argc > 2) {
        addReply(c, shared.syntaxerr);
        return;
    }
    sds info = genRedisInfoString(section);
    addReplyVerbatim(c, info, sdslen(info), "txt");
    sdsfree(info);
}

void monitorCommand(client *c) {
    /* ignore MONITOR if already slave or in monitor mode */
    if (c->flags & CLIENT_SLAVE) return;

    c->flags |= (CLIENT_SLAVE | CLIENT_MONITOR);
    listAddNodeTail(server.monitors, c);
    addReply(c, shared.ok);
}

/* =================================== Main! ================================ */

#ifdef __linux__
                                                                                                                        int linuxOvercommitMemoryValue(void) {
    FILE *fp = fopen("/proc/sys/vm/overcommit_memory","r");
    char buf[64];

    if (!fp) return -1;
    if (fgets(buf,64,fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    return atoi(buf);
}

void linuxMemoryWarnings(void) {
    if (linuxOvercommitMemoryValue() == 0) {
        serverLog(LL_WARNING,"WARNING overcommit_memory is set to 0! Background save may fail under low memory condition. To fix this issue add 'vm.overcommit_memory = 1' to /etc/sysctl.conf and then reboot or run the command 'sysctl vm.overcommit_memory=1' for this to take effect.");
    }
    if (THPIsEnabled()) {
        serverLog(LL_WARNING,"WARNING you have Transparent Huge Pages (THP) support enabled in your kernel. This will create latency and memory usage issues with Redis. To fix this issue run the command 'echo madvise > /sys/kernel/mm/transparent_hugepage/enabled' as root, and add it to your /etc/rc.local in order to retain the setting after a reboot. Redis must be restarted after THP is disabled (set to 'madvise' or 'never').");
    }
}
#endif /* __linux__ */

//createPidFile()函数的内部实现 FILE *fp =fopen(server.pidfile,"w")，以可写的方式打开server.pidfile配置的文件。
void createPidFile(void) {
    /* If pidfile requested, but no pidfile defined, use
     * default pidfile path */
    if (!server.pidfile) server.pidfile = zstrdup(CONFIG_DEFAULT_PID_FILE);

    /* Try to write the pid file in a best-effort way. */
    FILE *fp = fopen(server.pidfile, "w");
    if (fp) {
        fprintf(fp, "%d\n", (int) getpid());
        fclose(fp);
    }
}

void daemonize(void) {
    int fd;

    if (fork() != 0) exit(0); /* parent exits */
    setsid(); /* create a new session */

    /* Every output goes to /dev/null. If Redis is daemonized but
     * the 'logfile' is set to 'stdout' in the configuration file
     * it will not log at all. */
    if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
        // 赋给fd标准输入句柄
        dup2(fd, STDIN_FILENO);
        // 关闭标准输入，赋给fd标准输出句柄
        dup2(fd, STDOUT_FILENO);
        // 关闭标准输出，赋给fd标准错误输出句柄
        dup2(fd, STDERR_FILENO);
        // 关闭标准错误输出
        if (fd > STDERR_FILENO) close(fd);
    }
}

void version(void) {
    printf("Redis server v=%s sha=%s:%d malloc=%s bits=%d build=%llx\n",
           REDIS_VERSION,
           redisGitSHA1(),
           atoi(redisGitDirty()) > 0,
           ZMALLOC_LIB,
           sizeof(long) == 4 ? 32 : 64,
           (unsigned long long) redisBuildId());
    exit(0);
}

void usage(void) {
    fprintf(stderr, "Usage: ./redis-server [/path/to/redis.conf] [options]\n");
    fprintf(stderr, "       ./redis-server - (read config from stdin)\n");
    fprintf(stderr, "       ./redis-server -v or --version\n");
    fprintf(stderr, "       ./redis-server -h or --help\n");
    fprintf(stderr, "       ./redis-server --test-memory <megabytes>\n\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "       ./redis-server (run the server with default conf)\n");
    fprintf(stderr, "       ./redis-server /etc/redis/6379.conf\n");
    fprintf(stderr, "       ./redis-server --port 7777\n");
    fprintf(stderr, "       ./redis-server --port 7777 --replicaof 127.0.0.1 8888\n");
    fprintf(stderr, "       ./redis-server /etc/myredis.conf --loglevel verbose\n\n");
    fprintf(stderr, "Sentinel mode:\n");
    fprintf(stderr, "       ./redis-server /etc/sentinel.conf --sentinel\n");
    exit(1);
}

void redisAsciiArt(void) {
#include "asciilogo.h"

    char *buf = zmalloc(1024 * 16);
    char *mode;

    if (server.cluster_enabled) mode = "cluster";
    else if (server.sentinel_mode) {
        // 是否开启redis的哨兵模式，也就是是否监测，通知，自动错误恢复，是用来管理多个redis实例的方式。
        mode = "sentinel";
    } else mode = "standalone";

    /* Show the ASCII logo if: log file is stdout AND stdout is a
     * tty AND syslog logging is disabled. Also show logo if the user
     * forced us to do so via redis.conf. */
    int show_logo = ((!server.syslog_enabled &&
                      server.logfile[0] == '\0' &&
                      isatty(fileno(stdout))) ||
                     server.always_show_logo);

    if (!show_logo) {
        serverLog(LL_NOTICE,
                  "Running mode=%s, port=%d.",
                  mode, server.port ? server.port : server.tls_port
        );
    } else {
        snprintf(buf, 1024 * 16, ascii_logo,
                 REDIS_VERSION,
                 redisGitSHA1(),
                 strtol(redisGitDirty(), NULL, 10) > 0,
                 (sizeof(long) == 8) ? "64" : "32",
                 mode, server.port ? server.port : server.tls_port,
                 (long) getpid()
        );
        serverLogRaw(LL_NOTICE | LL_RAW, buf);
    }
    zfree(buf);
}

static void sigShutdownHandler(int sig) {
    char *msg;

    switch (sig) {
        case SIGINT:
            msg = "Received SIGINT scheduling shutdown...";
            break;
        case SIGTERM:
            msg = "Received SIGTERM scheduling shutdown...";
            break;
        default:
            msg = "Received shutdown signal, scheduling shutdown...";
    };

    /* SIGINT is often delivered via Ctrl+C in an interactive session.
     * If we receive the signal the second time, we interpret this as
     * the user really wanting to quit ASAP without waiting to persist
     * on disk. */
    if (server.shutdown_asap && sig == SIGINT) {
        serverLogFromHandler(LL_WARNING, "You insist... exiting now.");
        rdbRemoveTempFile(getpid());
        exit(1); /* Exit with an error since this was not a clean shutdown. */
    } else if (server.loading) {
        serverLogFromHandler(LL_WARNING, "Received shutdown signal during loading, exiting now.");
        exit(0);
    }

    serverLogFromHandler(LL_WARNING, msg);
    server.shutdown_asap = 1;
}

void setupSignalHandlers(void) {
    struct sigaction act;

    /* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction is used.
     * Otherwise, sa_handler is used. */
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = sigShutdownHandler;
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGINT, &act, NULL);

#ifdef HAVE_BACKTRACE
                                                                                                                            sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_RESETHAND | SA_SIGINFO;
    act.sa_sigaction = sigsegvHandler;
    sigaction(SIGSEGV, &act, NULL);
    sigaction(SIGBUS, &act, NULL);
    sigaction(SIGFPE, &act, NULL);
    sigaction(SIGILL, &act, NULL);
#endif
    return;
}

/* This is the signal handler for children process. It is currently useful
 * in order to track the SIGUSR1, that we send to a child in order to terminate
 * it in a clean way, without the parent detecting an error and stop
 * accepting writes because of a write error condition. */
static void sigKillChildHandler(int sig) {
    UNUSED(sig);
    serverLogFromHandler(LL_WARNING, "Received SIGUSR1 in child, exiting now.");
    exitFromChild(SERVER_CHILD_NOERROR_RETVAL);
}

void setupChildSignalHandlers(void) {
    struct sigaction act;

    /* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction is used.
     * Otherwise, sa_handler is used. */
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = sigKillChildHandler;
    sigaction(SIGUSR1, &act, NULL);
    return;
}

/* After fork, the child process will inherit the resources
 * of the parent process, e.g. fd(socket or flock) etc.
 * should close the resources not used by the child process, so that if the
 * parent restarts it can bind/lock despite the child possibly still running. */
void closeClildUnusedResourceAfterFork() {
    closeListeningSockets(0);
    if (server.cluster_enabled && server.cluster_config_file_lock_fd != -1)
        close(server.cluster_config_file_lock_fd);  /* don't care if this fails */
}

int redisFork() {
    int childpid;
    long long start = ustime();
    if ((childpid = fork()) == 0) {
        /* Child */
        setOOMScoreAdj(CONFIG_OOM_BGCHILD);
        setupChildSignalHandlers();
        closeClildUnusedResourceAfterFork();
    } else {
        /* Parent */
        server.stat_fork_time = ustime() - start;
        server.stat_fork_rate = (double) zmalloc_used_memory() * 1000000 / server.stat_fork_time /
                                (1024 * 1024 * 1024); /* GB per second. */
        latencyAddSampleIfNeeded("fork", server.stat_fork_time / 1000);
        if (childpid == -1) {
            return -1;
        }
        updateDictResizePolicy();
    }
    return childpid;
}

void sendChildCOWInfo(int ptype, char *pname) {
    size_t private_dirty = zmalloc_get_private_dirty(-1);

    if (private_dirty) {
        serverLog(LL_NOTICE,
                  "%s: %zu MB of memory used by copy-on-write",
                  pname, private_dirty / (1024 * 1024));
    }

    server.child_info_data.cow_size = private_dirty;
    sendChildInfo(ptype);
}

void memtest(size_t megabytes, int passes);

/* Returns 1 if there is --sentinel among the arguments or if
 * argv[0] contains "redis-sentinel".
 * 这个函数如果第一个参数是"redis-sentinel"，或者其他参数中有“—sentinel”都会返还1,否则0。这句话完全可以放入initServerConfig()函数。
 * */
int checkForSentinelMode(int argc, char **argv) {
    int j;

    if (strstr(argv[0], "redis-sentinel") != NULL) return 1;
    for (j = 1; j < argc; j++)
        if (!strcmp(argv[j], "--sentinel")) return 1;
    return 0;
}

/* Function called at startup to load RDB or AOF file in memory. */
void loadDataFromDisk(void) {
    long long start = ustime();
    if (server.aof_state == AOF_ON) {
        if (loadAppendOnlyFile(server.aof_filename) == C_OK)
            serverLog(LL_NOTICE, "DB loaded from append only file: %.3f seconds", (float) (ustime() - start) / 1000000);
    } else {
        rdbSaveInfo rsi = RDB_SAVE_INFO_INIT;
        errno = 0; /* Prevent a stale value from affecting error checking */
        if (rdbLoad(server.rdb_filename, &rsi, RDBFLAGS_NONE) == C_OK) {
            serverLog(LL_NOTICE, "DB loaded from disk: %.3f seconds",
                      (float) (ustime() - start) / 1000000);

            /* Restore the replication ID / offset from the RDB file. */
            if ((server.masterhost ||
                 (server.cluster_enabled &&
                  nodeIsSlave(server.cluster->myself))) &&
                rsi.repl_id_is_set &&
                rsi.repl_offset != -1 &&
                /* Note that older implementations may save a repl_stream_db
                 * of -1 inside the RDB file in a wrong way, see more
                 * information in function rdbPopulateSaveInfo. */
                rsi.repl_stream_db != -1) {
                memcpy(server.replid, rsi.repl_id, sizeof(server.replid));
                server.master_repl_offset = rsi.repl_offset;
                /* If we are a slave, create a cached master from this
                 * information, in order to allow partial resynchronizations
                 * with masters. */
                replicationCacheMasterUsingMyself();
                selectDb(server.cached_master, rsi.repl_stream_db);
            }
        } else if (errno != ENOENT) {
            serverLog(LL_WARNING, "Fatal error loading the DB: %s. Exiting.", strerror(errno));
            exit(1);
        }
    }
}

void redisOutOfMemoryHandler(size_t allocation_size) {
    serverLog(LL_WARNING, "Out Of Memory allocating %zu bytes!",
              allocation_size);
    serverPanic("Redis aborting for OUT OF MEMORY. Allocating %zu bytes!",
                allocation_size);
}

void redisSetProcTitle(char *title) {
#ifdef USE_SETPROCTITLE
                                                                                                                            char *server_mode = "";
    if (server.cluster_enabled) server_mode = " [cluster]";
    else if (server.sentinel_mode) server_mode = " [sentinel]";

    setproctitle("%s %s:%d%s",
        title,
        server.bindaddr_count ? server.bindaddr[0] : "*",
        server.port ? server.port : server.tls_port,
        server_mode);
#else
    UNUSED(title);
#endif
}

void redisSetCpuAffinity(const char *cpulist) {
#ifdef USE_SETCPUAFFINITY
    setcpuaffinity(cpulist);
#else
    UNUSED(cpulist);
#endif
}

/*
 * Check whether systemd or upstart have been used to start redis.
 */

int redisSupervisedUpstart(void) {
    const char *upstart_job = getenv("UPSTART_JOB");

    if (!upstart_job) {
        serverLog(LL_WARNING,
                  "upstart supervision requested, but UPSTART_JOB not found");
        return 0;
    }

    serverLog(LL_NOTICE, "supervised by upstart, will stop to signal readiness");
    raise(SIGSTOP);
    unsetenv("UPSTART_JOB");
    return 1;
}

int redisCommunicateSystemd(const char *sd_notify_msg) {
    const char *notify_socket = getenv("NOTIFY_SOCKET");
    if (!notify_socket) {
        serverLog(LL_WARNING,
                  "systemd supervision requested, but NOTIFY_SOCKET not found");
    }

#ifdef HAVE_LIBSYSTEMD
        (void) sd_notify(0, sd_notify_msg);
#else
    UNUSED(sd_notify_msg);
#endif
    return 0;
}

int redisIsSupervised(int mode) {
    if (mode == SUPERVISED_AUTODETECT) {
        const char *upstart_job = getenv("UPSTART_JOB");
        const char *notify_socket = getenv("NOTIFY_SOCKET");

        if (upstart_job) {
            redisSupervisedUpstart();
        } else if (notify_socket) {
            server.supervised_mode = SUPERVISED_SYSTEMD;
            serverLog(LL_WARNING,
                      "WARNING auto-supervised by systemd - you MUST set appropriate values for TimeoutStartSec and TimeoutStopSec in your service unit.");
            return redisCommunicateSystemd("STATUS=Redis is loading...\n");
        }
    } else if (mode == SUPERVISED_UPSTART) {
        return redisSupervisedUpstart();
    } else if (mode == SUPERVISED_SYSTEMD) {
        serverLog(LL_WARNING,
                  "WARNING supervised by systemd - you MUST set appropriate values for TimeoutStartSec and TimeoutStopSec in your service unit.");
        return redisCommunicateSystemd("STATUS=Redis is loading...\n");
    }

    return 0;
}

int iAmMaster(void) {
    return ((!server.cluster_enabled && server.masterhost == NULL) ||
            (server.cluster_enabled && nodeIsMaster(server.cluster->myself)));
}

/**
 * TODO  启动函数
 * @param argc
 * @param argv
 * @return
 */
int main(int argc, char **argv) {
    struct timeval tv;
    int j;

#ifdef REDIS_TEST
        if (argc == 3 && !strcasecmp(argv[1], "test")) {
        if (!strcasecmp(argv[2], "ziplist")) {
            return ziplistTest(argc, argv);
        } else if (!strcasecmp(argv[2], "quicklist")) {
            quicklistTest(argc, argv);
        } else if (!strcasecmp(argv[2], "intset")) {
            return intsetTest(argc, argv);
        } else if (!strcasecmp(argv[2], "zipmap")) {
            return zipmapTest(argc, argv);
        } else if (!strcasecmp(argv[2], "sha1test")) {
            return sha1Test(argc, argv);
        } else if (!strcasecmp(argv[2], "util")) {
            return utilTest(argc, argv);
        } else if (!strcasecmp(argv[2], "endianconv")) {
            return endianconvTest(argc, argv);
        } else if (!strcasecmp(argv[2], "crc64")) {
            return crc64Test(argc, argv);
        } else if (!strcasecmp(argv[2], "zmalloc")) {
            return zmalloc_test(argc, argv);
        }

        return -1; /* test not found */
    }
#endif

    /* We need to initialize our libraries, and the server configuration. */
#ifdef INIT_SETPROCTITLE_REPLACEMENT
    spt_init(argc, argv);
#endif
    setlocale(LC_COLLATE, "");
    tzset(); /* Populates 'timezone' global. */
    // 设置内存分配失败后的回调函数，如果不主动设置回调函数，redis将会会记录文件“zmalloc:Out of memory trying to allocate xxxbytes”，并退出线程。在这里，redis没有使用默认处理方式，而是选择记录Log，并主动报告一个OMM错误。
    zmalloc_set_oom_handler(redisOutOfMemoryHandler);
    // 用当前时间随机种子值，用当前时间与进程ID进行按位异或，增大随机种子的随机概率。
    srand(time(NULL) ^ getpid());
    //
    gettimeofday(&tv, NULL);
    crc64_init();

    uint8_t hashseed[16];
    getRandomBytes(hashseed, sizeof(hashseed));
    // 这个值用于hash数据结构，以后讲到hash数据结构即可。
    dictSetHashFunctionSeed(hashseed);
    // 服务器是否用监视模式运行，监视模式下，redis将记录监视主节点(master nodes)的一些数据。
    server.sentinel_mode = checkForSentinelMode(argc, argv);
    // 初始化服务器配置
    initServerConfig();
    ACLInit(); /* The ACL subsystem must be initialized ASAP because the
                  basic networking code and client creation depends on it. */
    moduleInitModulesSystem();
    tlsInit();

    /* Store the executable path and arguments in a safe place in order
     * to be able to restart the server later. （将可执行路径和参数存储在一个安全的地方，以便稍后能够重新启动服务器。）*/
    server.executable = getAbsolutePath(argv[0]);
    server.exec_argv = zmalloc(sizeof(char *) * (argc + 1));
    server.exec_argv[argc] = NULL;
    for (j = 0; j < argc; j++) server.exec_argv[j] = zstrdup(argv[j]);

    /* We need to init sentinel right now as parsing the configuration file
     * in sentinel mode will have the effect of populating the sentinel
     * data structures with master nodes to monitor.(我们现在就需要初始化sentinel，因为在sentinel模式下解析配置文件会产生用要监视的主节点填充sentinel数据结构的效果。) */
    if (server.sentinel_mode) {
        //initSentinelConfig();位于sentinel.c文件，设置监视端口为26379；
        initSentinelConfig();
        // initSentinel()位于sentinel.c文件，清空服务器的命令表，并加入SENTINEL命令。并初始化sentinel全局变量，这个变量时一个sentinelState类型，哨兵模式会详细深入。
        initSentinel();
    }

    /* Check if we need to start in redis-check-rdb/aof mode. We just execute
     * the program main. However the program is part of the Redis executable
     * so that we can easily execute an RDB check on loading errors.
     * （检查我们是否需要开始在redisk - Check -rdb/aof模式。我们只执行主程序。然而，该程序是Redis可执行的一部分，所以我们可以很容易地执行一个RDB检查加载错误。）
     * */
    if (strstr(argv[0], "redis-check-rdb") != NULL)
        redis_check_rdb_main(argc, argv, NULL);
    else if (strstr(argv[0], "redis-check-aof") != NULL)
        redis_check_aof_main(argc, argv);

    // 这部分进行读入选项和配置文件，并修改服务器配置，如果第二个启动参数是“-v”或者”--version”， 输出当前redis版本号后退出进程; 如果第二个参数是"--help"或"-h"，则调用usage() 输出控制台服务器一些帮助信息后退出;如果第二个参数是"--test-memory"，那么根据第三个参数进行内存测试后，退出。
    if (argc >= 2) {
        j = 1; /* First option to parse in argv[] */
        sds options = sdsempty();
        char *configfile = NULL;

        /* Handle special options --help and --version */
        if (strcmp(argv[1], "-v") == 0 ||
            strcmp(argv[1], "--version") == 0)
            version();
        if (strcmp(argv[1], "--help") == 0 ||
            strcmp(argv[1], "-h") == 0)
            usage();
        if (strcmp(argv[1], "--test-memory") == 0) {
            if (argc == 3) {
                memtest(atoi(argv[2]), 50);
                exit(0);
            } else {
                fprintf(stderr, "Please specify the amount of memory to test in megabytes.\n");
                fprintf(stderr, "Example: ./redis-server --test-memory 4096\n\n");
                exit(1);
            }
        }

        //C程序启动后，第一个启动参数是默认的，即是你的启动路径，第2~n个参数才是你配置的。只有是第二个参数不是以“--”开头，就认为是配置文件。
        /* First argument is the config file name? */
        if (argv[j][0] != '-' || argv[j][1] != '-') {
            configfile = argv[j];
            server.configfile = getAbsolutePath(configfile);
            /* Replace the config file in server.exec_argv with
             * its absolute path. */
            zfree(server.exec_argv[j]);
            server.exec_argv[j] = zstrdup(server.configfile);
            j++;
        }

        /* All the other options are parsed and conceptually appended to the
         * configuration file. For instance --port 6380 will generate the
         * string "port 6380\n" to be parsed after the actual file name
         * is parsed, if any. */
        // 解析其他配置选项，记录到options这个sds变量，并以“ ”结尾(关于sds，详见redis内部数据结构暂时理解为字符串)。如果是以“--”开头的参数，例如 “--port 6380”这个启动参数，将给options追加入“\nport 6380 ”。
        while (j != argc) {
            if (argv[j][0] == '-' && argv[j][1] == '-') {
                /* Option name */
                if (!strcmp(argv[j], "--check-rdb")) {
                    /* Argument has no options, need to skip for parsing. */
                    j++;
                    continue;
                }
                if (sdslen(options)) options = sdscat(options, "\n");
                // 如果是以“-”开头的参数,则需要进入sdscatrepr()函数特殊处理，并追加入options变量。这里的特殊处理是指对转义字符的处理，例如“-xx\nyy\t””将被处理成“xx\\nyy\\t”。最后两句,resetServerSaveParams(); //定义于 config.c ，释放并置server.saveparams，server.saveparamslen=0。
                // 这个+2目的是为了去掉”--”。
                options = sdscat(options, argv[j] + 2);
                options = sdscat(options, " ");
            } else {
                /* Option argument */
                options = sdscatrepr(options, argv[j], strlen(argv[j]));
                options = sdscat(options, " ");
            }
            j++;
        }
        if (server.sentinel_mode && configfile && *configfile == '-') {
            serverLog(LL_WARNING,
                      "Sentinel config from STDIN not allowed.");
            serverLog(LL_WARNING,
                      "Sentinel needs config file on disk to save state.  Exiting...");
            exit(1);
        }
        resetServerSaveParams();
        // 压栈分析loadServerConfig()函数
        // 根据配置文件和传入的选项，修改server 变量（服务器配置），如果配置文件名是以“-\0”开头，认为配置从标准输入。
        loadServerConfig(configfile, options);
        sdsfree(options);
    }

    server.supervised = redisIsSupervised(server.supervised_mode);
    //如果服务器配置是守护形式启动，就创建守护进程Daemonize()函数里面 if ((fd =open("/dev/null",O_RDWR, 0)) != -1){} //所有输出都放在 /dev/null下， 如果redis工作在守护线程下，配置文件的'stdout'设置为'logfile'，就不能正常记录日志。如果日志权限不足，就会进行以下行为：
    int background = server.daemonize && !server.supervised;
    if (background) daemonize();

    serverLog(LL_WARNING, "oO0OoO0OoO0Oo Redis is starting oO0OoO0OoO0Oo");
    serverLog(LL_WARNING,
              "Redis version=%s, bits=%d, commit=%s, modified=%d, pid=%d, just started",
              REDIS_VERSION,
              (sizeof(long) == 8) ? 64 : 32,
              redisGitSHA1(),
              strtol(redisGitDirty(), NULL, 10) > 0,
              (int) getpid());

    if (argc == 1) {
        serverLog(LL_WARNING,
                  "Warning: no config file specified, using the default config. In order to specify a config file use %s /path/to/%s.conf",
                  argv[0], server.sentinel_mode ? "sentinel" : "redis");
    } else {
        serverLog(LL_WARNING, "Configuration loaded");
    }

    readOOMScoreAdj();
    // 初始化服务端剩余的配置
    initServer();
    // createPidFile()函数的内部实现 FILE *fp =fopen(server.pidfile,"w")，以可写的方式打开server.pidfile配置的文件。
    if (background || server.pidfile) createPidFile();
    redisSetProcTitle(argv[0]);
    redisAsciiArt();
    checkTcpBacklogSettings();

    if (!server.sentinel_mode) {
        /* Things not needed when running in Sentinel mode. */
        serverLog(LL_WARNING, "Server initialized");
#ifdef __linux__
        linuxMemoryWarnings();
#endif
        moduleLoadFromQueue();
        ACLLoadUsersAtStartup();
        InitServerLast();
        // loadDataFromDisk ()如果不在后台模式下，会输出一些信息。这里只看一个函数loadDataFromDisk()，如果server.aof_state这个配置了REDIS_AOF_ON，要通过loadAppendOnlyFile()载入AOF文件，否则加载RDB文件，如果RDB加载失败，则记录报错日志并退出程序，loadAppendOnlyFile()函数分析见aof.c，rdbLoad()函数分析见rdb.c。
        loadDataFromDisk();
        if (server.cluster_enabled) {
            if (verifyClusterConfigWithData() == C_ERR) {
                serverLog(LL_WARNING,
                          "You can't have keys in a DB different than DB 0 when in "
                          "Cluster mode. Exiting.");
                exit(1);
            }
        }
        if (server.ipfd_count > 0 || server.tlsfd_count > 0)
            serverLog(LL_NOTICE, "Ready to accept connections");
        if (server.sofd > 0)
            serverLog(LL_NOTICE, "The server is now ready to accept connections at %s", server.unixsocket);
        if (server.supervised_mode == SUPERVISED_SYSTEMD) {
            if (!server.masterhost) {
                redisCommunicateSystemd("STATUS=Ready to accept connections\n");
                redisCommunicateSystemd("READY=1\n");
            } else {
                redisCommunicateSystemd("STATUS=Waiting for MASTER <-> REPLICA sync\n");
            }
        }
    } else {
        // 多线程模式初始化
        InitServerLast();
        sentinelIsRunning();
        if (server.supervised_mode == SUPERVISED_SYSTEMD) {
            redisCommunicateSystemd("STATUS=Ready to accept connections\n");
            redisCommunicateSystemd("READY=1\n");
        }
    }

    /* Warning the user about suspicious maxmemory setting.
     * 打印内存限制警告
     * */
    if (server.maxmemory > 0 && server.maxmemory < 1024 * 1024) {
        serverLog(LL_WARNING,
                  "WARNING: You specified a maxmemory value that is less than 1MB (current value is %llu bytes). Are you sure this is what you really want?",
                  server.maxmemory);
    }

    redisSetCpuAffinity(server.server_cpulist);
    setOOMScoreAdj(-1);

    aeMain(server.el);
    // 主循环结束后会运行到这，关闭服务器，删除事件
    aeDeleteEventLoop(server.el);
    return 0;
}

/* The End */
