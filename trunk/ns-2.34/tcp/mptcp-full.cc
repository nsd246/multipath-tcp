/*
 * Copyright (C) 2010 WIDE Project.  All rights reserved.
 *
 * Yoshifumi Nishida  <nishida@sfc.wide.ad.jp>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "ip.h"
#include "tcp-full.h"
#include "mptcp-full.h"
#include "flags.h"
#include "random.h"
#include "template.h"
#include "mptcp.h"

#ifndef TRUE
#define	TRUE 	1
#endif

#ifndef FALSE
#define	FALSE 	0
#endif

static class MpFullTcpClass:public TclClass
{
public:
  MpFullTcpClass ():TclClass ("Agent/TCP/FullTcp/Sack/Multipath")
  {
  }
  TclObject *create (int, const char *const *)
  {
    return (new MpFullTcpAgent ());
  }
}

class_mpfull;

void
MpFullTcpAgent::prpkt (Packet * pkt)
{
  hdr_tcp *tcph = hdr_tcp::access (pkt);        // TCP header
  hdr_cmn *th = hdr_cmn::access (pkt);  // common header (size, etc)
  //hdr_flags *fh = hdr_flags::access(pkt);       // flags (CWR, CE, bits)
  hdr_ip *iph = hdr_ip::access (pkt);
  int datalen = th->size () - tcph->hlen ();    // # payload bytes

  fprintf (stdout,
           " [%d:%d.%d>%d.%d] (hlen:%d, dlen:%d, seq:%d, ack:%d, flags:0x%x (%s), salen:%d, reason:0x%x)\n",
           th->uid (), iph->saddr (), iph->sport (), iph->daddr (),
           iph->dport (), tcph->hlen (), datalen, tcph->seqno (),
           tcph->ackno (), tcph->flags (), flagstr (tcph->flags ()),
           tcph->sa_length (), tcph->reason ());
}

/*
 * this function should be virtual so others (e.g. SACK) can override
 */
int
MpFullTcpAgent::headersize ()
{
  int total = tcpip_base_hdr_size_;
  if (total < 1) {
    fprintf (stderr,
             "%f: MpFullTcpAgent(%s): warning: tcpip hdr size is only %d bytes\n",
             now (), name (), tcpip_base_hdr_size_);
  }

  if (ts_option_)
    total += ts_option_size_;

  total += mptcp_option_size_;

  return (total);
}

/*
 * sendpacket: 
 *	allocate a packet, fill in header fields, and send
 *	also keeps stats on # of data pkts, acks, re-xmits, etc
 *
 * fill in packet fields.  Agent::allocpkt() fills
 * in most of the network layer fields for us.
 * So fill in tcp hdr and adjust the packet size.
 *
 * Also, set the size of the tcp header.
 */
