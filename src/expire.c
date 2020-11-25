/* Implementation of EXPIRE (keys with fixed time to live).
 *
 * ----------------------------------------------------------------------------
 *
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

/*-----------------------------------------------------------------------------
 * Incremental collection of expired keys.
 * (键的过期操作。)
 *
 * When keys are accessed they are expired on-access. However we need a
 * mechanism in order to ensure keys are eventually removed when expired even
 * if no access is performed on them.
 * （当密钥被访问时，它们在访问时过期。但是，我们需要一种机制来确保密钥在过期时最终被删除，即使没有对它们执行访问。）
 *----------------------------------------------------------------------------*/

/* Helper function for the activeExpireCycle() function. (activeExpireCycle()函数的助手函数。)
 * This function will try to expire the key that is stored in the hash table
 * entry 'de' of the 'expires' hash table of a Redis database.  （这个函数将尝试使存储在Redis数据库的‘expires’哈希表条目‘de’中的键过期。）
 *
 * If the key is found to be expired, it is removed from the database and
 * 1 is returned. Otherwise no operation is performed and 0 is returned. (如果发现键过期了，就从数据库中删除它并返回1。否则不执行任何操作，返回0。)
 *
 * When a key is expired, server.stat_expiredkeys is incremented. (当密钥过期时，服务器。stat_expiredkeys递增。)
 *
 * The parameter 'now' is the current time in milliseconds as is passed
 * to the function to avoid too many gettimeofday() syscalls.
 * (参数'now'是传递给函数的当前时间(以毫秒为单位)，以避免过多的gettimeofday()系统调用。)
 *fixme 重要函数: main()函数, nitServerConfig(), initServer(), serverCron()函数, databasesCron()函数, activeExpireCycle()函数
 **/
/**
 * 检查给出的key 是否过期
 * @param db  db 编号
 * @param de key
 * @param now 时间
 * @return
 */
int activeExpireCycleTryExpire(redisDb *db, dictEntry *de, long long now) {
    long long t = dictGetSignedIntegerVal(de);
    if (now > t) {
        sds key = dictGetKey(de);
        robj *keyobj = createStringObject(key, sdslen(key));
        // 通知AOF文件和客户端都删除该主键，
        propagateExpire(db, keyobj, server.lazyfree_lazy_expire);
        if (server.lazyfree_lazy_expire)
            // 删除此主键
            dbAsyncDelete(db, keyobj);
        else
            dbSyncDelete(db, keyobj);
        notifyKeyspaceEvent(NOTIFY_EXPIRED,
                            "expired", keyobj, db->id);
        trackingInvalidateKey(NULL, keyobj);
        decrRefCount(keyobj);
        server.stat_expiredkeys++;
        return 1;
    } else {
        return 0;
    }
}

/* Try to expire a few timed out keys. The algorithm used is adaptive and
 * will use few CPU cycles if there are few expiring keys, otherwise
 * it will get more aggressive to avoid that too much memory is used by
 * keys that can be removed from the keyspace.
 *
 * Every expire cycle tests multiple databases: the next call will start
 * again from the next db, with the exception of exists for time limit: in that
 * case we restart again from the last database we were processing. Anyway
 * no more than CRON_DBS_PER_CALL databases are tested at every iteration.
 *
 * The function can perform more or less work, depending on the "type"
 * argument. It can execute a "fast cycle" or a "slow cycle". The slow
 * cycle is the main way we collect expired cycles: this happens with
 * the "server.hz" frequency (usually 10 hertz).
 *
 * However the slow cycle can exit for timeout, since it used too much time.
 * For this reason the function is also invoked to perform a fast cycle
 * at every event loop cycle, in the beforeSleep() function. The fast cycle
 * will try to perform less work, but will do it much more often.
 *
 * The following are the details of the two expire cycles and their stop
 * conditions:
 *
 * If type is ACTIVE_EXPIRE_CYCLE_FAST the function will try to run a
 * "fast" expire cycle that takes no longer than EXPIRE_FAST_CYCLE_DURATION
 * microseconds, and is not repeated again before the same amount of time.
 * The cycle will also refuse to run at all if the latest slow cycle did not
 * terminate because of a time limit condition.
 *
 * If type is ACTIVE_EXPIRE_CYCLE_SLOW, that normal expire cycle is
 * executed, where the time limit is a percentage of the REDIS_HZ period
 * as specified by the ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC define. In the
 * fast cycle, the check of every database is interrupted once the number
 * of already expired keys in the database is estimated to be lower than
 * a given percentage, in order to avoid doing too much work to gain too
 * little memory.
 *
 * The configured expire "effort" will modify the baseline parameters in
 * order to do more work in both the fast and slow expire cycles.
 */

