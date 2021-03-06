/*
 * Copyright (c) 2017 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <math.h>
#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vppinfra/elog.h>
#include <vnet/session/transport.h>
#include <vnet/session/session.h>
#include <vnet/session/application.h>
#include <vnet/session/session_debug.h>
#include <svm/queue.h>

vlib_node_registration_t session_queue_node;

typedef struct
{
  u32 session_index;
  u32 server_thread_index;
} session_queue_trace_t;

/* packet trace format function */
static u8 *
format_session_queue_trace (u8 * s, va_list * args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  session_queue_trace_t *t = va_arg (*args, session_queue_trace_t *);

  s = format (s, "SESSION_QUEUE: session index %d, server thread index %d",
	      t->session_index, t->server_thread_index);
  return s;
}

vlib_node_registration_t session_queue_node;

#define foreach_session_queue_error		\
_(TX, "Packets transmitted")                  	\
_(TIMER, "Timer events")			\
_(NO_BUFFER, "Out of buffers")

typedef enum
{
#define _(sym,str) SESSION_QUEUE_ERROR_##sym,
  foreach_session_queue_error
#undef _
    SESSION_QUEUE_N_ERROR,
} session_queue_error_t;

static char *session_queue_error_strings[] = {
#define _(sym,string) string,
  foreach_session_queue_error
#undef _
};

static void
session_tx_trace_frame (vlib_main_t * vm, vlib_node_runtime_t * node,
			u32 next_index, u32 * to_next, u16 n_segs,
			stream_session_t * s, u32 n_trace)
{
  session_queue_trace_t *t;
  vlib_buffer_t *b;
  int i;

  for (i = 0; i < clib_min (n_trace, n_segs); i++)
    {
      b = vlib_get_buffer (vm, to_next[i - n_segs]);
      vlib_trace_buffer (vm, node, next_index, b, 1 /* follow_chain */ );
      t = vlib_add_trace (vm, node, b, sizeof (*t));
      t->session_index = s->session_index;
      t->server_thread_index = s->thread_index;
    }
  vlib_set_trace_count (vm, node, n_trace - i);
}

always_inline void
session_tx_fifo_chain_tail (vlib_main_t * vm, session_tx_context_t * ctx,
			    vlib_buffer_t * b, u16 * n_bufs, u8 peek_data)
{
  session_manager_main_t *smm = &session_manager_main;
  vlib_buffer_t *chain_b, *prev_b;
  u32 chain_bi0, to_deq, left_from_seg;
  u16 len_to_deq, n_bytes_read;
  u8 *data, j;

  b->flags |= VLIB_BUFFER_TOTAL_LENGTH_VALID;
  b->total_length_not_including_first_buffer = 0;

  chain_b = b;
  left_from_seg = clib_min (ctx->snd_mss - b->current_length,
			    ctx->left_to_snd);
  to_deq = left_from_seg;
  for (j = 1; j < ctx->n_bufs_per_seg; j++)
    {
      prev_b = chain_b;
      len_to_deq = clib_min (to_deq, ctx->deq_per_buf);

      *n_bufs -= 1;
      chain_bi0 = smm->tx_buffers[ctx->s->thread_index][*n_bufs];
      _vec_len (smm->tx_buffers[ctx->s->thread_index]) = *n_bufs;

      chain_b = vlib_get_buffer (vm, chain_bi0);
      chain_b->current_data = 0;
      data = vlib_buffer_get_current (chain_b);
      if (peek_data)
	{
	  n_bytes_read = svm_fifo_peek (ctx->s->server_tx_fifo,
					ctx->tx_offset, len_to_deq, data);
	  ctx->tx_offset += n_bytes_read;
	}
      else
	{
	  if (ctx->transport_vft->tx_type == TRANSPORT_TX_DGRAM)
	    {
	      svm_fifo_t *f = ctx->s->server_tx_fifo;
	      session_dgram_hdr_t *hdr = &ctx->hdr;
	      u16 deq_now;
	      deq_now = clib_min (hdr->data_length - hdr->data_offset,
				  len_to_deq);
	      n_bytes_read = svm_fifo_peek (f, hdr->data_offset, deq_now,
					    data);
	      ASSERT (n_bytes_read > 0);

	      hdr->data_offset += n_bytes_read;
	      if (hdr->data_offset == hdr->data_length)
		svm_fifo_dequeue_drop (f, hdr->data_length);
	    }
	  else
	    n_bytes_read = svm_fifo_dequeue_nowait (ctx->s->server_tx_fifo,
						    len_to_deq, data);
	}
      ASSERT (n_bytes_read == len_to_deq);
      chain_b->current_length = n_bytes_read;
      b->total_length_not_including_first_buffer += chain_b->current_length;

      /* update previous buffer */
      prev_b->next_buffer = chain_bi0;
      prev_b->flags |= VLIB_BUFFER_NEXT_PRESENT;

      /* update current buffer */
      chain_b->next_buffer = 0;

      to_deq -= n_bytes_read;
      if (to_deq == 0)
	break;
    }
  ASSERT (to_deq == 0
	  && b->total_length_not_including_first_buffer == left_from_seg);
  ctx->left_to_snd -= left_from_seg;
}