void
MpFullTcpAgent::sendpacket (int seqno, int ackno, int pflags, int datalen,
                            int reason, Packet * p)
{
  if (!p)
    p = allocpkt ();
  hdr_tcp *tcph = hdr_tcp::access (p);
  hdr_flags *fh = hdr_flags::access (p);

  /* build basic header w/options */

  tcph->seqno () = seqno;
  tcph->ackno () = ackno;
  tcph->flags () = pflags;
  tcph->reason () |= reason;    // make tcph->reason look like ns1 pkt->flags?
  tcph->sa_length () = 0;       // may be increased by build_options()
  tcph->hlen () = tcpip_base_hdr_size_;
  tcph->hlen () += build_options (tcph);

  /*
   * Explicit Congestion Notification (ECN) related:
   * Bits in header:
   *      ECT (EC Capable Transport),
   *      ECNECHO (ECHO of ECN Notification generated at router),
   *      CWR (Congestion Window Reduced from RFC 2481)
   * States in TCP:
   *      ecn_: I am supposed to do ECN if my peer does
   *      ect_: I am doing ECN (ecn_ should be T and peer does ECN)
   */

  if (datalen > 0 && ecn_) {
    // set ect on data packets 
    fh->ect () = ect_;          // on after mutual agreement on ECT
  }
  else if (ecn_ && ecn_syn_ && ecn_syn_next_ && (pflags & TH_SYN)
           && (pflags & TH_ACK)) {
    // set ect on syn/ack packet, if syn packet was negotiating ECT
    fh->ect () = ect_;
  }
  else {
    /* Set ect() to 0.  -M. Weigle 1/19/05 */
    fh->ect () = 0;
  }
  if (ecn_ && ect_ && recent_ce_) {
    // This is needed here for the ACK in a SYN, SYN/ACK, ACK
    // sequence.
    pflags |= TH_ECE;
  }
  // fill in CWR and ECE bits which don't actually sit in
  // the tcp_flags but in hdr_flags
  if (pflags & TH_ECE) {
    fh->ecnecho () = 1;
  }
  else {
    fh->ecnecho () = 0;
  }
  if (pflags & TH_CWR) {
    fh->cong_action () = 1;
  }
  else {
    /* Set cong_action() to 0  -M. Weigle 1/19/05 */
    fh->cong_action () = 0;
  }

  /*
   * mptcp processing
   */
  mptcp_option_size_ = 0;
  if (pflags & TH_SYN) {
    if (!(pflags & TH_ACK)) {
      if (mptcp_is_primary ()) {
        tcph->mp_capable () = 1;
        mptcp_option_size_ += MPTCP_CAPABLEOPTION_SIZE;
      }
      else {
        tcph->mp_join () = 1;
        mptcp_option_size_ += MPTCP_JOINOPTION_SIZE;
      }
    }
    else {
      if (mpcapable_) {
        tcph->mp_capable () = 1;
        mptcp_option_size_ += MPTCP_CAPABLEOPTION_SIZE;
      }
      if (mpjoin_) {
        tcph->mp_join () = 1;
        mptcp_option_size_ += MPTCP_JOINOPTION_SIZE;
      }
    }
  }
  else {
    tcph->mp_ack () = mptcp_recv_getack (ackno);
    if (tcph->mp_ack ())
      mptcp_option_size_ += MPTCP_ACKOPTION_SIZE;
  }

  if (datalen && !mptcp_dsnmap_.empty ()) {
    vector < dsn_mapping >::iterator it;
    for (it = mptcp_dsnmap_.begin (); it != mptcp_dsnmap_.end (); ++it) {
      struct dsn_mapping *p = &*it;

      /* 
         check if this is the mapping for this packet 
         if so, attach the mapping info to the packet 
       */
      if (seqno >= p->sseqnum && seqno < p->sseqnum + p->length) {
        /* 
           if this is first transssion for this mapping or
           if this is retransmited packet, attach mapping 
         */
        if (!p->sentseq || p->sentseq == seqno) {
          tcph->mp_dsn () = p->dseqnum;
          tcph->mp_subseq () = p->sseqnum;
          tcph->mp_dsnlen () = p->length;
          p->sentseq = seqno;
          mptcp_option_size_ += MPTCP_DATAOPTION_SIZE;
          break;
        }
      }
    }
  }
  tcph->hlen () += mptcp_option_size_;

  /* actual size is data length plus header length */

  hdr_cmn *ch = hdr_cmn::access (p);
  ch->size () = datalen + tcph->hlen ();

  if (datalen <= 0)
    ++nackpack_;
  else {
    ++ndatapack_;
    ndatabytes_ += datalen;
    last_send_time_ = now ();   // time of last data
  }
  if (reason == REASON_TIMEOUT || reason == REASON_DUPACK
      || reason == REASON_SACK) {
    ++nrexmitpack_;
    nrexmitbytes_ += datalen;
  }

  last_ack_sent_ = ackno;

//if (state_ != TCPS_ESTABLISHED) {
//printf("MP %f(%s)[state:%s]: sending pkt ", now(), name(), statestr(state_));
//prpkt(p);
//}

  send (p, 0);

  return;
}

void
MpFullTcpAgent::mptcp_set_core (MptcpAgent * core)
{
  mptcp_core_ = core;
}

void
MpFullTcpAgent::mptcp_add_mapping (int dseqnum, int length)
{
  int sseqnum = (int) curseq_ + 1;
  struct dsn_mapping tmp_map (dseqnum, sseqnum, length);
  mptcp_dsnmap_.push_back (tmp_map);
}