#define ACTIVE_EXPIRE_CYCLE_KEYS_PER_LOOP 20 /* Keys for each DB loop. */
#define ACTIVE_EXPIRE_CYCLE_FAST_DURATION 1000 /* Microseconds. */
#define ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC 25 /* Max % of CPU to use. */
#define ACTIVE_EXPIRE_CYCLE_ACCEPTABLE_STALE 10 /* % of stale keys after which
                                                   we do extra efforts. */

/**
 * 牵涉到了过期主键淘汰(清除)机制，我将它比作redis的GC。Redis支持主键可以有过期时间，这种机制方便用户使用，比如一些缓存策略，过期的主键会占用大量内存资源，
 * 这就必要要求redis有及时从内存中清理失效主键。
 * Redis的主键淘汰有两种策略：
 *  被动策略（passive way），在访问主键的时候，如果发现主键已经过期，就清除掉。
 *  主动策略（active way），周期性地从过期主键空间中淘汰一部分失效的主键。
 *  本函数则代表着redis过期主键淘汰机制的主动策略，本函数是由时间事件来驱动执行。Redis的淘汰算法是灵活适应的。如果有很少的过期key，就用很少的CPU循环；
 *  否则就会占用更多CPU循环用于淘汰计算，以防止很多可删除键空间占用大量内存。每次迭代DB数目不能超过REDIS_DBCRON_DBS_PER_CALL。
 * 淘汰原理：
 *  遍历服务器每个数据库expires字典，从中尝试着随机抽样不超过ACTIVE_EXPIRE_CYCLE_LOOKUPS_PER_LOOP（20）个限时主键，检查其是否过期，如果过期则删除。
 *  如果过期的限时主键个数占本次抽样个数的比例超过1/4，Redis 会认为当前数据库中的失效主键依然很多，所以它会继续进行下一轮的随机抽样和删除，直到刚才的比例低于25%才停止对当前数据库的处理，
 *  转向下一个数据 库。这里我们需要注意的是，activeExpireCycle函数不会试图一次性处理Redis中的所有数据库，而是最多只处理REDIS_DBCRON_DBS_PER_CALL（默认值为16），
 *  此外 activeExpireCycle 函数还有处理时间上的限制，不是想执行多久就执行多久，凡此种种都只有一个目的，那就是避免失效主键删除占用过多的CPU资源
 *
 *通过以上对 Redis 主键失效机制的介绍，我们知道虽然 Redis 会定期地检查设置了失效时间的主键并删除已经失效的主键，但是通过对每次处理数据库个数的限制、activeExpireCycle
 * 函数在一秒钟内执行次数的限制、分配给activeExpireCycle 函数CPU时间的限制、继续删除主键的失效主键数百分比的限制，Redis 已经大大降低了主键失效机制对系统整体性能的影响，
 * 但是如果在实际应用中出现大量主键在短时间内同时失效的情况还是会使得系统的响应能力降低，所以这种情况无疑应该避免
 * @param type
 *  当 ACTIVE_EXPIRE_CYCLE_FAST时表示快速过期删除。执行快速过期删除有很多限制，当函数activeExpireCycle正在执行时直接返回；当上次执行快速过期键删除的时间小于2000微秒时直接返回。timelimit_exit是声明为static当timelimit_exit的值为1的时候，由此便可通过变量timelimit_exit判断函数activeExpireCycle是否正在执行。变量last_fast_cycle也是声明为static。同时可以看到当执行快速过期删除时，设置函数activeExpireCycle的最大执行时间为1000微秒。
 */
