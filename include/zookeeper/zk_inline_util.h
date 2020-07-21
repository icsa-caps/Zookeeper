#ifndef INLINE_UTILS_H
#define INLINE_UTILS_H


#include "zk_kvs_util.h"
#include "zk_debug_util.h"
#include "zk_reservation_stations_util.h"

/* ---------------------------------------------------------------------------
//------------------------------TRACE --------------------------------
//---------------------------------------------------------------------------*/


// Both Leader and Followers use this to read the trace, propagate reqs to the cache and maintain their prepare/write fifos
static inline void zk_batch_from_trace_to_KVS(context_t *ctx)
{
  zk_ctx_t *zk_ctx = (zk_ctx_t *) ctx->appl_ctx;
  zk_trace_op_t *ops = zk_ctx->ops;
  zk_resp_t *resp = zk_ctx->resp;
  trace_t *trace = zk_ctx->trace;

  if (zk_ctx->protocol == FOLLOWER && MAKE_FOLLOWERS_PASSIVE) return ;
  uint16_t op_i = 0;
  int working_session = -1;
  // if there are clients the "all_sessions_stalled" flag is not used,
  // so we need not bother checking it
  if (!ENABLE_CLIENTS && zk_ctx->all_sessions_stalled) {
    return;
  }
  for (uint16_t i = 0; i < SESSIONS_PER_THREAD; i++) {
    uint16_t sess_i = (uint16_t)((zk_ctx->last_session + i) % SESSIONS_PER_THREAD);
    if (pull_request_from_this_session(zk_ctx->stalled[sess_i], sess_i, ctx->t_id)) {
      working_session = sess_i;
      break;
    }
  }
  if (ENABLE_CLIENTS) {
    if (working_session == -1) return;
  }
  else if (ENABLE_ASSERTIONS) assert(working_session != -1);

  bool passed_over_all_sessions = false;

  /// main loop
  while (op_i < ZK_TRACE_BATCH && !passed_over_all_sessions) {

    zk_fill_trace_op(ctx, &trace[zk_ctx->trace_iter], &ops[op_i], working_session);
    while (!pull_request_from_this_session(zk_ctx->stalled[working_session],
                                           (uint16_t) working_session, ctx->t_id)) {

      MOD_INCR(working_session, SESSIONS_PER_THREAD);
      if (working_session == zk_ctx->last_session) {
        passed_over_all_sessions = true;
        // If clients are used the condition does not guarantee that sessions are stalled
        if (!ENABLE_CLIENTS) zk_ctx->all_sessions_stalled = true;
        break;
      }
    }
    resp[op_i].type = EMPTY;
    if (!ENABLE_CLIENTS) {
      zk_ctx->trace_iter++;
      if (trace[zk_ctx->trace_iter].opcode == NOP) zk_ctx->trace_iter = 0;
    }
    op_i++;
  }
  //printf("Session %u pulled: ops %u, req_array ptr %u \n",
  //       working_session, op_i, ops[0].index_to_req_array);
  zk_ctx->last_session = (uint16_t) working_session;
  t_stats[ctx->t_id].cache_hits_per_thread += op_i;
  zk_KVS_batch_op_trace(zk_ctx, op_i, ops, resp, ctx->t_id);

  for (uint16_t i = 0; i < op_i; i++) {
    // my_printf(green, "After: OP_i %u -> session %u \n", i, *(uint32_t *) &ops[i]);
    if (resp[i].type == KVS_MISS)  {
      my_printf(green, "KVS %u: bkt %u, server %u, tag %u \n", i,
                ops[i].key.bkt, ops[i].key.server, ops[i].key.tag);
      assert(false);
      continue;
    }
    // check_version_after_batching_trace_to_cache(&ops[i], &resp[i], t_id);
    // Local reads
    else if (resp[i].type == KVS_GET_SUCCESS) {
      if (ENABLE_ASSERTIONS) {
        assert(machine_id != LEADER_MACHINE);
        assert(USE_REMOTE_READS);
      }
      insert_mes(ctx, R_QP_ID, R_SIZE, (uint32_t) R_REP_BIG_SIZE, false, NULL, NOT_USED);
      //(ctx, zk_ctx, &ops[op_i]);
    }
    else if (resp[i].type == KVS_LOCAL_GET_SUCCESS) {
        //check_state_with_allowed_flags(2, interface[t_id].req_array[ops[i].session_id][ops[i].index_to_req_array].state, IN_PROGRESS_REQ);
        //assert(interface[t_id].req_array[ops[i].session_id][ops[i].index_to_req_array].state == IN_PROGRESS_REQ);
        signal_completion_to_client(ops[i].session_id, ops[i].index_to_req_array, ctx->t_id);
    }
    else { // WRITE
      if (zk_ctx->protocol == FOLLOWER)
          //flr_insert_write(ctx, zk_ctx, &ops[i]);
        insert_mes(ctx, COMMIT_W_QP_ID, (uint32_t) W_SIZE, 1, false, &ops[i], NOT_USED);
      else
        insert_mes(ctx, PREP_ACK_QP_ID, (uint32_t) PREP_SIZE, 1, false, &ops[i], LOCAL_PREP);
        //ldr_insert_prep(ctx, zk_ctx, (void *) &ops[i], true);
    }
  }

}


/* ---------------------------------------------------------------------------
//------------------------------  -----------------------------
//---------------------------------------------------------------------------*/