void
MpFullTcpAgent::mptcp_recv_add_mapping (int dseqnum, int sseqnum, int length)
{
  struct dsn_mapping tmp_map (dseqnum, sseqnum, length);
  mptcp_recv_dsnmap_.push_back (tmp_map);
}

/*
 * return dsn for mptcp from subseqnum
 */
int
MpFullTcpAgent::mptcp_recv_getack (int ackno)
{
  if (ackno == 1)
    return 0;                   /* we don't receive any data */
  vector < dsn_mapping >::iterator it = mptcp_recv_dsnmap_.begin ();
  while (it != mptcp_recv_dsnmap_.end ()) {
    struct dsn_mapping *p = &*it;

    /* check if this is the mapping for this packet */
    if (ackno >= p->sseqnum && ackno <= p->sseqnum + p->length) {
      int offset = ackno - p->sseqnum;

      mptcp_core_->set_dataack (p->dseqnum, offset);
      return mptcp_core_->get_dataack ();
    }
    if (ackno > p->sseqnum + p->length) {
      it = mptcp_recv_dsnmap_.erase (it);
    }
    else
      ++it;
  }
  fprintf (stderr, "fatal. no mapping info was found. ackno %d\n", ackno);
  abort ();
}

void
MpFullTcpAgent::mptcp_remove_mapping (int seqnum)
{
  vector < dsn_mapping >::iterator it;
  for (it = mptcp_dsnmap_.begin (); it != mptcp_dsnmap_.end (); ++it) {
    struct dsn_mapping *p = &*it;

    if (seqnum > p->dseqnum + p->length) {
      it = mptcp_dsnmap_.erase (it);
      return;
    }
  }
}

/*
 * calculate byte_acked for this packet for congestion control
 */
void
MpFullTcpAgent::mptcp_set_byteacked (Packet * pkt)
{
  mptcp_byte_acked_ = 0;
  int minseq_ = sq_.minseq();
  if (minseq_ < 0) {
    // we don't have sack blocks
//    mptcp_byte_acked_ = ((mptcp_prev_sqtotal_ > 0) ? mptcp_prev_sqtotal_ : 0);
    mptcp_byte_acked_ += (int)highest_ack_ - mptcp_prev_ackno_ - mptcp_prev_sqtotal_; 
  } else {
    // we have sack blocks
    if (sq_.minseq() > mptcp_prev_sqminseq_ && mptcp_prev_sqminseq_ > 0) 
      mptcp_byte_acked_ = sq_.minseq() - mptcp_prev_sqminseq_;
    else
      mptcp_byte_acked_ = sq_.total() - mptcp_prev_sqtotal_;
  }

  mptcp_prev_sqtotal_ = (int)sq_.total();
  mptcp_prev_sqminseq_ = (int)sq_.minseq();
  mptcp_prev_ackno_ = (int)highest_ack_;

  mptcp_byte_acked_ /= maxseg_;
}

/*
 * open up the congestion window based on linked increase algorithm
 * in draft-raiciu-mptcp-congestion-01
 */
void
MpFullTcpAgent::opencwnd ()
{
  double increment;
  if (cwnd_ < ssthresh_ && mptcp_allow_slowstart_) {
    /* slow-start (exponential) */
    cwnd_ += 1;
  }
  else {
#if 1
    double alpha = mptcp_core_->get_alpha ();
    double totalcwnd = mptcp_core_->get_totalcwnd ();
    if (totalcwnd > 0.1)
      increment = min (alpha * mptcp_byte_acked_ / totalcwnd,
                       mptcp_byte_acked_ / cwnd_);
    else
#endif
      increment = increase_num_ / cwnd_;

    if ((last_cwnd_action_ == 0 || last_cwnd_action_ == CWND_ACTION_TIMEOUT)
        && max_ssthresh_ > 0) {
      increment = limited_slow_start (cwnd_, max_ssthresh_, increment);
    }
    cwnd_ += increment;
  }
  // if maxcwnd_ is set (nonzero), make it the cwnd limit
  if (maxcwnd_ && (int (cwnd_) > maxcwnd_))
    cwnd_ = maxcwnd_;

  /* reset byte_acked */
  mptcp_byte_acked_ = 0;

  return;
}
