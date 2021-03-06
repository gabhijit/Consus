// Copyright (c) 2015-2017, Robert Escriva, Cornell University
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//     * Redistributions of source code must retain the above copyright notice,
//       this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of Consus nor the names of its contributors may be
//       used to endorse or promote products derived from this software without
//       specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

// STL
#include <sstream>

// consus
#include "txman/paxos_synod.h"

using consus::paxos_synod;

paxos_synod :: paxos_synod()
    : m_init(false)
    , m_us()
    , m_group()
    , m_proposed(0)
    , m_acceptor_ballot()
    , m_acceptor_pvalue()
    , m_leader_phase()
    , m_leader_ballot()
    , m_leader_pvalue()
    , m_value()
{
}

paxos_synod :: ~paxos_synod() throw ()
{
}

void
paxos_synod :: init(comm_id us, const paxos_group& pg, comm_id leader)
{
    assert(!m_init);
    m_init = true;
    m_us = us;
    m_group = pg;

    // implicit leader
    ballot implicit_leader(1, leader);

    // setup the acceptor
    m_acceptor_ballot = implicit_leader;

    // setup the leader
    if (us == leader)
    {
        m_leader_phase = PHASE1;
        m_leader_ballot = implicit_leader;

        for (unsigned i = 0; i < m_group.members_sz; ++i)
        {
            m_promises[i].current_ballot = implicit_leader;
        }
    }
    else
    {
        m_leader_ballot = ballot(1, m_us);
    }
}

void
paxos_synod :: propose(uint64_t value)
{
    assert(!m_proposed || m_proposed == value);
    m_proposed = value;
}

void
paxos_synod :: advance(bool* send_p1a, ballot* p1a,
                       bool* send_p2a, pvalue* p2a,
                       bool* send_learn, uint64_t* _learned)
{
    *send_p1a = false;
    *send_p2a = false;
    *send_learn = false;

    // once learned, always learned
    // Paxos will uphold this, but why waste resources running the protocol
    // again?
    if (m_leader_phase == LEARNED)
    {
        *send_learn = true;
        *_learned = learned();
        return;
    }

    for (size_t i = 0; i < m_group.members_sz; ++i)
    {
        if (m_promises[i].current_ballot > m_leader_ballot)
        {
            m_leader_ballot.number = m_promises[i].current_ballot.number + 1;
        }
    }

    if (!m_proposed)
    {
        return;
    }

    if (m_leader_phase == PHASE1)
    {
        unsigned accepted = 0;

        for (size_t i = 0; i < m_group.members_sz; ++i)
        {
            if (m_leader_ballot != ballot() &&
                m_promises[i].current_ballot == m_leader_ballot)
            {
                ++accepted;
            }
        }

        if (accepted >= m_group.quorum())
        {
            m_leader_phase = PHASE2;
            m_leader_pvalue = pvalue();

            for (unsigned i = 0; i < m_group.members_sz; ++i)
            {
                m_leader_pvalue = std::max(m_leader_pvalue, m_promises[i].current_pvalue);
            }

            if (m_leader_pvalue == pvalue())
            {
                m_leader_pvalue = pvalue(m_leader_ballot, m_proposed);
            }
            else
            {
                m_leader_pvalue.b = m_leader_ballot;
            }
        }

        if (accepted < m_group.members_sz)
        {
            *send_p1a = true;
            *p1a = m_leader_ballot;
        }
    }

    if (m_leader_phase == PHASE2)
    {
        unsigned accepted = 0;

        for (size_t i = 0; i < m_group.members_sz; ++i)
        {
            if (m_promises[i].current_ballot == m_leader_ballot &&
                m_promises[i].current_pvalue == m_leader_pvalue &&
                m_leader_pvalue.b == m_leader_ballot)
            {
                ++accepted;
            }
        }

        if (accepted >= m_group.quorum())
        {
            m_leader_phase = LEARNED;
            m_value = m_leader_pvalue.v;
            *send_learn = true;
            *_learned = m_value;
        }

        if (accepted < m_group.members_sz)
        {
            *send_p2a = true;
            *p2a = m_leader_pvalue;
        }
    }

}