// Send a batched ack that denotes the first local write id and the number of subsequent lid that are being acked
static inline void send_acks_to_ldr(context_t *ctx)
{
  zk_ctx_t *zk_ctx = (zk_ctx_t *) ctx->appl_ctx;
  p_acks_t *p_acks = zk_ctx->p_acks;
  zk_ack_mes_t *ack = zk_ctx->ack;
  if (p_acks->acks_to_send == 0) return;
  struct ibv_send_wr *bad_send_wr;
  per_qp_meta_t *qp_meta = &ctx->qp_meta[PREP_ACK_QP_ID];
  ack->opcode = KVS_OP_ACK;
  ack->follower_id = ctx->m_id;
  ack->ack_num = (uint16_t) p_acks->acks_to_send;
  uint64_t l_id_to_send = zk_ctx->local_w_id + p_acks->slots_ahead;
  for (uint32_t i = 0; i < ack->ack_num; i++) {
    uint16_t w_ptr = (uint16_t) ((zk_ctx->w_pull_ptr + p_acks->slots_ahead + i) % FLR_PENDING_WRITES);
    if (ENABLE_ASSERTIONS) assert(zk_ctx->w_state[w_ptr] == VALID);
    zk_ctx->w_state[w_ptr] = SENT;
  }
  ack->l_id = l_id_to_send;
  p_acks->slots_ahead += p_acks->acks_to_send;
  p_acks->acks_to_send = 0;
  qp_meta->send_sgl->addr = (uint64_t) (uintptr_t) ack;
  if (ENABLE_ASSERTIONS) {
    assert(ack->follower_id == ctx->m_id);
    assert(qp_meta->send_wr->sg_list == qp_meta->send_sgl);
    assert(qp_meta->send_sgl->length == FLR_ACK_SEND_SIZE);
  }
  check_stats_prints_when_sending_acks(ack, zk_ctx, p_acks, l_id_to_send, ctx->t_id);
  selective_signaling_for_unicast(&qp_meta->sent_tx, ACK_SEND_SS_BATCH, qp_meta->send_wr,
                                  0, qp_meta->send_cq, true,
                                  "sending acks", ctx->t_id);
  // RECEIVES for prepares
  uint32_t posted_recvs = qp_meta->recv_info->posted_recvs;
  uint32_t recvs_to_post_num = FLR_MAX_RECV_PREP_WRS - posted_recvs;
  if (recvs_to_post_num > 0) {
    post_recvs_with_recv_info(qp_meta->recv_info, recvs_to_post_num);
    checks_and_prints_posting_recvs_for_preps(qp_meta->recv_info, recvs_to_post_num, ctx->t_id);
  }
  // SEND the ack
  int ret = ibv_post_send(qp_meta->send_qp, &qp_meta->send_wr[0], &bad_send_wr);
  CPE(ret, "ACK ibv_post_send error", ret);
}


//Send credits for the commits
static inline void send_credits_for_commits(context_t *ctx,
                                            uint16_t credit_num)
{
  per_qp_meta_t *qp_meta = &ctx->qp_meta[FC_QP_ID];
  struct ibv_send_wr *bad_send_wr;
  // RECEIVES FOR COMMITS
  uint32_t recvs_to_post_num = (uint32_t) (credit_num * FLR_CREDITS_IN_MESSAGE);
  if (ENABLE_ASSERTIONS) assert(recvs_to_post_num < FLR_MAX_RECV_COM_WRS);
  post_recvs_with_recv_info(ctx->qp_meta[COMMIT_W_QP_ID].recv_info, recvs_to_post_num);
  //printf("FLR %d posting %u recvs and has a total of %u recvs for commits \n",
  //		    t_id, recvs_to_post_num,  com_recv_info->posted_recvs);

  for (uint16_t credit_wr_i = 0; credit_wr_i < credit_num; credit_wr_i++) {
    selective_signaling_for_unicast(&qp_meta->sent_tx, qp_meta->ss_batch, qp_meta->send_wr,
                                    credit_wr_i, qp_meta->send_cq, true,
                                    "sending credits", ctx->t_id);
  }
  qp_meta->send_wr[credit_num - 1].next = NULL;
  //my_printf(yellow, "I am sending %d credit message(s)\n", credit_num);
  int ret = ibv_post_send(qp_meta->send_qp, &qp_meta->send_wr[0], &bad_send_wr);
  CPE(ret, "ibv_post_send error in credits", ret);
}


static inline bool can_send_unicasts (per_qp_meta_t *qp_meta)
{
  fifo_t *send_fifo = qp_meta->send_fifo;
  if (qp_meta->needs_credits)
    return send_fifo->capacity > 0 && *qp_meta->credits > 0;
  else return send_fifo->capacity > 0;

}


static inline void send_writes_helper(context_t *ctx)
{
  zk_ctx_t *zk_ctx = (zk_ctx_t *) ctx->appl_ctx;
  per_qp_meta_t *qp_meta = &ctx->qp_meta[COMMIT_W_QP_ID];
  fifo_t *send_fifo = qp_meta->send_fifo;
  zk_w_mes_t *w_mes = (zk_w_mes_t *) get_fifo_pull_slot(send_fifo);
  uint16_t coalesce_num = get_fifo_slot_meta_pull(send_fifo)->coalesce_num;
  w_mes->coalesce_num = (uint8_t) coalesce_num;
    checks_and_stats_when_sending_unicasts(ctx, COMMIT_W_QP_ID, coalesce_num);
  zk_checks_and_print_when_forging_unicast(ctx, COMMIT_W_QP_ID);

  add_to_the_mirrored_buffer(qp_meta->mirror_remote_recv_fifo,
                             (uint8_t) coalesce_num, 1,
                             LEADER_W_BUF_SLOTS, zk_ctx->q_info);

}


static inline void send_reads_helper(context_t *ctx)
{
  per_qp_meta_t *qp_meta = &ctx->qp_meta[R_QP_ID];
  fifo_t *send_fifo = qp_meta->send_fifo;
  zk_r_mes_t *r_mes = (zk_r_mes_t *) get_fifo_pull_slot(send_fifo);
  uint16_t coalesce_num = get_fifo_slot_meta_pull(send_fifo)->coalesce_num;
  r_mes->coalesce_num = (uint8_t) coalesce_num;
  checks_and_stats_when_sending_unicasts(ctx, R_QP_ID, coalesce_num);
  zk_checks_and_print_when_forging_unicast(ctx, R_QP_ID);

}