always_inline int
session_output_try_get_buffers (vlib_main_t * vm,
				session_manager_main_t * smm,
				u32 thread_index, u16 * n_bufs, u32 wanted)
{
  u32 n_alloc;
  vec_validate_aligned (smm->tx_buffers[thread_index], wanted - 1,
			CLIB_CACHE_LINE_BYTES);
  n_alloc = vlib_buffer_alloc (vm, &smm->tx_buffers[thread_index][*n_bufs],
			       wanted - *n_bufs);
  *n_bufs += n_alloc;
  _vec_len (smm->tx_buffers[thread_index]) = *n_bufs;
  return n_alloc;
}

always_inline void
session_tx_fill_buffer (vlib_main_t * vm, session_tx_context_t * ctx,
			vlib_buffer_t * b, u16 * n_bufs, u8 peek_data)
{
  u32 len_to_deq;
  u8 *data0;
  int n_bytes_read;

  /*
   * Start with the first buffer in chain
   */
  b->error = 0;
  b->flags = VNET_BUFFER_F_LOCALLY_ORIGINATED;
  b->current_data = 0;
  b->total_length_not_including_first_buffer = 0;

  data0 = vlib_buffer_make_headroom (b, MAX_HDRS_LEN);
  len_to_deq = clib_min (ctx->left_to_snd, ctx->deq_per_first_buf);

  if (peek_data)
    {
      n_bytes_read = svm_fifo_peek (ctx->s->server_tx_fifo, ctx->tx_offset,
				    len_to_deq, data0);
      ASSERT (n_bytes_read > 0);
      /* Keep track of progress locally, transport is also supposed to
       * increment it independently when pushing the header */
      ctx->tx_offset += n_bytes_read;
    }
  else
    {
      if (ctx->transport_vft->tx_type == TRANSPORT_TX_DGRAM)
	{
	  session_dgram_hdr_t *hdr = &ctx->hdr;
	  svm_fifo_t *f = ctx->s->server_tx_fifo;
	  u16 deq_now;
	  u32 offset;

	  ASSERT (hdr->data_length > hdr->data_offset);
	  deq_now = clib_min (hdr->data_length - hdr->data_offset,
			      len_to_deq);
	  offset = hdr->data_offset + SESSION_CONN_HDR_LEN;
	  n_bytes_read = svm_fifo_peek (f, offset, deq_now, data0);
	  ASSERT (n_bytes_read > 0);

	  if (ctx->s->session_state == SESSION_STATE_LISTENING)
	    {
	      ip_copy (&ctx->tc->rmt_ip, &hdr->rmt_ip, ctx->tc->is_ip4);
	      ctx->tc->rmt_port = hdr->rmt_port;
	    }
	  hdr->data_offset += n_bytes_read;
	  if (hdr->data_offset == hdr->data_length)
	    {
	      offset = hdr->data_length + SESSION_CONN_HDR_LEN;
	      svm_fifo_dequeue_drop (f, offset);
	    }
	}
      else
	{
	  n_bytes_read = svm_fifo_dequeue_nowait (ctx->s->server_tx_fifo,
						  len_to_deq, data0);
	  ASSERT (n_bytes_read > 0);
	}
    }
  b->current_length = n_bytes_read;
  ctx->left_to_snd -= n_bytes_read;

  /*
   * Fill in the remaining buffers in the chain, if any
   */
  if (PREDICT_FALSE (ctx->n_bufs_per_seg > 1 && ctx->left_to_snd))
    session_tx_fifo_chain_tail (vm, ctx, b, n_bufs, peek_data);

  /* *INDENT-OFF* */
  SESSION_EVT_DBG(SESSION_EVT_DEQ, ctx->s, ({
	ed->data[0] = FIFO_EVENT_APP_TX;
	ed->data[1] = ctx->max_dequeue;
	ed->data[2] = len_to_deq;
	ed->data[3] = ctx->left_to_snd;
  }));
  /* *INDENT-ON* */
}

