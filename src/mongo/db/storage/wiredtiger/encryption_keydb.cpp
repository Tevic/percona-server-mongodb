/*======
This file is part of Percona Server for MongoDB.

Copyright (c) 2006, 2018, Percona and/or its affiliates. All rights reserved.

    Percona Server for MongoDB is free software: you can redistribute
    it and/or modify it under the terms of the GNU Affero General
    Public License, version 3, as published by the Free Software
    Foundation.

    Percona Server for MongoDB is distributed in the hope that it will
    be useful, but WITHOUT ANY WARRANTY; without even the implied
    warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
    See the GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public
    License along with Percona Server for MongoDB.  If not, see
    <http://www.gnu.org/licenses/>.
======= */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include <boost/filesystem.hpp>

#include "mongo/db/encryption/encryption_options.h"
#include "mongo/db/storage/wiredtiger/encryption_keydb.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/log.h"

#include <third_party/wiredtiger/ext/encryptors/percona/encryption_keydb_c_api.h>

#include <cstring>  // memcpy

namespace mongo {

static EncryptionKeyDB *encryptionKeyDB = nullptr;

constexpr int EncryptionKeyDB::_key_len;

static void dump_key(unsigned char *key, const int _key_len, const char * msg) {
    const char* m = "0123456789ABCDEF";
    char buf[_key_len * 3 + 1];
    char* p=buf;
    for (int i=0; i<_key_len; ++i) {
        *p++ = m[*key >> 4];
        *p++ = m[*key & 0xf];
        *p++ = ' ';
        ++key;
    }
    *p = 0;
    log() << msg << ": " << buf;
}

static void dump_table(WT_SESSION* _sess, const int _key_len, const char* msg) {
    log() << msg;
    WT_CURSOR *cursor;
    int res = _sess->open_cursor(_sess, "table:key", nullptr, nullptr, &cursor);
    if (res) {
        log() << wiredtiger_strerror(res);
        return;
    }
    while ((res = cursor->next(cursor)) == 0) {
        char* k;
        WT_ITEM v;
        res = cursor->get_key(cursor, &k);
        res = cursor->get_value(cursor, &v);
        std::stringstream ss;
        ss << v.size << ": " << k;
        dump_key((unsigned char*)v.data, _key_len, ss.str().c_str());
    }
    res = cursor->close(cursor);
}

EncryptionKeyDB::EncryptionKeyDB(const std::string& path)
  : _path(path) {
    // only single instance is allowed
    invariant(encryptionKeyDB == nullptr);
    encryptionKeyDB = this;
}

EncryptionKeyDB::~EncryptionKeyDB() {
    DEV if (_sess)
        dump_table(_sess, _key_len, "dump_table from destructor");
    if (_sess)
        _sess->close(_sess, nullptr);
    if (_conn)
        _conn->close(_conn, nullptr);
    // should be the last line because closing wiredTiger's handles may write to DB
    encryptionKeyDB = nullptr;
}

void EncryptionKeyDB::init() {
    _srng = SecureRandom::create();
    _prng = std::make_unique<PseudoRandom>(_srng->nextInt64());
    try {
        if (!boost::filesystem::exists(encryptionGlobalParams.encryptionKeyFile)) {
            throw std::runtime_error(std::string("specified encryption key file doesn't exist: ")
                                                 + encryptionGlobalParams.encryptionKeyFile);
        }
        std::ifstream f(encryptionGlobalParams.encryptionKeyFile);
        if (!f.is_open()) {
            throw std::runtime_error(std::string("cannot open specified encryption key file: ")
                                                 + encryptionGlobalParams.encryptionKeyFile);
        }
        std::string encoded_key;
        f >> encoded_key;
        auto key = base64::decode(encoded_key);
        if (key.length() != _key_len) {
            throw std::runtime_error(str::stream() << "encryption key length should be " << _key_len << " bytes");
        }
        memcpy(_masterkey, key.c_str(), _key_len);

        std::stringstream ss;
        ss << "create,";
        ss << "config_base=false,";
        ss << "extensions=[local=(entry=percona_encryption_extension_init,early_load=true,config=(cipher=" << encryptionGlobalParams.encryptionCipherMode << "))],";
        ss << "encryption=(name=percona,keyid=\"\"),";
        int res = wiredtiger_open(_path.c_str(), nullptr, ss.str().c_str(), &_conn);
        if (res) {
            throw std::runtime_error(std::string("error opening keys DB at '") + _path + "': " + wiredtiger_strerror(res));
        }

        // empty keyid means masterkey
        res = _conn->open_session(_conn, nullptr, nullptr, &_sess);
        if (res) {
            throw std::runtime_error(std::string("error opening wiredTiger session: ") + wiredtiger_strerror(res));
        }

        DEV dump_table(_sess, _key_len, "before create");
        // try to create table
        // ignore error if table already exists
        res = _sess->create(_sess, "table:key", "key_format=S,value_format=u,access_pattern_hint=random");
        if (res) {
            throw std::runtime_error(std::string("error creating/opening key table: ") + wiredtiger_strerror(res));
        }
        DEV dump_table(_sess, _key_len, "after create");
    } catch (std::exception& e) {
        error() << e.what();
        throw;
    }
}

int EncryptionKeyDB::get_key_by_id(const char *keyid, size_t len, unsigned char *key) {
    LOG(4) << "get_key_by_id for keyid: '" << std::string(keyid, len) << "'";
    // return key from keyfile if len == 0
    if (len == 0) {
        memcpy(key, _masterkey, _key_len);
        DEV dump_key(key, _key_len, "returning masterkey");
        return 0;
    }

    // open cursor
    WT_CURSOR *cursor;
    int res = _sess->open_cursor(_sess, "table:key", nullptr, nullptr, &cursor);
    if (res){
        error() << "get_key_by_id: error opening cursor: " << wiredtiger_strerror(res);
        return res;
    }

    // create cursor delete guard
    std::unique_ptr<WT_CURSOR, std::function<void(WT_CURSOR*)>> cursor_guard(cursor, [](WT_CURSOR* c)
        {
            c->close(c);
        });

    // read key from DB
    std::string c_str(keyid, len);
    LOG(4) << "trying to load encryption key for keyid: " << c_str;
    cursor->set_key(cursor, c_str.c_str());
    res = cursor->search(cursor);
    if (res == 0) {
        WT_ITEM v;
        cursor->get_value(cursor, &v);
        invariant(v.size == _key_len);
        memcpy(key, v.data, _key_len);
        DEV dump_key(key, _key_len, "loaded key from key DB");
        return 0;
    }
    if (res != WT_NOTFOUND) {
        error() << "cursor->search error " << res << " : " <<wiredtiger_strerror(res);
        return res;
    }

    // create key if it does not exist
    for (int i = 0; i < 4; ++i) {
        ((int64_t*)key)[i] = _srng->nextInt64();
    }
    WT_ITEM v;
    v.size = _key_len;
    v.data = key;
    cursor->set_key(cursor, c_str.c_str());
    cursor->set_value(cursor, &v);
    res = cursor->insert(cursor);
    if (res) {
        error() << "cursor->insert error " << res << " : " <<wiredtiger_strerror(res);
        return res;
    }

    DEV dump_key(key, _key_len, "generated and stored key");
    return 0;
}

void EncryptionKeyDB::store_pseudo_bytes(uint8_t *buf, int len) {
    invariant((len % 4) == 0);
    for (int i = 0; i < len / 4; ++i) {
        *(int32_t*)buf = _prng->nextInt32();
        buf += 4;
    }
}

extern "C" void store_pseudo_bytes(uint8_t *buf, int len) {
    invariant(encryptionKeyDB);
    encryptionKeyDB->store_pseudo_bytes(buf, len);
}

// returns encryption key from keys DB
// create key if it does not exists
// return key from keyfile if len == 0
extern "C" int get_key_by_id(const char *keyid, size_t len, unsigned char *key) {
    invariant(encryptionKeyDB);
    return encryptionKeyDB->get_key_by_id(keyid, len, key);
}

}  // namespace mongo