void activeExpireCycle(int type) {
    /* Adjust the running parameters according to the configured expire
     * effort. The default effort is 1, and the maximum configurable effort
     * is 10. */
    unsigned long
            effort = server.active_expire_effort - 1, /* Rescale from 0 to 9. */
            config_keys_per_loop = ACTIVE_EXPIRE_CYCLE_KEYS_PER_LOOP +
                                   ACTIVE_EXPIRE_CYCLE_KEYS_PER_LOOP / 4 * effort,
            config_cycle_fast_duration = ACTIVE_EXPIRE_CYCLE_FAST_DURATION +
                                         ACTIVE_EXPIRE_CYCLE_FAST_DURATION / 4 * effort,
            config_cycle_slow_time_perc = ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC +
                                          2 * effort,
            config_cycle_acceptable_stale = ACTIVE_EXPIRE_CYCLE_ACCEPTABLE_STALE -
                                            effort;

    /* This function has some global state in order to continue the work
     * incrementally across calls. (这个函数具有一些全局状态，以便在调用之间以增量方式继续工作。)
     *注意一下这几个静态变量，current_db,用来保存每次函数调用处理的最后一个Redis数据库的编号，因为activeExpireCycle不是一次性遍历所有数据库，而是渐进式的，下次执行本函数时，
     * 从current_db继续开始处理。timelimit_exit保存上一次调用本函数时，是否到达指定期限(这个期限下文有详细说明)last_fast_cycle用于保存上一次快速循环的执行时间。
     * */
    static unsigned int current_db = 0; /* Last DB tested. 保存每次函数调用处理的最后一个Redis数据库的编号*/
    static int timelimit_exit = 0;      /* Time limit hit in previous call? 保存上一次调用本函数时，是否到达指定期限*/
    static long long last_fast_cycle = 0; /* When last fast cycle ran. 保存上一次快速循环的执行时间*/

    int j, iteration = 0;
    // 每次最多处理DB个数
    int dbs_per_call = CRON_DBS_PER_CALL;
    long long start = ustime(), timelimit, elapsed;

    /* When clients are paused the dataset should be static not just from the
     * POV of clients not being able to write, but also from the POV of
     * expires and evictions of keys not being performed. */
    if (clientsArePaused()) return;

    /**
     * 如果传入参数是ACTIVE_EXPIRE_CYCLE_FAST，则在快速循环执行期间内不再重复执行快速循环，且在距离上次快速循环执行时间小于2倍EXPIRE_FAST_CYCLE_DURATION微秒 则不执行。
     * 如果参数是ACTIVE_EXPIRE_CYCLE_SLOW，则执行普通过期循环。
     */
    if (type == ACTIVE_EXPIRE_CYCLE_FAST) {
        /* Don't start a fast cycle if the previous cycle did not exit
         * for time limit, unless the percentage of estimated stale keys is
         * too high. Also never repeat a fast cycle for the same period
         * as the fast cycle total duration itself. */
        if (!timelimit_exit &&
            server.stat_expired_stale_perc < config_cycle_acceptable_stale)
            return;

        if (start < last_fast_cycle + (long long) config_cycle_fast_duration * 2)
            return;

        last_fast_cycle = start;
    }

    /* We usually should test CRON_DBS_PER_CALL per iteration, with
     * two exceptions:
     *
     * 1) Don't test more DBs than we have.
     * 2) If last time we hit the time limit, we want to scan all DBs
     * in this iteration, as there is work to do in some DB and we don't want
     * expired keys to use memory for too much time.
     * 通常每次迭代要通常每次迭代要检测REDIS_DBCRON_DBS_PER_CALL(16)个db，有两种例外情况：
     *  1）实际数据库数量小于REDIS_DBCRON_DBS_PER_CAL；
     *  2)上一次调用activeExpireCycle()函数到达指定期限，这种情况说明数据库的过期主键很多，占用大量内存，所以需要处理全部数据库。*/
    if (dbs_per_call > server.dbnum || timelimit_exit)
        dbs_per_call = server.dbnum;

    /* We can use at max 'config_cycle_slow_time_perc' percentage of CPU
     * time per iteration. Since this function gets called with a frequency of
     * server.hz times per second, the following is the max amount of
     * microseconds we can spend in this function.
     * 每次迭代执行activeExpireCycle()的指定期限(单位为微秒)，普通循环(参数是ACTIVE_EXPIRE_CYCLE_SLOW)的期限是REDIS_HZ的百分比即(1000000 *(REDIS_EXPIRELOOKUPS_TIME_PERC / 100)) / server.hz；
     * 而快速循环的指定期限是ACTIVE_EXPIRE_CYCLE_FAST_DURATION(1000微秒)。
     * */
    timelimit = config_cycle_slow_time_perc * 1000000 / server.hz / 100;
    timelimit_exit = 0;
    if (timelimit <= 0) timelimit = 1;

    if (type == ACTIVE_EXPIRE_CYCLE_FAST)
        timelimit = config_cycle_fast_duration; /* in microseconds. */

    /* Accumulate some global stats as we expire keys, to have some idea
     * about the number of keys that are already logically expired, but still
     * existing inside the database. */
    long total_sampled = 0;
    long total_expired = 0;
    // 循环遍历固定数目的DB
    for (j = 0; j < dbs_per_call && timelimit_exit == 0; j++) {
        /* Expired and checked in a single loop. */
        unsigned long expired, sampled;

        redisDb *db = server.db + (current_db % server.dbnum);

        /* Increment the DB now so we are sure if we run out of time
         * in the current DB we'll restart from the next. This allows to
         * distribute the time evenly across DBs. (增加数据库现在，所以我们确定，如果我们用完了当前数据库的时间，我们将从下一个重新启动。这允许在DBs中均匀地分配时间。)
         * 此处立刻就将current_db加一，这样可以保证即使这次无法在时间限制内删除完所有当前数据库中的失效主键，下一次调用activeExpireCycle一样会从下一个数据库开始处理，
         * 从而保证每个数据库都有被处理的机会。*/
        current_db++;

        /* Continue to expire if at the end of the cycle there are still
         * a big percentage of keys to expire, compared to the number of keys
         * we scanned. The percentage, stored in config_cycle_acceptable_stale
         * is not fixed, but depends on the Redis configured "expire effort".
         * 这个循环处理当前数据库中的失效主键，直到过期的限时主键个数占本次抽样个数(20)的比例小于等于1/4，则不重复循环.
         * */
        do {
            unsigned long num, slots;
            long long now, ttl_sum;
            int ttl_samples;
            iteration++;

            /* If there is nothing to expire try next DB ASAP.
             * 如果expires字典表大小为0，说明该数据库中没有设置失效时间的主键，直接检查下一数据库。
             * */
            if ((num = dictSize(db->expires)) == 0) {
                db->avg_ttl = 0;
                break;
            }
            // 记录当前数据库限时主键hash的桶数
            slots = dictSlots(db->expires);
            // 记录当前时间
            now = mstime();

            /* When there are less than 1% filled slots, sampling the key
             * space is expensive, so stop here waiting for better times...
             * The dictionary will be resized asap.
             * 如果expires字典表不为空，但是其填充率不足1%，那么随机选择主键进行检查的代价会很高，所以这里直接检查下一数据库。
             * */
            if (num && slots > DICT_HT_INITIAL_SIZE &&
                (num * 100 / slots < 1))
                break;

            /* The main collection cycle. Sample random keys among keys
             * with an expire set, checking for expired ones. */
            expired = 0;
            sampled = 0;
            ttl_sum = 0;
            ttl_samples = 0;
            // 如果expires字典表中的entry个数大于ACTIVE_EXPIRE_CYCLE_LOOKUPS_PER_LOOP，抽样次数为ACTIVE_EXPIRE_CYCLE_LOOKUPS_PER_LOOP。
            if (num > config_keys_per_loop)
                num = config_keys_per_loop;

            /* Here we access the low level representation of the hash table
             * for speed concerns: this makes this code coupled with dict.c,
             * but it hardly changed in ten years.
             *
             * Note that certain places of the hash table may be empty,
             * so we want also a stop condition about the number of
             * buckets that we scanned. However scanning for free buckets
             * is very fast: we are in the cache line scanning a sequential
             * array of NULL pointers, so we can scan a lot more buckets
             * than keys in the same time. */
            long max_buckets = num * 20;
            long checked_buckets = 0;

            while (sampled < num && checked_buckets < max_buckets) {
                for (int table = 0; table < 2; table++) {
                    if (table == 1 && !dictIsRehashing(db->expires)) break;

                    unsigned long idx = db->expires_cursor;
                    idx &= db->expires->ht[table].sizemask;
                    dictEntry *de = db->expires->ht[table].table[idx];
                    long long ttl;

                    /* Scan the current bucket of the current table. */
                    checked_buckets++;
                    while (de) {
                        /* Get the next entry now since this entry may get
                         * deleted. */
                        dictEntry *e = de;
                        de = de->next;
                        // 随机获取一个设置了失效时间的主键
                        ttl = dictGetSignedIntegerVal(e) - now;
                        // 检查是否过期
                        if (activeExpireCycleTryExpire(db, e, now)) {
                            // 如果过期，则增加统计
                            expired++;
                        }
                        if (ttl > 0) {
                            /* We want the average TTL of keys yet
                             * not expired. */
                            ttl_sum += ttl;
                            ttl_samples++;
                        }
                        sampled++;
                    }
                }
                db->expires_cursor++;
            }
            total_expired += expired;
            total_sampled += sampled;

            /* Update the average TTL stats for this database.
             * 给DB更新平均TTL状态
             * */
            if (ttl_samples) {
                long long avg_ttl = ttl_sum / ttl_samples;

                /* Do a simple running average with a few samples.
                 * We just use the current estimate with a weight of 2%
                 * and the previous estimate with a weight of 98%. */
                if (db->avg_ttl == 0) db->avg_ttl = avg_ttl;
                db->avg_ttl = (db->avg_ttl / 50) * 49 + (avg_ttl / 50);
            }

            /* We can't block forever here even if there are many keys to
             * expire. So after a given amount of milliseconds return to the
             * caller waiting for the other active expire cycle. */
            if ((iteration & 0xf) ==
                0) { /* check once every 16 iterations.这句主要是检测是否到了16次抽样，每经过16次抽样就判断一次执行时间是否已经达到指定时间限制，如果已达到时间限制，则把timelimit_exit这个静态变量设置为1，并退出函数。 */
                elapsed = ustime() - start;
                if (elapsed > timelimit) {
                    timelimit_exit = 1;
                    server.stat_expired_time_cap_reached_count++;
                    break;
                }
            }
            /* We don't repeat the cycle for the current database if there are
             * an acceptable amount of stale keys (logically expired but yet
             * not reclaimed). */
        } while (sampled == 0 ||
                 (expired * 100 / sampled) > config_cycle_acceptable_stale);
    }

    elapsed = ustime() - start;
    server.stat_expire_cycle_time_used += elapsed;
    latencyAddSampleIfNeeded("expire-cycle", elapsed / 1000);

    /* Update our estimate of keys existing but yet to be expired.
     * Running average with this sample accounting for 5%. */
    double current_perc;
    if (total_sampled) {
        current_perc = (double) total_expired / total_sampled;
    } else
        current_perc = 0;
    server.stat_expired_stale_perc = (current_perc * 0.05) +
                                     (server.stat_expired_stale_perc * 0.95);
}

