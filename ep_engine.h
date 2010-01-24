/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "locks.hh"
#include "ep.hh"
#include "sqlite-kvstore.hh"

#include <map>
#include <list>

extern "C" {
    EXPORT_FUNCTION
    ENGINE_ERROR_CODE create_instance(uint64_t interface,
                                      GET_SERVER_API get_server_api,
                                      ENGINE_HANDLE **handle);

    static const char *EvpItemGetKey(const item *it);
    static char *EvpItemGetData(const item *it);
}

#define CMD_STOP_PERSISTENCE  0x80
#define CMD_START_PERSISTENCE 0x81

/**
 * The Item structure we use to pass information between the memcached
 * core and the backend. Please note that the kvstore don't store these
 * objects, so we do have an extra layer of memory copying :(
 */
class Item : public item {
private:
    friend class EventuallyPersistentEngine;
    friend const char *EvpItemGetKey(const item *it);
    friend char *EvpItemGetData(const item *it);
    friend class TapConnection;

    Item(const void* k, const size_t nk, const size_t nb,
         const int fl, const rel_time_t exp) :
        key(static_cast<const char*>(k), nk)
    {
        nkey = static_cast<uint16_t>(nk);
        nbytes = static_cast<uint32_t>(nb);
        flags = fl;
        iflag = 0;
        exptime = exp;
        data = new char[nbytes];
    }

    Item(const std::string &k, const int fl, const rel_time_t exp,
         const void *dta, const size_t nb) :
        key(k)
    {
        nkey = static_cast<uint16_t>(key.length());
        nbytes = static_cast<uint32_t>(nb);
        flags = fl;
        iflag = 0;
        exptime = exp;
        data = new char[nbytes];
        memcpy(data, dta, nbytes);
    }

    ~Item() {
        delete []data;
    }

    Item* clone() {
        return new Item(key, flags, exptime, data, nbytes);
    }

    std::string key;
    char *data;
};

/**
 * I don't care about the set callbacks right now, but since this is a
 * demo, let's dump core if one of them fails so that we can debug it later
 */
class IgnoreCallback : public Callback<bool>
{
    virtual void callback(bool &value) {
        assert(value);
    }
};

/**
 * Class used by the EventuallyPersistentEngine to keep track of all
 * information needed per Tap connection.
 */
class TapConnection {
friend class EventuallyPersistentEngine;
private:
    /**
     * Add a new item to the tap queue.
     * @return true if the the queue was empty
     */
    bool addEvent(Item *it) {
        return addEvent(it->key);
    }

    /**
     * Add a key to the tap queue.
     * @return true if the the queue was empty
     */
    bool addEvent(std::string &key) {
        bool ret = queue.empty();
        /* @todo don't insert the key if it's already in the list! */
        queue.push_back(key);
        return ret;
    }

    std::string next() {
        assert(!empty());
        std::string key = queue.front();
        queue.pop_front();
        ++recordsFetched;
        return key;
    }

    bool empty() {
        return queue.empty();
    }

    void flush() {
        pendingFlush = true;
        /* No point of keeping the rep queue when someone wants to flush it */
        queue.clear();
    }

    bool shouldFlush() {
        bool ret = pendingFlush;
        pendingFlush = false;
        return ret;
    }

    TapConnection(const char *n) : client(n) {
        recordsFetched = 0;
        pendingFlush = false;
    }

    /**
     * String used to identify the client.
     * @todo design the connect packet and fill inn som info here
     */
    std::string client;
    /**
     * The queue of keys that needs to be sent (this is the "live stream")
     */
    std::list<std::string> queue;
    /**
     * Counter of the number of records fetched from this stream since the
     * beginning
     */
    size_t recordsFetched;
    /**
     * Do we have a pending flush command?
     */
    bool pendingFlush;

    DISALLOW_COPY_AND_ASSIGN(TapConnection);
};

/**
 *
 */
