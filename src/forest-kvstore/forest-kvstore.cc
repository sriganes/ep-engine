/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc
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

#include "forest-kvstore/forest-kvstore.h"
#include <sys/stat.h>
#include <platform/dirutils.h>
#include <vbucket.h>
#include <cJSON.h>

using namespace CouchbaseDirectoryUtilities;

ForestKVStore::ForestKVStore(KVStoreConfig &config) :
    KVStore(config), intransaction(false),
    dbname(config.getDBName()), dbFileRevNum(1) {

    /* create the data directory */
    createDataDir(dbname);
    fdb_status status;

    uint16_t shardId = config.getShardId();
    uint16_t maxVbuckets = config.getMaxVBuckets();
    uint16_t maxShards = config.getMaxShards();

    std::stringstream dbFile;
    dbFile << dbname << "/" << shardId << ".fdb";

    std::stringstream prefix;
    prefix << shardId << ".fdb";

    std::vector<std::string> files = findFilesContaining(dbname,
                                     prefix.str().c_str());

    std::vector<std::string>::iterator fileItr;

    for (fileItr = files.begin(); fileItr != files.end(); ++fileItr) {
        const std::string &filename = *fileItr;
        size_t secondDot = filename.rfind(".");
        std::string revNumStr = filename.substr(secondDot + 1);
        char *ptr = NULL;
        uint64_t revNum = strtoull(revNumStr.c_str(), &ptr, 10);
        if (revNum == 0) {
            LOG(EXTENSION_LOG_WARNING,
                "Invalid revision number obtained for database file");
            abort();
        }

        if (revNum > dbFileRevNum) {
            dbFileRevNum = revNum;
        }
    }

    dbFile << "." << dbFileRevNum;

    fileConfig = fdb_get_default_config();
    kvsConfig = fdb_get_default_kvs_config();

    status = fdb_open(&dbFileHandle, dbFile.str().c_str(), &fileConfig);
    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING,
            "Opening the database file instance failed with error: %s\n",
            fdb_error_msg(status));
        abort();
    }

    fdb_kvs_handle *kvsHandle = NULL;
    /* Initialize ForestDB KV store instances for all the vbuckets
     * that belong to this shard */
    for (uint16_t i = shardId; i < maxVbuckets; i += maxShards) {
        writeHandleMap.insert(std::make_pair(i, kvsHandle));
        readHandleMap.insert(std::make_pair(i, kvsHandle));
    }

    /* Initialize the handle for the vbucket state information */
    status = fdb_kvs_open(dbFileHandle, &vbStateHandle, "vbstate",
                          &kvsConfig);

    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING,
            "Opening the vbucket state KV store instance failed "
            "with error: %s\n", fdb_error_msg(status));
        abort();
    }

    cachedVBStates.reserve(config.getMaxVBuckets());
    for (uint16_t i = shardId; i < maxVbuckets; i += maxShards) {
        readVBState(i);
    }
}

fdb_config ForestKVStore::getFileConfig() {
    return fileConfig;
}

fdb_kvs_config ForestKVStore::getKVConfig() {
    return kvsConfig;
}

ForestKVStore::ForestKVStore(const ForestKVStore &copyFrom) :
    KVStore(copyFrom), intransaction(false),
    dbname(copyFrom.dbname) {

    /* create the data directory */
    createDataDir(dbname);
}

ForestKVStore::~ForestKVStore() {
   close();

   /* delete all the cached vbucket states */
   std::vector<vbucket_state *>::iterator stateItr;
   for (stateItr = cachedVBStates.begin(); stateItr != cachedVBStates.end();
        stateItr++) {
        vbucket_state *vbstate = *stateItr;
        if (vbstate) {
            delete vbstate;
            *stateItr = NULL;
        }
   }

   /* Close all the KV store instances */
   unordered_map<uint16_t, fdb_kvs_handle *>::iterator handleItr;
   for (handleItr = writeHandleMap.begin(); handleItr != writeHandleMap.end();
        handleItr++) {
       fdb_kvs_handle *kvsHandle = handleItr->second;
       fdb_kvs_close(kvsHandle);
   }

   writeHandleMap.clear();

   for (handleItr = readHandleMap.begin(); handleItr != readHandleMap.end();
        handleItr++) {
       fdb_kvs_handle *kvsHandle = handleItr->second;
       fdb_kvs_close(kvsHandle);
   }

   readHandleMap.clear();

   /* Close the database file instance */
   fdb_close(dbFileHandle);
}

