#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <new>
#include <rvcache/cache.h>
#include <rvcache/rv_cache.h>
#include <raimd/md_msg.h>
#include <raimd/rv_msg.h>
#include <raimd/sass.h>
#include <raikv/ev_publish.h>
#include <natsmd/ev_nats_client.h>
#include <natsmd/submgr.h>

using namespace rai;
using namespace kv;
using namespace md;
using namespace natsmd;
using namespace rvcache;

/* NATS nets (SPEC Milestone 6): the sass2 subject schema carried over a
 * NATS server through natsmd's EvNatsClient.
 *   feed: SUB _TIC.> (or _TIC.<wild>.>) -> handle_tic(), as an rv feed
 *   sub:  NatsSubscriptionDB -- interest from the clients' session
 *         advisories (_NATS.LISTEN.START/STOP.<session>, resynced with
 *         _INBOX.<session>.SUBS), _SNAP.<subject> served from the cache,
 *         _TIC.REPLY.SASS.DATA.DICTIONARY answered with the cfile dict.
 * Ticks reach a nats sub net through publish_mask() -> publish_msg(),
 * which picks nats_conns[ net ] when the net has no rv client. */

/* ------------------------------------------------------------------ */
/* feed side */

void
RvCache::on_nats_feed_msg( EvPublish &pub ) noexcept
{
  /* natsmd delivers every message as MD_STRING, the bytes say what the
   * codec is (RVMSG / TIBMSG / TIB_SASS ...) */
  if ( pub.msg_len > 0 ) {
    MDMsgMem mem;
    MDMsg  * m = MDMsg::unpack( (void *) pub.msg, 0, pub.msg_len, 0, NULL,
                                mem );
    if ( m == NULL )
      return;
    pub.msg_enc = m->get_type_id();
  }
  this->on_feed_msg( pub );
}

NatsFeedCB::NatsFeedCB( EvPoll &p,  EvNatsClient &c,  RvCache &rc,
                        uint32_t idx,  const char *wild ) noexcept
          : poll( p ), client( c ), cache( rc ), wildcard( wild )
{
  ::snprintf( this->label, sizeof( this->label ), "feed(net%u,nats)", idx );
}

void
NatsFeedCB::on_connect( EvSocket &conn ) noexcept
{
  int    len  = (int) conn.get_peer_address_strlen();
  char   sub[ 1024 ];
  int    slen;
  size_t wlen = ( this->wildcard == NULL ? 0 : ::strlen( this->wildcard ) );

  printf( "%s connected: %.*s\n", this->label, len, conn.peer_address.buf );
  if ( this->wildcard == NULL )
    slen = ::snprintf( sub, sizeof( sub ), "_TIC.>" );
  else {
    bool has_gt = ( wlen > 0 && this->wildcard[ wlen - 1 ] == '>' );
    slen = ::snprintf( sub, sizeof( sub ), "_TIC.%s%s", this->wildcard,
                       has_gt ? "" : ".>" );
  }
  printf( "%s subscribe: %s\n", this->label, sub );
  this->client.subscribe( sub, slen, NULL, 0 );
  this->client.idle_push_write();
  fflush( stdout );
}

void
NatsFeedCB::on_shutdown( EvSocket &conn,  const char *err,
                         size_t errlen ) noexcept
{
  int len = (int) conn.get_peer_address_strlen();
  printf( "%s shutdown: %.*s %.*s\n", this->label, len,
          conn.peer_address.buf, (int) errlen, err );
  if ( this->poll.quit == 0 )
    this->poll.quit = 1;
}

bool
NatsFeedCB::on_nats_msg( EvPublish &pub ) noexcept
{
  this->cache.on_nats_feed_msg( pub );
  return true;
}

/* ------------------------------------------------------------------ */
/* sub side */

NatsSubCB::NatsSubCB( EvPoll &p,  EvNatsClient &c,  RvCache &rc,
                      uint32_t idx,  const char *wild ) noexcept
         : poll( p ), client( c ), cache( rc ), sub_db( c, this ),
           net( idx - 1 ), wildcard( wild )
{
  ::snprintf( this->label, sizeof( this->label ), "sub(net%u,nats)", idx );
}

void
NatsSubCB::on_connect( EvSocket &conn ) noexcept
{
  int len = (int) conn.get_peer_address_strlen();
  printf( "%s connected: %.*s\n", this->label, len, conn.peer_address.buf );
  fflush( stdout );
  if ( this->wildcard != NULL )
    this->sub_db.add_wildcard( this->wildcard );
  /* advisories always, _SNAP.> and _TIC.REPLY.> (dictionary) served */
  this->sub_db.start_subscriptions( this->wildcard == NULL, true, true );
  this->poll.timer.add_timer_millis( *this, 1000, 1, 0 );
}

void
NatsSubCB::on_shutdown( EvSocket &conn,  const char *err,
                        size_t errlen ) noexcept
{
  int len = (int) conn.get_peer_address_strlen();
  printf( "%s shutdown: %.*s %.*s\n", this->label, len,
          conn.peer_address.buf, (int) errlen, err );
  if ( this->poll.quit == 0 )
    this->poll.quit = 1;
}

