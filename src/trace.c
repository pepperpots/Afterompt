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
#include <stdlib.h>
#include <string.h>

#include <aftermath/trace/on_disk_structs.h>
#include <aftermath/trace/on_disk_write_to_buffer.h>

#include "trace.h"

/* Application trace */
struct am_buffered_trace am_ompt_trace;

/* Name of the trace file */
static const char* am_ompt_trace_file;

/* Protected by trace_lock */
static am_hierarchy_node_id_t curr_hierarchy_node_id = 2;

/* Size of event collection write buffers */
static size_t am_ompt_cbuf_size;

/* Lock for trace-wide operations */
static pthread_spinlock_t am_ompt_trace_lock;

/* Create a new event collection and attach it to the trace */
static struct am_buffered_event_collection* am_ompt_create_event_collection(
    pthread_t tid) {
  struct am_buffered_event_collection* c;
  am_event_collection_id_t id;
  struct am_dsk_event_collection dsk_ec;
  struct am_dsk_hierarchy_node dsk_hn;
  struct am_simple_hierarchy_node* hn;
  char name_buf[64];

  id = tid;

  /* Prepare on-disk structures before taking the lock */
  snprintf(name_buf, sizeof(name_buf), "%u", id);
  dsk_ec.id = id;
  dsk_ec.name.str = name_buf;
  dsk_ec.name.len = strlen(name_buf);

  dsk_hn.parent_id = 1;
  dsk_hn.name.str = name_buf;
  dsk_hn.name.len = strlen(name_buf);

  if (!(c = malloc(sizeof(*c)))) {
    fprintf(stderr,
            "Afterompt: Could not allocate event "
            "collection.\n");
    goto out_err;
  }

  if (!(hn = malloc(sizeof(*hn)))) goto out_err_free_c;

  if (!(hn->name = strdup(name_buf))) goto out_err_free_hn;

  hn->first_child = NULL;

  if (am_buffered_event_collection_init(c, id, am_ompt_cbuf_size)) {
    fprintf(stderr,
            "Afterompt: Could not initialize event "
            "collection.\n");
    goto out_err_free_hn_name;
  }

  /* Use pthread lock rather than critical section to avoid
     triggering callbacks before thread data is initialized */
  if (pthread_spin_lock(&am_ompt_trace_lock)) {
    fprintf(stderr, "Afterompt: Could not acquire lock. \n");
    goto out_err_destroy;
  }

  if (am_buffered_trace_add_collection(&am_ompt_trace, c)) {
    fprintf(stderr,
            "Afterompt: Could not add event collection "
            "to trace.\n");
    goto out_err_unlock;
  }

  if (am_dsk_event_collection_write_to_buffer_defid(&am_ompt_trace.data,
                                                    &dsk_ec)) {
    fprintf(stderr,
            "Afterompt: Could not trace event collection "
            "frame.\n");
    goto out_err_unlock;
  }

  hn->id = curr_hierarchy_node_id++;
  dsk_hn.hierarchy_id = am_ompt_trace.hierarchies[0]->id;
  dsk_hn.id = hn->id;

  am_simple_hierarchy_node_add_child(am_ompt_trace.hierarchies[0]->root, hn);

  if (am_dsk_hierarchy_node_write_to_buffer_defid(&am_ompt_trace.data,
                                                  &dsk_hn)) {
    am_simple_hierarchy_node_remove_first_child(
        am_ompt_trace.hierarchies[0]->root);

    fprintf(stderr,
            "Afterompt: Could not trace hierrachy node "
            "frame.\n");

    goto out_err_unlock;
  }

  if (pthread_spin_unlock(&am_ompt_trace_lock)) {
    fprintf(stderr, "Afterompt: Could not release the lock. \n");
    goto out_err_destroy;
  }

  return c;

out_err_unlock:
  if (pthread_spin_unlock(&am_ompt_trace_lock)) {
    fprintf(stderr, "Afterompt: Could not release the lock. \n");
  }
out_err_destroy:
  am_buffered_event_collection_destroy(c);
out_err_free_hn_name:
  free(hn->name);
out_err_free_hn:
  free(hn);
out_err_free_c:
  free(c);
out_err:
  return NULL;
}