ForestRequest::ForestRequest(const Item &it, MutationRequestCallback &cb ,bool del)
    : IORequest(it.getVBucketId(), cb, del, it.getKey()),
      status(MUTATION_SUCCESS) { }

ForestRequest::~ForestRequest() {
}

void ForestKVStore::close() {
    intransaction = false;
}

static const std::string getJSONObjString(const cJSON *i) {
    if (i == NULL) {
        return "";
    }
    if (i->type != cJSON_String) {
        abort();
    }
    return i->valuestring;
}

void ForestKVStore::readVBState(uint16_t vbId) {
    fdb_status status;
    vbucket_state_t state = vbucket_state_dead;
    uint64_t checkpointId = 0;
    uint64_t maxDeletedSeqno = 0;
    std::string failovers("[{\"id\":0,\"seq\":0}]");
    uint64_t lastSnapStart = 0;
    uint64_t lastSnapEnd = 0;
    uint64_t maxCas = 0;
    int64_t driftCounter = INITIAL_DRIFT;

    char keybuf[20];
    fdb_doc *statDoc;
    sprintf(keybuf, "partition%d", vbId);
    fdb_doc_create(&statDoc, (void *)keybuf, strlen(keybuf), NULL, 0, NULL, 0);
    status = fdb_get(vbStateHandle, statDoc);

    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_DEBUG,
            "Warning: failed to retrieve stat info for vBucket=%d "
            "error=%s", vbId, fdb_error_msg(status));
    } else {
        cJSON *jsonObj = cJSON_Parse((char *)statDoc->body);

        if (!jsonObj) {
            LOG(EXTENSION_LOG_WARNING,
                "Warning: failed to parse the vbstat json doc for vbucket %d: %s",
                vbId, (char *)statDoc->body);
            fdb_doc_free(statDoc);
            abort();
        }

        const std::string vb_state = getJSONObjString(
                                 cJSON_GetObjectItem(jsonObj, "state"));

        const std::string checkpoint_id = getJSONObjString(
                                 cJSON_GetObjectItem(jsonObj,"checkpoint_id"));

        const std::string max_deleted_seqno = getJSONObjString(
                                 cJSON_GetObjectItem(jsonObj, "max_deleted_seqno"));

        const std::string snapStart = getJSONObjString(
                                 cJSON_GetObjectItem(jsonObj, "snap_start"));

        const std::string snapEnd = getJSONObjString(
                                 cJSON_GetObjectItem(jsonObj, "snap_end"));

        const std::string maxCasValue = getJSONObjString(
                                 cJSON_GetObjectItem(jsonObj, "max_cas"));

        const std::string driftCount = getJSONObjString(
                                 cJSON_GetObjectItem(jsonObj, "drift_counter"));

        cJSON *failover_json = cJSON_GetObjectItem(jsonObj, "failover_table");
        if (vb_state.compare("") == 0 || checkpoint_id.compare("") == 0
               || max_deleted_seqno.compare("") == 0) {
             LOG(EXTENSION_LOG_WARNING,
                 "Warning: state JSON doc for vbucket %d is in the wrong format: %s",
                 vbId, (char *)statDoc->body);
        } else {
            state = VBucket::fromString(vb_state.c_str());
            parseUint64(max_deleted_seqno.c_str(), &maxDeletedSeqno);
            parseUint64(checkpoint_id.c_str(), &checkpointId);

            if (snapStart.compare("")) {
                parseUint64(snapStart.c_str(), &lastSnapStart);
            }

            if (snapEnd.compare("")) {
                parseUint64(snapEnd.c_str(), &lastSnapEnd);
            }

            if (maxCasValue.compare("")) {
                parseUint64(maxCasValue.c_str(), &maxCas);
            }

            if (driftCount.compare("")) {
                parseInt64(driftCount.c_str(), &driftCounter);
            }

            if (failover_json) {
                char* json = cJSON_PrintUnformatted(failover_json);
                failovers.assign(json);
                free(json);
            }
        }

        cJSON_Delete(jsonObj);
    }

    cachedVBStates[vbId] = new vbucket_state(state, checkpointId,
                                             maxDeletedSeqno, 0, 0,
                                             lastSnapStart, lastSnapEnd,
                                             maxCas, driftCounter,
                                             failovers);
}

