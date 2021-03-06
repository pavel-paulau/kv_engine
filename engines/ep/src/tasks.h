/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#pragma once

#include "config.h"

#include "globaltask.h"

#include <platform/processclock.h>

#include <array>
#include <string>

class EPBucket;
class EventuallyPersistentEngine;

/**
 * A task for persisting items to disk.
 */
class Flusher;
class FlusherTask : public GlobalTask {
public:
    FlusherTask(EventuallyPersistentEngine *e, Flusher* f, uint16_t shardid,
                bool completeBeforeShutdown = true)
        : GlobalTask(e, TaskId::FlusherTask, 0, completeBeforeShutdown),
          flusher(f) {
        std::stringstream ss;
        ss<<"Running a flusher loop: shard "<<shardid;
        desc = ss.str();
    }

    bool run();

    cb::const_char_buffer getDescription() {
        return desc;
    }

    std::chrono::microseconds maxExpectedDuration() {
        // Flusher duration is likely to vary significantly; depending on
        // number of documents to flush and speed/capacity of disk subsystem.
        // As such, selecting a good maximum duration for all scenarios is hard.
        // Choose a relatively generous value of 400ms - this should record
        // any significantly slow executions without creating too much log
        // noise.
        return std::chrono::milliseconds(400);
    }

private:
    Flusher* flusher;
    std::string desc;
};

/**
 * A task for compacting a vbucket db file
 */
class CompactTask : public GlobalTask {
public:
    CompactTask(EPBucket& bucket,
                compaction_ctx c,
                const void* ck,
                bool completeBeforeShutdown = false);

    bool run();

    cb::const_char_buffer getDescription() {
        return desc;
    }

    std::chrono::microseconds maxExpectedDuration() {
        // Empirical evidence suggests this task runs under 25s 99.98% of
        // the time.
        return std::chrono::seconds(25);
    }

private:
    EPBucket& bucket;
    compaction_ctx compactCtx;
    const void* cookie;
    std::string desc;
};

/**
 * A task that periodically takes a snapshot of the stats and persists them to
 * disk.
 */
class StatSnap : public GlobalTask {
public:
    StatSnap(EventuallyPersistentEngine *e, bool runOneTimeOnly = false,
             bool sleeptime = 0, bool completeBeforeShutdown = false)
        : GlobalTask(e, TaskId::StatSnap, sleeptime, completeBeforeShutdown),
          runOnce(runOneTimeOnly) {}

    bool run();

    cb::const_char_buffer getDescription() {
        return "Updating stat snapshot on disk";
    }

    std::chrono::microseconds maxExpectedDuration() {
        // A background periodic Writer task; which no front-end operation
        // depends on. However it does run on a writer thread; which we don't
        // want to slow down persistTo times; so expect to complete quickly.
        // p99.9 at 250ms.
        // TODO: Consider moving this to AuxIO?
        return std::chrono::milliseconds(250);
    }

private:
    bool runOnce;
};

/**
 * A task for fetching items from disk.
 * This task is used if EPBucket::multiBGFetchEnabled is true.
 */
class BgFetcher;
class MultiBGFetcherTask : public GlobalTask {
public:
    MultiBGFetcherTask(EventuallyPersistentEngine* e, BgFetcher* b);

    bool run();

    cb::const_char_buffer getDescription() {
        return "Batching background fetch";
    }

    std::chrono::microseconds maxExpectedDuration() {
        // Much like other disk tasks (e.g. Flusher), duration is likely to
        // vary significantly; depending on number of documents to fetch and
        // speed/capacity of disk subsystem. As such, selecting a good maximum
        // duration for all scenarios is hard.
        // Choose a relatively generous value of 700ms - this should record
        // any significantly slow executions without creating too much log
        // noise.
        return std::chrono::milliseconds(700);
    }

private:
    BgFetcher *bgfetcher;
};

/**
 * A task that performs the bucket flush operation.
 */
class DeleteAllTask : public GlobalTask {
public:
    DeleteAllTask(EventuallyPersistentEngine* e)
        : GlobalTask(e, TaskId::FlushAllTask, 0, false) {
    }

    bool run();

