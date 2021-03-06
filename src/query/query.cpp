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

    Copyright Vitaly Valtman 2013-2015
    Copyright Topology LP 2016-2017
*/

#include "query.h"

#include "auto/auto_fetch_ds.h"
#include "auto/auto_free_ds.h"
#include "auto/auto_skip.h"
#include "query_user_info.h"
#include "tgl/tgl_timer.h"

namespace tgl {
namespace impl {

constexpr int32_t TGL_SCHEME_LAYER = 45;
constexpr int TGL_MAX_DC_NUM = 100;

void query::clear_timers()
{
    if (m_timer) {
        m_timer->cancel();
    }
    m_timer = nullptr;

    if (m_retry_timer) {
        m_retry_timer->cancel();
    }
    m_retry_timer = nullptr;
}

inline bool query::is_in_the_same_session() const
{
    return m_client && m_session_id && m_client->session() && m_client->session()->session_id == m_session_id;
}

bool query::send()
{
    m_ack_received = false;

    will_send();

    TGL_DEBUG("sending query \"" << m_name << "\" of size " << m_serializer->char_size() << " to DC " << m_client->id());

    m_msg_id = m_client->send_message(m_serializer->i32_data(), m_serializer->i32_size(), m_msg_id_override, is_force(), is_file_transfer());
    if (m_msg_id == -1) {
        m_msg_id = 0;
        handle_error(400, "client failed to send message");
        return false;
    }
    if (is_logout()) {
        m_client->set_logout_query(shared_from_this());
    }
    m_user_agent.add_active_query(shared_from_this());
    m_session_id = m_client->session()->session_id;
    timeout_within(timeout_interval());
    sent();

    return true;
}

void query::alarm()
{
    TGL_DEBUG("alarm query #" << msg_id() << " (type '" << m_name << "') to DC " << m_client->id());

    clear_timers();

    if (msg_id()) {
        m_user_agent.remove_active_query(shared_from_this());
    }

    if (!check_logging_out()) {
        return;
    }

    if (!check_pending()) {
        return;
    }

    if (is_in_the_same_session()) {
        mtprotocol_serializer s;
        s.out_i32(CODE_msg_container);
        s.out_i32(1);
        s.out_i64(msg_id());
        s.out_i32(m_seq_no);
        s.out_i32(m_serializer->char_size());
        s.out_i32s(m_serializer->i32_data(), m_serializer->i32_size());
        if (!send()) {
            return;
        }
        TGL_NOTICE("resent query #" << msg_id() << " of size " << m_serializer->char_size() << " to DC " << m_client->id());
    } else {
        assert(m_client->session());
        int64_t old_id = msg_id();
        if (!send()) {
            return;
        }
        TGL_NOTICE("resent query #" << old_id << " as #" << msg_id() << " of size " << m_serializer->char_size() << " to DC " << m_client->id());
    }
}

void query::regen()
{
    m_ack_received = false;
    if (!is_in_the_same_session() || (!m_client->is_configured() && !is_force())) {
        m_session_id = 0;
    }
    retry_within(0);
}

void query::timeout_alarm()
{
    clear_timers();
    on_timeout();

    if (!should_retry_on_timeout()) {
        if (msg_id()) {
            m_user_agent.remove_active_query(shared_from_this());
        }
        m_client->remove_pending_query(shared_from_this());
    } else {
        alarm();
    }
}

void query::execute(const std::shared_ptr<mtproto_client>& client, execution_option option)
{
    if (m_client) {
        m_client->remove_connection_status_observer(shared_from_this());
    }
    m_exec_option = option;
    m_client = client;
    assert(m_client);
    m_client->add_connection_status_observer(shared_from_this());

    if (!check_logging_out()) {
        return;
    }

    if (!check_pending(true)) {
        return;
    }

    if (!send()) {
        return;
    }
    m_seq_no = m_client->session()->seq_no - 1;
    TGL_DEBUG("sent query \"" << m_name << "\" of size " << m_serializer->char_size() << " to DC " << m_client->id() << ": #" << msg_id());
}

void query::connection_status_changed(tgl_connection_status status)
{
    m_connection_status = status;
    on_connection_status_changed(status);
}

bool query::execute_after_pending()
{
    // We only return false when the query is pending.

    assert(m_client);
    assert(m_exec_option != execution_option::UNKNOWN);

    if (!check_logging_out()) {
        return true;
    }

    if (!check_pending()) {
        return false;
    }

    if (!send()) {
        return true;
    }

    TGL_DEBUG("sent pending query \"" << m_name << "\" (" << msg_id() << ") of size " << m_serializer->char_size() << " to DC " << m_client->id());

    return true;
}

void query::out_peer_id(const tgl_peer_id_t& id, int64_t access_hash)
{
    switch (id.peer_type) {
    case tgl_peer_type::chat:
        m_serializer->out_i32(CODE_input_peer_chat);
        m_serializer->out_i32(id.peer_id);
        break;
    case tgl_peer_type::user:
        if (id.peer_id == m_user_agent.our_id().peer_id) {
            m_serializer->out_i32(CODE_input_peer_self);
        } else {
            m_serializer->out_i32(CODE_input_peer_user);
            m_serializer->out_i32(id.peer_id);
            m_serializer->out_i64(access_hash);
        }
        break;
    case tgl_peer_type::channel:
        m_serializer->out_i32(CODE_input_peer_channel);
        m_serializer->out_i32(id.peer_id);
        m_serializer->out_i64(access_hash);
        break;
    default:
        assert(false);
    }
}

void query::out_input_peer(const tgl_input_peer_t& id)
{
    out_peer_id(tgl_peer_id_t(id.peer_type, id.peer_id), id.access_hash);
}

void query::ack()
{
    if (m_ack_received) {
        return;
    }

    m_ack_received = true;
    timeout_within(timeout_interval());

    // FIXME: This a workaround to the weird server behavour. The server
    // replies a logout query with ack and then closes the connection.
    if (is_logout()) {
        mtprotocol_serializer s;
        s.out_i32(CODE_bool_true);
        tgl_in_buffer in = { s.i32_data(), s.i32_data() + s.i32_size() };
        handle_result(&in);
    }
}

static bool get_int_from_prefixed_string(int& number, const std::string& prefixed_string, const std::string& prefix)
{
    std::string number_string;
    if (prefixed_string.size() >= prefix.size() + 1 && !prefixed_string.compare(0, prefix.size(), prefix)) {
        number_string = prefixed_string.substr(prefix.size());
    }

    if (number_string.size()) {
        try {
            number = std::stoi(number_string);
            return true;
        } catch (...) {
            return false;
        }
    }

    return false;
}

static int get_dc_from_migration(const std::string& migration_error_string)
{
    int dc = -1;
    if (get_int_from_prefixed_string(dc, migration_error_string, "USER_MIGRATE_")) {
        return dc;
    }

    if (get_int_from_prefixed_string(dc, migration_error_string, "PHONE_MIGRATE_")) {
        return dc;
    }

    if (get_int_from_prefixed_string(dc, migration_error_string, "NETWORK_MIGRATE_")) {
        return dc;
    }

    return dc;
}

int query::handle_error(int error_code, const std::string& error_string)
{
    clear_timers();

    if (msg_id()) {
        m_user_agent.remove_active_query(shared_from_this());
    }

    int retry_within_seconds = 0;
    bool should_retry = false;
    bool error_handled = false;

    switch (error_code) {
        case 303: // migrate
        {
            TGL_NOTICE("trying to handle migration error of " << error_string);
            int new_dc = get_dc_from_migration(error_string);
            if (new_dc > 0 && new_dc < TGL_MAX_DC_NUM) {
                m_user_agent.set_active_dc(new_dc);
                auto dc = m_user_agent.active_client();

                if (!dc->is_authorized()) {
                    dc->restart_authorization();
                }

                m_ack_received = false;
                m_session_id = 0;
                if (m_client) {
                    m_client->remove_connection_status_observer(shared_from_this());
                }
                m_client = m_user_agent.active_client();
                m_client->add_connection_status_observer(shared_from_this());
                if (should_retry_after_recover_from_error() || is_login()) {
                    should_retry = true;
                }
                error_handled = true;
            }
            break;
        }
        case 400:
            // nothing to handle
            // bad user input probably
            break;
        case 401:
            if (error_string == "SESSION_PASSWORD_NEEDED") {
                error_handled = handle_session_password_needed(should_retry);
            } else if (error_string == "AUTH_KEY_UNREGISTERED" || error_string == "AUTH_KEY_INVALID") {
                m_user_agent.set_client_logged_out(m_client, true);
                m_user_agent.login();
                if (should_retry_after_recover_from_error()) {
                    should_retry = true;
                }
                error_handled = true;
            } else if (error_string == "AUTH_KEY_PERM_EMPTY") {
                assert(m_user_agent.pfs_enabled());
                m_client->restart_temp_authorization();
                if (should_retry_after_recover_from_error()) {
                    should_retry = true;
                }
                error_handled = true;
            }
            break;
        case 403: // privacy violation
            break;
        case 404: // not found
            break;
        case 420: // flood
        case 500: // internal error
        default: // anything else treated as internal error
        {
            if (!get_int_from_prefixed_string(retry_within_seconds, error_string, "FLOOD_WAIT_")) {
                if (error_code == 420) {
                    TGL_ERROR("error 420: " << error_string);
                }
                retry_within_seconds = 10;
            }
            m_ack_received = false;
            if (should_retry_after_recover_from_error()) {
                should_retry = true;
            }
            if (!m_client->is_configured() && !is_force()) {
                m_session_id = 0;
            }
            error_handled = true;
            break;
        }
    }

    if (should_retry) {
        retry_within(retry_within_seconds);
    }

    if (error_handled) {
        TGL_NOTICE("error for query #" << msg_id() << " error:" << error_code << " " << error_string << " (HANDLED)");
        return 0;
    }

    return on_error_internal(error_code, error_string);
}

bool query::handle_session_password_needed(bool& should_retry)
{
    m_user_agent.set_dc_logged_in(m_user_agent.active_client()->id(), false);
    should_retry = true;

    if (m_user_agent.is_password_locked()) {
        return true;
    }

    m_user_agent.set_password_locked(true);

    m_user_agent.check_password([=](bool success) {
        if (!success) {
            return;
        }
        m_user_agent.set_dc_logged_in(m_user_agent.active_client()->id());
        auto q = std::make_shared<query_user_info>(m_user_agent, nullptr);
        q->out_i32(CODE_users_get_full_user);
        q->out_i32(CODE_input_user_self);
        q->execute(m_user_agent.active_client());
    });
    return true;
}

void query::retry_within(double seconds)
{
    m_user_agent.add_retry_query(shared_from_this());

    if (!m_retry_timer) {
        std::weak_ptr<query> weak_this(shared_from_this());
        m_retry_timer = m_user_agent.timer_factory()->create_timer([weak_this] {
            if (auto shared_this = weak_this.lock()) {
                shared_this->m_user_agent.remove_retry_query(shared_this);
                shared_this->alarm();
            }
        });
    }
    m_retry_timer->start(seconds);
}

void query::timeout_within(double seconds)
{
    if (!m_timer) {
        std::weak_ptr<query> weak_this(shared_from_this());
        m_timer = m_user_agent.timer_factory()->create_timer([weak_this] {
            if (auto shared_this = weak_this.lock()) {
                shared_this->timeout_alarm();
            }
        });
    }

    m_timer->start(seconds);
}

bool query::check_logging_out()
{
    if (m_client->is_logging_out()) {
        assert(!is_logout());
        if (!is_force()) {
            on_error_internal(600, "LOGGING_OUT");
            return false;
        }
    }

    return true;
}

bool query::check_pending(bool transfer_auth)
{
    bool pending = false;

    if (!m_client->session()) {
        pending = true;
        m_client->create_session();
    }

    if (m_connection_status != tgl_connection_status::connected) {
        pending = true;
    }

    if (!m_client->is_configured() && !is_force()) {
        pending = true;
    }

    if (!m_client->is_logged_in() && !is_login() && !is_force()) {
        pending = true;
        if (transfer_auth && m_client != m_user_agent.active_client()) {
            m_client->transfer_auth_to_me();
        }
    }

    if (pending) {
        will_be_pending();
        m_client->add_pending_query(shared_from_this());
        TGL_DEBUG("added query #" << msg_id() << "(type '" << name() << "') to pending list");
        return false;
    }

    return true;
}

void query::on_answer_internal(void* DS)
{
    assert(m_client);
    m_client->remove_connection_status_observer(shared_from_this());
    on_answer(DS);
}

int query::on_error_internal(int error_code, const std::string& error_string)
{
    assert(m_client);
    m_client->remove_connection_status_observer(shared_from_this());
    return on_error(error_code, error_string);
}

int query::handle_result(tgl_in_buffer* in)
{
    int32_t op = prefetch_i32(in);

    tgl_in_buffer save_in = { nullptr, nullptr };
    std::unique_ptr<int32_t[]> packed_buffer;

    if (op == CODE_gzip_packed) {
        fetch_i32(in);
        int l = prefetch_strlen(in);
        const char* s = fetch_str(in, l);

        constexpr size_t MAX_PACKED_SIZE = 1 << 24;
        packed_buffer.reset(new int32_t[MAX_PACKED_SIZE / 4]);

        int total_out = tgl_inflate(s, l, packed_buffer.get(), MAX_PACKED_SIZE);
        TGL_DEBUG("inflated " << total_out << " bytes");
        save_in = *in;
        in->ptr = packed_buffer.get();
        in->end = in->ptr + total_out / 4;
    }

    TGL_DEBUG("result for query #" << msg_id() << ". Size " << (long)4 * (in->end - in->ptr) << " bytes");

    tgl_in_buffer skip_in = *in;
    if (skip_type_any(&skip_in, &m_type) < 0) {
        TGL_ERROR("skipped " << (long)(skip_in.ptr - in->ptr) << " int out of " << (long)(skip_in.end - in->ptr) << " (type " << m_type.type.id << ") (query type " << name() << ")");
        TGL_ERROR("0x" << std::hex << *(in->ptr - 1) << " 0x" << *(in->ptr) << " 0x" << *(in->ptr + 1) << " 0x" << *(in->ptr + 2));
        TGL_ERROR(in->print_buffer());
        assert(false);
    }

    assert(skip_in.ptr == skip_in.end);

    void* DS = fetch_ds_type_any(in, &m_type);
    assert(DS);

    on_answer_internal(DS);
    free_ds_type_any(DS, &m_type);

    assert(in->ptr == in->end);

    clear_timers();

    m_user_agent.remove_active_query(shared_from_this());

    if (save_in.ptr) {
        *in = save_in;
    }

    return 0;
}

void query::out_header()
{
    m_serializer->out_i32(CODE_invoke_with_layer);
    m_serializer->out_i32(TGL_SCHEME_LAYER);

    // initConnection#69796de9 {X:Type} api_id:int device_model:string system_version:string app_version:string lang_code:string query:!X = X;
    m_serializer->out_i32(CODE_init_connection);
    m_serializer->out_i32(m_user_agent.app_id());
    m_serializer->out_std_string(m_user_agent.device_model());
    m_serializer->out_std_string(m_user_agent.system_version());
    m_serializer->out_std_string(m_user_agent.app_version());
    m_serializer->out_std_string(m_user_agent.lang_code());
}

}
}