void ForestKVStore::delVBucket(uint16_t vbucket) {

}

void ForestKVStore::getWithHeader(void *dbHandle, const std::string &key,
                                  uint16_t vb, Callback<GetValue> &cb,
                                  bool fetchDelete) {

}

vbucket_state * ForestKVStore::getVBucketState(uint16_t vbucketId) {
    return cachedVBStates[vbucketId];
}

ENGINE_ERROR_CODE ForestKVStore::updateVBState(uint16_t vbucketId,
                                               vbucket_state *vbState) {
    std::stringstream jsonState;
    jsonState << "{\"state\": \"" << VBucket::toString(vbState->state) << "\""
              << ",\"checkpoint_id\": \"" << vbState->checkpointId << "\""
              << ",\"max_deleted_seqno\": \"" << vbState->maxDeletedSeqno << "\""
              << ",\"failover_table\": " << vbState->failovers
              << ",\"snap_start\": \"" << vbState->lastSnapStart << "\""
              << ",\"snap_end\": \"" << vbState->lastSnapEnd << "\""
              << ",\"max_cas\": \"" << vbState->maxCas << "\""
              << ",\"drift_counter\": \"" << vbState->driftCounter << "\""
              << "}";

    std::string state = jsonState.str();
    char keybuf[20];
    fdb_doc statDoc;
    sprintf(keybuf, "partition%d", vbucketId);
    statDoc.key = keybuf;
    statDoc.keylen = strlen(keybuf);
    statDoc.meta = NULL;
    statDoc.metalen = 0;
    statDoc.body = const_cast<char *>(state.c_str());
    statDoc.bodylen = state.length();
    statDoc.deleted = false;
    fdb_kvs_handle *stateHandle;
    fdb_kvs_open(dbFileHandle, &stateHandle, "vbstate", &kvsConfig);
    fdb_status status = fdb_set(stateHandle, &statDoc);

    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING, "Failed to save vbucket state for "
            "vbucket=%d error=%s", vbucketId, fdb_error_msg(status));
        return ENGINE_FAILED;
    }

    fdb_kvs_close(stateHandle); 

    return ENGINE_SUCCESS;
}

bool ForestKVStore::commit(Callback<kvstats_ctx> *cb, uint64_t snapStartSeqno,
                           uint64_t snapEndSeqno, uint64_t maxCas,
                           uint64_t driftCounter) {
    if (intransaction) {
        if (save2forestdb(cb)) {
            intransaction = false;
        }
    }

    return !intransaction;
}

void ForestKVStore::commitCallback(std::vector<ForestRequest *> &committedReqs) {
    size_t commitSize = committedReqs.size();
    int rv;

    for (size_t index = 0; index < commitSize; index++) {
        rv = committedReqs[index]->getStatus();
        if (committedReqs[index]->isDelete()) {
            committedReqs[index]->getDelCallback()->callback(rv);
        } else {
            mutation_result p(rv, false);
            committedReqs[index]->getSetCallback()->callback(p);
        }
    }
}

bool ForestKVStore::save2forestdb(Callback<kvstats_ctx> *cb) {
    size_t pendingCommitCnt = pendingReqsQ.size();
    if (pendingCommitCnt == 0) {
        return true;
    }

    cb_assert(pendingReqsQ[0]);

    fdb_status status = fdb_commit(dbFileHandle, FDB_COMMIT_NORMAL);
    if (status != FDB_RESULT_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING,
            "fdb_commit failed with error:%s", fdb_error_msg(status));
    }

    commitCallback(pendingReqsQ);

    for (size_t i = 0; i < pendingCommitCnt; ++i) {
        delete pendingReqsQ[i];
    }

    pendingReqsQ.clear();

    return true;
}

StorageProperties ForestKVStore::getStorageProperties(void) {
    StorageProperties rv(true, true, true, true);
    return rv;
}

fdb_kvs_handle* ForestKVStore::getKvsHandle(uint16_t vbucketId) {
    char kvsName[20];
    sprintf(kvsName, "partition%d", vbucketId);
    fdb_kvs_handle *kvsHandle = NULL;
    fdb_status status;
    unordered_map<uint16_t, fdb_kvs_handle*>::iterator found =
                                            writeHandleMap.find(vbucketId);
    cb_assert (found != writeHandleMap.end());
    if (!(found->second)) {
        status = fdb_kvs_open(dbFileHandle, &kvsHandle, kvsName, &kvsConfig);
        if (status != FDB_RESULT_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING,
                "Opening the KV store instance for vbucket %d failed "
                "with error: %s", vbucketId, fdb_error_msg(status));
            return NULL;
        }
        found->second = kvsHandle;
    } else {
        kvsHandle = found->second;
    }

    return kvsHandle;
}

