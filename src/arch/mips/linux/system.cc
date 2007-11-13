/*
 * Copyright .AN) 2007 MIPS Technologies, Inc.  All Rights Reserved
 *
 * This software is part of the M5 simulator.
 *
 * THIS IS A LEGAL AGREEMENT.  BY DOWNLOADING, USING, COPYING, CREATING
 * DERIVATIVE WORKS, AND/OR DISTRIBUTING THIS SOFTWARE YOU ARE AGREEING
 * TO THESE TERMS AND CONDITIONS.
 *
 * Permission is granted to use, copy, create derivative works and
 * distribute this software and such derivative works for any purpose,
 * so long as (1) the copyright notice above, this grant of permission,
 * and the disclaimer below appear in all copies and derivative works
 * made, (2) the copyright notice above is augmented as appropriate to
 * reflect the addition of any new copyrightable work in a derivative
 * work (e.g., Copyright .AN) <Publication Year> Copyright Owner), and (3)
 * the name of MIPS Technologies, Inc. ($B!H(BMIPS$B!I(B) is not used in any
 * advertising or publicity pertaining to the use or distribution of
 * this software without specific, written prior authorization.
 *
 * THIS SOFTWARE IS PROVIDED $B!H(BAS IS.$B!I(B  MIPS MAKES NO WARRANTIES AND
 * DISCLAIMS ALL WARRANTIES, WHETHER EXPRESS, STATUTORY, IMPLIED OR
 * OTHERWISE, INCLUDING BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND
 * NON-INFRINGEMENT OF THIRD PARTY RIGHTS, REGARDING THIS SOFTWARE.
 * IN NO EVENT SHALL MIPS BE LIABLE FOR ANY DAMAGES, INCLUDING DIRECT,
 * INDIRECT, INCIDENTAL, CONSEQUENTIAL, SPECIAL, OR PUNITIVE DAMAGES OF
 * ANY KIND OR NATURE, ARISING OUT OF OR IN CONNECTION WITH THIS AGREEMENT,
 * THIS SOFTWARE AND/OR THE USE OF THIS SOFTWARE, WHETHER SUCH LIABILITY
 * IS ASSERTED ON THE BASIS OF CONTRACT, TORT (INCLUDING NEGLIGENCE OR
 * STRICT LIABILITY), OR OTHERWISE, EVEN IF MIPS HAS BEEN WARNED OF THE
 * POSSIBILITY OF ANY SUCH LOSS OR DAMAGE IN ADVANCE.
 *
 *
 * Authors: Ali G. Saidi
 *          Lisa R. Hsu
 *          Nathan L. Binkert
 *          Steven K. Reinhardt
 */

/**
 * @file
 * This code loads the linux kernel, console, pal and patches certain
 * functions.  The symbol tables are loaded so that traces can show
 * the executing function and we can skip functions. Various delay
 * loops are skipped and their final values manually computed to speed
 * up boot time.
 */

#include "arch/vtophys.hh"
#include "arch/mips/idle_event.hh"
#include "arch/mips/linux/system.hh"
#include "arch/mips/linux/threadinfo.hh"
#include "arch/mips/system.hh"
#include "base/loader/symtab.hh"
#include "cpu/thread_context.hh"
#include "cpu/base.hh"
#include "dev/platform.hh"
#include "kern/linux/printk.hh"
#include "kern/linux/events.hh"
#include "mem/physical.hh"
#include "mem/port.hh"
#include "sim/arguments.hh"
#include "sim/byteswap.hh"

using namespace std;
using namespace MipsISA;
using namespace Linux;