static inline void send_r_reps_helper(context_t *ctx)
{
  per_qp_meta_t *qp_meta = &ctx->qp_meta[R_QP_ID];
  fifo_t *send_fifo = qp_meta->send_fifo;
  zk_r_rep_mes_t *r_rep_mes = (zk_r_rep_mes_t *) get_fifo_pull_slot(send_fifo);
  uint16_t coalesce_num = get_fifo_slot_meta_pull(send_fifo)->coalesce_num;

  r_rep_mes->coalesce_num = (uint8_t) coalesce_num;

  if (DEBUG_READ_REPS)
    my_printf(yellow, "Wrkr %u SENDING R_REP: coalesce_num %u, l_id %lu, to m_id %u\n",
              ctx->t_id, r_rep_mes->coalesce_num, r_rep_mes->l_id,
              get_fifo_slot_meta_pull(send_fifo)->rm_id);


}


// Send the local writes to the ldr
static inline void send_unicasts(context_t *ctx,
                                 uint16_t qp_id)
{
  struct ibv_send_wr *bad_send_wr;
  uint16_t mes_i = 0;
  per_qp_meta_t *qp_meta = &ctx->qp_meta[qp_id];
  fifo_t *send_fifo = qp_meta->send_fifo;


  while (can_send_unicasts(qp_meta)) {


    if (qp_meta->mfs->send_helper != NULL)
      qp_meta->mfs->send_helper(ctx);

    forge_unicast_wr(ctx, qp_id, qp_meta->send_string, mes_i);

    fifo_send_from_pull_slot(send_fifo);

    // Credit management
    if (qp_meta->needs_credits)
      (*qp_meta->credits)--;
    mes_i++;
  }

  if (mes_i > 0) {
    if (qp_id != COMMIT_W_QP_ID) {
      post_recvs_with_recv_info(qp_meta->recv_info, qp_meta->recv_wr_num - qp_meta->recv_info->posted_recvs);
    }
    //printf("W_i %u, length %u, address %p/ %p \n", mes_i, qp_meta->send_wr[0].sg_list->length,
    //       (void *)qp_meta->send_wr[0].sg_list->addr, send_fifo->fifo);
    qp_meta->send_wr[mes_i - 1].next = NULL;
    check_unicast_before_send(ctx, qp_meta->leader_m_id, qp_id);
    int ret = ibv_post_send(qp_meta->send_qp, &qp_meta->send_wr[0], &bad_send_wr);
    CPE(ret, "Unicast ibv_post_send error", ret);
  }
}




// Follower propagates Updates that have seen all acks to the KVS
static inline void flr_propagate_updates(context_t *ctx)
{
  zk_ctx_t *zk_ctx = (zk_ctx_t *) ctx->appl_ctx;
  fifo_t *prep_buf_mirror = ctx->qp_meta[PREP_ACK_QP_ID].mirror_remote_recv_fifo;

  uint16_t update_op_i = 0;
  // remember the starting point to use it when writing the KVS
  uint32_t starting_pull_ptr = zk_ctx->w_pull_ptr;
  // Read the latest committed g_id
  uint64_t committed_g_id = atomic_load_explicit(&committed_global_w_id, memory_order_relaxed);
  flr_increase_counter_if_waiting_for_commit(zk_ctx, committed_g_id, ctx->t_id);

  while(zk_ctx->w_state[zk_ctx->w_pull_ptr] == READY) {
    if (!is_expected_g_id_ready(zk_ctx, &committed_g_id, &update_op_i,
                                FLR_PENDING_WRITES, ctx->t_id))
      break;
  }

  if (update_op_i > 0) {
    if (ENABLE_ASSERTIONS) {
      assert(zk_ctx->w_size >= update_op_i);
      assert(zk_ctx->p_acks->slots_ahead >= update_op_i);
    }
    remove_from_the_mirrored_buffer(prep_buf_mirror, update_op_i, ctx->t_id, 0, FLR_PREP_BUF_SLOTS);

    zk_ctx->local_w_id += update_op_i; // advance the local_w_id

    zk_ctx->p_acks->slots_ahead -= update_op_i;
    zk_ctx->w_size -= update_op_i;
    zk_KVS_batch_op_updates((uint16_t) update_op_i, zk_ctx->ptrs_to_ops,  starting_pull_ptr,
                            FLR_PENDING_WRITES, true, ctx->t_id);
    if (ENABLE_GIDS)
      atomic_store_explicit(&committed_global_w_id, committed_g_id, memory_order_relaxed);
    zk_signal_completion_and_bookkeepfor_writes(zk_ctx, update_op_i, starting_pull_ptr,
                                                FLR_PENDING_WRITES, FOLLOWER, ctx->t_id);
  }
}



/* ---------------------------------------------------------------------------
//------------------------------ LEADER SPECIFIC -----------------------------
//---------------------------------------------------------------------------*/


// Leader calls this to handout global ids to pending writes
static inline void zk_get_g_ids(context_t *ctx)
{
  if (!ENABLE_GIDS) return;
  zk_ctx_t *zk_ctx = (zk_ctx_t *) ctx->appl_ctx;
	uint16_t unordered_writes_num = (uint16_t) ((LEADER_PENDING_WRITES + zk_ctx->w_push_ptr - zk_ctx->unordered_ptr)
																	% LEADER_PENDING_WRITES);
  if (unordered_writes_num == 0) return;
	uint64_t id = atomic_fetch_add_explicit(&global_w_id, (uint64_t) unordered_writes_num, memory_order_relaxed);
  if (ENABLE_STAT_COUNTING) {
    t_stats[ctx->t_id].batches_per_thread++;
    t_stats[ctx->t_id].total_writes += unordered_writes_num;
  }

	for (uint16_t i = 0; i < unordered_writes_num; ++i) {
    assert(zk_ctx->unordered_ptr == ((LEADER_PENDING_WRITES + zk_ctx->w_push_ptr - unordered_writes_num + i)
                                      % LEADER_PENDING_WRITES));
    uint32_t unordered_ptr = zk_ctx->unordered_ptr;
		zk_ctx->g_id[unordered_ptr] = id + i;
		zk_prepare_t *prep = zk_ctx->ptrs_to_ops[unordered_ptr];
		prep->g_id = zk_ctx->g_id[unordered_ptr];
    MOD_INCR(zk_ctx->unordered_ptr, LEADER_PENDING_WRITES);
	}
  zk_ctx->highest_g_id_taken = id + unordered_writes_num - 1;


	if (ENABLE_ASSERTIONS)
    assert(zk_ctx->unordered_ptr == zk_ctx->w_push_ptr);

}