/*-----------------------------------------------------------------------------
 * Expires of keys created in writable slaves
 *
 * Normally slaves do not process expires: they wait the masters to synthesize
 * DEL operations in order to retain consistency. However writable slaves are
 * an exception: if a key is created in the slave and an expire is assigned
 * to it, we need a way to expire such a key, since the master does not know
 * anything about such a key.
 *
 * In order to do so, we track keys created in the slave side with an expire
 * set, and call the expireSlaveKeys() function from time to time in order to
 * reclaim the keys if they already expired.
 *
 * Note that the use case we are trying to cover here, is a popular one where
 * slaves are put in writable mode in order to compute slow operations in
 * the slave side that are mostly useful to actually read data in a more
 * processed way. Think at sets intersections in a tmp key, with an expire so
 * that it is also used as a cache to avoid intersecting every time.
 *
 * This implementation is currently not perfect but a lot better than leaking
 * the keys as implemented in 3.2.
 *----------------------------------------------------------------------------*/

/* The dictionary where we remember key names and database ID of keys we may
 * want to expire from the slave. Since this function is not often used we
 * don't even care to initialize the database at startup. We'll do it once
 * the feature is used the first time, that is, when rememberSlaveKeyWithExpire()
 * is called.
 *
 * The dictionary has an SDS string representing the key as the hash table
 * key, while the value is a 64 bit unsigned integer with the bits corresponding
 * to the DB where the keys may exist set to 1. Currently the keys created
 * with a DB id > 63 are not expired, but a trivial fix is to set the bitmap
 * to the max 64 bit unsigned value when we know there is a key with a DB
 * ID greater than 63, and check all the configured DBs in such a case. */
