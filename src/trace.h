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

#ifndef AM_OMPT_TRACE_H
#define AM_OMPT_TRACE_H

#include <aftermath/trace/buffered_event_collection.h>
#include <aftermath/trace/buffered_trace.h>
#include <aftermath/trace/timestamp.h>

#define AM_OMPT_DEFAULT_TRACE_BUFFER_SIZE (2 << 20)
#define AM_OMPT_DEFAULT_EVENT_COLLECTION_BUFFER_SIZE (2 << 24)
#define AM_OMPT_DEFAULT_MAX_STATE_STACK_ENTRIES 64

/* Application trace */
extern struct am_buffered_trace am_ompt_trace;

/* Stack for tracing states containing intervals */
struct am_ompt_stack {
  am_timestamp_t tsc;
  uint64_t data;
};

/* Struct containing tracing information for a single thread */
struct am_ompt_thread_data {
  struct am_buffered_event_collection* event_collection;
  struct am_ompt_stack* state_stack;
  uint32_t state_stack_top;
};

/*
  Initialize new trace. The function has to be called before any
  other tracing related function is called.
*/
int am_ompt_init_trace();

/*
  Initialize an event collection and state stack for a specific
  thread and add add them to the trace.
*/
struct am_ompt_thread_data* am_ompt_create_thread_data(pthread_t tid);

/*
  Free the core data and destroy the state stack.
*/
void am_ompt_destroy_thread_data(struct am_ompt_thread_data* thread_data);

/*
  Save trace to the file and clean up all structures.
*/
void am_ompt_exit_trace();

#endif