/* ---------------------------------------------------------------------------
//------------------------------ -----------------------------
//---------------------------------------------------------------------------*/



static inline void zk_increase_prep_credits(uint16_t *credits, zk_ack_mes_t *ack,
                                            struct fifo *remote_prep_buf, uint16_t t_id)
{
  uint8_t rm_id = (uint8_t) (ack->follower_id > LEADER_MACHINE ? ack->follower_id - 1 : ack->follower_id);
  credits[ack->follower_id] +=
    remove_from_the_mirrored_buffer(remote_prep_buf, ack->ack_num, t_id, rm_id, FLR_PREP_BUF_SLOTS);

  if (ENABLE_ASSERTIONS) {
    if (credits[ack->follower_id] > PREPARE_CREDITS)
      my_printf(red, "Prepare credits %u for follower %u \n", credits[ack->follower_id], ack->follower_id);
  }
}

static inline uint32_t zk_find_the_first_prepare_that_gets_acked(uint16_t *ack_num,
                                                                 uint64_t l_id, zk_ctx_t *zk_ctx,
                                                                 uint64_t pull_lid, uint16_t t_id)
{

  if (pull_lid >= l_id) {
    (*ack_num) -= (pull_lid - l_id);
    if (ENABLE_ASSERTIONS) assert(*ack_num > 0 && *ack_num <= FLR_PENDING_WRITES);
    return zk_ctx->w_pull_ptr;
  }
  else { // l_id > pull_lid
    return (uint32_t) (zk_ctx->w_pull_ptr + (l_id - pull_lid)) % LEADER_PENDING_WRITES;
  }
}

static inline void zk_apply_acks(uint16_t ack_num, uint32_t ack_ptr,
                                 uint64_t l_id, zk_ctx_t *zk_ctx,
                                 uint64_t pull_lid, uint32_t *outstanding_prepares,
                                 uint16_t t_id)
{
  for (uint16_t ack_i = 0; ack_i < ack_num; ack_i++) {
    if (ENABLE_ASSERTIONS && (ack_ptr == zk_ctx->w_push_ptr)) {
      uint32_t origin_ack_ptr = (uint32_t) (ack_ptr - ack_i + LEADER_PENDING_WRITES) % LEADER_PENDING_WRITES;
      my_printf(red, "Origin ack_ptr %u/%u, acks %u/%u, w_pull_ptr %u, w_push_ptr % u, capacity %u \n",
                origin_ack_ptr,  (zk_ctx->w_pull_ptr + (l_id - pull_lid)) % LEADER_PENDING_WRITES,
                ack_i, ack_num, zk_ctx->w_pull_ptr, zk_ctx->w_push_ptr, zk_ctx->w_size);
    }
    zk_ctx->acks_seen[ack_ptr]++;
    if (zk_ctx->acks_seen[ack_ptr] == LDR_QUORUM_OF_ACKS) {
      if (ENABLE_ASSERTIONS) (*outstanding_prepares)--;
//        printf("Leader %d valid ack %u/%u write at ptr %d with g_id %lu is ready \n",
//               t_id, ack_i, ack_num,  ack_ptr, zk_ctx->g_id[ack_ptr]);
      zk_ctx->w_state[ack_ptr] = READY;

    }
    MOD_INCR(ack_ptr, LEADER_PENDING_WRITES);
  }
}






// Leader propagates Updates that have seen all acks to the KVS
static inline void ldr_propagate_updates(context_t *ctx)
{
  zk_ctx_t *zk_ctx = (zk_ctx_t *) ctx->appl_ctx;
//  printf("Ldr %d propagating updates \n", t_id);
	uint16_t update_op_i = 0;
  // remember the starting point to use it when writing the KVS
	uint32_t starting_pull_ptr = zk_ctx->w_pull_ptr;
  fifo_t *com_fifo = ctx->qp_meta[COMMIT_W_QP_ID].send_fifo;
  // Read the latest committed g_id
	uint64_t committed_g_id = atomic_load_explicit(&committed_global_w_id, memory_order_relaxed);
	while(zk_ctx->w_state[zk_ctx->w_pull_ptr] == READY) {
    // the commit prep_message is full: that may be too restricting,
    // but it does not affect performance or correctness
		if (com_fifo->capacity == COMMIT_FIFO_SIZE) break;
    if (!is_expected_g_id_ready(zk_ctx, &committed_g_id, &update_op_i,
                                LEADER_PENDING_WRITES, ctx->t_id))
      break;
 	}
	if (update_op_i > 0) {
    zk_create_commit_message(com_fifo, zk_ctx->local_w_id, update_op_i);
		zk_ctx->local_w_id += update_op_i; // advance the local_w_id

    if (ENABLE_ASSERTIONS) assert(zk_ctx->w_size >= update_op_i);
    zk_ctx->w_size -= update_op_i;
    zk_KVS_batch_op_updates((uint16_t) update_op_i, zk_ctx->ptrs_to_ops, starting_pull_ptr,
                            LEADER_PENDING_WRITES, false, ctx->t_id);

		atomic_store_explicit(&committed_global_w_id, committed_g_id, memory_order_relaxed);
    zk_signal_completion_and_bookkeepfor_writes(zk_ctx, update_op_i, starting_pull_ptr,
                                                LEADER_PENDING_WRITES, LEADER, ctx->t_id);
	}

  check_ldr_p_states(zk_ctx, ctx->t_id);
}