class EventuallyPersistentEngine : public ENGINE_HANDLE_V1 {
public:
    ENGINE_ERROR_CODE initialize(const char* config)
    {
        ENGINE_ERROR_CODE ret = ENGINE_SUCCESS;

        if (config != NULL) {
            char *dbn = NULL;
            const int max_items = 4;
            struct config_item items[max_items];
            int ii = 0;
            memset(items, 0, sizeof(items));

            items[ii].key = "dbname";
            items[ii].datatype = DT_STRING;
            items[ii].value.dt_string = &dbn;

            ++ii;
            items[ii].key = "warmup";
            items[ii].datatype = DT_BOOL;
            items[ii].value.dt_bool = &warmup;

            ++ii;
            items[ii].key = "config_file";
            items[ii].datatype = DT_CONFIGFILE;

            ++ii;
            items[ii].key = NULL;

            if (serverApi.parse_config(config, items, stderr) != 0) {
                ret = ENGINE_FAILED;
            } else {
                if (dbn != NULL) {
                    dbname = dbn;
                }
            }
        }

        if (ret == ENGINE_SUCCESS) {
            sqliteDb = new Sqlite3(dbname);
            backend = epstore = new EventuallyPersistentStore(sqliteDb);

            if (backend == NULL) {
                ret = ENGINE_ENOMEM;
            } else {
                if (warmup) {
                    loadDatabase();
                } else {
                    backend->reset();
                }
            }
        }

        return ret;
    }

    void destroy()
    {
        // empty
    }

    ENGINE_ERROR_CODE itemAllocate(const void* cookie,
                                   item** item,
                                   const void* key,
                                   const size_t nkey,
                                   const size_t nbytes,
                                   const int flags,
                                   const rel_time_t exptime)
    {
        (void)cookie;
        *item = new Item(key, nkey, nbytes, flags, exptime);
        if (*item == NULL) {
            return ENGINE_ENOMEM;
        } else {
            return ENGINE_SUCCESS;
        }
    }

    ENGINE_ERROR_CODE itemDelete(const void* cookie, item *item)
    {
        (void)cookie;
        RememberingCallback<bool> delCb;
        Item *it = static_cast<Item*>(item);

        backend->del(it->key, delCb);;
        delCb.waitForValue();
        if (delCb.val) {
            addDeleteEvent(it);
            return ENGINE_SUCCESS;
        } else {
            return ENGINE_KEY_ENOENT;
        }
    }

    void itemRelease(const void* cookie, item *item)
    {
        (void)cookie;
        delete (Item*)item;
    }

    ENGINE_ERROR_CODE get(const void* cookie,
                          item** item,
                          const void* key,
                          const int nkey)
    {
        (void)cookie;
        std::string k(static_cast<const char*>(key), nkey);
        RememberingCallback<GetValue> getCb;
        backend->get(k, getCb);
        getCb.waitForValue();
        if (getCb.val.success) {
            *item = new Item(k, 0, 0, getCb.val.value.c_str(),
                             getCb.val.value.length());
            return ENGINE_SUCCESS;
        } else {
            return ENGINE_KEY_ENOENT;
        }
    }