dict *slaveKeysWithExpire = NULL;

/* Check the set of keys created by the master with an expire set in order to
 * check if they should be evicted. */
void expireSlaveKeys(void) {
    if (slaveKeysWithExpire == NULL ||
        dictSize(slaveKeysWithExpire) == 0)
        return;

    int cycles = 0, noexpire = 0;
    mstime_t start = mstime();
    while (1) {
        dictEntry *de = dictGetRandomKey(slaveKeysWithExpire);
        sds keyname = dictGetKey(de);
        uint64_t dbids = dictGetUnsignedIntegerVal(de);
        uint64_t new_dbids = 0;

        /* Check the key against every database corresponding to the
         * bits set in the value bitmap. */
        int dbid = 0;
        while (dbids && dbid < server.dbnum) {
            if ((dbids & 1) != 0) {
                redisDb *db = server.db + dbid;
                dictEntry *expire = dictFind(db->expires, keyname);
                int expired = 0;

                if (expire &&
                    activeExpireCycleTryExpire(server.db + dbid, expire, start)) {
                    expired = 1;
                }

                /* If the key was not expired in this DB, we need to set the
                 * corresponding bit in the new bitmap we set as value.
                 * At the end of the loop if the bitmap is zero, it means we
                 * no longer need to keep track of this key. */
                if (expire && !expired) {
                    noexpire++;
                    new_dbids |= (uint64_t) 1 << dbid;
                }
            }
            dbid++;
            dbids >>= 1;
        }

        /* Set the new bitmap as value of the key, in the dictionary
         * of keys with an expire set directly in the writable slave. Otherwise
         * if the bitmap is zero, we no longer need to keep track of it. */
        if (new_dbids)
            dictSetUnsignedIntegerVal(de, new_dbids);
        else
            dictDelete(slaveKeysWithExpire, keyname);

        /* Stop conditions: found 3 keys we cna't expire in a row or
         * time limit was reached. */
        cycles++;
        if (noexpire > 3) break;
        if ((cycles % 64) == 0 && mstime() - start > 1) break;
        if (dictSize(slaveKeysWithExpire) == 0) break;
    }
}

