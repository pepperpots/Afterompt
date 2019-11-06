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

Currently the following information is traced and saved on the
disk:

```
am_dsk_openmp_thread,
am_dsk_openmp_parallel,
am_dsk_openmp_task_create,
am_dsk_openmp_task_schedule,
am_dsk_openmp_implicit_task,
am_dsk_openmp_sync_region_wait,
am_dsk_openmp_mutex_released,
am_dsk_openmp_dependences,
am_dsk_openmp_task_dependence,
am_dsk_openmp_work,
am_dsk_openmp_master,
am_dsk_openmp_sync_region,
am_dsk_openmp_lock_init,
am_dsk_openmp_lock_destroy,
am_dsk_openmp_mutex_acquire,
am_dsk_openmp_mutex_acquired,
am_dsk_openmp_nest_lock,
am_dsk_openmp_flush,
am_dsk_openmp_cancel
```

Data above has corresponding in-memory representation in Aftermath that
can be used to visualize and analyse the data. Please refer to Aftermath
on disk data types definitions for detailed description of each state.

## Callbacks status

To produced described tracing information the following
callbacks were implemented:

* `ompt_callback_thread_begin`
* `ompt_callback_thread_end`
* `ompt_callback_parallel_begin`
* `ompt_callback_parallel_end`
* `ompt_callback_implicit_task`
* `ompt_callback_work`
* `ompt_callback_master`
* `ompt_callback_sync_region`
* `ompt_callback_task_create`
* `ompt_callback_task_shcedule`
* `ompt_callback_sync_region_wait`
* `ompt_callback_mutex_released`
* `ompt_callback_dependences`
* `ompt_callback_task_dependence`
* `ompt_callback_lock_init`
* `ompt_callback_lock_destory`
* `ompt_callback_mutex_acquire`
* `ompt_callback_mutex_acquired`
* `ompt_callback_nest_lock`
* `ompt_callback_flush`
* `ompt_callback_cancel`

The following callbacks are not implemented in LLVM
(9.0.0, tag: llvmorg-9.0.0), so are subject of the
future work:

* `ompt_callback_target`
* `ompt_callback_target_data_op`
* `ompt_callback_target_submit`
* `ompt_callback_control_tool`
* `ompt_callback_device_initialize`
* `ompt_callback_device_finalize`
* `ompt_callback_device_load`
* `ompt_callback_device_unload`
* `ompt_callback_target_map`
* `ompt_callback_reduction`
* `ompt_callback_dispatch`

## Additional information

* Tracing is done on per worker basis, in oppose to per
core basis, so states are bound to the thread they happened
at. In the case of one-to-one mapping between cores and
threads per worker traces can be treated as per core traces
from the moment affinity of the thread was set.

* It is assumed that states finish in the reverse order
to their starting order. For example if B starts after A
then B will finish before A. We also assume the state does
NOT move between threads. It should be case for any
standard constructs in OpenMP. This assumption allows
handling of nested constructs with a single unified
stack on every thread.

## Dependencies

The tools was build and tested with following dependencies:

* ubuntu 18.04 LTS
* [aftermath](https://github.com/pepperpots/aftermath) (branch: afterompt-support)
* [llvm-project](https://github.com/llvm/llvm-project) (tag: llvmorg-9.0.0)
* clang (6.0.0-1ubuntu2)

However it should work with any major compiler and runtime,
and operating system that support OMPT. Please note that
default system package may be built with OMPT disabled.

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
cmake -DCMAKE_BUILD_TYPE=Release ..
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