always_inline u8
session_tx_not_ready (stream_session_t * s, u8 peek_data)
{
  if (peek_data)
    {
      /* Can retransmit for closed sessions but can't send new data if
       * session is not ready or closed */
      if (s->session_state < SESSION_STATE_READY)
	return 1;
      if (s->session_state == SESSION_STATE_CLOSED)
	return 2;
    }
  return 0;
}

always_inline transport_connection_t *
session_tx_get_transport (session_tx_context_t * ctx, u8 peek_data)
{
  if (peek_data)
    {
      return ctx->transport_vft->get_connection (ctx->s->connection_index,
						 ctx->s->thread_index);
    }
  else
    {
      if (ctx->s->session_state == SESSION_STATE_LISTENING)
	return ctx->transport_vft->get_listener (ctx->s->connection_index);
      else
	{
	  return ctx->transport_vft->get_connection (ctx->s->connection_index,
						     ctx->s->thread_index);
	}
    }
}

always_inline void
session_tx_set_dequeue_params (vlib_main_t * vm, session_tx_context_t * ctx,
			       u32 max_segs, u8 peek_data)
{
  u32 n_bytes_per_buf, n_bytes_per_seg;
  ctx->max_dequeue = svm_fifo_max_dequeue (ctx->s->server_tx_fifo);
  if (peek_data)
    {
      /* Offset in rx fifo from where to peek data */
      ctx->tx_offset = ctx->transport_vft->tx_fifo_offset (ctx->tc);
      if (PREDICT_FALSE (ctx->tx_offset >= ctx->max_dequeue))
	{
	  ctx->max_len_to_snd = 0;
	  return;
	}
      ctx->max_dequeue -= ctx->tx_offset;
    }
  else
    {
      if (ctx->transport_vft->tx_type == TRANSPORT_TX_DGRAM)
	{
	  if (ctx->max_dequeue <= sizeof (ctx->hdr))
	    {
	      ctx->max_len_to_snd = 0;
	      return;
	    }
	  svm_fifo_peek (ctx->s->server_tx_fifo, 0, sizeof (ctx->hdr),
			 (u8 *) & ctx->hdr);
	  ASSERT (ctx->hdr.data_length > ctx->hdr.data_offset);
	  ctx->max_dequeue = ctx->hdr.data_length - ctx->hdr.data_offset;
	}
    }
  ASSERT (ctx->max_dequeue > 0);

  /* Ensure we're not writing more than transport window allows */
  if (ctx->max_dequeue < ctx->snd_space)
    {
      /* Constrained by tx queue. Try to send only fully formed segments */
      ctx->max_len_to_snd =
	(ctx->max_dequeue > ctx->snd_mss) ?
	ctx->max_dequeue - ctx->max_dequeue % ctx->snd_mss : ctx->max_dequeue;
      /* TODO Nagle ? */
    }
  else
    {
      /* Expectation is that snd_space0 is already a multiple of snd_mss */
      ctx->max_len_to_snd = ctx->snd_space;
    }

  /* Check if we're tx constrained by the node */
  ctx->n_segs_per_evt = ceil ((f64) ctx->max_len_to_snd / ctx->snd_mss);
  if (ctx->n_segs_per_evt > max_segs)
    {
      ctx->n_segs_per_evt = max_segs;
      ctx->max_len_to_snd = max_segs * ctx->snd_mss;
    }

  n_bytes_per_buf = vlib_buffer_free_list_buffer_size (vm,
						       VLIB_BUFFER_DEFAULT_FREE_LIST_INDEX);
  ASSERT (n_bytes_per_buf > MAX_HDRS_LEN);
  n_bytes_per_seg = MAX_HDRS_LEN + ctx->snd_mss;
  ctx->n_bufs_per_seg = ceil ((f64) n_bytes_per_seg / n_bytes_per_buf);
  ctx->deq_per_buf = clib_min (ctx->snd_mss, n_bytes_per_buf);
  ctx->deq_per_first_buf = clib_min (ctx->snd_mss,
				     n_bytes_per_buf - MAX_HDRS_LEN);
}