static inline bool ack_handler(context_t *ctx)
{
  zk_ctx_t *zk_ctx = (zk_ctx_t *) ctx->appl_ctx;
  per_qp_meta_t *qp_meta = &ctx->qp_meta[PREP_ACK_QP_ID];
  fifo_t *recv_fifo = qp_meta->recv_fifo;
  volatile zk_ack_mes_ud_t *incoming_acks = (volatile zk_ack_mes_ud_t *) recv_fifo->fifo;
  zk_ack_mes_t *ack = (zk_ack_mes_t *) &incoming_acks[recv_fifo->pull_ptr].ack;
  uint16_t ack_num = ack->ack_num;
  uint64_t l_id = ack->l_id;
  uint64_t pull_lid = zk_ctx->local_w_id; // l_id at the pull pointer
  uint32_t ack_ptr; // a pointer in the FIFO, from where ack should be added
  zk_check_polled_ack_and_print(ack, ack_num, pull_lid, recv_fifo->pull_ptr, ctx->t_id);
  zk_increase_prep_credits(qp_meta->credits, ack, qp_meta->mirror_remote_recv_fifo, ctx->t_id);
  if ((zk_ctx->w_size == 0 ) ||
      (pull_lid >= l_id && (pull_lid - l_id) >= ack_num))
    return true;

  zk_check_ack_l_id_is_small_enough(ack_num, l_id, zk_ctx, pull_lid, ctx->t_id);
  ack_ptr = zk_find_the_first_prepare_that_gets_acked(&ack_num, l_id, zk_ctx, pull_lid, ctx->t_id);

  // Apply the acks that refer to stored writes
  zk_apply_acks(ack_num, ack_ptr, l_id, zk_ctx, pull_lid,
                &qp_meta->outstanding_messages, ctx->t_id);

  return true;
}


static inline bool write_handler(context_t *ctx)
{
  zk_ctx_t *zk_ctx = (zk_ctx_t *) ctx->appl_ctx;
  if (ENABLE_ASSERTIONS) assert(zk_ctx->w_size < LEADER_PENDING_WRITES);
  per_qp_meta_t *qp_meta = &ctx->qp_meta[COMMIT_W_QP_ID];
  fifo_t *recv_fifo = qp_meta->recv_fifo;
  volatile zk_w_mes_ud_t *incoming_ws = (volatile zk_w_mes_ud_t *) qp_meta->recv_fifo->fifo;
  zk_w_mes_t *w_mes = (zk_w_mes_t *) &incoming_ws[recv_fifo->pull_ptr].w_mes;
  if (DEBUG_WRITES) printf("Leader sees a write Opcode %d at offset %d  \n",
                           w_mes->write[0].opcode, recv_fifo->pull_ptr);

  uint8_t w_num = w_mes->coalesce_num;
  if (zk_ctx->w_size + w_num > LEADER_PENDING_WRITES) {
    return false;
  }
  for (uint16_t i = 0; i < w_num; i++) {
    zk_write_t *write = &w_mes->write[i];
    if (ENABLE_ASSERTIONS) if(write->opcode != KVS_OP_PUT)
        my_printf(red, "Opcode %u, i %u/%u \n",write->opcode, i, w_num);
    if (DEBUG_WRITES)
      printf("Poll for writes passes session id %u \n", write->sess_id);
    insert_mes(ctx, PREP_ACK_QP_ID, (uint32_t) PREP_SIZE, 1, false, (void *) write, REMOTE_WRITE);
    //ldr_insert_prep(ctx, zk_ctx, (void *) write, false);
    write->opcode = 0;
  }
  if (ENABLE_STAT_COUNTING) {
    t_stats[ctx->t_id].received_writes += w_num;
    t_stats[ctx->t_id].received_writes_mes_num++;
  }
  return true;
}



static inline bool r_rep_handler(context_t *ctx)
{

  zk_ctx_t *zk_ctx = (zk_ctx_t *) ctx->appl_ctx;
  per_qp_meta_t *qp_meta = &ctx->qp_meta[R_QP_ID];
  fifo_t *recv_fifo = qp_meta->recv_fifo;
  volatile zk_r_rep_mes_ud_t *incoming_r_reps = (volatile zk_r_rep_mes_ud_t *) recv_fifo->fifo;
  zk_r_rep_mes_t *r_rep_mes = (zk_r_rep_mes_t *) &incoming_r_reps[recv_fifo->pull_ptr].r_rep_mes;
  fifo_t *r_meta_fifo = zk_ctx->r_meta;
  if (DEBUG_READ_REPS)
    my_printf(cyan, "WRKR %u: RECEIVING R_REP: l_id %u/%lu, coalesce_num %u \n",
              ctx->t_id, r_rep_mes->l_id, zk_ctx->local_r_id, r_rep_mes->coalesce_num);

  assert(r_meta_fifo->capacity > 0);
  uint64_t l_id = r_rep_mes->l_id;
  (*qp_meta->credits)++;
  uint16_t byte_ptr = R_REP_MES_HEADER;
  for (int r_rep_i = 0; r_rep_i < r_rep_mes->coalesce_num; ++r_rep_i) {
    r_meta_t *r_meta = (r_meta_t *) get_fifo_pull_slot(r_meta_fifo);

    zk_r_rep_big_t *r_rep = (zk_r_rep_big_t *) (((void *) r_rep_mes) + byte_ptr);

    if (DEBUG_READ_REPS)
      my_printf(yellow, "Wrkr: %u R_rep %u/%u opcode %u, session %u\n",
                ctx->t_id, r_rep_i, r_rep_mes->coalesce_num, r_rep->opcode, r_meta->sess_id);

    assert(r_meta->state = VALID);
    assert(r_meta->l_id == r_rep_mes->l_id + r_rep_i);
    byte_ptr += get_size_from_opcode(r_rep->opcode);
    uint8_t *value_to_read =  r_rep->opcode == G_ID_EQUAL ? r_meta->value : r_rep->value;
    memcpy(r_meta->value_to_read, value_to_read, VALUE_SIZE);
    zk_ctx->local_r_id++;

    zk_ctx->stalled[r_meta->sess_id] = false;
    zk_ctx->all_sessions_stalled = false;
    r_meta->state = INVALID;
    fifo_incr_pull_ptr(r_meta_fifo);
    fifo_decr_capacity(r_meta_fifo);
  }

  return true;

}