bool
NatsSubCB::on_nats_msg( EvPublish &pub ) noexcept
{
  this->sub_db.process_pub( pub );
  return true;
}

bool
NatsSubCB::timer_cb( uint64_t,  uint64_t ) noexcept
{
  this->sub_db.process_events();
  return true;
}

void NatsSubCB::on_listen_start( Start &add ) noexcept {
  this->cache.on_nats_listen_start( add, this->net ); }
void NatsSubCB::on_listen_stop( Stop &rem ) noexcept {
  this->cache.on_nats_listen_stop( rem, this->net ); }
void NatsSubCB::on_snapshot( Snap &snp ) noexcept {
  this->cache.on_nats_snapshot( snp, this->net ); }
void NatsSubCB::on_tic_reply( Tic &tic ) noexcept {
  this->cache.on_nats_tic_reply( tic, this->net ); }

/* a nats session subscribed (LISTEN.START, or found in its SUBS reply):
 * the sass2 on_listen_start() semantics.  natsmd's LISTEN.START of a
 * subject carries the reply _INBOX.<session>.I.<subject>; the INITIAL goes
 * there and the client delivers it on <subject> to that subscriber only.
 * The INITIAL is broadcast on the subject only when the interest was
 * (re)discovered from a SUBS reply -- the session resynced after a
 * reconnect / digest mismatch, or rv_cache started after the listener --
 * and the subject just went live on this net (refcnt 1). */
void
RvCache::on_nats_listen_start( NatsSubscriptionListener::Start &add,
                               uint32_t net ) noexcept
{
  const char * subj = add.sub.value;
  size_t       len  = add.sub.len;
  if ( len == 0 || subj[ 0 ] == '_' )
    return;
  if ( add.sub.refcnt > 0 )
    this->interest_edge_set( subj, len, net );
  if ( add.sub.refcnt == 1 ) {
    this->stats.subscription_starts++;
    this->stats.subscriptions_active++;
  }
  this->acct_event( "subscribe", subj, len, NULL, "nats", 0, NULL, 0, 0, 0,
                    NULL, &add.session );
  /* network disruption / late start: no inbox on the SUBS reply path */
  if ( ! add.is_listen_start && add.sub.refcnt == 1 )
    this->broadcast_initial( net, subj, len, NULL, NULL, "nats",
                             &add.session );

  /* initial-on-listen: the LISTEN.START inbox is the request */
  if ( add.reply_len > 0 ) {
    CacheEntry * e = this->find_for_image( subj, len );
    void   * img;
    size_t   img_len;
    uint32_t img_enc;
    if ( e != NULL && this->cache.get_image( *e, img, img_len, img_enc ) ) {
      this->stamp_msg_type( img, img_len, img_enc,
                            (uint16_t) MD_INITIAL_TYPE );
      this->publish_msg( net, add.reply, add.reply_len, NULL, 0, img,
                         img_len, img_enc );
      e->snap_count++;
      this->acct_event( "initial", subj, len, NULL, "nats", 0,
                        NULL, 0, 0, 0, NULL, &add.session );
      this->stats.initials_sent++;
    }
    else {
      /* miss -> status to the inbox, never silence */
      this->serve_miss( net, subj, len, add.reply, add.reply_len );
      this->stats.initials_not_found++;
    }
  }
}

void
RvCache::on_nats_listen_stop( NatsSubscriptionListener::Stop &rem,
                              uint32_t net ) noexcept
{
  const char * subj = rem.sub.value;
  size_t       len  = rem.sub.len;
  if ( len == 0 || subj[ 0 ] == '_' )
    return;
  if ( rem.is_orphan )
    return;
  /* advisory stop vs session stop / timeout / resync sweep */
  const char * reason = rem.is_listen_stop ? "listen_stop" : "session_stop";
  uint32_t     now    = this->cur_mono();
  double open_secs = ( now >= rem.sub.start_mono ) ?
                     (double) ( now - rem.sub.start_mono ) : 0.0;
  CacheEntry * e = this->cache.find( subj, len );
  this->acct_event( "unsubscribe", subj, len, NULL, "nats", 0, reason,
                    open_secs, e != NULL ? e->forward_count : 0,
                    e != NULL ? e->snap_count : 0, NULL, &rem.session );
  if ( rem.sub.refcnt == 0 ) {
    this->interest_edge_clear( subj, len, net );
    this->stats.subscription_stops++;
    this->stats.subscriptions_active--;
    this->emit_nosubscribers( net, subj, len );
  }
}

void
RvCache::on_nats_snapshot( NatsSubscriptionListener::Snap &snp,
                           uint32_t net ) noexcept
{
  this->serve_snapshot( net, snp.sub.value, snp.sub.len, snp.reply,
                        snp.reply_len, NULL, snp.flags, NULL, snp.session );
}

void
RvCache::on_nats_tic_reply( NatsSubscriptionListener::Tic &tic,
                            uint32_t net ) noexcept
{
  this->serve_dictionary( net, tic.sub.value, tic.sub.len, tic.reply,
                          tic.reply_len );
}