always_inline int
session_tx_fifo_read_and_snd_i (vlib_main_t * vm, vlib_node_runtime_t * node,
				session_fifo_event_t * e,
				stream_session_t * s, int *n_tx_packets,
				u8 peek_data)
{
  u32 next_index, next0, next1, *to_next, n_left_to_next;
  u32 n_trace = vlib_get_trace_count (vm, node), n_bufs_needed = 0;
  u32 thread_index = s->thread_index, n_left, pbi;
  session_manager_main_t *smm = &session_manager_main;
  session_tx_context_t *ctx = &smm->ctx[thread_index];
  transport_proto_t tp;
  vlib_buffer_t *pb;
  u16 n_bufs, rv;

  if (PREDICT_FALSE ((rv = session_tx_not_ready (s, peek_data))))
    {
      if (rv < 2)
	vec_add1 (smm->pending_event_vector[thread_index], *e);
      return 0;
    }

  next_index = smm->session_type_to_next[s->session_type];
  next0 = next1 = next_index;

  tp = session_get_transport_proto (s);
  ctx->s = s;
  ctx->transport_vft = transport_protocol_get_vft (tp);
  ctx->tc = session_tx_get_transport (ctx, peek_data);
  ctx->snd_mss = ctx->transport_vft->send_mss (ctx->tc);
  ctx->snd_space = ctx->transport_vft->send_space (ctx->tc);
  if (ctx->snd_space == 0 || ctx->snd_mss == 0)
    {
      vec_add1 (smm->pending_event_vector[thread_index], *e);
      return 0;
    }

  /* Allow enqueuing of a new event */
  svm_fifo_unset_event (s->server_tx_fifo);

  /* Check how much we can pull. */
  session_tx_set_dequeue_params (vm, ctx, VLIB_FRAME_SIZE - *n_tx_packets,
				 peek_data);

  if (PREDICT_FALSE (!ctx->max_len_to_snd))
    return 0;

  n_bufs = vec_len (smm->tx_buffers[thread_index]);
  n_bufs_needed = ctx->n_segs_per_evt * ctx->n_bufs_per_seg;

  /*
   * Make sure we have at least one full frame of buffers ready
   */
  if (n_bufs < n_bufs_needed)
    {
      session_output_try_get_buffers (vm, smm, thread_index, &n_bufs,
				      ctx->n_bufs_per_seg * VLIB_FRAME_SIZE);
      if (PREDICT_FALSE (n_bufs < n_bufs_needed))
	{
	  vec_add1 (smm->pending_event_vector[thread_index], *e);
	  return -1;
	}
    }

  /*
   * Write until we fill up a frame
   */
  vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next);
  if (PREDICT_FALSE (ctx->n_segs_per_evt > n_left_to_next))
    {
      ctx->n_segs_per_evt = n_left_to_next;
      ctx->max_len_to_snd = ctx->snd_mss * n_left_to_next;
    }
  ctx->left_to_snd = ctx->max_len_to_snd;
  n_left = ctx->n_segs_per_evt;

  while (n_left >= 4)
    {
      vlib_buffer_t *b0, *b1;
      u32 bi0, bi1;

      pbi = smm->tx_buffers[thread_index][n_bufs - 3];
      pb = vlib_get_buffer (vm, pbi);
      vlib_prefetch_buffer_header (pb, STORE);
      pbi = smm->tx_buffers[thread_index][n_bufs - 4];
      pb = vlib_get_buffer (vm, pbi);
      vlib_prefetch_buffer_header (pb, STORE);

      to_next[0] = bi0 = smm->tx_buffers[thread_index][--n_bufs];
      to_next[1] = bi1 = smm->tx_buffers[thread_index][--n_bufs];

      b0 = vlib_get_buffer (vm, bi0);
      b1 = vlib_get_buffer (vm, bi1);

      session_tx_fill_buffer (vm, ctx, b0, &n_bufs, peek_data);
      session_tx_fill_buffer (vm, ctx, b1, &n_bufs, peek_data);

      ctx->transport_vft->push_header (ctx->tc, b0);
      ctx->transport_vft->push_header (ctx->tc, b1);

      to_next += 2;
      n_left_to_next -= 2;
      n_left -= 2;

      VLIB_BUFFER_TRACE_TRAJECTORY_INIT (b0);
      VLIB_BUFFER_TRACE_TRAJECTORY_INIT (b1);

      vlib_validate_buffer_enqueue_x2 (vm, node, next_index, to_next,
				       n_left_to_next, bi0, bi1, next0,
				       next1);
    }
  while (n_left)
    {
      vlib_buffer_t *b0;
      u32 bi0;

      ASSERT (n_bufs >= 1);
      to_next[0] = bi0 = smm->tx_buffers[thread_index][--n_bufs];
      b0 = vlib_get_buffer (vm, bi0);
      session_tx_fill_buffer (vm, ctx, b0, &n_bufs, peek_data);

      /* Ask transport to push header after current_length and
       * total_length_not_including_first_buffer are updated */
      ctx->transport_vft->push_header (ctx->tc, b0);

      to_next += 1;
      n_left_to_next -= 1;
      n_left -= 1;

      VLIB_BUFFER_TRACE_TRAJECTORY_INIT (b0);

      vlib_validate_buffer_enqueue_x1 (vm, node, next_index, to_next,
				       n_left_to_next, bi0, next0);
    }

  if (PREDICT_FALSE (n_trace > 0))
    session_tx_trace_frame (vm, node, next_index, to_next,
			    ctx->n_segs_per_evt, s, n_trace);

  _vec_len (smm->tx_buffers[thread_index]) = n_bufs;
  *n_tx_packets += ctx->n_segs_per_evt;
  vlib_put_next_frame (vm, node, next_index, n_left_to_next);

  /* If we couldn't dequeue all bytes mark as partially read */
  ASSERT (ctx->left_to_snd == 0);
  if (ctx->max_len_to_snd < ctx->max_dequeue)
    if (svm_fifo_set_event (s->server_tx_fifo))
      vec_add1 (smm->pending_event_vector[thread_index], *e);

  if (!peek_data && ctx->transport_vft->tx_type == TRANSPORT_TX_DGRAM)
    {
      /* Fix dgram pre header */
      if (ctx->max_len_to_snd < ctx->max_dequeue)
	svm_fifo_overwrite_head (s->server_tx_fifo, (u8 *) & ctx->hdr,
				 sizeof (session_dgram_pre_hdr_t));
      /* More data needs to be read */
      else if (svm_fifo_max_dequeue (s->server_tx_fifo) > 0)
	if (svm_fifo_set_event (s->server_tx_fifo))
	  vec_add1 (smm->pending_event_vector[thread_index], *e);
    }
  return 0;
}