static inline bool r_handler(context_t *ctx)
{

  zk_ctx_t *zk_ctx = (zk_ctx_t *) ctx->appl_ctx;

  per_qp_meta_t *qp_meta = &ctx->qp_meta[R_QP_ID];
  fifo_t *recv_fifo = qp_meta->recv_fifo;
  volatile zk_r_mes_ud_t *incoming_reads = (volatile zk_r_mes_ud_t *) recv_fifo->fifo;
  zk_r_mes_t *r_mes = (zk_r_mes_t *) &incoming_reads[recv_fifo->pull_ptr].r_mes;
  uint8_t r_num = r_mes->coalesce_num;
  uint16_t byte_ptr = R_MES_HEADER;
  if (DEBUG_READS)
      my_printf(green, "WRKR %u RECEIVING READ MESSAGE: lid %u, coalesce num %u from %u \n",
                ctx->t_id, r_mes->l_id, r_mes->coalesce_num, r_mes->m_id);

  ptrs_to_r_t *ptrs_to_r = zk_ctx->ptrs_to_r;
  if (zk_ctx->polled_messages == 0) ptrs_to_r->polled_reads = 0;
  for (uint16_t r_i = 0; r_i < r_num; r_i++) {
    zk_read_t *read = (zk_read_t*)(((void *) r_mes) + byte_ptr);
    //printf("Receiving read opcode %u \n", read->opcode);

    if (DEBUG_READS)
      my_printf(yellow, "wrkr %u Read %u, opcode %u, g_id %lu, key.bkt %u\n",
                ctx->t_id, r_i, read->opcode, read->g_id, read->key.bkt);

    if (ENABLE_ASSERTIONS) {
      assert(read->opcode == KVS_OP_GET);
      assert(read->key.bkt > 0);
    }


    ptrs_to_r->ptr_to_ops[ptrs_to_r->polled_reads] = read;
    ptrs_to_r->ptr_to_r_mes[ptrs_to_r->polled_reads] = r_mes;
    ptrs_to_r->coalesce_r_rep[ptrs_to_r->polled_reads] = r_i > 0;
    ptrs_to_r->polled_reads++;
    byte_ptr += R_SIZE;
  }

  return true;
}

static inline bool prepare_handler(context_t *ctx)
{
  zk_ctx_t *zk_ctx = (zk_ctx_t *) ctx->appl_ctx;
  per_qp_meta_t *qp_meta = &ctx->qp_meta[PREP_ACK_QP_ID];
  fifo_t *recv_fifo = qp_meta->recv_fifo;
  volatile zk_prep_mes_ud_t *incoming_preps = (volatile zk_prep_mes_ud_t *) recv_fifo->fifo;
  zk_prep_mes_t *prep_mes = (zk_prep_mes_t *) &incoming_preps[recv_fifo->pull_ptr].prepare;

  uint8_t coalesce_num = prep_mes->coalesce_num;
  zk_prepare_t *prepare = prep_mes->prepare;
  uint64_t incoming_l_id = prep_mes->l_id;
  uint64_t expected_l_id = zk_ctx->local_w_id + zk_ctx->w_size;

  if (qp_meta->mirror_remote_recv_fifo->capacity == MAX_PREP_BUF_SLOTS_TO_BE_POLLED) return false;
  if (zk_ctx->w_size + coalesce_num > FLR_PENDING_WRITES) return false;
  zk_check_polled_prep_and_print(prep_mes, zk_ctx, coalesce_num, recv_fifo->pull_ptr,
                                 incoming_l_id, expected_l_id, incoming_preps, ctx->t_id);

  zk_ctx->p_acks->acks_to_send+= coalesce_num; // lids are in order so ack them
  add_to_the_mirrored_buffer(qp_meta->mirror_remote_recv_fifo, coalesce_num, 1,
                             FLR_PREP_BUF_SLOTS, zk_ctx->q_info);
  ///Loop through prepares inside the message
  for (uint8_t prep_i = 0; prep_i < coalesce_num; prep_i++) {
    zk_check_prepare_and_print(&prepare[prep_i], zk_ctx, prep_i, ctx->t_id);
    fill_zk_ctx_entry(zk_ctx, &prepare[prep_i], ctx->m_id, ctx->t_id);
    MOD_INCR(zk_ctx->w_push_ptr, FLR_PENDING_WRITES);
    zk_ctx->w_size++;
  } ///

  if (ENABLE_ASSERTIONS) prep_mes->opcode = 0;

  return true;
}

static inline bool commit_handler(context_t *ctx)
{
  zk_ctx_t *zk_ctx = (zk_ctx_t *) ctx->appl_ctx;
  per_qp_meta_t *qp_meta = &ctx->qp_meta[COMMIT_W_QP_ID];
  fifo_t *recv_fifo = qp_meta->recv_fifo;
  volatile zk_com_mes_ud_t *incoming_coms = (volatile zk_com_mes_ud_t *) recv_fifo->fifo;

  zk_com_mes_t *com = (zk_com_mes_t *) &incoming_coms[recv_fifo->pull_ptr].com;
  uint16_t com_num = com->com_num;
  uint64_t l_id = com->l_id;
  uint64_t pull_lid = zk_ctx->local_w_id; // l_id at the pull pointer
  zk_check_polled_commit_and_print(com, zk_ctx, recv_fifo->pull_ptr,
                                   l_id, pull_lid, com_num, ctx->t_id);
  // This must always hold: l_id >= pull_lid,
  // because we need the commit to advance the pull_lid
  uint16_t com_ptr = (uint16_t)
    ((zk_ctx->w_pull_ptr + (l_id - pull_lid)) % FLR_PENDING_WRITES);
  /// loop through each commit
  for (uint16_t com_i = 0; com_i < com_num; com_i++) {
    if (zk_write_not_ready(com, com_ptr, com_i, com_num, zk_ctx, ctx->t_id))
      return false;

    assert(l_id + com_i - pull_lid < FLR_PENDING_WRITES);

    zk_ctx->w_state[com_ptr] = READY;
    flr_increases_write_credits(ctx, zk_ctx, com_ptr, qp_meta->mirror_remote_recv_fifo);
    MOD_INCR(com_ptr, FLR_PENDING_WRITES);
  } ///

  if (ENABLE_ASSERTIONS) com->opcode = 0;

  if (ENABLE_STAT_COUNTING) {
    t_stats[ctx->t_id].received_coms += com_num;
    t_stats[ctx->t_id].received_coms_mes_num++;
  }
  if (recv_fifo->pull_ptr % FLR_CREDITS_IN_MESSAGE == 0)
    send_credits_for_commits(ctx, 1);

  return true;
}

