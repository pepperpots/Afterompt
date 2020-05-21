# AfterOMPT

AfterOMPT is the OpenMP first-party tool that implements OMPT callbacks
in order to track OpenMP events and to access the runtime state of the
system. We use Aftermath tracing library to collect information about
those events and save them to the trace file that can be later viewed
using [Aftermath](https://www.aftermath-tracing.com/) GUI. Detailed
information about OMPT can be found in the [OpenMP standard](https://www.openmp.org/specifications/)

## Related publications

If you use this software in your research please cite us:

```
Work-in-progress
```

## Download

The code can be simply cloned from our git repository:

```
git clone https://github.com/pepperpots/Afterompt.git
```

## Dependencies

The only mandatory dependency is the Aftermath. The support for the OMPT types
has not been merged into the main repository yet, but the required code can be
accessed from our development fork in [here](https://github.com/pepperpots/aftermath)
(branch: afterompt-support).

To enable loops tracing with experimental callbacks a special custom version of
Aftermath, LLVM OpenMP runtime and Clang are need. All details can be found in
[this](https://github.com/IgWod/ompt-loops-tracing) repository.

## Configuration

Before running any commands please ensure that a required version of Aftermath
has been built. For the building instruction please refer to the
[official website](https://www.aftermath-tracing.com/prerelease/).


The next step is to export required variables:

```
export CMAKE_PREFIX_PATH="<path-to-aftermath>/install"
```

If the `ompt.h` header cannot be found the location should be added to the
include path:

```
export C_INCLUDE_PATH="/home/iwodiany/Projects/llvm-project/openmp/build/instal/usr/local/include"
```

Now the CMake project can be configured using following
commands:

```
mkdir build
cd build/
cmake -DCMAKE_BUILD_TYPE=Release -DTRACE_X=TRUE ..
```

Where `-DTRACE_X=TRUE` enables a specific set of callbacks with allowed
variables being: `TRACE_LOOPS`, `TRACE_TASKS`, `TRACE_OTHERS`. Experimental
callbacks are enabled with `-DALLOW_EXPERIMENTAL=TRUE`. The scope of each
variable is described later in this document.

Example:

```
cmake -DCMAKE_BUILD_TYPE=Release -DTRACE_TASKS=TURE -DTRACE_OTHERS=TRUE ..
```

## Build

Now the tool can be simply built by running the following command:

```
make install
```

The library is installed in the project's root inside `install/` directory.

## Usage

Now the tool can be used to trace any OpenMP application, as long as, OMPT
compatible runtime is available in the system. The following instruction
outlines one of the possible ways to use the tool:

```
export AFTEROMPT_LIBRARY_PATH="<path-to-afterompt>/install"
export LD_LIBRARY_PATH="<path-to-openmp-runtime":${LD_LIBRARY_PATH}

source <path-to-aftermath>/env.sh

clang -o omp-program -fopenmp omp-program.c

AFTERMATH_TRACE_FILE=trace.ost \
LD_PRELOAD=${AFTEROMPT_LIBRARY_PATH}/libafterompt.so \
./omp-program
```

In the given example the tool is dynamically attached to the runtime by using
the `LD_PRELOAD` variable. `LD_LIBRARY_PATH` is not required, however it helps
to ensure the correct version of the runtime is used.

## Available environmental variables

`AFTERMATH_TRACE_BUFFER_SIZE` (optional, default: 2^20) - Size of the trace wide
buffer in bytes.

`AFTERMATH_EVENT_COLLECTION_BUFFER_SIZE` (optional, default: 2^24) - Size of the per
core buffer in bytes.

`AFTERMATH_TRACE_FILE` (mandatory) - Name of the file where the data is written to.

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

And two provided by experimental non-standard callbacks:

```
openmp_loop
openmp_loop_chunk
```

Detailed information about data attached to each state
and event can be found in the Aftermath types definitions.
Some extra information (loop and task instances, iteration
and task periods, etc.) is generated* when the trace is loaded
and processed in the Aftermath GUI.

(*) Requires experimental Aftermath from [here](https://github.com/IgWod/ompt-loops-tracing)

## Implemented callbacks

The following callbacks are implemented and compile-time
switches were provided to allow selective tracing of the events.

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

* Tracing of untied tasks has not been tested

## Supported software

The library was currently tested with following versions of
the software.

Operating System:

* Ubuntu 18.04 LTS (Kernel version 5.3)
* Ubuntu 18.04 LTS (Kernel version 4.15)

Compilers:

* Clang 9.0.0
* Clang 4.0.1
* Intel Compiler 19.1.1

OpenMP runtime:

* LLVM 9.0.0
* Intel Runtime 19.1.1
