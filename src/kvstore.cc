/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2010 Couchbase, Inc
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

#include "config.h"

#include <map>
#include <string>
#include <fcntl.h>

#include "common.h"
#include "couch-kvstore/couch-kvstore.h"
#include "kvstore.h"
#include <platform/dirutils.h>
#include <sys/types.h>
#include <sys/stat.h>

using namespace CouchbaseDirectoryUtilities;

KVStoreConfig::KVStoreConfig(Configuration& config)
    : maxVBuckets(config.getMaxVbuckets()), dbname(config.getDbname()),
      backend(config.getBackend()) {

}

KVStoreConfig::KVStoreConfig(uint16_t _maxVBuckets, std::string& _dbname,
                             std::string& _backend)
    : maxVBuckets(_maxVBuckets), dbname(_dbname), backend(_backend) {

}

KVStore *KVStoreFactory::create(KVStoreConfig &config, bool read_only) {
    KVStore *ret = NULL;
    std::string backend = config.getBackend();
    if (backend.compare("couchdb") == 0) {
        ret = new CouchKVStore(config, read_only);
    } else {
        LOG(EXTENSION_LOG_WARNING, "Unknown backend: [%s]", backend.c_str());
    }

    return ret;
}

void KVStore::createDataDir(const std::string& dbname) {
    if (!mkdirp(dbname.c_str())) {
        if (errno != EEXIST) {
            std::stringstream ss;
            ss << "Warning: Failed to create data directory ["
               << dbname << "]: " << strerror(errno);
            throw std::runtime_error(ss.str());
        }
    }
}