// Poll for incoming write requests from followers
static inline void poll_incoming_messages(context_t *ctx,
                                          uint16_t qp_id)
{
  zk_ctx_t *zk_ctx = (zk_ctx_t *)ctx->appl_ctx;
  per_qp_meta_t *qp_meta = &ctx->qp_meta[qp_id];
  fifo_t *recv_fifo = qp_meta->recv_fifo;
  int completed_messages =
    find_how_many_messages_can_be_polled(qp_meta->recv_cq, qp_meta->recv_wc,
                                         &qp_meta->completed_but_not_polled,
                                         qp_meta->recv_buf_slot_num, ctx->t_id);
  if (completed_messages <= 0) {
    if (qp_meta->recv_type == RECV_REPLY) {
      if (qp_meta->outstanding_messages > 0) qp_meta->wait_for_reps_ctr++;
    }
    return;
  }
  //printf("completed %d \n", completed_messages);
	zk_ctx->polled_messages = 0;

	// Start polling
  while (zk_ctx->polled_messages < completed_messages) {

    if (!qp_meta->mfs->recv_handler(ctx)) break;

    fifo_incr_pull_ptr(recv_fifo);
    zk_ctx->polled_messages++;
	}
  qp_meta->completed_but_not_polled = completed_messages - zk_ctx->polled_messages;
  //printf("polled %d , completed not polled %d \n", zk_ctx->polled_messages, qp_meta->completed_but_not_polled);
  zk_debug_info_bookkeep(ctx, qp_id, completed_messages, zk_ctx->polled_messages);
  qp_meta->recv_info->posted_recvs -= zk_ctx->polled_messages;


  if (zk_ctx->polled_messages > 0 && qp_meta->mfs->recv_kvs != NULL)
    qp_meta->mfs->recv_kvs(ctx);

}



/* ---------------------------------------------------------------------------
//------------------------------ BROADCASTS -----------------------------
//---------------------------------------------------------------------------*/
static inline void send_commits_helper(context_t *ctx)
{
  per_qp_meta_t *cred_qp_meta = &ctx->qp_meta[FC_QP_ID];
}

// Leader broadcasts commits
static inline void broadcast_commits(context_t *ctx, uint16_t qp_id)
{
  zk_ctx_t *zk_ctx = (zk_ctx_t *) ctx->appl_ctx;
  per_qp_meta_t *qp_meta = &ctx->qp_meta[qp_id];
  per_qp_meta_t *cred_qp_meta = &ctx->qp_meta[FC_QP_ID];
  fifo_t *send_fifo = qp_meta->send_fifo;
  if (send_fifo->capacity == 0) return;
  uint8_t vc = COMM_VC;
  uint16_t  br_i = 0, credit_recv_counter = 0, mes_sent = 0, available_credits = 0;

  if (!check_bcast_credits(qp_meta->credits, zk_ctx->q_info, &qp_meta->time_out_cnt,
                           &available_credits, 1,  ctx->t_id)) {
    if (ENABLE_STAT_COUNTING) t_stats[ctx->t_id].stalled_com_credit++;
    return;
  }
  zk_com_mes_t *send_buffer = (zk_com_mes_t *) send_fifo->fifo;
  while (send_fifo->capacity > 0 && mes_sent < available_credits) {
		zk_com_mes_t *com_mes = &send_buffer[send_fifo->pull_ptr];
    // Create the broadcast messages
    forge_commit_wrs(ctx, com_mes, zk_ctx->q_info, br_i);
    post_recvs_with_recv_info(cred_qp_meta->recv_info,
                              cred_qp_meta->recv_wr_num - cred_qp_meta->recv_info->posted_recvs);
		send_fifo->capacity--;
		MOD_INCR(send_fifo->pull_ptr, send_fifo->max_size);
    br_i++;
    mes_sent++;
    zk_checks_and_stats_on_bcasting_commits(send_fifo, com_mes, br_i, ctx->t_id);
    //if (qp_meta->sent_tx % FLR_CREDITS_IN_MESSAGE == 0) credit_recv_counter++;
    if (br_i == MAX_BCAST_BATCH) {

      post_quorum_broadasts_and_recvs(qp_meta->recv_info, qp_meta->recv_wr_num - qp_meta->recv_info->posted_recvs,
                                      zk_ctx->q_info, br_i, qp_meta->sent_tx, qp_meta->send_wr,
                                      qp_meta->send_qp, qp_meta->enable_inlining);
      br_i = 0;
    }
  }
	if (br_i > 0) {
    //post_recvs_with_recv_info(cred_qp_meta->recv_info, cred_qp_meta->recv_wr_num - cred_qp_meta->recv_info->posted_recvs);
    post_quorum_broadasts_and_recvs(qp_meta->recv_info, qp_meta->recv_wr_num - qp_meta->recv_info->posted_recvs,
                                    zk_ctx->q_info, br_i, qp_meta->sent_tx, qp_meta->send_wr,
                                    qp_meta->send_qp, qp_meta->enable_inlining);
	}
	if (ENABLE_ASSERTIONS) assert(qp_meta->recv_info->posted_recvs <= LDR_MAX_RECV_W_WRS);
  if (mes_sent > 0) decrease_credits(qp_meta->credits, zk_ctx->q_info, mes_sent);
}