struct am_ompt_thread_data* am_ompt_create_thread_data(pthread_t tid) {
  struct am_ompt_thread_data* data;

  if ((data = malloc(sizeof(*data))) == NULL) {
    fprintf(stderr, "Afterompt: Could not allocate memory for thread data\n");
    goto out_err;
  }

  if ((data->state_stack.stack = malloc(sizeof(struct am_ompt_stack_item) *
                                  AM_OMPT_DEFAULT_MAX_STATE_STACK_ENTRIES)) ==
      NULL) {
    fprintf(stderr, "Afterompt: Could not allocate memory for state stack\n");
    goto out_err_free;
  }

  data->state_stack.top = 0;

  if ((data->event_collection = am_ompt_create_event_collection(tid)) == NULL) {
    fprintf(stderr,
            "Afterompt: Could not create event collection for thread\n");
    goto out_err_destroy;
  }

  data->tid = tid;
  data->unique_counter = 0;

  return data;

out_err_destroy:
  free(data->state_stack.stack);
out_err_free:
  free(data);
out_err:
  return NULL;
}

void am_ompt_destroy_thread_data(struct am_ompt_thread_data* thread_data) {
  free(thread_data->state_stack.stack);
  free(thread_data);

  // TODO: Event collection is not free, so it can be dumped on exit.
}

/* Write default ID of each state to the trace */
static int am_ompt_register_types() {
  if (am_dsk_hierarchy_description_write_default_id_to_buffer(
          &am_ompt_trace.data) ||
      am_dsk_hierarchy_node_write_default_id_to_buffer(&am_ompt_trace.data) ||
      am_dsk_event_collection_write_default_id_to_buffer(&am_ompt_trace.data) ||
      am_dsk_event_mapping_write_default_id_to_buffer(&am_ompt_trace.data) ||
      am_dsk_openmp_thread_write_default_id_to_buffer(&am_ompt_trace.data) ||
      am_dsk_openmp_parallel_write_default_id_to_buffer(&am_ompt_trace.data) ||
      am_dsk_openmp_implicit_task_write_default_id_to_buffer(
          &am_ompt_trace.data) ||
      am_dsk_openmp_task_create_write_default_id_to_buffer(
          &am_ompt_trace.data) ||
      am_dsk_openmp_task_schedule_write_default_id_to_buffer(
          &am_ompt_trace.data) ||
      am_dsk_openmp_sync_region_wait_write_default_id_to_buffer(
          &am_ompt_trace.data) ||
      am_dsk_openmp_mutex_released_write_default_id_to_buffer(
          &am_ompt_trace.data) ||
      am_dsk_openmp_dependences_write_default_id_to_buffer(
          &am_ompt_trace.data) ||
      am_dsk_openmp_task_dependence_write_default_id_to_buffer(
          &am_ompt_trace.data) ||
      am_dsk_openmp_work_write_default_id_to_buffer(&am_ompt_trace.data) ||
      am_dsk_openmp_master_write_default_id_to_buffer(&am_ompt_trace.data) ||
      am_dsk_openmp_sync_region_write_default_id_to_buffer(
          &am_ompt_trace.data) ||
      am_dsk_openmp_lock_init_write_default_id_to_buffer(&am_ompt_trace.data) ||
      am_dsk_openmp_lock_destroy_write_default_id_to_buffer(
          &am_ompt_trace.data) ||
      am_dsk_openmp_mutex_acquire_write_default_id_to_buffer(
          &am_ompt_trace.data) ||
      am_dsk_openmp_mutex_acquired_write_default_id_to_buffer(
          &am_ompt_trace.data) ||
      am_dsk_openmp_nest_lock_write_default_id_to_buffer(&am_ompt_trace.data) ||
      am_dsk_openmp_flush_write_default_id_to_buffer(&am_ompt_trace.data) ||
      am_dsk_openmp_cancel_write_default_id_to_buffer(&am_ompt_trace.data) ||
      am_dsk_openmp_loop_write_default_id_to_buffer(&am_ompt_trace.data) ||
      am_dsk_openmp_loop_chunk_write_default_id_to_buffer(&am_ompt_trace.data)) {
    return 1;
  }

  return 0;
}

