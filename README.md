# AfterOMPT

OMPT tool for generating [Aftermath](https://www.aftermath-tracing.com/) traces.

## Overview

AfterOMPT is the OpenMP first-party tool that implements OMPT callbacks
in order to track OpenMP events and to access the runtime state of the
system. We use Aftermath tracing library to collect information about
those events and save them to the trace file that can be later viewed
using Aftermath GUI. Detailed information about OMPT can be found
in the [OpenMP standard](https://www.openmp.org/specifications/)

## Available tracing information

Currently AfterOMPT traces the following states and events:

```
openmp_thread,
openmp_parallel,
openmp_task_create,
openmp_task_schedule,
openmp_implicit_task,
openmp_sync_region_wait,
openmp_mutex_released,
openmp_dependences,
openmp_task_dependence,
openmp_work,
openmp_master,
openmp_sync_region,
openmp_lock_init,
openmp_lock_destroy,
openmp_mutex_acquire,
openmp_mutex_acquired,
openmp_nest_lock,
openmp_flush,
openmp_cancel
```

And two attached to experimental non-standrd callbacks:

```
openmp_loop
openmp_loop_chunk
```

Detailed information about data attached to each state
and event can be found in the Aftermath types difinitions.

## Callbacks status

The following callbacks are implemented and compile-time
switches were provided to allow selective tracing of the events.
The switches are:
`TRACE_LOOPS`, `TRACE_TASKS`, `TRACE_OTHERS`
and can be enabled by passing `-DNAME=1` to CMake.

Always enabled:

* `ompt_callback_thread_begin`
* `ompt_callback_thread_end`

Enabled for `TRACE_LOOPS`:

* `ompt_callback_loop_begin`
* `ompt_callback_loop_end`
* `ompt_callback_loop_chunk`

(Currently to trace loops also `ALLOW_EXPERIMENTAL` has
to be enabled, and the customized runtime is need)

Enabled for `TRACE_TASKS`:

* `ompt_callback_task_create`
* `ompt_callback_task_schedule`
* `ompt_callback_task_dependence`

Enabled for `TRACE_OTHERS`:

* `ompt_callback_parallel_begin`
* `ompt_callback_parallel_end`
* `ompt_callback_implicit_task`
* `ompt_callback_work`
* `ompt_callback_master`
* `ompt_callback_sync_region`
* `ompt_callback_sync_region_wait`
* `ompt_callback_mutex_released`
* `ompt_callback_dependences`
* `ompt_callback_lock_init`
* `ompt_callback_lock_destory`
* `ompt_callback_mutex_acquire`
* `ompt_callback_mutex_acquired`
* `ompt_callback_nest_lock`
* `ompt_callback_flush`
* `ompt_callback_cancel`

## Additional information

* Tracing is done on per worker basis, in oppose to per
core basis, so states are bound to the thread they happened
at. In the case of one-to-one mapping between cores and
threads per worker traces can be treated as per core traces
from the moment affinity of the thread was set.

* Only tied tasks are supported.

## Dependencies

The tools was build and tested with the following dependencies:

* [aftermath](https://github.com/pepperpots/aftermath) (branch: afterompt-support)
* [llvm-project](https://github.com/llvm/llvm-project) (tag: llvmorg-9.0.0)
* [Clang](https://clang.llvm.org/) compiler

To enable loops tracing with experimental callbacks a custom runtime is needed
and can be downloaded [here](https://github.com/pepperpots/llvm-project-openmp).

## Build

Before building the library the [llvm project](https://github.com/llvm/llvm-project)
and [aftermath](https://www.aftermath-tracing.com/prerelease/) have to be
built. Instructions for that can be found at corresponding websites.

The next step is to export required variables:

```
export CMAKE_PREFIX_PATH="<path-to-aftermath>/install"

export CC="clang"
export CXX="clang++"

export C_INCLUDE_PATH="<path-to-llvm-project>/install/lib/clang/9.0.0/include"
export CXX_INCLUDE_PATH="<path-to-llvm-project>/install/lib/clang/9.0.0/include"
```

Then the library can be build and installed as follows:

```
mkdir build
cd build/
cmake -DCMAKE_BUILD_TYPE=Release [CALLBACKS SWITCHES] ..
make install
```

The library is installed in the project's root inside `install/` directory.

## Usage

Any OpenMP application can be run with tool attached to it generate
Aftermath traces. The following instruction outlines one of the
possible ways to do it:

```
export AFTEROMPT_LIBRARY_PATH="<path-to-afterompt>/install"
export LLVM_LIBRARY_PATH="<path-to-llvm-project>/install/lib"

source <path-to-aftermath>/env.sh

clang -fopenmp -o omp-program omp-program.c

AFTERMATH_TRACE_FILE=trace.ost \
LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:${LLVM_LIBRARY_PATH}:${AFTEROMPT_LIB_PATH} \
LD_PRELOAD=${LLVM_LIBRARY_PATH}/libomp.so:${AFTEROMPT_LIBRARY_PATH}/libafterompt.so \
./omp-program
```

In the given example the tool is dynamically attached to the runtime by using
the LD_PRELOAD variable. The LLVM runtime is also pre-loaded to ensure
that the system runtime is not used.