/* Track keys that received an EXPIRE or similar command in the context
 * of a writable slave. */
void rememberSlaveKeyWithExpire(redisDb *db, robj *key) {
    if (slaveKeysWithExpire == NULL) {
        static dictType dt = {
                dictSdsHash,                /* hash function */
                NULL,                       /* key dup */
                NULL,                       /* val dup */
                dictSdsKeyCompare,          /* key compare */
                dictSdsDestructor,          /* key destructor */
                NULL                        /* val destructor */
        };
        slaveKeysWithExpire = dictCreate(&dt, NULL);
    }
    if (db->id > 63) return;

    dictEntry *de = dictAddOrFind(slaveKeysWithExpire, key->ptr);
    /* If the entry was just created, set it to a copy of the SDS string
     * representing the key: we don't want to need to take those keys
     * in sync with the main DB. The keys will be removed by expireSlaveKeys()
     * as it scans to find keys to remove. */
    if (de->key == key->ptr) {
        de->key = sdsdup(key->ptr);
        dictSetUnsignedIntegerVal(de, 0);
    }

    uint64_t dbids = dictGetUnsignedIntegerVal(de);
    dbids |= (uint64_t) 1 << db->id;
    dictSetUnsignedIntegerVal(de, dbids);
}

/* Return the number of keys we are tracking. */
size_t getSlaveKeyWithExpireCount(void) {
    if (slaveKeysWithExpire == NULL) return 0;
    return dictSize(slaveKeysWithExpire);
}

/* Remove the keys in the hash table. We need to do that when data is
 * flushed from the server. We may receive new keys from the master with
 * the same name/db and it is no longer a good idea to expire them.
 *
 * Note: technically we should handle the case of a single DB being flushed
 * but it is not worth it since anyway race conditions using the same set
 * of key names in a wriatable slave and in its master will lead to
 * inconsistencies. This is just a best-effort thing we do. */
void flushSlaveKeysWithExpireList(void) {
    if (slaveKeysWithExpire) {
        dictRelease(slaveKeysWithExpire);
        slaveKeysWithExpire = NULL;
    }
}

int checkAlreadyExpired(long long when) {
    /* EXPIRE with negative TTL, or EXPIREAT with a timestamp into the past
     * should never be executed as a DEL when load the AOF or in the context
     * of a slave instance.
     *
     * Instead we add the already expired key to the database with expire time
     * (possibly in the past) and wait for an explicit DEL from the master. */
    return (when <= mstime() && !server.loading && !server.masterhost);
}