    ENGINE_ERROR_CODE getStats(const void* cookie,
                               const char* stat_key,
                               int nkey,
                               ADD_STAT add_stat)
    {
        (void)cookie;
        (void)nkey;
        (void)add_stat;

        if (stat_key == NULL) {
            // @todo add interesting stats
            struct ep_stats epstats;

            if (epstore) {
                epstore->getStats(&epstats);

                add_casted_stat("ep_storage_age",
                                epstats.dirtyAge, add_stat, cookie);
                add_casted_stat("ep_storage_age_highwat",
                                epstats.dirtyAgeHighWat, add_stat, cookie);
                add_casted_stat("ep_data_age",
                                epstats.dataAge, add_stat, cookie);
                add_casted_stat("ep_data_age_highwat",
                                epstats.dataAgeHighWat, add_stat, cookie);
                add_casted_stat("ep_queue_size",
                                epstats.queue_size, add_stat, cookie);
                add_casted_stat("ep_flusher_todo",
                                epstats.flusher_todo, add_stat, cookie);
                add_casted_stat("ep_commit_time",
                                epstats.commit_time, add_stat, cookie);
                add_casted_stat("ep_flush_duration",
                                epstats.flushDuration, add_stat, cookie);
                add_casted_stat("ep_flush_duration_highwat",
                                epstats.flushDurationHighWat, add_stat, cookie);
            }

            add_casted_stat("ep_dbname", dbname, add_stat, cookie);
            add_casted_stat("ep_warmup", warmup ? "true" : "false",
                            add_stat, cookie);
            if (warmup) {
                add_casted_stat("ep_warmup_thread", warmupComplete ? "complete" : "running",
                                add_stat, cookie);
            }

            LockHolder lh(tapQueueMapLock);
            std::map<const void*, TapConnection*>::iterator iter;
            size_t totalQueue = 0;
            size_t totalFetched = 0;
            for (iter = tapConnectionMap.begin(); iter != tapConnectionMap.end(); iter++) {
                char tap[80];
                sprintf(tap, "%s:qlen", iter->second->client.c_str());
                add_casted_stat(tap, iter->second->queue.size(), add_stat, cookie);
                totalQueue += iter->second->queue.size();
                sprintf(tap, "%s:rec_fetched", iter->second->client.c_str());
                add_casted_stat(tap, iter->second->recordsFetched, add_stat, cookie);
                totalFetched += iter->second->recordsFetched;
            }

            add_casted_stat("ep_tap_total_queue", totalQueue, add_stat, cookie);
            add_casted_stat("ep_tap_total_fetched", totalFetched, add_stat, cookie);
        }
        return ENGINE_SUCCESS;
    }

    ENGINE_ERROR_CODE store(const void *cookie,
                            item* itm,
                            uint64_t *cas,
                            ENGINE_STORE_OPERATION operation)
    {
        Item *it = static_cast<Item*>(itm);
        if (operation == OPERATION_SET) {
            backend->set(it->key, it->data, it->nbytes, ignoreCallback);
            *cas = 0;
            addMutationEvent(it);
            return ENGINE_SUCCESS;
        } else if (operation == OPERATION_ADD) {
            item *i;
            if (get(cookie, &i, it->key.c_str(), it->nkey) == ENGINE_SUCCESS) {
                itemRelease(cookie, i);
                return ENGINE_NOT_STORED;
            } else {
                backend->set(it->key, it->data, it->nbytes, ignoreCallback);
                *cas = 0;
                addMutationEvent(it);
                return ENGINE_SUCCESS;
            }
        } else if (operation == OPERATION_REPLACE) {
            item *i;
            if (get(cookie, &i, it->key.c_str(), it->nkey) == ENGINE_SUCCESS) {
                itemRelease(cookie, i);
                backend->set(it->key, it->data, it->nbytes, ignoreCallback);
                *cas = 0;
                addMutationEvent(it);
                return ENGINE_SUCCESS;
            } else {
                return ENGINE_NOT_STORED;
            }
        } else {
            return ENGINE_ENOTSUP;
        }
    }

    ENGINE_ERROR_CODE flush(const void *cookie, time_t when)
    {
        (void)cookie;
        if (when != 0) {
            return ENGINE_ENOTSUP;
        } else {
            epstore->reset();
            addFlushEvent();
        }

        return ENGINE_SUCCESS;
    }

    int walkTapQueue(const void *cookie, item **itm) {
        (void)cookie;
        LockHolder lh(tapQueueMapLock);
        TapConnection *connection = tapConnectionMap[cookie];
        int ret = 0;

        if (!connection->empty()) {
            std::string key = connection->next();

            ENGINE_ERROR_CODE r;
            r = get(cookie, itm, key.c_str(), key.length());
            if (r == ENGINE_SUCCESS) {
                ret = 1;
            } else if (r == ENGINE_KEY_ENOENT) {
                ret = 2;
                r = itemAllocate(cookie, itm,
                                 key.c_str(), key.length(), 0, 0, 0);
                if (r != ENGINE_SUCCESS) {
                    std::cerr << "Failed to allocate memory for deletion of: "
                              << key.c_str() << std::endl;
                    ret = 0;
                }
            }
        } else if (connection->shouldFlush()) {
            ret = 3;
        }

        return ret;
    }

