/*
    This file is part of tgl-library

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

    Copyright Vitaly Valtman 2014-2015
    Copyright Topology LP 2016-2017
*/

#include "download_task.h"

#include "auto/constants.h"
#include "crypto/crypto_md5.h"

#include <cstring>

namespace tgl {
namespace impl {

download_task::download_task(int64_t id, int32_t size, const tgl_file_location& location)
    : id(id)
    , offset(0)
    , downloaded_bytes(0)
    , size(size)
    , type(0)
    , location(location)
    , status(tgl_download_status::waiting)
    , iv()
    , key()
    , decryption_offset(0)
    , valid(true)
    , m_cancel_requested(false)
{
}

download_task::download_task(int64_t id, const std::shared_ptr<tgl_download_document>& document)
    : id(id)
    , offset(0)
    , downloaded_bytes(0)
    , size(document->size)
    , type(0)
    , location()
    , status(tgl_download_status::waiting)
    , iv()
    , key()
    , decryption_offset(0)
    , valid(true)
    , m_cancel_requested(false)
{
    location.set_dc(document->dc_id);
    location.set_local_id(0);
    location.set_secret(document->access_hash);
    location.set_volume(document->id);
    init_from_document(document);
}

download_task::~download_task()
{
    memset(iv.data(), 0, iv.size());
    memset(key.data(), 0, key.size());
}

void download_task::init_from_document(const std::shared_ptr<tgl_download_document>& document)
{
    if (document->is_encrypted()) {
        type = CODE_input_encrypted_file_location;
        iv = std::move(document->iv);
        key = std::move(document->key);
        unsigned char md5[16];
        unsigned char str[64];
        memcpy(str, key.data(), 32);
        memcpy(str + 32, iv.data(), 32);
        TGLC_md5(str, 64, md5);
        if (document->key_fingerprint != ((*(int *)md5) ^ (*(int *)(md5 + 4)))) {
            valid = false;
            return;
        }
        return;
    }

    switch (document->type) {
    case tgl_document_type::audio:
        type = CODE_input_audio_file_location;
        break;
    case tgl_document_type::video:
        type = CODE_input_video_file_location;
        break;
    default:
        type = CODE_input_document_file_location;
        break;
    }
}

void download_task::set_status(tgl_download_status status)
{
    this->status = status;
    if (callback) {
        std::string file_name = (status == tgl_download_status::succeeded || status == tgl_download_status::cancelled) ? this->file_name : std::string();
        callback(status, file_name, downloaded_bytes);
    }
}

bool download_task::check_cancelled()
{
    if (!m_cancel_requested && status != tgl_download_status::cancelled) {
        return false;
    }
    set_status(tgl_download_status::cancelled);
    return true;
}

}
}
