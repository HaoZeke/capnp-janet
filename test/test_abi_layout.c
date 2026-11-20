/* sizeof, alignof and offsetof of every complete public struct, as
 * they were at 40876e91. Callers allocate these objects.
 * capnp_dyn_t is incomplete and has no size.
 */
#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_canonical.h>
#include <capnp-janet/capnp_copy.h>
#include <capnp-janet/capnp_dyn.h>
#include <capnp-janet/capnp_janet.h>
#include <capnp-janet/capnp_kinds.h>
#include <capnp-janet/capnp_message.h>
#include <capnp-janet/capnp_packed.h>
#include <capnp-janet/capnp_pointer.h>
#include <capnp-janet/capnp_rpc.h>
#include <capnp-janet/capnp_transport.h>

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

static int g_fail;

static void check_n(const char *what, size_t got, size_t want) {
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %zu want %zu\n", what, got, want);
    g_fail++;
  }
}

static void layout_64(void) {
  check_n("sizeof capnp_bseg_t", sizeof(capnp_bseg_t), 24);
  check_n("alignof capnp_bseg_t", _Alignof(capnp_bseg_t), 8);
  check_n("offsetof capnp_bseg_t.data", offsetof(capnp_bseg_t, data), 0);
  check_n("offsetof capnp_bseg_t.words", offsetof(capnp_bseg_t, words), 8);
  check_n("offsetof capnp_bseg_t.cap", offsetof(capnp_bseg_t, cap), 16);
  check_n("sizeof capnp_builder_t", sizeof(capnp_builder_t), 32);
  check_n("alignof capnp_builder_t", _Alignof(capnp_builder_t), 8);
  check_n("offsetof capnp_builder_t.segs", offsetof(capnp_builder_t, segs), 0);
  check_n("offsetof capnp_builder_t.nsegs", offsetof(capnp_builder_t, nsegs), 8);
  check_n("offsetof capnp_builder_t.segs_cap", offsetof(capnp_builder_t, segs_cap), 12);
  check_n("offsetof capnp_builder_t.max_seg_words", offsetof(capnp_builder_t, max_seg_words), 16);
  check_n("offsetof capnp_builder_t.force_single", offsetof(capnp_builder_t, force_single), 24);
  check_n("sizeof capnp_bptr_t", sizeof(capnp_bptr_t), 24);
  check_n("alignof capnp_bptr_t", _Alignof(capnp_bptr_t), 8);
  check_n("offsetof capnp_bptr_t.b", offsetof(capnp_bptr_t, b), 0);
  check_n("offsetof capnp_bptr_t.seg", offsetof(capnp_bptr_t, seg), 8);
  check_n("offsetof capnp_bptr_t.word", offsetof(capnp_bptr_t, word), 16);
  check_n("sizeof capnp_orphan_t", sizeof(capnp_orphan_t), 48);
  check_n("alignof capnp_orphan_t", _Alignof(capnp_orphan_t), 8);
  check_n("offsetof capnp_orphan_t.b", offsetof(capnp_orphan_t, b), 0);
  check_n("offsetof capnp_orphan_t.seg", offsetof(capnp_orphan_t, seg), 8);
  check_n("offsetof capnp_orphan_t.word", offsetof(capnp_orphan_t, word), 16);
  check_n("offsetof capnp_orphan_t.kind", offsetof(capnp_orphan_t, kind), 24);
  check_n("offsetof capnp_orphan_t.dwords", offsetof(capnp_orphan_t, dwords), 28);
  check_n("offsetof capnp_orphan_t.pwords", offsetof(capnp_orphan_t, pwords), 30);
  check_n("offsetof capnp_orphan_t.esize", offsetof(capnp_orphan_t, esize), 32);
  check_n("offsetof capnp_orphan_t.count", offsetof(capnp_orphan_t, count), 36);
  check_n("offsetof capnp_orphan_t.live", offsetof(capnp_orphan_t, live), 40);
  check_n("sizeof capnp_segment_t", sizeof(capnp_segment_t), 16);
  check_n("alignof capnp_segment_t", _Alignof(capnp_segment_t), 8);
  check_n("offsetof capnp_segment_t.data", offsetof(capnp_segment_t, data), 0);
  check_n("offsetof capnp_segment_t.words", offsetof(capnp_segment_t, words), 8);
  check_n("sizeof capnp_message_t", sizeof(capnp_message_t), 56);
  check_n("alignof capnp_message_t", _Alignof(capnp_message_t), 8);
  check_n("offsetof capnp_message_t.segs", offsetof(capnp_message_t, segs), 0);
  check_n("offsetof capnp_message_t.nsegs", offsetof(capnp_message_t, nsegs), 8);
  check_n("offsetof capnp_message_t.owned", offsetof(capnp_message_t, owned), 16);
  check_n("offsetof capnp_message_t.owned_len", offsetof(capnp_message_t, owned_len), 24);
  check_n("offsetof capnp_message_t.traversal_left", offsetof(capnp_message_t, traversal_left), 32);
  check_n("offsetof capnp_message_t.depth_limit", offsetof(capnp_message_t, depth_limit), 40);
  check_n("offsetof capnp_message_t.ready", offsetof(capnp_message_t, ready), 48);
  check_n("sizeof capnp_ptr_t", sizeof(capnp_ptr_t), 56);
  check_n("alignof capnp_ptr_t", _Alignof(capnp_ptr_t), 8);
  check_n("offsetof capnp_ptr_t.msg", offsetof(capnp_ptr_t, msg), 0);
  check_n("offsetof capnp_ptr_t.seg", offsetof(capnp_ptr_t, seg), 8);
  check_n("offsetof capnp_ptr_t.word", offsetof(capnp_ptr_t, word), 16);
  check_n("offsetof capnp_ptr_t.kind", offsetof(capnp_ptr_t, kind), 24);
  check_n("offsetof capnp_ptr_t.dwords", offsetof(capnp_ptr_t, dwords), 28);
  check_n("offsetof capnp_ptr_t.pwords", offsetof(capnp_ptr_t, pwords), 30);
  check_n("offsetof capnp_ptr_t.esize", offsetof(capnp_ptr_t, esize), 32);
  check_n("offsetof capnp_ptr_t.count", offsetof(capnp_ptr_t, count), 36);
  check_n("offsetof capnp_ptr_t.step_words", offsetof(capnp_ptr_t, step_words), 40);
  check_n("offsetof capnp_ptr_t.body_byte", offsetof(capnp_ptr_t, body_byte), 48);
  check_n("offsetof capnp_ptr_t.data_bits", offsetof(capnp_ptr_t, data_bits), 50);
  check_n("offsetof capnp_ptr_t.depth_left", offsetof(capnp_ptr_t, depth_left), 52);
  check_n("sizeof capnp_rpc_answer_t", sizeof(capnp_rpc_answer_t), 8208);
  check_n("alignof capnp_rpc_answer_t", _Alignof(capnp_rpc_answer_t), 8);
  check_n("offsetof capnp_rpc_answer_t.used", offsetof(capnp_rpc_answer_t, used), 0);
  check_n("offsetof capnp_rpc_answer_t.question_id", offsetof(capnp_rpc_answer_t, question_id), 4);
  check_n("offsetof capnp_rpc_answer_t.frame", offsetof(capnp_rpc_answer_t, frame), 8);
  check_n("offsetof capnp_rpc_answer_t.len", offsetof(capnp_rpc_answer_t, len), 8200);
  check_n("sizeof capnp_rpc_export_t", sizeof(capnp_rpc_export_t), 24);
  check_n("alignof capnp_rpc_export_t", _Alignof(capnp_rpc_export_t), 8);
  check_n("offsetof capnp_rpc_export_t.used", offsetof(capnp_rpc_export_t, used), 0);
  check_n("offsetof capnp_rpc_export_t.refcount", offsetof(capnp_rpc_export_t, refcount), 4);
  check_n("offsetof capnp_rpc_export_t.server", offsetof(capnp_rpc_export_t, server), 8);
  check_n("offsetof capnp_rpc_export_t.dispatch", offsetof(capnp_rpc_export_t, dispatch), 16);
  check_n("sizeof capnp_rpc_question_t", sizeof(capnp_rpc_question_t), 8216);
  check_n("alignof capnp_rpc_question_t", _Alignof(capnp_rpc_question_t), 8);
  check_n("offsetof capnp_rpc_question_t.used", offsetof(capnp_rpc_question_t, used), 0);
  check_n("offsetof capnp_rpc_question_t.question_id", offsetof(capnp_rpc_question_t, question_id), 4);
  check_n("offsetof capnp_rpc_question_t.answered", offsetof(capnp_rpc_question_t, answered), 8);
  check_n("offsetof capnp_rpc_question_t.failed", offsetof(capnp_rpc_question_t, failed), 12);
  check_n("offsetof capnp_rpc_question_t.reply", offsetof(capnp_rpc_question_t, reply), 16);
  check_n("offsetof capnp_rpc_question_t.reply_len", offsetof(capnp_rpc_question_t, reply_len), 8208);
  check_n("sizeof capnp_rpc_provision_t", sizeof(capnp_rpc_provision_t), 48);
  check_n("alignof capnp_rpc_provision_t", _Alignof(capnp_rpc_provision_t), 8);
  check_n("offsetof capnp_rpc_provision_t.used", offsetof(capnp_rpc_provision_t, used), 0);
  check_n("offsetof capnp_rpc_provision_t.nonce", offsetof(capnp_rpc_provision_t, nonce), 8);
  check_n("offsetof capnp_rpc_provision_t.server", offsetof(capnp_rpc_provision_t, server), 16);
  check_n("offsetof capnp_rpc_provision_t.dispatch", offsetof(capnp_rpc_provision_t, dispatch), 24);
  check_n("offsetof capnp_rpc_provision_t.export_id", offsetof(capnp_rpc_provision_t, export_id), 32);
  check_n("offsetof capnp_rpc_provision_t.question_id", offsetof(capnp_rpc_provision_t, question_id), 36);
  check_n("offsetof capnp_rpc_provision_t.embargoed", offsetof(capnp_rpc_provision_t, embargoed), 40);
  check_n("offsetof capnp_rpc_provision_t.accept_question_id", offsetof(capnp_rpc_provision_t, accept_question_id), 44);
  check_n("sizeof capnp_rpc_introduction_t", sizeof(capnp_rpc_introduction_t), 88);
  check_n("alignof capnp_rpc_introduction_t", _Alignof(capnp_rpc_introduction_t), 8);
  check_n("offsetof capnp_rpc_introduction_t.used", offsetof(capnp_rpc_introduction_t, used), 0);
  check_n("offsetof capnp_rpc_introduction_t.nonce", offsetof(capnp_rpc_introduction_t, nonce), 8);
  check_n("offsetof capnp_rpc_introduction_t.vine_id", offsetof(capnp_rpc_introduction_t, vine_id), 16);
  check_n("offsetof capnp_rpc_introduction_t.port", offsetof(capnp_rpc_introduction_t, port), 20);
  check_n("offsetof capnp_rpc_introduction_t.host", offsetof(capnp_rpc_introduction_t, host), 22);
  check_n("sizeof capnp_rpc_stream_t", sizeof(capnp_rpc_stream_t), 276);
  check_n("alignof capnp_rpc_stream_t", _Alignof(capnp_rpc_stream_t), 4);
  check_n("offsetof capnp_rpc_stream_t.window", offsetof(capnp_rpc_stream_t, window), 0);
  check_n("offsetof capnp_rpc_stream_t.nout", offsetof(capnp_rpc_stream_t, nout), 4);
  check_n("offsetof capnp_rpc_stream_t.qids", offsetof(capnp_rpc_stream_t, qids), 8);
  check_n("offsetof capnp_rpc_stream_t.failed", offsetof(capnp_rpc_stream_t, failed), 264);
  check_n("offsetof capnp_rpc_stream_t.first_failure", offsetof(capnp_rpc_stream_t, first_failure), 268);
  check_n("offsetof capnp_rpc_stream_t.have_failure", offsetof(capnp_rpc_stream_t, have_failure), 272);
  check_n("sizeof capnp_rpc_join_t", sizeof(capnp_rpc_join_t), 208);
  check_n("alignof capnp_rpc_join_t", _Alignof(capnp_rpc_join_t), 4);
  check_n("offsetof capnp_rpc_join_t.used", offsetof(capnp_rpc_join_t, used), 0);
  check_n("offsetof capnp_rpc_join_t.join_id", offsetof(capnp_rpc_join_t, join_id), 4);
  check_n("offsetof capnp_rpc_join_t.part_count", offsetof(capnp_rpc_join_t, part_count), 8);
  check_n("offsetof capnp_rpc_join_t.nseen", offsetof(capnp_rpc_join_t, nseen), 12);
  check_n("offsetof capnp_rpc_join_t.seen", offsetof(capnp_rpc_join_t, seen), 16);
  check_n("offsetof capnp_rpc_join_t.qids", offsetof(capnp_rpc_join_t, qids), 80);
  check_n("offsetof capnp_rpc_join_t.eids", offsetof(capnp_rpc_join_t, eids), 144);
  check_n("sizeof capnp_rpc_vat_t", sizeof(capnp_rpc_vat_t), 1536);
  check_n("alignof capnp_rpc_vat_t", _Alignof(capnp_rpc_vat_t), 8);
  check_n("offsetof capnp_rpc_vat_t.provisions", offsetof(capnp_rpc_vat_t, provisions), 0);
  check_n("sizeof capnp_rpc_conn_t", sizeof(capnp_rpc_conn_t), 1056624);
  check_n("alignof capnp_rpc_conn_t", _Alignof(capnp_rpc_conn_t), 8);
  check_n("offsetof capnp_rpc_conn_t.exports", offsetof(capnp_rpc_conn_t, exports), 0);
  check_n("offsetof capnp_rpc_conn_t.answers", offsetof(capnp_rpc_conn_t, answers), 1536);
  check_n("offsetof capnp_rpc_conn_t.questions", offsetof(capnp_rpc_conn_t, questions), 526848);
  check_n("offsetof capnp_rpc_conn_t.vat", offsetof(capnp_rpc_conn_t, vat), 1052672);
  check_n("offsetof capnp_rpc_conn_t.own_vat", offsetof(capnp_rpc_conn_t, own_vat), 1052680);
  check_n("offsetof capnp_rpc_conn_t.introductions", offsetof(capnp_rpc_conn_t, introductions), 1054216);
  check_n("offsetof capnp_rpc_conn_t.next_question_id", offsetof(capnp_rpc_conn_t, next_question_id), 1054920);
  check_n("offsetof capnp_rpc_conn_t.joins", offsetof(capnp_rpc_conn_t, joins), 1054924);
  check_n("offsetof capnp_rpc_conn_t.bootstrap", offsetof(capnp_rpc_conn_t, bootstrap), 1056592);
  check_n("offsetof capnp_rpc_conn_t.bootstrap_dispatch", offsetof(capnp_rpc_conn_t, bootstrap_dispatch), 1056600);
  check_n("offsetof capnp_rpc_conn_t.send", offsetof(capnp_rpc_conn_t, send), 1056608);
  check_n("offsetof capnp_rpc_conn_t.send_ctx", offsetof(capnp_rpc_conn_t, send_ctx), 1056616);
  check_n("sizeof capnp_link_t", sizeof(capnp_link_t), 1056632);
  check_n("alignof capnp_link_t", _Alignof(capnp_link_t), 8);
  check_n("offsetof capnp_link_t.conn", offsetof(capnp_link_t, conn), 0);
  check_n("offsetof capnp_link_t.fd", offsetof(capnp_link_t, fd), 1056624);
}

