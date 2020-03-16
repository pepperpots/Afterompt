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

// TODO: The root cause of that in Aftermath should be found
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Waddress-of-packed-member"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>

#include <aftermath/core/on_disk_write_to_buffer.h>

#include <aftermath/trace/tsc.h>

#include "trace.h"

#include "afterompt.h"

#define DEBUG_INFO 0

/* Time reference */
static struct am_timestamp_reference am_ompt_tsref;

/* Pthread key to access thread tracing data */
static pthread_key_t am_thread_data_key;

ompt_set_callback_t am_set_callback;

ompt_start_tool_result_t* ompt_start_tool(unsigned int omp_version,
                                          const char* runtime_version) {
#if DEBUG_INFO
  printf("%s (omp ver. %d)\n", runtime_version, omp_version);
#endif

  static ompt_start_tool_result_t ompt_start_tool_result = {
      &ompt_initialize, &ompt_finalize, {}};

  return &ompt_start_tool_result;
}

#define REGISTER_CALLBACK(name)                                          \
  if (am_set_callback(ompt_##name, (ompt_callback_t)&am_##name) < 2) {   \
    fprintf(stderr, "Afterompt: Callback %s seems to be unsupported!\n", \
            #name);                                                      \
  }

int ompt_initialize(ompt_function_lookup_t lookup, int num, ompt_data_t* data) {
  am_set_callback = (ompt_set_callback_t)lookup("ompt_set_callback");

  REGISTER_CALLBACK(callback_thread_begin);
  REGISTER_CALLBACK(callback_thread_end);
  REGISTER_CALLBACK(callback_parallel_begin);
  REGISTER_CALLBACK(callback_parallel_end);
  REGISTER_CALLBACK(callback_implicit_task);
  REGISTER_CALLBACK(callback_task_create);
  REGISTER_CALLBACK(callback_task_schedule);
  REGISTER_CALLBACK(callback_sync_region_wait);
  REGISTER_CALLBACK(callback_mutex_released);
  REGISTER_CALLBACK(callback_dependences);
  REGISTER_CALLBACK(callback_task_dependence);
  REGISTER_CALLBACK(callback_work);
  REGISTER_CALLBACK(callback_master);
  REGISTER_CALLBACK(callback_sync_region);
  REGISTER_CALLBACK(callback_lock_init);
  REGISTER_CALLBACK(callback_lock_destroy);
  REGISTER_CALLBACK(callback_mutex_acquire);
  REGISTER_CALLBACK(callback_mutex_acquired);
  REGISTER_CALLBACK(callback_nest_lock);
  REGISTER_CALLBACK(callback_flush);
  REGISTER_CALLBACK(callback_cancel);

  am_timestamp_reference_init(&am_ompt_tsref, am_timestamp_now());

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
  pthread_key_delete(am_thread_data_key);

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
    // TODO: Dying may be too radical
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
    // TODO: Dying may be too radical
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
    // TODO: Dying may be too radical
    exit(1);
  }

  td->state_stack.top--;

  struct am_ompt_stack_item result =
                              {td->state_stack.stack[td->state_stack.top].tsc,
                               td->state_stack.stack[td->state_stack.top].data};

  return result;
}

static inline struct am_ompt_thread_data* am_get_thread_data() {
  struct am_ompt_thread_data* td;

  if (!(td = pthread_getspecific(am_thread_data_key))) {
    fprintf(stderr, "Afterompt: Could not read thread data\n");
    // TODO: Dying may be too radical
    exit(1);
  }

  return td;
}

void am_callback_thread_begin(ompt_thread_t type, ompt_data_t* data) {
  struct am_ompt_thread_data* td;

  if (!(td = am_ompt_create_thread_data(pthread_self()))) {
    fprintf(stderr, "Afterompt: Could not create thread data\n");
    // TODO: Dying may be too radical
    exit(1);
  }

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

  am_dsk_openmp_thread_write_to_buffer_defid(&c->data, &t);

  am_ompt_destroy_thread_data(td);
}

void am_callback_parallel_begin(ompt_data_t* task_data,
                                const ompt_frame_t* task_frame,
                                ompt_data_t* parallel_data,
                                unsigned int requested_parallelism, int flags,
                                const void* codeptr_ra) {
  union am_ompt_stack_item_data parallelism_data;
  parallelism_data.requested_parallelism = requested_parallelism;
  am_ompt_push_state(am_get_thread_data(), am_ompt_now(), parallelism_data);
}

void am_callback_parallel_end(ompt_data_t* parallel_data,
                              ompt_data_t* task_data, int flags,
                              const void* codeptr_ra) {
  struct am_ompt_thread_data* td = am_get_thread_data();
  struct am_buffered_event_collection* c = td->event_collection;

  struct am_ompt_stack_item state = am_ompt_pop_state(td);

  struct am_dsk_interval interval = {state.tsc, am_ompt_now()};

  struct am_dsk_openmp_parallel p = {c->id, interval,
                                     state.data.requested_parallelism, flags};

  am_dsk_openmp_parallel_write_to_buffer_defid(&c->data, &p);
}

void am_callback_task_create(ompt_data_t* task_data,
                             const ompt_frame_t* task_frame,
                             ompt_data_t* new_task_data, int flags,
                             int has_dependences, const void* codeptr_ra) {
  struct am_buffered_event_collection* c =
      am_get_thread_data()->event_collection;

  struct am_dsk_openmp_task_create tc = {c->id, am_ompt_now(), flags,
                                         has_dependences};

  am_dsk_openmp_task_create_write_to_buffer_defid(&c->data, &tc);
}

void am_callback_task_schedule(ompt_data_t* prior_task_data,
                               ompt_task_status_t prior_task_status,
                               ompt_data_t* next_task_data) {
  struct am_buffered_event_collection* c =
      am_get_thread_data()->event_collection;

  struct am_dsk_openmp_task_schedule ts = {c->id, am_ompt_now(),
                                           prior_task_status};

  am_dsk_openmp_task_schedule_write_to_buffer_defid(&c->data, &ts);
}

void am_callback_implicit_task(ompt_scope_endpoint_t endpoint,
                               ompt_data_t* parallel_data,
                               ompt_data_t task_data,
                               unsigned int actual_parallelism,
                               unsigned int index, int flags) {
  struct am_ompt_thread_data* td = am_get_thread_data();
  struct am_buffered_event_collection* c = td->event_collection;

  if (endpoint == ompt_scope_begin) {
    union am_ompt_stack_item_data parallelism_data;
    parallelism_data.actual_parallelism = actual_parallelism;
    am_ompt_push_state(td, am_ompt_now(), parallelism_data);
  }
  else{
    struct am_ompt_stack_item state = am_ompt_pop_state(td);

    struct am_dsk_interval interval = {state.tsc, am_ompt_now()};

    struct am_dsk_openmp_implicit_task it = {c->id, interval,
                               state.data.actual_parallelism, flags};

    am_dsk_openmp_implicit_task_write_to_buffer_defid(&c->data, &it);
  }
}

void am_callback_sync_region_wait(ompt_sync_region_t kind,
                                  ompt_scope_endpoint_t endpoint,
                                  ompt_data_t* parallel_data,
                                  ompt_data_t* task_data,
                                  const void* codeptr_ra) {
  struct am_ompt_thread_data* td = am_get_thread_data();
  struct am_buffered_event_collection* c = td->event_collection;

  if (endpoint == ompt_scope_begin) {
    union am_ompt_stack_item_data empty_data;
    am_ompt_push_state(td, am_ompt_now(), empty_data);
  }
  else{
    struct am_ompt_stack_item state = am_ompt_pop_state(td);

    struct am_dsk_interval interval = {state.tsc, am_ompt_now()};

    struct am_dsk_openmp_sync_region_wait srw = {c->id, interval, kind};

    am_dsk_openmp_sync_region_wait_write_to_buffer_defid(&c->data, &srw);
  }
}

void am_callback_mutex_released(ompt_mutex_t kind, ompt_wait_id_t wait_id,
                                const void* codeptr_ra) {
  struct am_buffered_event_collection* c =
      am_get_thread_data()->event_collection;

  struct am_dsk_openmp_mutex_released mr = {c->id, am_ompt_now(), wait_id,
                                            kind};

  am_dsk_openmp_mutex_released_write_to_buffer_defid(&c->data, &mr);
}

void am_callback_dependences(ompt_data_t* task_data,
                             const ompt_dependence_t* deps, int ndeps) {
  struct am_buffered_event_collection* c =
      am_get_thread_data()->event_collection;

  /* We could collect more information here by traversing the deps
     list to get the storage location of dependences. */
  struct am_dsk_openmp_dependences d = {c->id, am_ompt_now(), ndeps};

  am_dsk_openmp_dependences_write_to_buffer_defid(&c->data, &d);
}

void am_callback_task_dependence(ompt_data_t* src_task_data,
                                 ompt_data_t* sink_task_data) {
  struct am_buffered_event_collection* c =
      am_get_thread_data()->event_collection;

  struct am_dsk_openmp_task_dependence td = {c->id, am_ompt_now()};

  am_dsk_openmp_task_dependence_write_to_buffer_defid(&c->data, &td);
}

void am_callback_work(ompt_work_t wstype, ompt_scope_endpoint_t endpoint,
                      ompt_data_t* parallel_data, ompt_data_t* task_data,
                      uint64_t count, const void* codeptr_ca) {
  struct am_ompt_thread_data* td = am_get_thread_data();
  struct am_buffered_event_collection* c = td->event_collection;

  if (endpoint == ompt_scope_begin) {
    union am_ompt_stack_item_data count_data;
    count_data.count = count;
    am_ompt_push_state(td, am_ompt_now(), count_data);
  }
  else{
    struct am_ompt_stack_item state = am_ompt_pop_state(td);

    struct am_dsk_interval interval = {state.tsc, am_ompt_now()};

    struct am_dsk_openmp_work w = {c->id, interval, wstype, state.data.count};

    am_dsk_openmp_work_write_to_buffer_defid(&c->data, &w);
  }
}

void am_callback_master(ompt_scope_endpoint_t endpoint,
                        ompt_data_t* parallel_data, ompt_data_t* taks_data,
                        const void* codeptr_ra) {
  struct am_ompt_thread_data* td = am_get_thread_data();
  struct am_buffered_event_collection* c = td->event_collection;

  if (endpoint == ompt_scope_begin) {
    union am_ompt_stack_item_data empty_data;
    am_ompt_push_state(td, am_ompt_now(), empty_data);
  }
  else{
    struct am_ompt_stack_item state = am_ompt_pop_state(td);

    struct am_dsk_interval interval = {state.tsc, am_ompt_now()};

    struct am_dsk_openmp_master m = {c->id, interval};

    am_dsk_openmp_master_write_to_buffer_defid(&c->data, &m);
  }
}

void am_callback_sync_region(ompt_sync_region_t kind,
                             ompt_scope_endpoint_t endpoint,
                             ompt_data_t* parallel_data, ompt_data_t* task_data,
                             const void* codeptr_ra) {
  struct am_ompt_thread_data* td = am_get_thread_data();
  struct am_buffered_event_collection* c = td->event_collection;

  if (endpoint == ompt_scope_begin) {
    union am_ompt_stack_item_data empty_data;
    am_ompt_push_state(td, am_ompt_now(), empty_data);
  }
  else{
    struct am_ompt_stack_item state = am_ompt_pop_state(td);

    struct am_dsk_interval interval = {state.tsc, am_ompt_now()};

    struct am_dsk_openmp_sync_region sr = {c->id, interval, kind};

    am_dsk_openmp_sync_region_write_to_buffer_defid(&c->data, &sr);
  }
}

void am_callback_lock_init(ompt_mutex_t kind, ompt_wait_id_t wait_id,
                           const void* codeptr_ra) {
  struct am_buffered_event_collection* c =
      am_get_thread_data()->event_collection;

  struct am_dsk_openmp_lock_init li = {c->id, am_ompt_now(), wait_id, kind};

  am_dsk_openmp_lock_init_write_to_buffer_defid(&c->data, &li);
}

void am_callback_lock_destroy(ompt_mutex_t kind, ompt_wait_id_t wait_id,
                              const void* codeptr_ra) {
  struct am_buffered_event_collection* c =
      am_get_thread_data()->event_collection;

  struct am_dsk_openmp_lock_destroy ld = {c->id, am_ompt_now(), wait_id, kind};

  am_dsk_openmp_lock_destroy_write_to_buffer_defid(&c->data, &ld);
}

void am_callback_mutex_acquire(ompt_mutex_t kind, unsigned int hint,
                               unsigned int impl, ompt_wait_id_t wait_id,
                               const void* codeptr_pa) {
  struct am_buffered_event_collection* c =
      am_get_thread_data()->event_collection;

  struct am_dsk_openmp_mutex_acquire ma = {c->id, am_ompt_now(), wait_id,
                                           kind,  hint,          impl};

  am_dsk_openmp_mutex_acquire_write_to_buffer_defid(&c->data, &ma);
}

void am_callback_mutex_acquired(ompt_mutex_t kind, ompt_wait_id_t wait_id,
                                const void* codeptr_ra) {
  struct am_buffered_event_collection* c =
      am_get_thread_data()->event_collection;

  struct am_dsk_openmp_mutex_acquired ma = {c->id, am_ompt_now(), wait_id,
                                            kind};

  am_dsk_openmp_mutex_acquired_write_to_buffer_defid(&c->data, &ma);
}

void am_callback_nest_lock(ompt_scope_endpoint_t endpoint,
                           ompt_wait_id_t wait_id, const void* codeptr_ra) {
  struct am_ompt_thread_data* td = am_get_thread_data();
  struct am_buffered_event_collection* c = td->event_collection;

  if (endpoint == ompt_scope_begin) {
    union am_ompt_stack_item_data empty_data;
    am_ompt_push_state(td, am_ompt_now(), empty_data);
  }
  else{
    struct am_ompt_stack_item state = am_ompt_pop_state(td);

    struct am_dsk_interval interval = {state.tsc, am_ompt_now()};

    struct am_dsk_openmp_nest_lock nl = {c->id, interval, wait_id};

    am_dsk_openmp_nest_lock_write_to_buffer_defid(&c->data, &nl);
  }
}

void am_callback_flush(ompt_data_t* thread_data, const void* codeptr_ra) {
  struct am_buffered_event_collection* c =
      am_get_thread_data()->event_collection;

  struct am_dsk_openmp_flush f = {c->id, am_ompt_now()};

  am_dsk_openmp_flush_write_to_buffer_defid(&c->data, &f);
}

void am_callback_cancel(ompt_data_t* task_data, int flags,
                        const void* codeptr_ra) {
  struct am_buffered_event_collection* c =
      am_get_thread_data()->event_collection;

  struct am_dsk_openmp_cancel cc = {c->id, am_ompt_now(), flags};

  am_dsk_openmp_cancel_write_to_buffer_defid(&c->data, &cc);
}

#pragma clang pop
