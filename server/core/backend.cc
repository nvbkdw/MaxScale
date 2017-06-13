/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2020-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include <maxscale/backend.hh>
#include <maxscale/protocol/mysql.h>
#include <maxscale/debug.h>

using namespace maxscale;

Backend::Backend(SERVER_REF *ref):
    m_closed(false),
    m_backend(ref),
    m_dcb(NULL),
    m_num_result_wait(0),
    m_state(0)
{
}

Backend::~Backend()
{
    ss_dassert(m_closed);

    if (!m_closed)
    {
        close();
    }
}

void Backend::close(bool fatal)
{
    if (!m_closed)
    {
        m_closed = true;

        if (in_use())
        {
            CHK_DCB(m_dcb);

            /** Clean operation counter in bref and in SERVER */
            if (is_waiting_result())
            {
                clear_state(WAITING_RESULT);
            }
            clear_state(IN_USE);
            set_state(CLOSED);

            if (fatal)
            {
                set_state(FATAL_FAILURE);
            }

            dcb_close(m_dcb);

            /** decrease server current connection counters */
            atomic_add(&m_backend->connections, -1);
        }
    }
    else
    {
        ss_dassert(false);
    }
}

bool Backend::execute_session_command()
{
    if (is_closed() || !session_command_count())
    {
        return false;
    }

    CHK_DCB(m_dcb);

    SessionCommandList::iterator iter = m_session_commands.begin();
    SessionCommand& sescmd = *(*iter);
    GWBUF *buffer = sescmd.copy_buffer().release();
    bool rval = false;

    switch (sescmd.get_command())
    {
    case MYSQL_COM_QUIT:
    case MYSQL_COM_STMT_CLOSE:
        rval = write(buffer, false);
        break;

    case MYSQL_COM_CHANGE_USER:
        /** This makes it possible to handle replies correctly */
        gwbuf_set_type(buffer, GWBUF_TYPE_SESCMD);
        rval = auth(buffer);
        break;

    case MYSQL_COM_QUERY:
    default:
        /**
         * Mark session command buffer, it triggers writing
         * MySQL command to protocol
         */
        gwbuf_set_type(buffer, GWBUF_TYPE_SESCMD);
        rval = write(buffer);
        break;
    }

    return rval;
}

void Backend::add_session_command(GWBUF* buffer, uint64_t sequence)
{
    m_session_commands.push_back(SSessionCommand(new SessionCommand(buffer, sequence)));
}

uint64_t Backend::complete_session_command()
{
    uint64_t rval = m_session_commands.front()->get_position();
    m_session_commands.pop_front();
    return rval;
}

size_t Backend::session_command_count() const
{
    return m_session_commands.size();
}

void Backend::clear_state(enum bref_state state)
{
    if ((state & WAITING_RESULT) && (m_state & WAITING_RESULT))
    {
        ss_debug(int prev2 = )atomic_add(&m_backend->server->stats.n_current_ops, -1);
        ss_dassert(prev2 > 0);
        ss_debug(int prev1 = )atomic_add(&m_num_result_wait, -1);
        ss_dassert(prev1 > 0);
    }

    m_state &= ~state;
}

void Backend::set_state(enum bref_state state)
{
    if ((state & WAITING_RESULT) && (m_state & WAITING_RESULT) == 0)
    {
        ss_debug(int prev2 = )atomic_add(&m_backend->server->stats.n_current_ops, 1);
        ss_dassert(prev2 >= 0);
        ss_debug(int prev1 = )atomic_add(&m_num_result_wait, 1);
        ss_dassert(prev1 >= 0);
    }

    m_state |= state;
}

SERVER_REF* Backend::backend() const
{
    return m_backend;
}

bool Backend::connect(MXS_SESSION* session)
{
    bool rval = false;

    if ((m_dcb = dcb_connect(m_backend->server, session, m_backend->server->protocol)))
    {
        m_state = IN_USE;
        atomic_add(&m_backend->connections, 1);
        rval = true;
    }

    return rval;
}

DCB* Backend::dcb() const
{
    return m_dcb;
}

bool Backend::write(GWBUF* buffer, bool expect_response)
{
    bool rval = m_dcb->func.write(m_dcb, buffer) != 0;

    if (rval && expect_response)
    {
        set_state(WAITING_RESULT);
    }

    return rval;
}

bool Backend::auth(GWBUF* buffer)
{
    bool rval = false;

    if (m_dcb->func.auth(m_dcb, NULL, m_dcb->session, buffer) == 1)
    {
        set_state(WAITING_RESULT);
        rval = true;
    }

    return rval;
}

void Backend::ack_write()
{
    ss_dassert(is_waiting_result());
    clear_state(WAITING_RESULT);
}

void Backend::store_command(GWBUF* buffer)
{
    m_pending_cmd.reset(buffer);
}

bool Backend::write_stored_command()
{
    bool rval = false;

    if (m_pending_cmd.length())
    {
        rval = write(m_pending_cmd.release());

        if (!rval)
        {
            MXS_ERROR("Routing of pending query failed.");
        }
    }

    return rval;
}

bool Backend::in_use() const
{
    return m_state & IN_USE;
}

bool Backend::is_waiting_result() const
{
    return m_state & WAITING_RESULT;
}

bool Backend::is_closed() const
{
    return m_state & CLOSED;
}