int
session_tx_fifo_peek_and_snd (vlib_main_t * vm, vlib_node_runtime_t * node,
			      session_fifo_event_t * e0,
			      stream_session_t * s0, int *n_tx_pkts)
{
  return session_tx_fifo_read_and_snd_i (vm, node, e0, s0, n_tx_pkts, 1);
}

int
session_tx_fifo_dequeue_and_snd (vlib_main_t * vm, vlib_node_runtime_t * node,
				 session_fifo_event_t * e0,
				 stream_session_t * s0, int *n_tx_pkts)
{
  return session_tx_fifo_read_and_snd_i (vm, node, e0, s0, n_tx_pkts, 0);
}

int
session_tx_fifo_dequeue_internal (vlib_main_t * vm,
				  vlib_node_runtime_t * node,
				  session_fifo_event_t * e0,
				  stream_session_t * s0, int *n_tx_pkts)
{
  application_t *app;
  app = application_get (s0->opaque);
  svm_fifo_unset_event (s0->server_tx_fifo);
  return app->cb_fns.builtin_app_tx_callback (s0);
}

always_inline stream_session_t *
session_event_get_session (session_fifo_event_t * e, u8 thread_index)
{
  return session_get_if_valid (e->fifo->master_session_index, thread_index);
}

void
dump_thread_0_event_queue (void)
{
  session_manager_main_t *smm = vnet_get_session_manager_main ();
  vlib_main_t *vm = &vlib_global_main;
  u32 my_thread_index = vm->thread_index;
  session_fifo_event_t _e, *e = &_e;
  stream_session_t *s0;
  int i, index;
  i8 *headp;

  svm_queue_t *q;
  q = smm->vpp_event_queues[my_thread_index];

  index = q->head;

  for (i = 0; i < q->cursize; i++)
    {
      headp = (i8 *) (&q->data[0] + q->elsize * index);
      clib_memcpy (e, headp, q->elsize);

      switch (e->event_type)
	{
	case FIFO_EVENT_APP_TX:
	  s0 = session_event_get_session (e, my_thread_index);
	  fformat (stdout, "[%04d] TX session %d\n", i, s0->session_index);
	  break;

	case FIFO_EVENT_DISCONNECT:
	  s0 = session_get_from_handle (e->session_handle);
	  fformat (stdout, "[%04d] disconnect session %d\n", i,
		   s0->session_index);
	  break;

	case FIFO_EVENT_BUILTIN_RX:
	  s0 = session_event_get_session (e, my_thread_index);
	  fformat (stdout, "[%04d] builtin_rx %d\n", i, s0->session_index);
	  break;

	case FIFO_EVENT_RPC:
	  fformat (stdout, "[%04d] RPC call %llx with %llx\n",
		   i, (u64) (e->rpc_args.fp), (u64) (e->rpc_args.arg));
	  break;

	default:
	  fformat (stdout, "[%04d] unhandled event type %d\n",
		   i, e->event_type);
	  break;
	}

      index++;

      if (index == q->maxsize)
	index = 0;
    }
}

