/*
 * Copyright (c) 2000-2005 The Regents of The University of Michigan
 * Copyright (c) 2008 The Hewlett-Packard Development Company
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
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
 * Authors: Nathan Binkert
 */

#include <Python.h>
#include <marshal.h>
#include <signal.h>

#include <iostream>
#include <string>
#include <zlib.h>

#include "base/cprintf.hh"
#include "base/misc.hh"
#include "base/types.hh"
#include "sim/async.hh"
#include "sim/core.hh"
#include "sim/init.hh"

using namespace std;

/// Stats signal handler.
void
dumpStatsHandler(int sigtype)
{
    async_event = true;
    async_statdump = true;
}

void
dumprstStatsHandler(int sigtype)
{
    async_event = true;
    async_statdump = true;
    async_statreset = true;
}

/// Exit signal handler.
void
exitNowHandler(int sigtype)
{
    async_event = true;
    async_exit = true;
}

/// Abort signal handler.
void
abortHandler(int sigtype)
{
    ccprintf(cerr, "Program aborted at cycle %d\n", curTick);
}

/*
 * M5 can do several special things when various signals are sent.
 * None are mandatory.
 */
void
initSignals()
{
    // Floating point exceptions may happen on misspeculated paths, so
    // ignore them
    signal(SIGFPE, SIG_IGN);

    // We use SIGTRAP sometimes for debugging
    signal(SIGTRAP, SIG_IGN);

    // Dump intermediate stats
    signal(SIGUSR1, dumpStatsHandler);

    // Dump intermediate stats and reset them
    signal(SIGUSR2, dumprstStatsHandler);

    // Exit cleanly on Interrupt (Ctrl-C)
    signal(SIGINT, exitNowHandler);

    // Print out cycle number on abort
    signal(SIGABRT, abortHandler);
}

/*
 * Uncompress and unmarshal the code object stored in the
 * EmbeddedPyModule
 */
PyObject *
getCode(const EmbeddedPyModule *pymod)
{
    assert(pymod->zlen == pymod->code_end - pymod->code);
    Bytef *marshalled = new Bytef[pymod->mlen];
    uLongf unzlen = pymod->mlen;
    int ret = uncompress(marshalled, &unzlen, (const Bytef *)pymod->code,
        pymod->zlen);
    if (ret != Z_OK)
        panic("Could not uncompress code: %s\n", zError(ret));
    assert(unzlen == (uLongf)pymod->mlen);

    return PyMarshal_ReadObjectFromString((char *)marshalled, pymod->mlen);
}

// The python library is totally messed up with respect to constness,
// so make a simple macro to make life a little easier
#define PyCC(x) (const_cast<char *>(x))

/*
 * Load and initialize all of the python parts of M5, including Swig
 * and the embedded module importer.
 */
int
initM5Python()
{
    extern void initSwig();

    // initialize SWIG modules.  initSwig() is autogenerated and calls
    // all of the individual swig initialization functions.
    initSwig();

    // Load the importer module
    PyObject *code = getCode(&embeddedPyImporter);
    PyObject *module = PyImport_ExecCodeModule(PyCC("importer"), code);
    if (!module) {
        PyErr_Print();
        return 1;
    }

    // Load the rest of the embedded python files into the embedded
    // python importer
    const EmbeddedPyModule *pymod = &embeddedPyModules[0];
    while (pymod->filename) {
        PyObject *code = getCode(pymod);
        PyObject *result = PyObject_CallMethod(module, PyCC("add_module"),
            PyCC("sssO"), pymod->filename, pymod->abspath, pymod->modpath,
            code);
        if (!result) {
            PyErr_Print();
            return 1;
        }
        Py_DECREF(result);
        ++pymod;
    }

    return 0;
}

/*
 * Start up the M5 simulator.  This mostly vectors into the python
 * main function.
 */
int
m5Main(int argc, char **argv)
{
    PySys_SetArgv(argc, argv);

    // We have to set things up in the special __main__ module
    PyObject *module = PyImport_AddModule(PyCC("__main__"));
    if (module == NULL)
        panic("Could not import __main__");
    PyObject *dict = PyModule_GetDict(module);

    // import the main m5 module
    PyObject *result;
    result = PyRun_String("import m5", Py_file_input, dict, dict);
    if (!result) {
        PyErr_Print();
        return 1;
    }
    Py_DECREF(result);

    // Start m5
    result = PyRun_String("m5.main()", Py_file_input, dict, dict);
    if (!result) {
        PyErr_Print();
        return 1;
    }
    Py_DECREF(result);

    return 0;
}

PyMODINIT_FUNC
initm5(void)
{
    initM5Python();
    PyImport_ImportModule(PyCC("m5"));
}