static int8_t getMutationStatus(fdb_status errCode) {
    switch(errCode) {
        case FDB_RESULT_SUCCESS:
            return MUTATION_SUCCESS;
        case FDB_RESULT_NO_DB_HEADERS:
        case FDB_RESULT_NO_SUCH_FILE:
        case FDB_RESULT_KEY_NOT_FOUND:
            return DOC_NOT_FOUND;
        default:
            return MUTATION_FAILED;
    }
}

static void populateMetaData(const Item &itm, uint8_t *meta, bool deletion) {
    uint64_t cas = htonll(itm.getCas());
    uint64_t rev_seqno = htonll(itm.getRevSeqno());
    uint32_t flags = itm.getFlags();
    uint32_t vlen = itm.getNBytes();
    uint32_t exptime = itm.getExptime();
    uint32_t texptime = 0;
    uint8_t confresmode = static_cast<uint8_t>(itm.getConflictResMode());
    uint8_t datatype = 0x00;

    if (vlen) {
        datatype = itm.getDataType();
    }

    if (deletion) {
        texptime = ep_real_time();
    }

    exptime = htonl(exptime);
    texptime = htonl(texptime);

    memcpy(meta, &cas, 8);
    memcpy(meta + 8, &exptime, 4);
    memcpy(meta + 12, &texptime, 4);
    memcpy(meta + 16, &flags, 4);
    memcpy(meta + 20, &rev_seqno, 8);

    *(meta + 28) = FLEX_META_CODE;

    if (deletion) {
        *(meta + 29) = PROTOCOL_BINARY_RAW_BYTES;
    } else {
        memcpy(meta + 29, itm.getExtMeta(), itm.getExtMetaLen());
    }

    memcpy(meta + 30, &confresmode, CONFLICT_RES_META_LEN);
}

void ForestKVStore::set(const Item &itm, Callback<mutation_result> &cb) {
    cb_assert(!isReadOnly());
    cb_assert(intransaction);
    MutationRequestCallback requestcb;

    // each req will be de-allocated after commit
    requestcb.setCb = &cb;
    ForestRequest *req = new ForestRequest(itm, requestcb, false);
    fdb_doc setDoc;
    fdb_kvs_handle *kvsHandle = NULL;
    fdb_status status;
    uint8_t meta[FORESTDB_METADATA_SIZE];

    memset(meta, 0, sizeof(meta));
    populateMetaData(itm, meta, false);

    setDoc.key = const_cast<char *>(itm.getKey().c_str());
    setDoc.keylen = itm.getNKey();
    setDoc.meta = meta;
    setDoc.metalen = sizeof(meta);
    setDoc.body = const_cast<char *>(itm.getData());
    setDoc.bodylen = itm.getNBytes();
    setDoc.deleted = false;

    kvsHandle = getKvsHandle(req->getVBucketId());
    if (kvsHandle) {
        status = fdb_set(kvsHandle, &setDoc);
        if (status != FDB_RESULT_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING, "fdb_set failed for key: %s and "
                "vbucketId: %d with error: %s", req->getKey().c_str(),
                req->getVBucketId(), fdb_error_msg(status));
            req->setStatus(getMutationStatus(status));
        }
        setDoc.body = NULL;
        setDoc.bodylen = 0;
    } else {
        LOG(EXTENSION_LOG_WARNING, "Failed to open KV store instance "
            "for key: %s and vbucketId: %d", req->getKey().c_str(),
            req->getVBucketId());
        req->setStatus(MUTATION_FAILED);
    }

    pendingReqsQ.push_back(req);
}

void ForestKVStore::get(const std::string &key, uint16_t vb,
                        Callback<GetValue> &cb, bool fetchDelete) {

}

