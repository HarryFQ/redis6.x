/* Hash Tables Implementation.
 *
 * This file implements in-memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto-resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include <stdint.h>

#ifndef __DICT_H
#define __DICT_H

#define DICT_OK 0
#define DICT_ERR 1

/* Unused arguments generate annoying warnings... */
#define DICT_NOTUSED(V) ((void) V)

/* dictEntry就是一个key-value对 */
typedef struct dictEntry {
    void *key;
    union {
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v;
    struct dictEntry *next;
} dictEntry;

/* 每一个dict都有一个dictType，特定于dict的操作的集合。比如，不同类型的dict，其比较key的方式可能会是不一样的。 */
typedef struct dictType {
    uint64_t (*hashFunction)(const void *key);

    void *(*keyDup)(void *privdata, const void *key);

    void *(*valDup)(void *privdata, const void *obj);

    int (*keyCompare)(void *privdata, const void *key1, const void *key2);

    void (*keyDestructor)(void *privdata, void *key);

    void (*valDestructor)(void *privdata, void *obj);
} dictType;

/* This is our hash table structure. Every dictionary has two of this as we
 * implement incremental rehashing, for the old to the new table.  (这就是我们的哈希表结构。每个字典都有两个这样的词实现从旧表到新表的增量重散列。)
 **/
typedef struct dictht {
    dictEntry **table;  /* 指针数组，做为hash table的bucket */
    unsigned long size; /* table数组的大小，看起来在实现中一直会是2的倍数 */
    unsigned long sizemask; /* 为了方便操作记录mask */
    unsigned long used; /* 已经使用的bucket个数 */
} dictht;

/*
 * 关键数据结构dict的定义
 * 对于ht[2]这个成员，从实现上看，可能是出于实时的考虑，rehash过程(可能)需要分好几次才全部完成。
 * 那么在rehash过程中，需要一个额外的表做临时存储。
 * rehash的过程，总是从ht[0]往ht[1]迁移数据。迁移过程中，两个表都有数据。
 */
typedef struct dict {
    dictType *dict; /* 特定于dict的操作集合 */
    void *privdata; /* 特定于dict的数据 */
    dictht ht[2];    /* 两个hash表 */
    long rehashidx; /* rehashing not in progress if rehashidx == -1 */
    unsigned long iterators; /* number of iterators currently running */
} dict;

/* If safe is set to 1 this is a safe iterator, that means, you can call
 * dictAdd, dictFind, and other functions against the dictionary even while
 * iterating. Otherwise it is a non safe iterator, and only dictNext()
 * should be called while iterating. */
typedef struct dictIterator {
    dict *d;
    long index;
    int table, safe;
    dictEntry *entry, *nextEntry;
    /* unsafe iterator fingerprint for misuse detection. */
    long long fingerprint;
} dictIterator;

typedef void (dictScanFunction)(void *privdata, const dictEntry *de);

typedef void (dictScanBucketFunction)(void *privdata, dictEntry **bucketref);

/* This is the initial size of every hash table */
#define DICT_HT_INITIAL_SIZE     4

/* ------------------------------- Macros ------------------------------------*/
#define dictFreeVal(d, entry) \
    if ((d)->type->valDestructor) \
        (d)->type->valDestructor((d)->privdata, (entry)->v.val)

#define dictSetVal(d, entry, _val_) do { \
    if ((d)->type->valDup) \
        (entry)->v.val = (d)->type->valDup((d)->privdata, _val_); \
    else \
        (entry)->v.val = (_val_); \
} while(0)

#define dictSetSignedIntegerVal(entry, _val_) \
    do { (entry)->v.s64 = _val_; } while(0)

#define dictSetUnsignedIntegerVal(entry, _val_) \
    do { (entry)->v.u64 = _val_; } while(0)

#define dictSetDoubleVal(entry, _val_) \
    do { (entry)->v.d = _val_; } while(0)

#define dictFreeKey(d, entry) \
    if ((d)->type->keyDestructor) \
        (d)->type->keyDestructor((d)->privdata, (entry)->key)

#define dictSetKey(d, entry, _key_) do { \
    if ((d)->type->keyDup) \
        (entry)->key = (d)->type->keyDup((d)->privdata, _key_); \
    else \
        (entry)->key = (_key_); \
} while(0)

#define dictCompareKeys(d, key1, key2) \
    (((d)->type->keyCompare) ? \
        (d)->type->keyCompare((d)->privdata, key1, key2) : \
        (key1) == (key2))

#define dictHashKey(d, key) (d)->type->hashFunction(key)
#define dictGetKey(he) ((he)->key)
#define dictGetVal(he) ((he)->v.val)
#define dictGetSignedIntegerVal(he) ((he)->v.s64)
#define dictGetUnsignedIntegerVal(he) ((he)->v.u64)
#define dictGetDoubleVal(he) ((he)->v.d)
#define dictSlots(d) ((d)->ht[0].size+(d)->ht[1].size)
#define dictSize(d) ((d)->ht[0].used+(d)->ht[1].used)
#define dictIsRehashing(d) ((d)->rehashidx != -1)

/* API */
dict *dictCreate(dictType *type, void *privDataPtr);

int dictExpand(dict *d, unsigned long size);

int dictAdd(dict *d, void *key, void *val);

dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing);

dictEntry *dictAddOrFind(dict *d, void *key);

int dictReplace(dict *d, void *key, void *val);

int dictDelete(dict *d, const void *key);

dictEntry *dictUnlink(dict *ht, const void *key);

void dictFreeUnlinkedEntry(dict *d, dictEntry *he);

void dictRelease(dict *d);

dictEntry *dictFind(dict *d, const void *key);

void *dictFetchValue(dict *d, const void *key);

int dictResize(dict *d);

dictIterator *dictGetIterator(dict *d);

dictIterator *dictGetSafeIterator(dict *d);

dictEntry *dictNext(dictIterator *iter);

void dictReleaseIterator(dictIterator *iter);

dictEntry *dictGetRandomKey(dict *d);

dictEntry *dictGetFairRandomKey(dict *d);

unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count);

void dictGetStats(char *buf, size_t bufsize, dict *d);

uint64_t dictGenHashFunction(const void *key, int len);

uint64_t dictGenCaseHashFunction(const unsigned char *buf, int len);

void dictEmpty(dict *d, void(callback)(void *));

void dictEnableResize(void);

void dictDisableResize(void);

int dictRehash(dict *d, int n);

int dictRehashMilliseconds(dict *d, int ms);

void dictSetHashFunctionSeed(uint8_t *seed);

uint8_t *dictGetHashFunctionSeed(void);

unsigned long
dictScan(dict *d, unsigned long v, dictScanFunction *fn, dictScanBucketFunction *bucketfn, void *privdata);

uint64_t dictGetHash(dict *d, const void *key);

dictEntry **dictFindEntryRefByPtrAndHash(dict *d, const void *oldptr, uint64_t hash);

/* Hash table types */
extern dictType dictTypeHeapStringCopyKey;
extern dictType dictTypeHeapStrings;
extern dictType dictTypeHeapStringCopyKeyValue;

#endif /* __DICT_H */
