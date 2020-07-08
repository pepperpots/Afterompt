/**
 * Copyright (C) 2018 Andi Drebes <andi@drebesium.org>
 * Copyright (C) 2019 Igor Wodiany <igor.wodiany@manchester.ac.uk>
 *
 * Afterompt is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

// TODO: The root cause of that should be found in Aftermath.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Waddress-of-packed-member"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>

#include <aftermath/core/on_disk_write_to_buffer.h>

#include <aftermath/trace/tsc.h>

#include "trace.h"

#include "afterompt.h"

/* Time reference */
static int tsref_set = 0;
static struct am_timestamp_reference am_ompt_tsref;

/* Pthread key to access thread tracing data */
static pthread_key_t am_thread_data_key;

#ifdef SUPPORT_TRACE_CALLSTACK
/* If 1, then we are within region of interest for function tracing */
static int call_stack_tracing = 0;

/* Before the multi-threaded constructs are built, all entries must be on
 * a single CPU. So just push the entries here, and pop them on each
 * function exit at the end 
 */
#define MAX_NUM_PRE_INIT_FN_ENTRIES 100
static uint64_t initial_thread_fn_entry_times[MAX_NUM_PRE_INIT_FN_ENTRIES];
static int num_pre_init_entries = 0;

#endif

ompt_set_callback_t am_set_callback;

ompt_start_tool_result_t* ompt_start_tool(unsigned int omp_version,
                                          const char* runtime_version) {
  printf("%s (omp ver. %d)\n", runtime_version, omp_version);

  static ompt_start_tool_result_t ompt_start_tool_result = {
      &ompt_initialize, &ompt_finalize, {}};

  return &ompt_start_tool_result;
}