static inline void send_prepares_helper(context_t *ctx)
{
  zk_ctx_t *zk_ctx = (zk_ctx_t *) ctx->appl_ctx;
  per_qp_meta_t *qp_meta = &ctx->qp_meta[PREP_ACK_QP_ID];
  fifo_t *send_fifo = qp_meta->send_fifo;
  if (DEBUG_PREPARES)
    printf("LDR %d has %u bcasts to send credits %d\n", ctx->t_id,
           send_fifo->net_capacity, qp_meta->credits[1]);
  // Create the broadcast messages
  zk_prep_mes_t *prep_buf = (zk_prep_mes_t *) qp_meta->send_fifo->fifo;
  zk_prep_mes_t *prep = &prep_buf[send_fifo->pull_ptr];

  slot_meta_t *slot_meta = get_fifo_slot_meta_pull(send_fifo);
  uint8_t coalesce_num = (uint8_t) slot_meta->coalesce_num;
  prep->coalesce_num = (uint8_t) slot_meta->coalesce_num;
  uint32_t backward_ptr = fifo_get_pull_backward_ptr(send_fifo);

  for (uint16_t i = 0; i < coalesce_num; i++) {
    zk_ctx->w_state[(backward_ptr + i) % LEADER_PENDING_WRITES] = SENT;
    if (DEBUG_PREPARES)
      printf("Prepare %d, val-len %u, total message capacity %d\n", i, prep->prepare[i].val_len,
             slot_meta->byte_size);
    if (ENABLE_ASSERTIONS) {
      assert(prep->prepare[i].val_len == VALUE_SIZE >> SHIFT_BITS);
      assert(prep->prepare[i].opcode == KVS_OP_PUT);
    }
  }

  if (DEBUG_PREPARES)
    my_printf(green, "Leader %d : I BROADCAST a prepare message %d of "
                "%u prepares with total w_size %u,  with  credits: %d, lid: %lu  \n",
              ctx->t_id, prep->opcode, coalesce_num, slot_meta->byte_size,
              qp_meta->credits[0], prep->l_id);


  add_to_the_mirrored_buffer(qp_meta->mirror_remote_recv_fifo,
                             coalesce_num, FOLLOWER_MACHINE_NUM,
                             FLR_PREP_BUF_SLOTS, zk_ctx->q_info);
  zk_checks_and_stats_on_bcasting_prepares(zk_ctx, coalesce_num,
                                           &qp_meta->outstanding_messages, ctx->t_id);
}

// Leader Broadcasts its Prepares
static inline void send_broadcasts(context_t *ctx, uint16_t qp_id)
{
  zk_ctx_t *zk_ctx = (zk_ctx_t *) ctx->appl_ctx;
  per_qp_meta_t *qp_meta = &ctx->qp_meta[qp_id];
	uint16_t br_i = 0, mes_sent = 0, available_credits = 0;
  fifo_t *send_fifo = qp_meta->send_fifo;
  if (send_fifo->net_capacity == 0) return;
  else if (!check_bcast_credits(qp_meta->credits, zk_ctx->q_info,
                                &qp_meta->time_out_cnt,
                                &available_credits, 1,
                                ctx->t_id)) return;


	while (send_fifo->net_capacity > 0 && mes_sent < available_credits) {

    qp_meta->mfs->send_helper(ctx);
    forge_bcast_wr(ctx, qp_id, br_i);
    fifo_send_from_pull_slot(send_fifo);
    br_i++;
    mes_sent++;


		if (br_i == MAX_BCAST_BATCH) {
      post_quorum_broadasts_and_recvs(qp_meta->recv_info, qp_meta->recv_wr_num - qp_meta->recv_info->posted_recvs,
                                      zk_ctx->q_info, br_i, qp_meta->sent_tx, qp_meta->send_wr,
                                      qp_meta->send_qp, qp_meta->enable_inlining);
      br_i = 0;
		}
	}
  if (br_i > 0) {
    post_quorum_broadasts_and_recvs(qp_meta->recv_info, qp_meta->recv_wr_num - qp_meta->recv_info->posted_recvs,
                                    zk_ctx->q_info, br_i, qp_meta->sent_tx, qp_meta->send_wr,
                                    qp_meta->send_qp, qp_meta->enable_inlining);
  }
  if (ENABLE_ASSERTIONS) assert(qp_meta->recv_info->posted_recvs <= qp_meta->recv_wr_num);
  if (mes_sent > 0) decrease_credits(qp_meta->credits, zk_ctx->q_info, mes_sent);
}

/* ---------------------------------------------------------------------------
//------------------------------ MAIN LOOPS -----------------------------
//---------------------------------------------------------------------------*/

static inline void flr_main_loop(context_t *ctx)
{

  if (PUT_A_MACHINE_TO_SLEEP && (machine_id == MACHINE_THAT_SLEEPS) &&
      (t_stats[WORKERS_PER_MACHINE - 1].cache_hits_per_thread > 4 * MILLION)) {
    if (ctx->t_id == 0) my_printf(yellow, "Machine performs scheduled failure\n");
    exit(0);
  }

  poll_incoming_messages(ctx, PREP_ACK_QP_ID);

  send_acks_to_ldr(ctx);


  poll_incoming_messages(ctx, COMMIT_W_QP_ID);


  flr_propagate_updates(ctx);


  poll_incoming_messages(ctx, R_QP_ID);


  zk_batch_from_trace_to_KVS(ctx);


  send_unicasts(ctx, COMMIT_W_QP_ID);


  send_unicasts(ctx, R_QP_ID);

}


static inline void ldr_main_loop(context_t *ctx)
{


  ldr_check_debug_cntrs(ctx);

  poll_incoming_messages(ctx, PREP_ACK_QP_ID);

  ldr_propagate_updates(ctx);

  ldr_poll_credits(ctx);
  broadcast_commits(ctx, COMMIT_W_QP_ID);

  // Get a new batch from the trace, pass it through the cache and create
  // the appropriate prepare messages
  zk_batch_from_trace_to_KVS(ctx);

  // get local and remote writes back to back to increase the write batch
  poll_incoming_messages(ctx, COMMIT_W_QP_ID);

  // Assign a global write  id to each new write
  zk_get_g_ids(ctx);

  send_broadcasts(ctx, PREP_ACK_QP_ID);

  poll_incoming_messages(ctx, R_QP_ID);

  send_unicasts(ctx, R_QP_ID);

}

static inline void main_loop(context_t *ctx)
{
  zk_ctx_t * zk_ctx =(zk_ctx_t *) ctx->appl_ctx;
  while (true) {
    switch (zk_ctx->protocol) {
      case FOLLOWER:
        flr_main_loop(ctx);
        break;
      case LEADER:
        ldr_main_loop(ctx);
        break;
    }
  }
}

#endif /* INLINE_UTILS_H */
