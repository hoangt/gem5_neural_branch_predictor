/*
 * Copyright (c) 2007 The Hewlett-Packard Development Company
 * All rights reserved.
 *
 * Redistribution and use of this software in source and binary forms,
 * with or without modification, are permitted provided that the
 * following conditions are met:
 *
 * The software must be used only for Non-Commercial Use which means any
 * use which is NOT directed to receiving any direct monetary
 * compensation for, or commercial advantage from such use.  Illustrative
 * examples of non-commercial use are academic research, personal study,
 * teaching, education and corporate research & development.
 * Illustrative examples of commercial use are distributing products for
 * commercial advantage and providing services using the software for
 * commercial advantage.
 *
 * If you wish to use this software or functionality therein that may be
 * covered by patents for commercial use, please contact:
 *     Director of Intellectual Property Licensing
 *     Office of Strategy and Technology
 *     Hewlett-Packard Company
 *     1501 Page Mill Road
 *     Palo Alto, California  94304
 *
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.  Redistributions
 * in binary form must reproduce the above copyright notice, this list of
 * conditions and the following disclaimer in the documentation and/or
 * other materials provided with the distribution.  Neither the name of
 * the COPYRIGHT HOLDER(s), HEWLETT-PACKARD COMPANY, nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.  No right of
 * sublicense is granted herewith.  Derivatives of the software and
 * output created using the software may be prepared, but only for
 * Non-Commercial Uses.  Derivatives of the software may be shared with
 * others provided: (i) the others agree to abide by the list of
 * conditions herein which includes the Non-Commercial Use restrictions;
 * and (ii) such Derivatives of the software include the above copyright
 * notice to acknowledge the contribution from this software where
 * applicable, this list of conditions and the disclaimer below.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Gabe Black
 */

#include "arch/x86/pagetable.hh"
#include "arch/x86/pagetable_walker.hh"
#include "arch/x86/tlb.hh"
#include "base/bitfield.hh"
#include "cpu/thread_context.hh"
#include "cpu/base.hh"
#include "mem/packet_access.hh"
#include "mem/request.hh"
#include "sim/system.hh"