void
paxos_synod :: phase1a(const ballot& b, ballot* a, pvalue* p)
{
    assert(m_init);

    if (b > m_acceptor_ballot)
    {
        m_acceptor_ballot = b;
    }

    *a = m_acceptor_ballot;
    *p = m_acceptor_pvalue;
}

void
paxos_synod :: phase1b(comm_id m, const ballot& a, const pvalue& p)
{
    assert(m_init);
    unsigned idx = m_group.index(m);

    if (idx >= m_group.members_sz)
    {
        return;
    }

    if (m_promises[idx].current_ballot <= a)
    {
        m_promises[idx].current_ballot = a;
        m_promises[idx].current_pvalue = p;
    }
}

void
paxos_synod :: phase2a(const pvalue& p, bool* a)
{
    assert(m_init);

    if (p.b == m_acceptor_ballot)
    {
        m_acceptor_pvalue = p;
        *a = true;
    }
    else
    {
        *a = false;
    }
}

void
paxos_synod :: phase2b(comm_id m, const pvalue& p)
{
    assert(m_init);
    unsigned idx = m_group.index(m);

    if (idx >= m_group.members_sz)
    {
        return;
    }

    if (m_promises[idx].current_ballot == p.b)
    {
        m_promises[idx].current_pvalue = p;
    }
    else if (m_promises[idx].current_ballot < p.b)
    {
        m_leader_phase = PHASE1;
    }
}

void
paxos_synod :: force_learn(uint64_t value)
{
    assert(m_init);
    m_leader_phase = LEARNED;
    m_value = value;
}

bool
paxos_synod :: has_learned()
{
    assert(m_init);
    return m_leader_phase == LEARNED;
}

uint64_t
paxos_synod :: learned()
{
    assert(m_init);
    assert(m_leader_phase == LEARNED);
    return m_value;
}

std::string
paxos_synod :: debug_dump()
{
    if (!m_init)
    {
        return "uninitialized paxos instance";
    }

    std::ostringstream ostr;
    ostr << m_us << " in " << m_group << "\n";

    if (m_proposed)
    {
        ostr << "proposed " << m_proposed << "\n";
    }
    else
    {
        ostr << "no value proposed\n";
    }

    ostr << "acceptor " << m_acceptor_ballot << "\n";
    ostr << "acceptor " << m_acceptor_pvalue << "\n";

    switch (m_leader_phase)
    {
        case PHASE1:
            ostr << "phase 1\n";
            break;
        case PHASE2:
            ostr << "phase 2\n";
            break;
        case LEARNED:
            ostr << "learned\n";
            break;
        default:
            ostr << "phase unknown\n";
            break;
    }

    ostr << "leader " << m_leader_ballot << "\n";
    ostr << "leader " << m_leader_pvalue << "\n";

    for (size_t i = 0; i < m_group.members_sz; ++i)
    {
        ostr << "promise[" << i << "] " << m_promises[i].current_ballot << " " << m_promises[i].current_pvalue << "\n";
    }

    if (m_value)
    {
        ostr << "learned " << m_value << "\n";
    }
    else
    {
        ostr << "no value learned\n";
    }

    return ostr.str();
}

paxos_synod :: ballot :: ballot()
    : number(0)
    , leader()
{
}

paxos_synod :: ballot :: ballot(uint64_t n, comm_id l)
    : number(n)
    , leader(l)
{
}

paxos_synod :: ballot :: ballot(const ballot& other)
    : number(other.number)
    , leader(other.leader)
{
}

paxos_synod :: ballot :: ~ballot() throw ()
{
}

int
paxos_synod :: ballot :: compare(const ballot& rhs) const
{
    if (number < rhs.number)
    {
        return -1;
    }
    if (number > rhs.number)
    {
        return 1;
    }

    if (leader < rhs.leader)
    {
        return -1;
    }
    if (leader > rhs.leader)
    {
        return 1;
    }

    return 0;
}