static void layout_32(void) {
  check_n("sizeof capnp_bseg_t (32)", sizeof(capnp_bseg_t), 12);
  check_n("alignof capnp_bseg_t (32)", _Alignof(capnp_bseg_t), 4);
  check_n("offsetof capnp_bseg_t.data (32)", offsetof(capnp_bseg_t, data), 0);
  check_n("offsetof capnp_bseg_t.words (32)", offsetof(capnp_bseg_t, words), 4);
  check_n("offsetof capnp_bseg_t.cap (32)", offsetof(capnp_bseg_t, cap), 8);
  check_n("sizeof capnp_builder_t (32)", sizeof(capnp_builder_t), 20);
  check_n("alignof capnp_builder_t (32)", _Alignof(capnp_builder_t), 4);
  check_n("offsetof capnp_builder_t.segs (32)", offsetof(capnp_builder_t, segs), 0);
  check_n("offsetof capnp_builder_t.nsegs (32)", offsetof(capnp_builder_t, nsegs), 4);
  check_n("offsetof capnp_builder_t.segs_cap (32)",
          offsetof(capnp_builder_t, segs_cap), 8);
  check_n("offsetof capnp_builder_t.max_seg_words (32)",
          offsetof(capnp_builder_t, max_seg_words), 12);
  check_n("offsetof capnp_builder_t.force_single (32)",
          offsetof(capnp_builder_t, force_single), 16);
  check_n("sizeof capnp_bptr_t (32)", sizeof(capnp_bptr_t), 12);
  check_n("alignof capnp_bptr_t (32)", _Alignof(capnp_bptr_t), 4);
  check_n("offsetof capnp_bptr_t.b (32)", offsetof(capnp_bptr_t, b), 0);
  check_n("offsetof capnp_bptr_t.seg (32)", offsetof(capnp_bptr_t, seg), 4);
  check_n("offsetof capnp_bptr_t.word (32)", offsetof(capnp_bptr_t, word), 8);
  check_n("sizeof capnp_orphan_t (32)", sizeof(capnp_orphan_t), 32);
  check_n("alignof capnp_orphan_t (32)", _Alignof(capnp_orphan_t), 4);
  check_n("offsetof capnp_orphan_t.b (32)", offsetof(capnp_orphan_t, b), 0);
  check_n("offsetof capnp_orphan_t.seg (32)", offsetof(capnp_orphan_t, seg), 4);
  check_n("offsetof capnp_orphan_t.word (32)", offsetof(capnp_orphan_t, word), 8);
  check_n("offsetof capnp_orphan_t.kind (32)", offsetof(capnp_orphan_t, kind), 12);
  check_n("offsetof capnp_orphan_t.dwords (32)",
          offsetof(capnp_orphan_t, dwords), 16);
  check_n("offsetof capnp_orphan_t.pwords (32)",
          offsetof(capnp_orphan_t, pwords), 18);
  check_n("offsetof capnp_orphan_t.esize (32)", offsetof(capnp_orphan_t, esize), 20);
  check_n("offsetof capnp_orphan_t.count (32)", offsetof(capnp_orphan_t, count), 24);
  check_n("offsetof capnp_orphan_t.live (32)", offsetof(capnp_orphan_t, live), 28);
}

