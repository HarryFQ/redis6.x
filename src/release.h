#ifndef __RELEASE_H
#define __RELEASE_H

#include <stdint.h>
#include <stdio.h>

#define REDIS_GIT_SHA1 "redis_version_ec70d0de"
#define REDIS_GIT_DIRTY "181812"

#define REDIS_BUILD_ID "181812"
char *redisGitSHA1(void);

char *redisGitDirty(void);

uint64_t redisBuildId(void);

#endif