/*-----------------------------------------------------------------------------
 * Expires Commands
 *----------------------------------------------------------------------------*/

/* This is the generic command implementation for EXPIRE, PEXPIRE, EXPIREAT
 * and PEXPIREAT. Because the commad second argument may be relative or absolute
 * the "basetime" argument is used to signal what the base time is (either 0
 * for *AT variants of the command, or the current time for relative expires).
 *
 * unit is either UNIT_SECONDS or UNIT_MILLISECONDS, and is only used for
 * the argv[2] parameter. The basetime is always specified in milliseconds. */
void expireGenericCommand(client *c, long long basetime, int unit) {
    robj *key = c->argv[1], *param = c->argv[2];
    long long when; /* unix time in milliseconds when the key will expire. */

    if (getLongLongFromObjectOrReply(c, param, &when, NULL) != C_OK)
        return;

    if (unit == UNIT_SECONDS) when *= 1000;
    when += basetime;

    /* No key, return zero. */
    if (lookupKeyWrite(c->db, key) == NULL) {
        addReply(c, shared.czero);
        return;
    }

    if (checkAlreadyExpired(when)) {
        robj *aux;

        int deleted = server.lazyfree_lazy_expire ? dbAsyncDelete(c->db, key) :
                      dbSyncDelete(c->db, key);
        serverAssertWithInfo(c, key, deleted);
        server.dirty++;

        /* Replicate/AOF this as an explicit DEL or UNLINK. */
        aux = server.lazyfree_lazy_expire ? shared.unlink : shared.del;
        rewriteClientCommandVector(c, 2, aux, key);
        signalModifiedKey(c, c->db, key);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, c->db->id);
        addReply(c, shared.cone);
        return;
    } else {
        setExpire(c, c->db, key, when);
        addReply(c, shared.cone);
        signalModifiedKey(c, c->db, key);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "expire", key, c->db->id);
        server.dirty++;
        return;
    }
}

/* EXPIRE key seconds */
void expireCommand(client *c) {
    expireGenericCommand(c, mstime(), UNIT_SECONDS);
}

/* EXPIREAT key time */
void expireatCommand(client *c) {
    expireGenericCommand(c, 0, UNIT_SECONDS);
}

/* PEXPIRE key milliseconds */
void pexpireCommand(client *c) {
    expireGenericCommand(c, mstime(), UNIT_MILLISECONDS);
}

/* PEXPIREAT key ms_time */
void pexpireatCommand(client *c) {
    expireGenericCommand(c, 0, UNIT_MILLISECONDS);
}

/* Implements TTL and PTTL */
void ttlGenericCommand(client *c, int output_ms) {
    long long expire, ttl = -1;

    /* If the key does not exist at all, return -2 */
    if (lookupKeyReadWithFlags(c->db, c->argv[1], LOOKUP_NOTOUCH) == NULL) {
        addReplyLongLong(c, -2);
        return;
    }
    /* The key exists. Return -1 if it has no expire, or the actual
     * TTL value otherwise. */
    expire = getExpire(c->db, c->argv[1]);
    if (expire != -1) {
        ttl = expire - mstime();
        if (ttl < 0) ttl = 0;
    }
    if (ttl == -1) {
        addReplyLongLong(c, -1);
    } else {
        addReplyLongLong(c, output_ms ? ttl : ((ttl + 500) / 1000));
    }
}

/* TTL key */
void ttlCommand(client *c) {
    ttlGenericCommand(c, 0);
}

/* PTTL key */
void pttlCommand(client *c) {
    ttlGenericCommand(c, 1);
}

/* PERSIST key */
void persistCommand(client *c) {
    if (lookupKeyWrite(c->db, c->argv[1])) {
        if (removeExpire(c->db, c->argv[1])) {
            signalModifiedKey(c, c->db, c->argv[1]);
            notifyKeyspaceEvent(NOTIFY_GENERIC, "persist", c->argv[1], c->db->id);
            addReply(c, shared.cone);
            server.dirty++;
        } else {
            addReply(c, shared.czero);
        }
    } else {
        addReply(c, shared.czero);
    }
}

/* TOUCH key1 [key2 key3 ... keyN] */
void touchCommand(client *c) {
    int touched = 0;
    for (int j = 1; j < c->argc; j++)
        if (lookupKeyRead(c->db, c->argv[j]) != NULL) touched++;
    addReplyLongLong(c, touched);
}