int am_ompt_init_trace() {
  /* Size of trace-wide buffer */
  size_t tbuf_size;
  const char* size;

  /* Get size for trace-wide buffer */
  if ((size = getenv("AFTERMATH_TRACE_BUFFER_SIZE")))
    sscanf(size, "%zu", &tbuf_size);
  else
    tbuf_size = AM_OMPT_DEFAULT_TRACE_BUFFER_SIZE;

  /* Get size for per event-collection buffers */
  if ((size = getenv("AFTERMATH_EVENT_COLLECTION_BUFFER_SIZE"))) {
    sscanf(size, "%zu", &am_ompt_cbuf_size);
  } else {
    am_ompt_cbuf_size = AM_OMPT_DEFAULT_EVENT_COLLECTION_BUFFER_SIZE;
  }

  /* Filename of the trace file */
  if (!(am_ompt_trace_file = getenv("AFTERMATH_TRACE_FILE"))) {
    fprintf(stderr, "Afterompt: No trace file specified.\n");
    goto out_err;
  }

  if (am_buffered_trace_init(&am_ompt_trace, tbuf_size)) {
    fprintf(stderr, "Afterompt: Could not initialize trace.\n");
    goto out_err;
  }

  if (!am_buffered_trace_new_hierarchy(&am_ompt_trace, "Workers", "\"\" {}")) {
    fprintf(stderr,
            "Afterompt: Could not create main "
            "hierarchy.\n");
    goto out_err_trace;
  }

  if (am_ompt_register_types()) goto out_err_trace;

  if (am_simple_hierarchy_write_to_buffer_defid(&am_ompt_trace.data,
                                                am_ompt_trace.hierarchies[0])) {
    fprintf(stderr,
            "Afterompt: Could not write hierarchy "
            "description and root node.\n");
    goto out_err_trace;
  }

  if (pthread_spin_init(&am_ompt_trace_lock, 0)) {
    fprintf(stderr, "Afterompt: Could not create spin lock.\n");
    goto out_err_trace;
  }

  return 0;

out_err_trace:
  am_buffered_trace_destroy(&am_ompt_trace);
out_err:
  return 1;
}

/* Writes an entry for each event collection to the trace buffer */
static int am_ompt_trace_mappings() {
  struct am_dsk_event_mapping dsk_em;

  for (size_t i = 0; i < am_ompt_trace.num_collections; i++) {
    dsk_em.collection_id = am_ompt_trace.collections[i]->id;
    dsk_em.hierarchy_id = 0;
    dsk_em.node_id = i + 2;
    dsk_em.interval.start = 0;
    dsk_em.interval.end = AM_TIMESTAMP_T_MAX;

    if (am_dsk_event_mapping_write_to_buffer_defid(&am_ompt_trace.data,
                                                   &dsk_em)) {
      fprintf(stderr,
              "Afterompt: Could not write event "
              "mapping for event collection %u"
              " .\n",
              am_ompt_trace.collections[i]->id);
      return 1;
    }
  }

  return 0;
}

void am_ompt_exit_trace() {
  if (am_ompt_trace_mappings()) {
    fprintf(stderr, "Afterompt: Could not trace event mappings.\n");
  }

  if (am_buffered_trace_dump(&am_ompt_trace, am_ompt_trace_file)) {
    fprintf(stderr,
            "Afterompt: Could not write trace file "
            "\"%s\".\n",
            am_ompt_trace_file);
  }

  am_buffered_trace_destroy(&am_ompt_trace);
  pthread_spin_destroy(&am_ompt_trace_lock);
}

#pragma clang diagnostic pop