paxos_synod::ballot&
paxos_synod :: ballot :: operator = (const ballot& rhs)
{
    if (this != &rhs)
    {
        number = rhs.number;
        leader = rhs.leader;
    }

    return *this;
}

bool paxos_synod :: ballot :: operator < (const ballot& rhs) const { return compare(rhs) < 0; }
bool paxos_synod :: ballot :: operator <= (const ballot& rhs) const { return compare(rhs) <= 0; }
bool paxos_synod :: ballot :: operator == (const ballot& rhs) const { return compare(rhs) == 0; }
bool paxos_synod :: ballot :: operator != (const ballot& rhs) const { return compare(rhs) != 0; }
bool paxos_synod :: ballot :: operator >= (const ballot& rhs) const { return compare(rhs) >= 0; }
bool paxos_synod :: ballot :: operator > (const ballot& rhs) const { return compare(rhs) > 0; }

paxos_synod :: pvalue :: pvalue()
    : b()
    , v()
{
}

paxos_synod :: pvalue :: pvalue(const ballot& _b, uint64_t _v)
    : b(_b)
    , v(_v)
{
}

paxos_synod :: pvalue :: pvalue(const pvalue& other)
    : b(other.b)
    , v(other.v)
{
}

paxos_synod :: pvalue :: ~pvalue() throw ()
{
}

int
paxos_synod :: pvalue :: compare(const pvalue& rhs) const
{
    return b.compare(rhs.b);
}

paxos_synod::pvalue&
paxos_synod :: pvalue :: operator = (const pvalue& rhs)
{
    if (this != &rhs)
    {
        b = rhs.b;
        v = rhs.v;
    }

    return *this;
}

bool paxos_synod :: pvalue :: operator < (const pvalue& rhs) const { return compare(rhs) < 0; }
bool paxos_synod :: pvalue :: operator <= (const pvalue& rhs) const { return compare(rhs) <= 0; }
bool paxos_synod :: pvalue :: operator == (const pvalue& rhs) const { return compare(rhs) == 0; }
bool paxos_synod :: pvalue :: operator != (const pvalue& rhs) const { return compare(rhs) != 0; }
bool paxos_synod :: pvalue :: operator >= (const pvalue& rhs) const { return compare(rhs) >= 0; }
bool paxos_synod :: pvalue :: operator > (const pvalue& rhs) const { return compare(rhs) > 0; }

paxos_synod :: promise :: promise()
    : current_ballot()
    , current_pvalue()
{
}

paxos_synod :: promise :: ~promise() throw ()
{
}

std::ostream&
consus :: operator << (std::ostream& out, const paxos_synod::ballot& b)
{
    return out << "ballot(number=" << b.number
               << ", leader=" << b.leader.get() << ")";
}

std::ostream&
consus :: operator << (std::ostream& out, const paxos_synod::pvalue& p)
{
    return out << "pvalue(number=" << p.b.number
               << ", leader=" << p.b.leader.get()
               << ", value=" << p.v << ")";
}

e::packer
consus :: operator << (e::packer pa, const paxos_synod::ballot& rhs)
{
    return pa << rhs.number << rhs.leader;
}

e::unpacker
consus :: operator >> (e::unpacker up, paxos_synod::ballot& rhs)
{
    return up >> rhs.number >> rhs.leader;
}

size_t
consus :: pack_size(const paxos_synod::ballot& b)
{
    return sizeof(uint64_t) + pack_size(b.leader);
}

e::packer
consus :: operator << (e::packer pa, const paxos_synod::pvalue& rhs)
{
    return pa << rhs.b << rhs.v;
}

e::unpacker
consus :: operator >> (e::unpacker up, paxos_synod::pvalue& rhs)
{
    return up >> rhs.b >> rhs.v;
}

size_t
consus :: pack_size(const paxos_synod::pvalue& p)
{
    return sizeof(uint64_t) + pack_size(p.b);
}