LinuxMipsSystem::LinuxMipsSystem(Params *p)
    : MipsSystem(p)
{
    Addr addr = 0;

    /**
     * The symbol swapper_pg_dir marks the beginning of the kernel and
     * the location of bootloader passed arguments
     */
    if (!kernelSymtab->findAddress("swapper_pg_dir", KernelStart)) {
        panic("Could not determine start location of kernel");
    }

    /**
     * Since we aren't using a bootloader, we have to copy the
     * kernel arguments directly into the kernel's memory.
     */
    virtPort.writeBlob(CommandLine(), (uint8_t*)params()->boot_osflags.c_str(),
                params()->boot_osflags.length()+1);

    /**
     * find the address of the est_cycle_freq variable and insert it
     * so we don't through the lengthly process of trying to
     * calculated it by using the PIT, RTC, etc.
     */
    if (kernelSymtab->findAddress("est_cycle_freq", addr))
        virtPort.write(addr, (uint64_t)(Clock::Frequency /
                    p->boot_cpu_frequency));


    /**
     * EV5 only supports 127 ASNs so we are going to tell the kernel that the
     * paritiuclar EV6 we have only supports 127 asns.
     * @todo At some point we should change ev5.hh and the palcode to support
     * 255 ASNs.
     */
    if (kernelSymtab->findAddress("dp264_mv", addr))
        virtPort.write(addr + 0x18, LittleEndianGuest::htog((uint32_t)127));
    else
        panic("could not find dp264_mv\n");

#ifndef NDEBUG
    kernelPanicEvent = addKernelFuncEvent<BreakPCEvent>("panic");
    if (!kernelPanicEvent)
        panic("could not find kernel symbol \'panic\'");

#if 0
    kernelDieEvent = addKernelFuncEvent<BreakPCEvent>("die_if_kernel");
    if (!kernelDieEvent)
        panic("could not find kernel symbol \'die_if_kernel\'");
#endif

#endif

    /**
     * Any time ide_delay_50ms, calibarte_delay or
     * determine_cpu_caches is called just skip the
     * function. Currently determine_cpu_caches only is used put
     * information in proc, however if that changes in the future we
     * will have to fill in the cache size variables appropriately.
     */

    skipIdeDelay50msEvent =
        addKernelFuncEvent<SkipFuncEvent>("ide_delay_50ms");
    skipDelayLoopEvent =
        addKernelFuncEvent<SkipDelayLoopEvent>("calibrate_delay");
    skipCacheProbeEvent =
        addKernelFuncEvent<SkipFuncEvent>("determine_cpu_caches");
    debugPrintkEvent = addKernelFuncEvent<DebugPrintkEvent>("dprintk");
    idleStartEvent = addKernelFuncEvent<IdleStartEvent>("cpu_idle");

    // Disable for now as it runs into panic() calls in VPTr methods
    // (see sim/vptr.hh).  Once those bugs are fixed, we can
    // re-enable, but we should find a better way to turn it on than
    // using DTRACE(Thread), since looking at a trace flag at tick 0
    // leads to non-intuitive behavior with --trace-start.
    if (false && kernelSymtab->findAddress("mips_switch_to", addr)) {
        printThreadEvent = new PrintThreadInfo(&pcEventQueue, "threadinfo",
                                               addr + sizeof(MachInst) * 6);
    } else {
        printThreadEvent = NULL;
    }
}

LinuxMipsSystem::~LinuxMipsSystem()
{
#ifndef NDEBUG
    delete kernelPanicEvent;
#endif
    delete skipIdeDelay50msEvent;
    delete skipDelayLoopEvent;
    delete skipCacheProbeEvent;
    delete debugPrintkEvent;
    delete idleStartEvent;
    delete printThreadEvent;
}


void
LinuxMipsSystem::setDelayLoop(ThreadContext *tc)
{
    Addr addr = 0;
    if (kernelSymtab->findAddress("loops_per_jiffy", addr)) {
        Tick cpuFreq = tc->getCpuPtr()->frequency();
        Tick intrFreq = platform->intrFrequency();
        VirtualPort *vp;

        vp = tc->getVirtPort();
        vp->writeHtoG(addr, (uint32_t)((cpuFreq / intrFreq) * 0.9988));
        tc->delVirtPort(vp);
    }
}


void
LinuxMipsSystem::SkipDelayLoopEvent::process(ThreadContext *tc)
{
    SkipFuncEvent::process(tc);
    // calculate and set loops_per_jiffy
    ((LinuxMipsSystem *)tc->getSystemPtr())->setDelayLoop(tc);
}

void
LinuxMipsSystem::PrintThreadInfo::process(ThreadContext *tc)
{
    Linux::ThreadInfo ti(tc);

    DPRINTF(Thread, "Currently Executing Thread %s, pid %d, started at: %d\n",
            ti.curTaskName(), ti.curTaskPID(), ti.curTaskStart());
}

LinuxMipsSystem *
LinuxMipsSystemParams::create()
{
    return new LinuxMipsSystem(this);
}