static u8
session_node_cmp_event (session_fifo_event_t * e, svm_fifo_t * f)
{
  stream_session_t *s;
  switch (e->event_type)
    {
    case FIFO_EVENT_APP_RX:
    case FIFO_EVENT_APP_TX:
    case FIFO_EVENT_BUILTIN_RX:
      if (e->fifo == f)
	return 1;
      break;
    case FIFO_EVENT_DISCONNECT:
      break;
    case FIFO_EVENT_RPC:
      s = session_get_from_handle (e->session_handle);
      if (!s)
	{
	  clib_warning ("session has event but doesn't exist!");
	  break;
	}
      if (s->server_rx_fifo == f || s->server_tx_fifo == f)
	return 1;
      break;
    default:
      break;
    }
  return 0;
}

u8
session_node_lookup_fifo_event (svm_fifo_t * f, session_fifo_event_t * e)
{
  session_manager_main_t *smm = vnet_get_session_manager_main ();
  svm_queue_t *q;
  session_fifo_event_t *pending_event_vector, *evt;
  int i, index, found = 0;
  i8 *headp;
  u8 thread_index;

  ASSERT (e);
  thread_index = f->master_thread_index;
  /*
   * Search evt queue
   */
  q = smm->vpp_event_queues[thread_index];
  index = q->head;
  for (i = 0; i < q->cursize; i++)
    {
      headp = (i8 *) (&q->data[0] + q->elsize * index);
      clib_memcpy (e, headp, q->elsize);
      found = session_node_cmp_event (e, f);
      if (found)
	return 1;
      if (++index == q->maxsize)
	index = 0;
    }
  /*
   * Search pending events vector
   */
  pending_event_vector = smm->pending_event_vector[thread_index];
  vec_foreach (evt, pending_event_vector)
  {
    found = session_node_cmp_event (evt, f);
    if (found)
      {
	clib_memcpy (e, evt, sizeof (*evt));
	break;
      }
  }
  return found;
}