    void createTapQueue(const void *cookie) {
        // map is set-assocative, so this will create an instance here..
        LockHolder lh(tapQueueMapLock);
        char name[80];
        snprintf(name, sizeof(name), "ep_tapq:%lx", (long)cookie);
        tapConnectionMap[cookie] = new TapConnection(name);
    }

    protocol_binary_response_status stopFlusher(const char **msg) {
        protocol_binary_response_status rv = PROTOCOL_BINARY_RESPONSE_SUCCESS;
        *msg = NULL;
        if (epstore->getFlusherState() == RUNNING) {
            epstore->stopFlusher();
        } else {
            std::cerr << "Attempted to stop flusher in state "
                      << epstore->getFlusherState() << std::endl;
            *msg = "Flusher not running.";
            rv = PROTOCOL_BINARY_RESPONSE_EINVAL;
        }
        return rv;
    }

    protocol_binary_response_status startFlusher(const char **msg) {
        protocol_binary_response_status rv = PROTOCOL_BINARY_RESPONSE_SUCCESS;
        *msg = NULL;
        if (epstore->getFlusherState() == STOPPED) {
            epstore->startFlusher();
        } else {
            std::cerr << "Attempted to stop flusher in state "
                      << epstore->getFlusherState() << std::endl;
            *msg = "Flusher not shut down.";
            rv = PROTOCOL_BINARY_RESPONSE_EINVAL;
        }
        return rv;
    }

    void resetStats()
    {
        if (epstore) {
            epstore->resetStats();
        }

        // @todo reset statistics
    }

    static void loadDatabase(EventuallyPersistentEngine *instance) {
        instance->sqliteDb->dump(instance->epstore->getLoadStorageKVPairCallback());
        instance->warmupComplete = true;
    }

private:
    EventuallyPersistentEngine(SERVER_HANDLE_V1 *sApi);
    friend ENGINE_ERROR_CODE create_instance(uint64_t interface,
                                             GET_SERVER_API get_server_api,
                                             ENGINE_HANDLE **handle);

    void notifyTapQueues(std::list<const void*> &clients)
    {
        std::list<const void*>::iterator iter;
        for (iter = clients.begin(); iter != clients.end(); iter++) {
            serverApi.perform_callbacks(ON_TAP_QUEUE, NULL, *iter);
        }
    }


    void addMutationEvent(Item *it) {
        std::list<const void*> clients;
        {
            LockHolder lh(tapQueueMapLock);

            std::map<const void*, TapConnection*>::iterator iter;
            for (iter = tapConnectionMap.begin(); iter != tapConnectionMap.end(); iter++) {
                if (iter->second->addEvent(it)) {
                    clients.push_back(iter->first);
                }
            }
        }
        notifyTapQueues(clients);
    }

    void addDeleteEvent(Item *it) {
        /** The internal datastructures for mutation and delete is the same */
        addMutationEvent(it);
    }

    void addFlushEvent() {
        std::list<const void*> clients;
        LockHolder lh(tapQueueMapLock);
        std::map<const void*, TapConnection*>::iterator iter;
        for (iter = tapConnectionMap.begin(); iter != tapConnectionMap.end(); iter++) {
            iter->second->flush();
            clients.push_back(iter->first);
        }
        lh.unlock();
        notifyTapQueues(clients);
    }

    void add_casted_stat(const char *k, const char *v,
                         ADD_STAT add_stat, const void *cookie) {
        add_stat(k, static_cast<uint16_t>(strlen(k)),
                 v, static_cast<uint32_t>(strlen(v)), cookie);
    }

    void add_casted_stat(const char *k, size_t v,
                         ADD_STAT add_stat, const void *cookie) {
        char valS[32];
        snprintf(valS, sizeof(valS), "%lu", static_cast<unsigned long>(v));
        add_casted_stat(k, valS, add_stat, cookie);
    }

    void loadDatabase(void);

    const char *dbname;
    bool warmup;
    volatile bool warmupComplete;
    SERVER_HANDLE_V1 serverApi;
    IgnoreCallback ignoreCallback;
    KVStore *backend;
    Sqlite3 *sqliteDb;
    EventuallyPersistentStore *epstore;
    std::map<const void*, TapConnection*> tapConnectionMap;

    Mutex tapQueueMapLock;
};