void ForestKVStore::getMulti(uint16_t vb, vb_bgfetch_queue_t &itms) {
    bool meta_only = true;
    vb_bgfetch_queue_t::iterator itr = itms.begin();

    fprintf(stderr, "DEBUG: Entering getMulti\n");

    for (; itr != itms.end(); ++itr) {
        std::list<VBucketBGFetchItem *> &fetches = (*itr).second;
        std::list<VBucketBGFetchItem *>:: iterator fitr = fetches.begin();

        /* Check if we need to just fetch meta data or the whole data */
        for (; fitr != fetches.end(); ++fitr) {
            if (!((*fitr)->metaDataOnly)) {
                meta_only = false;
                break;
            }
        }
 
        RememberingCallback<GetValue> gcb;
        if (meta_only) {
            gcb.val.setPartial();
        }
        
        const std::string &key = (*itr).first;

        fprintf(stderr, "DEBUG: About to call get\n");
 
        get(key, vb, gcb);

        fprintf(stderr, "DEBUG: Waiting for value\n");

        gcb.waitForValue();
        cb_assert(gcb.fired);
        ENGINE_ERROR_CODE status = gcb.val.getStatus();
        if (status != ENGINE_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING, "Failed to retrieve key: %s",
                key.c_str()); 
        }

        LOG(EXTENSION_LOG_WARNING, "GetMulti: Retrieved key: %s\n",
            key.c_str());

        for (fitr = fetches.begin(); fitr != fetches.end(); ++fitr) {
            (*fitr)->value = gcb.val; 
        }
        meta_only = true;
    }
}

void ForestKVStore::del(const Item &itm, Callback<int> &cb) {
    cb_assert(!isReadOnly());
    cb_assert(intransaction);
    MutationRequestCallback requestcb;
    requestcb.delCb = &cb;
    ForestRequest *req = new ForestRequest(itm, requestcb, true);
    fdb_doc delDoc;
    fdb_kvs_handle *kvsHandle = NULL;
    fdb_status status;
    uint8_t meta[FORESTDB_METADATA_SIZE];

    memset(meta, 0, sizeof(meta));
    populateMetaData(itm, meta, true);

    delDoc.key = const_cast<char *>(itm.getKey().c_str());
    delDoc.keylen = itm.getNKey();
    delDoc.meta = meta;
    delDoc.metalen = sizeof(meta);
    delDoc.deleted = true;

    kvsHandle = getKvsHandle(req->getVBucketId());
    if (kvsHandle) {
        status = fdb_del(kvsHandle, &delDoc);
        if (status != FDB_RESULT_SUCCESS) {
            LOG(EXTENSION_LOG_WARNING, "fdb_del failed for key: %s and "
                "vbucketId: %d with error: %s",  req->getKey().c_str(),
                req->getVBucketId(), fdb_error_msg(status));
            req->setStatus(getMutationStatus(status));
        }
    } else {
        LOG(EXTENSION_LOG_WARNING, "Failure to open KV store instance "
            "for key: %s and vbucketId: %d", req->getKey().c_str(),
            req->getVBucketId());
        req->setStatus(MUTATION_FAILED);
    }

    pendingReqsQ.push_back(req);
}

void ForestKVStore::optimizeWrites(std::vector<queued_item> &items) {
    cb_assert(!isReadOnly());
    if (items.empty()) {
        return;
    }
    CompareQueuedItemsBySeqnoAndKey cq;
    std::sort(items.begin(), items.end(), cq);
}

std::vector<vbucket_state *> ForestKVStore::listPersistedVbuckets(void) {
    return cachedVBStates;
}

std::list<PersistenceCallback *>& ForestKVStore::getPersistenceCallbacks(void) {
    return pcbs;
}

DBFileInfo ForestKVStore::getDbFileInfo(uint16_t dbId) {
    DBFileInfo dbInfo;
    return dbInfo;
}

bool ForestKVStore::snapshotStats(const std::map<std::string,
                                  std::string> &engine_stats) {
    return true;
}

bool ForestKVStore::snapshotVBucket(uint16_t vbucketId,
                                    vbucket_state &vbstate,
                                    Callback<kvstats_ctx> *cb) {
    return true;
}

bool ForestKVStore::compactVBucket(const uint16_t vbid, compaction_ctx *cookie,
                                   Callback<kvstats_ctx> &kvcb) {
    return true;
}

RollbackResult ForestKVStore::rollback(uint16_t vbid, uint64_t rollbackSeqno,
                                       shared_ptr<RollbackCB> cb) {
   return RollbackResult(true, 0, 0, 0);
}