static uword
session_queue_node_fn (vlib_main_t * vm, vlib_node_runtime_t * node,
		       vlib_frame_t * frame)
{
  session_manager_main_t *smm = vnet_get_session_manager_main ();
  session_fifo_event_t *my_pending_event_vector, *e;
  session_fifo_event_t *my_fifo_events;
  u32 n_to_dequeue, n_events;
  svm_queue_t *q;
  application_t *app;
  int n_tx_packets = 0;
  u32 thread_index = vm->thread_index;
  int i, rv;
  f64 now = vlib_time_now (vm);
  void (*fp) (void *);

  SESSION_EVT_DBG (SESSION_EVT_POLL_GAP_TRACK, smm, thread_index);

  /*
   *  Update transport time
   */
  transport_update_time (now, thread_index);

  /*
   * Get vpp queue events
   */
  q = smm->vpp_event_queues[thread_index];
  if (PREDICT_FALSE (q == 0))
    return 0;

  my_fifo_events = smm->free_event_vector[thread_index];

  /* min number of events we can dequeue without blocking */
  n_to_dequeue = q->cursize;
  my_pending_event_vector = smm->pending_event_vector[thread_index];

  if (!n_to_dequeue && !vec_len (my_pending_event_vector)
      && !vec_len (smm->pending_disconnects[thread_index]))
    return 0;

  SESSION_EVT_DBG (SESSION_EVT_DEQ_NODE, 0);

  /*
   * If we didn't manage to process previous events try going
   * over them again without dequeuing new ones.
   */
  /* XXX: Block senders to sessions that can't keep up */
  if (0 && vec_len (my_pending_event_vector) >= 100)
    {
      clib_warning ("too many fifo events unsolved");
      goto skip_dequeue;
    }

  /* See you in the next life, don't be late */
  if (pthread_mutex_trylock (&q->mutex))
    return 0;

  for (i = 0; i < n_to_dequeue; i++)
    {
      vec_add2 (my_fifo_events, e, 1);
      svm_queue_sub_raw (q, (u8 *) e);
    }

  /* The other side of the connection is not polling */
  if (q->cursize < (q->maxsize / 8))
    (void) pthread_cond_broadcast (&q->condvar);
  pthread_mutex_unlock (&q->mutex);

  vec_append (my_fifo_events, my_pending_event_vector);
  vec_append (my_fifo_events, smm->pending_disconnects[thread_index]);

  _vec_len (my_pending_event_vector) = 0;
  smm->pending_event_vector[thread_index] = my_pending_event_vector;
  _vec_len (smm->pending_disconnects[thread_index]) = 0;

skip_dequeue:
  n_events = vec_len (my_fifo_events);
  for (i = 0; i < n_events; i++)
    {
      stream_session_t *s0;	/* $$$ prefetch 1 ahead maybe */
      session_fifo_event_t *e0;

      e0 = &my_fifo_events[i];
      switch (e0->event_type)
	{
	case FIFO_EVENT_APP_TX:
	  if (n_tx_packets == VLIB_FRAME_SIZE)
	    {
	      vec_add1 (smm->pending_event_vector[thread_index], *e0);
	      break;
	    }

	  s0 = session_event_get_session (e0, thread_index);
	  if (PREDICT_FALSE (!s0))
	    {
	      clib_warning ("It's dead, Jim!");
	      continue;
	    }

	  /* Spray packets in per session type frames, since they go to
	   * different nodes */
	  rv = (smm->session_tx_fns[s0->session_type]) (vm, node, e0, s0,
							&n_tx_packets);
	  /* Out of buffers */
	  if (PREDICT_FALSE (rv < 0))
	    {
	      vlib_node_increment_counter (vm, node->node_index,
					   SESSION_QUEUE_ERROR_NO_BUFFER, 1);
	      continue;
	    }
	  break;
	case FIFO_EVENT_DISCONNECT:
	  /* Make sure stream disconnects run after the pending list is
	   * drained */
	  s0 = session_get_from_handle (e0->session_handle);
	  if (!e0->postponed)
	    {
	      e0->postponed = 1;
	      vec_add1 (smm->pending_disconnects[thread_index], *e0);
	      continue;
	    }
	  /* If tx queue is still not empty, wait */
	  if (svm_fifo_max_dequeue (s0->server_tx_fifo))
	    {
	      vec_add1 (smm->pending_disconnects[thread_index], *e0);
	      continue;
	    }

	  stream_session_disconnect_transport (s0);
	  break;
	case FIFO_EVENT_BUILTIN_RX:
	  s0 = session_event_get_session (e0, thread_index);
	  if (PREDICT_FALSE (!s0))
	    continue;
	  svm_fifo_unset_event (s0->server_rx_fifo);
	  app = application_get (s0->app_index);
	  app->cb_fns.builtin_app_rx_callback (s0);
	  break;
	case FIFO_EVENT_RPC:
	  fp = e0->rpc_args.fp;
	  (*fp) (e0->rpc_args.arg);
	  break;

	default:
	  clib_warning ("unhandled event type %d", e0->event_type);
	}
    }

  _vec_len (my_fifo_events) = 0;
  smm->free_event_vector[thread_index] = my_fifo_events;

  vlib_node_increment_counter (vm, session_queue_node.index,
			       SESSION_QUEUE_ERROR_TX, n_tx_packets);

  SESSION_EVT_DBG (SESSION_EVT_DISPATCH_END, smm, thread_index);

  return n_tx_packets;
}