/* Init must stay inside the bytes the caller reserved. */
static void test_builder_init_bounded(void) {
  capnp_builder_t *b;
  capnp_orphan_t *o;
  capnp_bptr_t body;
  b = (capnp_builder_t *)malloc(sizeof *b);
  o = (capnp_orphan_t *)malloc(sizeof *o);
  if (!b || !o) {
    fprintf(stderr, "FAIL malloc\n");
    g_fail++;
    free(b);
    free(o);
    return;
  }
  capnp_builder_init(b);
  check_n("segments after init", capnp_builder_nsegs(b), 1);
  if (capnp_builder_new_orphan_struct(b, 1, 0, o, &body) != CAPNP_OK) {
    fprintf(stderr, "FAIL new orphan\n");
    g_fail++;
  } else {
    capnp_orphan_t *copy = (capnp_orphan_t *)malloc(sizeof *copy);
    if (!copy) {
      fprintf(stderr, "FAIL malloc copy\n");
      g_fail++;
    } else {
      *copy = *o;
      if (capnp_builder_orphan_free(o) != CAPNP_OK) {
        fprintf(stderr, "FAIL free orphan\n");
        g_fail++;
      }
      if (capnp_builder_orphan_free(copy) == CAPNP_OK) {
        fprintf(stderr, "FAIL second free\n");
        g_fail++;
      }
      free(copy);
    }
  }
  capnp_builder_free(b);
  free(b);
  free(o);
}

int main(void) {
  if (sizeof(void *) == 8)
    layout_64();
  else if (sizeof(void *) == 4)
    layout_32();
  test_builder_init_bounded();
  if (g_fail) {
    fprintf(stderr, "%d abi layout checks failed\n", g_fail);
    return 1;
  }
  printf("ok abi_layout\n");
  return 0;
}