#define REGISTER_CALLBACK(name)                                                \
  switch (am_set_callback(ompt_callback_##name,                                \
                          (ompt_callback_t)&am_callback_##name)) {             \
    case ompt_set_error:                                                       \
      fprintf(stderr, "Afterompt: Failed to set %s callback with an error!\n", \
              #name);                                                          \
      break;                                                                   \
    case ompt_set_never:                                                       \
      fprintf(stderr, "Afterompt: Callback %s will never be invoked!\n",       \
              #name);                                                          \
      break;                                                                   \
    case ompt_set_impossible:                                                  \
      fprintf(                                                                 \
          stderr,                                                              \
          "Afterompt: Callback %s may occur, but tracing is impossible!\n",    \
          #name);                                                              \
      break;                                                                   \
    case ompt_set_sometimes:                                                   \
      fprintf(stderr, "Afterompt: Callback %s is only called sometimes!\n",    \
              #name);                                                          \
      break;                                                                   \
    case ompt_set_sometimes_paired:                                            \
      fprintf(stderr,                                                          \
              "Afterompt: Callback %s is only called sometimes (paired)!\n",   \
              #name);                                                          \
      break;                                                                   \
    case ompt_set_always:                                                      \
      break;                                                                   \
    default:                                                                   \
      fprintf(                                                                 \
          stderr,                                                              \
          "Afterompt: ompt_set_callback for %s returned unexpected value!\n",  \
          #name);                                                              \
      break;                                                                   \
  }

int ompt_initialize(ompt_function_lookup_t lookup, int num, ompt_data_t* data) {
  am_set_callback = (ompt_set_callback_t)lookup("ompt_set_callback");

  REGISTER_CALLBACK(thread_begin);
  REGISTER_CALLBACK(thread_end);

#ifdef TRACE_LOOPS
#ifdef ALLOW_EXPERIMENTAL
  REGISTER_CALLBACK(loop_begin);
  REGISTER_CALLBACK(loop_end);
  REGISTER_CALLBACK(loop_chunk);
#endif
#endif

#ifdef TRACE_TASKS
  REGISTER_CALLBACK(task_create);
  REGISTER_CALLBACK(task_schedule);
  REGISTER_CALLBACK(task_dependence);
#endif

#ifdef TRACE_OTHERS
  REGISTER_CALLBACK(parallel_begin);
  REGISTER_CALLBACK(parallel_end);
  REGISTER_CALLBACK(implicit_task);
  REGISTER_CALLBACK(mutex_released);
  REGISTER_CALLBACK(dependences);
  REGISTER_CALLBACK(work);
  REGISTER_CALLBACK(master);
  REGISTER_CALLBACK(sync_region);
  REGISTER_CALLBACK(lock_init);
  REGISTER_CALLBACK(lock_destroy);
  REGISTER_CALLBACK(mutex_acquire);
  REGISTER_CALLBACK(mutex_acquired);
  REGISTER_CALLBACK(nest_lock);
  REGISTER_CALLBACK(flush);
  REGISTER_CALLBACK(cancel);
#endif

	if(tsref_set == 0){
		am_timestamp_reference_init(&am_ompt_tsref, am_timestamp_now());
		tsref_set = 1;
	}

  am_ompt_init_trace();

  if (pthread_key_create(&am_thread_data_key, NULL)) {
    fprintf(stderr, "Afterompt: Failed to create thread data key.\n");
    /* Zero means failure */
    return 0;
  }

  /* In this context non-zero means success */
  return 1;
}

void ompt_finalize(ompt_data_t* data) {
  if (pthread_key_delete(am_thread_data_key)) {
    fprintf(stderr, "Afterompt: Failed to delete thread data key.\n"
                    "           Continuing....\n");
  }

  am_ompt_exit_trace();
}

/*
  Returns the current timestamp normalized to the reference. If the result
  would be negative, the process is aborted.
*/
static inline am_timestamp_t am_ompt_now(void) {
  am_timestamp_t now;

  if (am_timestamp_reference_now(&am_ompt_tsref, &now)) {
    fprintf(
        stderr,
        "Afterompt: Local timestamp normalized to reference is negative.\n");
    // TODO: Dying may be too radical.
    exit(1);
  }

  return now;
}

/* Push state on the state stack */
static inline void am_ompt_push_state(struct am_ompt_thread_data* td,
                                      am_timestamp_t tsc,
                                      union am_ompt_stack_item_data data) {
  if (td->state_stack.top >= AM_OMPT_DEFAULT_MAX_STATE_STACK_ENTRIES) {
    fprintf(stderr, "Afterompt: Could not push state. \n");
    // TODO: Dying may be too radical.
    exit(1);
  }

  td->state_stack.stack[td->state_stack.top].tsc = tsc;
  td->state_stack.stack[td->state_stack.top].data = data;

  td->state_stack.top++;
}

/* Pop state from the state stack */
static inline struct am_ompt_stack_item am_ompt_pop_state(
    struct am_ompt_thread_data* td) {
  if (td->state_stack.top == 0) {
    fprintf(stderr, "Afterompt: Could not pop state.\n");
    // TODO: Dying may be too radical.
    exit(1);
  }

  td->state_stack.top--;

  struct am_ompt_stack_item result = {
      td->state_stack.stack[td->state_stack.top].tsc,
      td->state_stack.stack[td->state_stack.top].data};

  return result;
}

#ifdef SUPPORT_TRACE_CALLSTACK
/* Push frame on the call stack */
static inline void am_ompt_push_call_stack_frame(struct am_ompt_thread_data* td,
                                      am_timestamp_t tsc,
                                      union am_ompt_stack_item_data data) {
  if (td->call_stack.top >= AM_OMPT_DEFAULT_MAX_CALL_STACK_ENTRIES) {
    fprintf(stderr, "Afterompt: Could not push stack frame. \n");
    // TODO: Dying may be too radical
    exit(1);
  }

  td->call_stack.stack[td->call_stack.top].tsc = tsc;
  td->call_stack.stack[td->call_stack.top].data = data;

  td->call_stack.top++;
}

/* Pop frame from the call stack */
static inline struct am_ompt_stack_item am_ompt_pop_call_stack_frame(
    struct am_ompt_thread_data* td) {

	// We may have entered a function before the am thread data structures were set up
	// meaning we didn't push it to any stack
	// So we expect to fail the pop
  if (td->call_stack.top == 0) {
		union am_ompt_stack_item_data dummy_data;
		dummy_data.addr = 0;
		struct am_ompt_stack_item result = {0, dummy_data};
		return result;
  }

  td->call_stack.top--;

  struct am_ompt_stack_item result = {
      td->call_stack.stack[td->call_stack.top].tsc,
      td->call_stack.stack[td->call_stack.top].data};

  return result;
}
#endif

static inline struct am_ompt_thread_data* am_get_thread_data() {
  struct am_ompt_thread_data* td;

  if (!(td = pthread_getspecific(am_thread_data_key))) {
#ifdef SUPPORT_TRACE_CALLSTACK
		return NULL; // As function entries may occur prior to thread data init
#else
    fprintf(stderr, "Afterompt: Could not read thread data\n");
    // TODO: Dying may be too radical.
    exit(1);
#endif
  }

  return td;
}

#define CHECK_WRITE(func_call)                                           \
  if (func_call) {                                                       \
    fprintf(stderr,                                                      \
            "Afterompt: Failed to write data to disk in %s\n"            \
            "           Consider increasing AFTERMATH_TRACE_BUFFER_SIZE" \
            " and AFTERMATH_EVENT_COLLECTION_BUFFER_SIZE\n",             \
            #func_call);                                                 \
    exit(1);                                                             \
  }

void am_callback_thread_begin(ompt_thread_t type, ompt_data_t* data) {
  struct am_ompt_thread_data* td;

  if (!(td = am_ompt_create_thread_data(pthread_self()))) {
    fprintf(stderr, "Afterompt: Could not create thread data\n");
    // TODO: Dying may be too radical.
    exit(1);
  }

  // TODO: Use initialization list.
  union am_ompt_stack_item_data type_data;
  type_data.thread_type = type;

  am_ompt_push_state(td, am_ompt_now(), type_data);

  pthread_setspecific(am_thread_data_key, td);
}

void am_callback_thread_end(ompt_data_t* data) {
  struct am_ompt_thread_data* td = am_get_thread_data();
  struct am_buffered_event_collection* c = td->event_collection;

  struct am_ompt_stack_item state = am_ompt_pop_state(td);

  struct am_dsk_interval interval = {state.tsc, am_ompt_now()};

  struct am_dsk_openmp_thread t = {c->id, interval, state.data.thread_type};

  CHECK_WRITE(am_dsk_openmp_thread_write_to_buffer_defid(&c->data, &t))

  am_ompt_destroy_thread_data(td);
}

void am_callback_parallel_begin(ompt_data_t* task_data,
                                const ompt_frame_t* task_frame,
                                ompt_data_t* parallel_data,
                                unsigned int requested_parallelism, int flags,
                                const void* codeptr_ra) {
  // TODO: task_frame and codeptr_ra data are not captured by the callback.
  // TODO: Assign id to the parallel region and associated task.
  // TODO: Use initialization list.
  union am_ompt_stack_item_data parallelism_data;
  parallelism_data.requested_parallelism = requested_parallelism;
  am_ompt_push_state(am_get_thread_data(), am_ompt_now(), parallelism_data);
}

void am_callback_parallel_end(ompt_data_t* parallel_data,
                              ompt_data_t* task_data, int flags,
                              const void* codeptr_ra) {
  // TODO: codeptr_ra data is not captured by the callback.
  struct am_ompt_thread_data* td = am_get_thread_data();
  struct am_buffered_event_collection* c = td->event_collection;

  struct am_ompt_stack_item state = am_ompt_pop_state(td);

  struct am_dsk_interval interval = {state.tsc, am_ompt_now()};

  struct am_dsk_openmp_parallel p = {c->id, interval,
                                     state.data.requested_parallelism, flags};

  CHECK_WRITE(am_dsk_openmp_parallel_write_to_buffer_defid(&c->data, &p))
}

void am_callback_task_create(ompt_data_t* task_data,
                             const ompt_frame_t* task_frame,
                             ompt_data_t* new_task_data, int flags,
                             int has_dependences, const void* codeptr_ra) {
  struct am_ompt_thread_data* tdata = am_get_thread_data();

  struct am_buffered_event_collection* c = tdata->event_collection;

  new_task_data->value = (tdata->tid << 32) | (tdata->unique_counter++);

  uint64_t current_task_id = (task_data == NULL) ? 0 : task_data->value;

  struct am_dsk_openmp_task_create tc = {
      c->id, am_ompt_now(),   current_task_id,    new_task_data->value,
      flags, has_dependences, (uint64_t)codeptr_ra};

  CHECK_WRITE(am_dsk_openmp_task_create_write_to_buffer_defid(&c->data, &tc))
}

void am_callback_task_schedule(ompt_data_t* prior_task_data,
                               ompt_task_status_t prior_task_status,
                               ompt_data_t* next_task_data) {
  struct am_buffered_event_collection* c =
      am_get_thread_data()->event_collection;

  struct am_dsk_openmp_task_schedule ts = {
      c->id, am_ompt_now(), prior_task_data->value, next_task_data->value,
      prior_task_status};

  CHECK_WRITE(am_dsk_openmp_task_schedule_write_to_buffer_defid(&c->data, &ts))
}

void am_callback_implicit_task(ompt_scope_endpoint_t endpoint,
                               ompt_data_t* parallel_data,
                               ompt_data_t task_data,
                               unsigned int actual_parallelism,
                               unsigned int index, int flags) {
  // TODO: Assign id the implicit task and associated parallel region.
  //       Actually parallel region may have it id assigned by the
  //       parallel region callback, so check first!
  struct am_ompt_thread_data* td = am_get_thread_data();
  struct am_buffered_event_collection* c = td->event_collection;

  if (endpoint == ompt_scope_begin) {
    // TODO: Use initialization list.
    union am_ompt_stack_item_data parallelism_data;
    parallelism_data.actual_parallelism = actual_parallelism;
    am_ompt_push_state(td, am_ompt_now(), parallelism_data);
  } else {
    struct am_ompt_stack_item state = am_ompt_pop_state(td);

    struct am_dsk_interval interval = {state.tsc, am_ompt_now()};

    struct am_dsk_openmp_implicit_task it = {
        c->id, interval, state.data.actual_parallelism, flags};

    CHECK_WRITE(am_dsk_openmp_implicit_task_write_to_buffer_defid(&c->data, &it))
  }
}

void am_callback_sync_region_wait(ompt_sync_region_t kind,
                                  ompt_scope_endpoint_t endpoint,
                                  ompt_data_t* parallel_data,
                                  ompt_data_t* task_data,
                                  const void* codeptr_ra) {
  // TODO: codeptr_ra data is not captured by the callback.
  // TODO: Task id can be capture to relate wait region with the task.
  // TODO: Capture parallel region id as well.
  struct am_ompt_thread_data* td = am_get_thread_data();
  struct am_buffered_event_collection* c = td->event_collection;

  if (endpoint == ompt_scope_begin) {
    // TODO: Use initialization list.
    union am_ompt_stack_item_data empty_data;
    am_ompt_push_state(td, am_ompt_now(), empty_data);
  } else {
    struct am_ompt_stack_item state = am_ompt_pop_state(td);

    struct am_dsk_interval interval = {state.tsc, am_ompt_now()};

    struct am_dsk_openmp_sync_region_wait srw = {c->id, interval, kind};

    CHECK_WRITE(am_dsk_openmp_sync_region_wait_write_to_buffer_defid(&c->data, &srw))
  }
}

void am_callback_mutex_released(ompt_mutex_t kind, ompt_wait_id_t wait_id,
                                const void* codeptr_ra) {
  // TODO: codeptr_ra data is not captured by the callback.
  struct am_buffered_event_collection* c =
      am_get_thread_data()->event_collection;

  struct am_dsk_openmp_mutex_released mr = {c->id, am_ompt_now(), wait_id,
                                            kind};

  CHECK_WRITE(am_dsk_openmp_mutex_released_write_to_buffer_defid(&c->data, &mr))
}

void am_callback_dependences(ompt_data_t* task_data,
                             const ompt_dependence_t* deps, int ndeps) {
  // TODO: Capture task id as well for this event.
  struct am_buffered_event_collection* c =
      am_get_thread_data()->event_collection;

  // TODO: We could collect more information here by traversing the deps
  //       list to get the storage location of dependences.
  struct am_dsk_openmp_dependences d = {c->id, am_ompt_now(), ndeps};

  CHECK_WRITE(am_dsk_openmp_dependences_write_to_buffer_defid(&c->data, &d))
}

void am_callback_task_dependence(ompt_data_t* src_task_data,
                                 ompt_data_t* sink_task_data) {
  struct am_buffered_event_collection* c =
      am_get_thread_data()->event_collection;

  struct am_dsk_openmp_task_dependence td = {
      c->id, am_ompt_now(), src_task_data->value, sink_task_data->value};

  CHECK_WRITE(am_dsk_openmp_task_dependence_write_to_buffer_defid(&c->data, &td))
}

void am_callback_work(ompt_work_t wstype, ompt_scope_endpoint_t endpoint,
                      ompt_data_t* parallel_data, ompt_data_t* task_data,
                      uint64_t count, const void* codeptr_ra) {
  // TODO: codeptr_ra data is not captured by the callback.
  // TODO: Check when the region is generated for static loops, dynamic
  //       loops and tasks.
  // TODO: Capture task and parallel region id.
  struct am_ompt_thread_data* td = am_get_thread_data();
  struct am_buffered_event_collection* c = td->event_collection;

  if (endpoint == ompt_scope_begin) {
    // TODO: Use initialization list.
    union am_ompt_stack_item_data count_data;
    count_data.count = count;
    am_ompt_push_state(td, am_ompt_now(), count_data);
  } else {
    struct am_ompt_stack_item state = am_ompt_pop_state(td);

    struct am_dsk_interval interval = {state.tsc, am_ompt_now()};

    struct am_dsk_openmp_work w = {c->id, interval, wstype, state.data.count};

    CHECK_WRITE(am_dsk_openmp_work_write_to_buffer_defid(&c->data, &w))
  }
}

void am_callback_master(ompt_scope_endpoint_t endpoint,
                        ompt_data_t* parallel_data, ompt_data_t* taks_data,
                        const void* codeptr_ra) {
  // TODO: codeptr_ra data is not captured by the callback.
  // TODO: Capture id of the task and parallel region associated with master
  //       region.
  struct am_ompt_thread_data* td = am_get_thread_data();
  struct am_buffered_event_collection* c = td->event_collection;

  if (endpoint == ompt_scope_begin) {
    // TODO: Use initialization list.
    union am_ompt_stack_item_data empty_data;
    am_ompt_push_state(td, am_ompt_now(), empty_data);
  } else {
    struct am_ompt_stack_item state = am_ompt_pop_state(td);

    struct am_dsk_interval interval = {state.tsc, am_ompt_now()};

    struct am_dsk_openmp_master m = {c->id, interval};

    CHECK_WRITE(am_dsk_openmp_master_write_to_buffer_defid(&c->data, &m))
  }
}

void am_callback_sync_region(ompt_sync_region_t kind,
                             ompt_scope_endpoint_t endpoint,
                             ompt_data_t* parallel_data, ompt_data_t* task_data,
                             const void* codeptr_ra) {
  // TODO: codeptr_ra data is not captured by the callback.
  // TODO: Task and parallel id can be captured to relate region to the task.
  struct am_ompt_thread_data* td = am_get_thread_data();
  struct am_buffered_event_collection* c = td->event_collection;

  if (endpoint == ompt_scope_begin) {
    // TODO: Use initialization list.
    union am_ompt_stack_item_data empty_data;
    am_ompt_push_state(td, am_ompt_now(), empty_data);
  } else {
    struct am_ompt_stack_item state = am_ompt_pop_state(td);

    struct am_dsk_interval interval = {state.tsc, am_ompt_now()};

    struct am_dsk_openmp_sync_region sr = {c->id, interval, kind};

    CHECK_WRITE(am_dsk_openmp_sync_region_write_to_buffer_defid(&c->data, &sr))
  }
}

void am_callback_lock_init(ompt_mutex_t kind, ompt_wait_id_t wait_id,
                           const void* codeptr_ra) {
  // TODO: codeptr_ra data is not captured by the callback.
  struct am_buffered_event_collection* c =
      am_get_thread_data()->event_collection;

  struct am_dsk_openmp_lock_init li = {c->id, am_ompt_now(), wait_id, kind};

  CHECK_WRITE(am_dsk_openmp_lock_init_write_to_buffer_defid(&c->data, &li))
}

void am_callback_lock_destroy(ompt_mutex_t kind, ompt_wait_id_t wait_id,
                              const void* codeptr_ra) {
  // TODO: codeptr_ra data is not captured by the callback.
  struct am_buffered_event_collection* c =
      am_get_thread_data()->event_collection;

  struct am_dsk_openmp_lock_destroy ld = {c->id, am_ompt_now(), wait_id, kind};

  CHECK_WRITE(am_dsk_openmp_lock_destroy_write_to_buffer_defid(&c->data, &ld))
}

void am_callback_mutex_acquire(ompt_mutex_t kind, unsigned int hint,
                               unsigned int impl, ompt_wait_id_t wait_id,
                               const void* codeptr_pa) {
  // TODO: codeptr_ra data is not captured by the callback.
  struct am_buffered_event_collection* c =
      am_get_thread_data()->event_collection;

  struct am_dsk_openmp_mutex_acquire ma = {c->id, am_ompt_now(), wait_id,
                                           kind,  hint,          impl};

  CHECK_WRITE(am_dsk_openmp_mutex_acquire_write_to_buffer_defid(&c->data, &ma))
}

void am_callback_mutex_acquired(ompt_mutex_t kind, ompt_wait_id_t wait_id,
                                const void* codeptr_ra) {
  // TODO: codeptr_ra data is not captured by the callback.
  struct am_buffered_event_collection* c =
      am_get_thread_data()->event_collection;

  struct am_dsk_openmp_mutex_acquired ma = {c->id, am_ompt_now(), wait_id,
                                            kind};

  CHECK_WRITE(am_dsk_openmp_mutex_acquired_write_to_buffer_defid(&c->data, &ma))
}

void am_callback_nest_lock(ompt_scope_endpoint_t endpoint,
                           ompt_wait_id_t wait_id, const void* codeptr_ra) {
  // TODO: codeptr_ra data is not captured by the callback.
  struct am_ompt_thread_data* td = am_get_thread_data();
  struct am_buffered_event_collection* c = td->event_collection;

  if (endpoint == ompt_scope_begin) {
    // TODO: Use initialization list.
    union am_ompt_stack_item_data empty_data;
    am_ompt_push_state(td, am_ompt_now(), empty_data);
  } else {
    struct am_ompt_stack_item state = am_ompt_pop_state(td);

    struct am_dsk_interval interval = {state.tsc, am_ompt_now()};

    struct am_dsk_openmp_nest_lock nl = {c->id, interval, wait_id};

    CHECK_WRITE(am_dsk_openmp_nest_lock_write_to_buffer_defid(&c->data, &nl))
  }
}

void am_callback_flush(ompt_data_t* thread_data, const void* codeptr_ra) {
  // TODO: codeptr_ra data is not captured by the callback
  struct am_buffered_event_collection* c =
      am_get_thread_data()->event_collection;

  struct am_dsk_openmp_flush f = {c->id, am_ompt_now()};

  CHECK_WRITE(am_dsk_openmp_flush_write_to_buffer_defid(&c->data, &f))
}

void am_callback_cancel(ompt_data_t* task_data, int flags,
                        const void* codeptr_ra) {
  // TODO: codeptr_ra data is not captured by the callback
  // TODO: Task id can be captured to relate cancel event with the task
  struct am_buffered_event_collection* c =
      am_get_thread_data()->event_collection;

  struct am_dsk_openmp_cancel cc = {c->id, am_ompt_now(), flags};

  CHECK_WRITE(am_dsk_openmp_cancel_write_to_buffer_defid(&c->data, &cc))
}

void am_callback_loop_begin(ompt_data_t* parallel_data, ompt_data_t* task_data,
                            int flags, int64_t lower_bound, int64_t upper_bound,
                            int64_t increment, int num_workers,
                            void* codeptr_ra) {
  struct am_ompt_thread_data* tdata = am_get_thread_data();

  task_data->value = (tdata->tid << 32) | (tdata->unique_counter++);

  union am_ompt_stack_item_data loop_info;

  loop_info.loop_info.flags = flags;
  loop_info.loop_info.lower_bound = lower_bound;
  loop_info.loop_info.upper_bound = upper_bound;
  loop_info.loop_info.increment = increment;
  loop_info.loop_info.num_workers = num_workers;
  loop_info.loop_info.codeptr_ra = (uint64_t)codeptr_ra;

  am_ompt_push_state(am_get_thread_data(), am_ompt_now(), loop_info);
}

void am_callback_loop_end(ompt_data_t* parallel_data, ompt_data_t* task_data) {
  struct am_ompt_thread_data* td = am_get_thread_data();
  struct am_buffered_event_collection* c = td->event_collection;

  struct am_ompt_stack_item state = am_ompt_pop_state(td);
  struct am_ompt_loop_info loop_info = state.data.loop_info;

  struct am_dsk_interval interval = {state.tsc, am_ompt_now()};

  struct am_dsk_openmp_loop l = {c->id,
                                 interval,
                                 task_data->value,
                                 loop_info.flags,
                                 loop_info.lower_bound,
                                 loop_info.upper_bound,
                                 loop_info.increment,
                                 loop_info.num_workers,
                                 loop_info.codeptr_ra};

  CHECK_WRITE(am_dsk_openmp_loop_write_to_buffer_defid(&c->data, &l))

  /* We need a marker in the trace to close the last period in the loop. Not
     sure it is the best solution, so probably it needs to be revisited. */
  // TODO: Revisit this later.
  struct am_dsk_openmp_loop_chunk lc = {c->id, am_ompt_now(), task_data->value,
                                        0, 0, 1};

  CHECK_WRITE(am_dsk_openmp_loop_chunk_write_to_buffer_defid(&c->data, &lc))

}

void am_callback_loop_chunk(ompt_data_t* parallel_data, ompt_data_t* task_data,
                            int64_t lower_bound, int64_t upper_bound) {
  struct am_buffered_event_collection* c =
      am_get_thread_data()->event_collection;

  /* Zero indicates that it is not the end of the last period. This should be
     treated as a small hack, since maybe there is a better solution. */
  // TODO: Revisit this later.
  struct am_dsk_openmp_loop_chunk lc = {c->id, am_ompt_now(), task_data->value,
                                        lower_bound, upper_bound, 0};

  CHECK_WRITE(am_dsk_openmp_loop_chunk_write_to_buffer_defid(&c->data, &lc))
}

#ifdef SUPPORT_TRACE_CALLSTACK

/* If start_trace_signal == 1 then ensure tracing is enabled */
void am_function_entry(void* addr, int start_trace_signal){

	call_stack_tracing = (call_stack_tracing | start_trace_signal);

	if(call_stack_tracing){

		// TODO should allow user to provide a file of blacklisted functions
		if(((uint64_t) addr) == 4201200)
			return;

		struct am_ompt_thread_data* td = am_get_thread_data();
		if(td == NULL){
			if(num_pre_init_entries >= MAX_NUM_PRE_INIT_FN_ENTRIES){
				fprintf(stderr, "Maximum number of pre initialisation function entries \
					reached.\n");	
				exit(1);
			}

			if(tsref_set == 0){
				am_timestamp_reference_init(&am_ompt_tsref, am_timestamp_now());
				tsref_set = 1;
			}

			initial_thread_fn_entry_times[num_pre_init_entries] = am_ompt_now();
			num_pre_init_entries++;
			return;
		}

		union am_ompt_stack_item_data func_info;
		func_info.addr = (uint64_t) addr;

		am_ompt_push_call_stack_frame(td, am_ompt_now(), func_info);
	}

}

/* If stop_trace_signal == 1 then trace the exit and disable further tracing */
void am_function_exit(void* addr, int stop_trace_signal){

	if(call_stack_tracing == 0)
		return;

	// TODO should allow user to provide a file of blacklisted functions
	if(((uint64_t) addr) == 4201200)
		return;

  struct am_ompt_thread_data* td = am_get_thread_data();
	if(td == NULL){
		// this probably shouldn't happen - we *left* a function before the
		// afterompt constructs were initialised?
		return;
	}

  struct am_buffered_event_collection* c = td->event_collection;

	uint64_t frame_start = 0;

  struct am_ompt_stack_item frame = am_ompt_pop_call_stack_frame(td);
	if(frame.data.addr == 0){
		// then we are popping a frame that wasn't pushed
		// i.e. the thread structures were not initialised when function entered
		// I must be the initial thread

		// It is not possible for (num_pre_init_entries < 1) here
		frame_start = initial_thread_fn_entry_times[num_pre_init_entries-1];
		num_pre_init_entries--;

		frame.data.addr = (uint64_t) addr;
	} else {
		frame_start = frame.tsc;
	}

  struct am_dsk_interval interval = {frame_start, am_ompt_now()};

  struct am_dsk_stack_frame t = {c->id, frame.data.addr, interval};

  am_dsk_stack_frame_write_to_buffer_defid(&c->data, &t);

	if(stop_trace_signal)
		call_stack_tracing = 0;
}

void __cyg_profile_func_enter(void *func, void *caller){
	am_function_entry(func, 1);
}

void __cyg_profile_func_exit(void *func, void *caller){
	am_function_exit(func, 0);
}
#endif

#pragma clang pop