    std::chrono::microseconds maxExpectedDuration() {
        // Flushing the entire Bucket can take a non-trivial amount of time;
        // moreover it's a relatively rare event so there is limited historical
        // information on it's typical runtime.
        // Selecting 10s here as an expected duration - this could be low,
        // but the relative (in)frequency of running this shouldn't pollute
        // the logs too much even if it is too low.
        return std::chrono::seconds(10);
    }

    cb::const_char_buffer getDescription() {
        return "Performing flush_all operation.";
    }
};

/**
 * A task for performing disk fetches for "stats vkey".
 */
class VKeyStatBGFetchTask : public GlobalTask {
public:
    VKeyStatBGFetchTask(EventuallyPersistentEngine *e, const DocKey& k,
                        uint16_t vbid, uint64_t s, const void *c, int sleeptime = 0,
                        bool completeBeforeShutdown = false)
        : GlobalTask(e, TaskId::VKeyStatBGFetchTask, sleeptime, completeBeforeShutdown),
          key(k),
          vbucket(vbid),
          bySeqNum(s),
          cookie(c),
          description("Fetching item from disk for vkey stat: key{" +
                      std::string(key.c_str()) + "} vb:" +
                      std::to_string(vbucket)) {
    }

    bool run();

    cb::const_char_buffer getDescription() {
        return description;
    }

    std::chrono::microseconds maxExpectedDuration() {
        // Much like other disk tasks, duration is likely to
        // vary significantly; depending on speed/capacity of disk subsystem.
        // As such, selecting a good maximum duration for all scenarios is hard.
        // Choose a relatively generous value of 250ms - this should record
        // any significantly slow executions without creating too much log
        // noise.
        return std::chrono::milliseconds(250);
    }

private:
    const StoredDocKey key;
    const uint16_t vbucket;
    uint64_t                         bySeqNum;
    const void                      *cookie;
    const std::string description;
};

/**
 * A task that performs disk fetches for non-resident get requests.
 * This task is used if EPBucket::multiBGFetchEnabled is false.
 */
class SingleBGFetcherTask : public GlobalTask {
public:
    SingleBGFetcherTask(EventuallyPersistentEngine* e,
                        const DocKey& k,
                        uint16_t vbid,
                        const void* c,
                        bool isMeta,
                        int sleeptime = 0,
                        bool completeBeforeShutdown = false)
        : GlobalTask(e,
                     TaskId::SingleBGFetcherTask,
                     sleeptime,
                     completeBeforeShutdown),
          key(k),
          vbucket(vbid),
          cookie(c),
          metaFetch(isMeta),
          init(ProcessClock::now()),
          description("Fetching item from disk: key{" +
                      std::string(key.c_str()) + "}, vb:" +
                      std::to_string(vbucket)) {
    }

    bool run();

    cb::const_char_buffer getDescription() {
        return description;
    }

    std::chrono::microseconds maxExpectedDuration() {
        // Much like other disk tasks, duration is likely to
        // vary significantly; depending on speed/capacity of disk subsystem.
        // As such, selecting a good maximum duration for all scenarios is hard.
        // Choose a relatively generous value of 250ms - this should record
        // any significantly slow executions without creating too much log
        // noise.
        return std::chrono::milliseconds(250);
    }

private:
    const StoredDocKey key;
    const uint16_t vbucket;
    const void*                cookie;
    bool                       metaFetch;
    ProcessClock::time_point   init;
    const std::string description;
};

/**
 * A task that monitors if a bucket is read-heavy, write-heavy, or mixed.
 */
class WorkLoadMonitor : public GlobalTask {
public:
    WorkLoadMonitor(EventuallyPersistentEngine *e,
                    bool completeBeforeShutdown = false);

    bool run();

    std::chrono::microseconds maxExpectedDuration() {
        // Runtime should be very quick (lookup a few statistics; perform
        // some calculation on them). p99.9 is <50us.
        return std::chrono::milliseconds(1);
    }

    cb::const_char_buffer getDescription() {
        return "Monitoring a workload pattern";
    }

private:

    size_t getNumMutations();
    size_t getNumGets();

    size_t prevNumMutations;
    size_t prevNumGets;
};