namespace X86ISA {

// Unfortunately, the placement of the base field in a page table entry is
// very erratic and would make a mess here. It might be moved here at some
// point in the future.
BitUnion64(PageTableEntry)
    Bitfield<63> nx;
    Bitfield<11, 9> avl;
    Bitfield<8> g;
    Bitfield<7> ps;
    Bitfield<6> d;
    Bitfield<5> a;
    Bitfield<4> pcd;
    Bitfield<3> pwt;
    Bitfield<2> u;
    Bitfield<1> w;
    Bitfield<0> p;
EndBitUnion(PageTableEntry)

void
Walker::doNext(PacketPtr &read, PacketPtr &write)
{
    assert(state != Ready && state != Waiting);
    write = NULL;
    PageTableEntry pte;
    if (size == 8)
        pte = read->get<uint64_t>();
    else
        pte = read->get<uint32_t>();
    VAddr vaddr = entry.vaddr;
    bool uncacheable = pte.pcd;
    Addr nextRead = 0;
    bool doWrite = false;
    bool badNX = pte.nx && (!tlb->allowNX() || !enableNX);
    switch(state) {
      case LongPML4:
        nextRead = ((uint64_t)pte & (mask(40) << 12)) + vaddr.longl3 * size;
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = pte.w;
        entry.user = pte.u;
        if (badNX)
            panic("NX violation!\n");
        entry.noExec = pte.nx;
        if (!pte.p)
            panic("Page not present!\n");
        nextState = LongPDP;
        break;
      case LongPDP:
        nextRead = ((uint64_t)pte & (mask(40) << 12)) + vaddr.longl2 * size;
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = entry.writable && pte.w;
        entry.user = entry.user && pte.u;
        if (badNX)
            panic("NX violation!\n");
        if (!pte.p)
            panic("Page not present!\n");
        nextState = LongPD;
        break;
      case LongPD:
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = entry.writable && pte.w;
        entry.user = entry.user && pte.u;
        if (badNX)
            panic("NX violation!\n");
        if (!pte.p)
            panic("Page not present!\n");
        if (!pte.ps) {
            // 4 KB page
            entry.size = 4 * (1 << 10);
            nextRead =
                ((uint64_t)pte & (mask(40) << 12)) + vaddr.longl1 * size;
            nextState = LongPTE;
            break;
        } else {
            // 2 MB page
            entry.size = 2 * (1 << 20);
            entry.paddr = (uint64_t)pte & (mask(31) << 21);
            entry.uncacheable = uncacheable;
            entry.global = pte.g;
            entry.patBit = bits(pte, 12);
            entry.vaddr = entry.vaddr & ~((2 * (1 << 20)) - 1);
            tlb->insert(entry.vaddr, entry);
            nextState = Ready;
            delete read->req;
            delete read;
            read = NULL;
            return;
        }
      case LongPTE:
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = entry.writable && pte.w;
        entry.user = entry.user && pte.u;
        if (badNX)
            panic("NX violation!\n");
        if (!pte.p)
            panic("Page not present!\n");
        entry.paddr = (uint64_t)pte & (mask(40) << 12);
        entry.uncacheable = uncacheable;
        entry.global = pte.g;
        entry.patBit = bits(pte, 12);
        entry.vaddr = entry.vaddr & ~((4 * (1 << 10)) - 1);
        tlb->insert(entry.vaddr, entry);
        nextState = Ready;
        delete read->req;
        delete read;
        read = NULL;
        return;
      case PAEPDP:
        nextRead = ((uint64_t)pte & (mask(40) << 12)) + vaddr.pael2 * size;
        if (!pte.p)
            panic("Page not present!\n");
        nextState = PAEPD;
        break;
      case PAEPD:
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = pte.w;
        entry.user = pte.u;
        if (badNX)
            panic("NX violation!\n");
        if (!pte.p)
            panic("Page not present!\n");
        if (!pte.ps) {
            // 4 KB page
            entry.size = 4 * (1 << 10);
            nextRead = ((uint64_t)pte & (mask(40) << 12)) + vaddr.pael1 * size;
            nextState = PAEPTE;
            break;
        } else {
            // 2 MB page
            entry.size = 2 * (1 << 20);
            entry.paddr = (uint64_t)pte & (mask(31) << 21);
            entry.uncacheable = uncacheable;
            entry.global = pte.g;
            entry.patBit = bits(pte, 12);
            entry.vaddr = entry.vaddr & ~((2 * (1 << 20)) - 1);
            tlb->insert(entry.vaddr, entry);
            nextState = Ready;
            delete read->req;
            delete read;
            read = NULL;
            return;
        }
      case PAEPTE:
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = entry.writable && pte.w;
        entry.user = entry.user && pte.u;
        if (badNX)
            panic("NX violation!\n");
        if (!pte.p)
            panic("Page not present!\n");
        entry.paddr = (uint64_t)pte & (mask(40) << 12);
        entry.uncacheable = uncacheable;
        entry.global = pte.g;
        entry.patBit = bits(pte, 7);
        entry.vaddr = entry.vaddr & ~((4 * (1 << 10)) - 1);
        tlb->insert(entry.vaddr, entry);
        nextState = Ready;
        delete read->req;
        delete read;
        read = NULL;
        return;
      case PSEPD:
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = pte.w;
        entry.user = pte.u;
        if (!pte.p)
            panic("Page not present!\n");
        if (!pte.ps) {
            // 4 KB page
            entry.size = 4 * (1 << 10);
            nextRead =
                ((uint64_t)pte & (mask(20) << 12)) + vaddr.norml2 * size;
            nextState = PTE;
            break;
        } else {
            // 4 MB page
            entry.size = 4 * (1 << 20);
            entry.paddr = bits(pte, 20, 13) << 32 | bits(pte, 31, 22) << 22;
            entry.uncacheable = uncacheable;
            entry.global = pte.g;
            entry.patBit = bits(pte, 12);
            entry.vaddr = entry.vaddr & ~((4 * (1 << 20)) - 1);
            tlb->insert(entry.vaddr, entry);
            nextState = Ready;
            delete read->req;
            delete read;
            read = NULL;
            return;
        }
      case PD:
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = pte.w;
        entry.user = pte.u;
        if (!pte.p)
            panic("Page not present!\n");
        // 4 KB page
        entry.size = 4 * (1 << 10);
        nextRead = ((uint64_t)pte & (mask(20) << 12)) + vaddr.norml2 * size;
        nextState = PTE;
        break;
        nextState = PTE;
        break;
      case PTE:
        doWrite = !pte.a;
        pte.a = 1;
        entry.writable = pte.w;
        entry.user = pte.u;
        if (!pte.p)
            panic("Page not present!\n");
        entry.paddr = (uint64_t)pte & (mask(20) << 12);
        entry.uncacheable = uncacheable;
        entry.global = pte.g;
        entry.patBit = bits(pte, 7);
        entry.vaddr = entry.vaddr & ~((4 * (1 << 10)) - 1);
        tlb->insert(entry.vaddr, entry);
        nextState = Ready;
        delete read->req;
        delete read;
        read = NULL;
        return;
      default:
        panic("Unknown page table walker state %d!\n");
    }
    PacketPtr oldRead = read;
    //If we didn't return, we're setting up another read.
    uint32_t flags = oldRead->req->getFlags();
    if (uncacheable)
        flags |= UNCACHEABLE;
    else
        flags &= ~UNCACHEABLE;
    RequestPtr request =
        new Request(nextRead, oldRead->getSize(), flags);
    read = new Packet(request, MemCmd::ReadExReq, Packet::Broadcast);
    read->allocate();
    //If we need to write, adjust the read packet to write the modified value
    //back to memory.
    if (doWrite) {
        write = oldRead;
        write->set<uint64_t>(pte);
        write->cmd = MemCmd::WriteReq;
        write->setDest(Packet::Broadcast);
    } else {
        write = NULL;
        delete oldRead->req;
        delete oldRead;
    }
}

void
Walker::start(ThreadContext * _tc, Addr vaddr)
{
    assert(state == Ready);
    assert(!tc);
    tc = _tc;

    VAddr addr = vaddr;

    //Figure out what we're doing.
    CR3 cr3 = tc->readMiscRegNoEffect(MISCREG_CR3);
    Addr top = 0;
    // Check if we're in long mode or not
    Efer efer = tc->readMiscRegNoEffect(MISCREG_EFER);
    size = 8;
    if (efer.lma) {
        // Do long mode.
        state = LongPML4;
        top = (cr3.longPdtb << 12) + addr.longl4 * size;
    } else {
        // We're in some flavor of legacy mode.
        CR4 cr4 = tc->readMiscRegNoEffect(MISCREG_CR4);
        if (cr4.pae) {
            // Do legacy PAE.
            state = PAEPDP;
            top = (cr3.paePdtb << 5) + addr.pael3 * size;
        } else {
            size = 4;
            top = (cr3.pdtb << 12) + addr.norml2 * size;
            if (cr4.pse) {
                // Do legacy PSE.
                state = PSEPD;
            } else {
                // Do legacy non PSE.
                state = PD;
            }
        }
    }

    nextState = Ready;
    entry.vaddr = vaddr;

    enableNX = efer.nxe;

    RequestPtr request =
        new Request(top, size, PHYSICAL | cr3.pcd ? UNCACHEABLE : 0);
    read = new Packet(request, MemCmd::ReadExReq, Packet::Broadcast);
    read->allocate();
    Enums::MemoryMode memMode = sys->getMemoryMode();
    if (memMode == Enums::timing) {
        tc->suspend();
        port.sendTiming(read);
    } else if (memMode == Enums::atomic) {
        do {
            port.sendAtomic(read);
            PacketPtr write = NULL;
            doNext(read, write);
            state = nextState;
            nextState = Ready;
            if (write)
                port.sendAtomic(write);
        } while(read);
        tc = NULL;
        state = Ready;
        nextState = Waiting;
    } else {
        panic("Unrecognized memory system mode.\n");
    }
}

bool
Walker::WalkerPort::recvTiming(PacketPtr pkt)
{
    return walker->recvTiming(pkt);
}

bool
Walker::recvTiming(PacketPtr pkt)
{
    inflight--;
    if (pkt->isResponse() && !pkt->wasNacked()) {
        if (pkt->isRead()) {
            assert(inflight);
            assert(state == Waiting);
            assert(!read);
            state = nextState;
            nextState = Ready;
            PacketPtr write = NULL;
            doNext(pkt, write);
            state = Waiting;
            read = pkt;
            if (write) {
                writes.push_back(write);
            }
            sendPackets();
        } else {
            sendPackets();
        }
        if (inflight == 0 && read == NULL && writes.size() == 0) {
            tc->activate(0);
            tc = NULL;
            state = Ready;
            nextState = Waiting;
        }
    } else if (pkt->wasNacked()) {
        pkt->reinitNacked();
        if (!port.sendTiming(pkt)) {
            retrying = true;
            if (pkt->isWrite()) {
                writes.push_back(pkt);
            } else {
                assert(!read);
                read = pkt;
            }
        } else {
            inflight++;
        }
    }
    return true;
}

Tick
Walker::WalkerPort::recvAtomic(PacketPtr pkt)
{
    return 0;
}

void
Walker::WalkerPort::recvFunctional(PacketPtr pkt)
{
    return;
}

void
Walker::WalkerPort::recvStatusChange(Status status)
{
    if (status == RangeChange) {
        if (!snoopRangeSent) {
            snoopRangeSent = true;
            sendStatusChange(Port::RangeChange);
        }
        return;
    }

    panic("Unexpected recvStatusChange.\n");
}

void
Walker::WalkerPort::recvRetry()
{
    walker->recvRetry();
}

void
Walker::recvRetry()
{
    retrying = false;
    sendPackets();
}

void
Walker::sendPackets()
{
    //If we're already waiting for the port to become available, just return.
    if (retrying)
        return;

    //Reads always have priority
    if (read) {
        if (!port.sendTiming(read)) {
            retrying = true;
            return;
        } else {
            inflight++;
            delete read->req;
            delete read;
            read = NULL;
        }
    }
    //Send off as many of the writes as we can.
    while (writes.size()) {
        PacketPtr write = writes.back();
        if (!port.sendTiming(write)) {
            retrying = true;
            return;
        } else {
            inflight++;
            delete write->req;
            delete write;
            writes.pop_back();
        }
    }
}

Port *
Walker::getPort(const std::string &if_name, int idx)
{
    if (if_name == "port")
        return &port;
    else
        panic("No page table walker port named %s!\n", if_name);
}

}

X86ISA::Walker *
X86PagetableWalkerParams::create()
{
    return new X86ISA::Walker(this);
}