/* *INDENT-OFF* */
VLIB_REGISTER_NODE (session_queue_node) =
{
  .function = session_queue_node_fn,
  .name = "session-queue",
  .format_trace = format_session_queue_trace,
  .type = VLIB_NODE_TYPE_INPUT,
  .n_errors = ARRAY_LEN (session_queue_error_strings),
  .error_strings = session_queue_error_strings,
  .state = VLIB_NODE_STATE_DISABLED,
};
/* *INDENT-ON* */

static clib_error_t *
session_queue_exit (vlib_main_t * vm)
{
  if (vec_len (vlib_mains) < 2)
    return 0;

  /*
   * Shut off (especially) worker-thread session nodes.
   * Otherwise, vpp can crash as the main thread unmaps the
   * API segment.
   */
  vlib_worker_thread_barrier_sync (vm);
  session_node_enable_disable (0 /* is_enable */ );
  vlib_worker_thread_barrier_release (vm);
  return 0;
}

VLIB_MAIN_LOOP_EXIT_FUNCTION (session_queue_exit);

static uword
session_queue_process (vlib_main_t * vm, vlib_node_runtime_t * rt,
		       vlib_frame_t * f)
{
  f64 now, timeout = 1.0;
  uword *event_data = 0;
  uword event_type;

  while (1)
    {
      vlib_process_wait_for_event_or_clock (vm, timeout);
      now = vlib_time_now (vm);
      event_type = vlib_process_get_events (vm, (uword **) & event_data);

      switch (event_type)
	{
	case SESSION_Q_PROCESS_FLUSH_FRAMES:
	  /* Flush the frames by updating all transports times */
	  transport_update_time (now, 0);
	  break;
	case SESSION_Q_PROCESS_STOP:
	  timeout = 100000.0;
	  break;
	case ~0:
	  /* Timed out. Update time for all transports to trigger all
	   * outstanding retransmits. */
	  transport_update_time (now, 0);
	  break;
	}
      vec_reset_length (event_data);
    }
  return 0;
}

/* *INDENT-OFF* */
VLIB_REGISTER_NODE (session_queue_process_node) =
{
  .function = session_queue_process,
  .type = VLIB_NODE_TYPE_PROCESS,
  .name = "session-queue-process",
  .state = VLIB_NODE_STATE_DISABLED,
};
/* *INDENT-ON* */


/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
