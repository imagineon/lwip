/*
 * Copyright (c) 2001-2003 Swedish Institute of Computer Science.
 * All rights reserved. 
 * 
 * Redistribution and use in source and binary forms, with or without modification, 
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED 
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT 
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, 
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT 
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING 
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY 
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 * 
 * Author: Adam Dunkels <adam@sics.se>
 *
 */

/*-----------------------------------------------------------------------------------*/
/* tcp_output.c
 *
 * The output functions of TCP.
 *
 */
/*-----------------------------------------------------------------------------------*/

#include "lwip/debug.h"

#include "lwip/def.h"
#include "lwip/opt.h"

#include "lwip/mem.h"
#include "lwip/memp.h"
#include "lwip/sys.h"

#include "lwip/netif.h"

#include "lwip/inet.h"
#include "lwip/tcp.h"

#include "lwip/stats.h"


#define MIN(x,y) (x) < (y)? (x): (y)



/* Forward declarations.*/
static void tcp_output_segment(struct tcp_seg *seg, struct tcp_pcb *pcb);


/*-----------------------------------------------------------------------------------*/
err_t
tcp_send_ctrl(struct tcp_pcb *pcb, u8_t flags)
{
  return tcp_enqueue(pcb, NULL, 0, flags, 1, NULL, 0);

}
/*-----------------------------------------------------------------------------------*/
err_t
tcp_write(struct tcp_pcb *pcb, const void *arg, u16_t len, u8_t copy)
{
  DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_write(pcb=%p, arg=%p, len=%u, copy=%d)\n", (void *)pcb, arg, len, copy));
  if(pcb->state == SYN_SENT ||
     pcb->state == SYN_RCVD ||
     pcb->state == ESTABLISHED ||
     pcb->state == CLOSE_WAIT) {
    if(len > 0) {
      return tcp_enqueue(pcb, (void *)arg, len, 0, copy, NULL, 0);
    }
    return ERR_OK;
  } else {
    DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_write() called in invalid state\n"));
    return ERR_CONN;
  }
}
/*-----------------------------------------------------------------------------------*/
err_t
tcp_enqueue(struct tcp_pcb *pcb, void *arg, u16_t len,
	    u8_t flags, u8_t copy,
            u8_t *optdata, u8_t optlen)
{
  struct pbuf *p;
  struct tcp_seg *seg, *useg, *queue;
  u32_t left, seqno;
  u16_t seglen;
  void *ptr;
  u8_t queuelen;

  DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_enqueue(pcb=%p, arg=%p, len=%u, flags=%x, copy=%d)\n", (void *)pcb, arg, len, flags, copy));
  left = len;
  ptr = arg;
  /* fail on too much data */
  if(len > pcb->snd_buf) {
    DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_enqueue: too much data (len=%d > snd_buf=%d)\n", len, pcb->snd_buf));
    return ERR_MEM;
  }

  /* seqno will be the sequence number of the first segment enqueued
  by the call to this function. */
  seqno = pcb->snd_lbb;

    queue = NULL;
  DEBUGF(TCP_QLEN_DEBUG, ("tcp_enqueue: queuelen: %d\n", pcb->snd_queuelen));

  /* Check if the queue length exceeds the configured maximum queue
  length. If so, we return an error. */
  queuelen = pcb->snd_queuelen;
  if(queuelen >= TCP_SND_QUEUELEN) {
    DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_enqueue: too long queue %d (max %d)\n", queuelen, TCP_SND_QUEUELEN));
    goto memerr;
  }   

#ifdef LWIP_DEBUG
  if(pcb->snd_queuelen != 0) {
    LWIP_ASSERT("tcp_enqueue: valid queue length", pcb->unacked != NULL ||
    pcb->unsent != NULL);      
  }
#endif /* LWIP_DEBUG */

  seg = NULL;
  seglen = 0;

  /* First, break up the data into segments and tuck them together in
  the local "queue" variable. */
  while(queue == NULL || left > 0) {

    /* The segment length should be the MSS if the data to be enqueued
    is larger than the MSS. */
    seglen = left > pcb->mss? pcb->mss: left;

    /* Allocate memory for tcp_seg, and fill in fields. */
    seg = memp_malloc(MEMP_TCP_SEG);
    if(seg == NULL) {
      DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_enqueue: could not allocate memory for tcp_seg\n"));
      goto memerr;
    }
    seg->next = NULL;
    seg->p = NULL;

    if(queue == NULL) {
      queue = seg;
    }
    else {
      /* Attach the segment to the end of the queued segments. */
      for(useg = queue; useg->next != NULL; useg = useg->next);
      useg->next = seg;
    }

    /* If copy is set, memory should be allocated
    and data copied into pbuf, otherwise data comes from
    ROM or other static memory, and need not be copied. If
    optdata is != NULL, we have options instead of data. */
    if(optdata != NULL) {
      if((seg->p = pbuf_alloc(PBUF_TRANSPORT, optlen, PBUF_RAM)) == NULL) {
        goto memerr;
      }
      ++queuelen;
      seg->dataptr = seg->p->payload;
    }
    else if(copy) {
      if((seg->p = pbuf_alloc(PBUF_TRANSPORT, seglen, PBUF_RAM)) == NULL) {
        DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_enqueue: could not allocate memory for pbuf copy size %u\n", seglen));	  
        goto memerr;
      }
      ++queuelen;
      if(arg != NULL) {
        memcpy(seg->p->payload, ptr, seglen);
      }
      seg->dataptr = seg->p->payload;
    }
    else {
      /* Do not copy the data. */

      /* First, allocate a pbuf for holding the data. */
      if((p = pbuf_alloc(PBUF_TRANSPORT, seglen, PBUF_ROM)) == NULL) {
        DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_enqueue: could not allocate memory for pbuf non-copy\n"));	  	  
        goto memerr;
      }
      ++queuelen;
      p->payload = ptr;
      seg->dataptr = ptr;

      /* Second, allocate a pbuf for the headers. */
      if((seg->p = pbuf_alloc(PBUF_TRANSPORT, 0, PBUF_RAM)) == NULL) {
        /* If allocation fails, we have to deallocate the data pbuf as
           well. */
        pbuf_free(p);
        DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_enqueue: could not allocate memory for header pbuf\n"));		  
        goto memerr;
      }
      ++queuelen;

      /* Chain the headers and data pbufs together. */
      pbuf_chain(seg->p, p);
    }

    /* Now that there are more segments queued, we check again if the
    length of the queue exceeds the configured maximum. */
    if(queuelen > TCP_SND_QUEUELEN) {
      DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_enqueue: queue too long %d (%d)\n", queuelen, TCP_SND_QUEUELEN)); 	
      goto memerr;
    }

    seg->len = seglen;
    /*    if((flags & TCP_SYN) || (flags & TCP_FIN)) { 
    ++seg->len;
    }*/

    /* Build TCP header. */
    if(pbuf_header(seg->p, TCP_HLEN)) {

      DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_enqueue: no room for TCP header in pbuf.\n"));

#ifdef TCP_STATS
      ++lwip_stats.tcp.err;
#endif /* TCP_STATS */
      goto memerr;
    }
    seg->tcphdr = seg->p->payload;
    seg->tcphdr->src = htons(pcb->local_port);
    seg->tcphdr->dest = htons(pcb->remote_port);
    seg->tcphdr->seqno = htonl(seqno);
    seg->tcphdr->urgp = 0;
    TCPH_FLAGS_SET(seg->tcphdr, flags);
    /* don't fill in tcphdr->ackno and tcphdr->wnd until later */

    /* Copy the options into the header, if they are present. */
    if(optdata == NULL) {
      TCPH_OFFSET_SET(seg->tcphdr, 5 << 4);
    }
    else {
      TCPH_OFFSET_SET(seg->tcphdr, (5 + optlen / 4) << 4);
      /* Copy options into data portion of segment.
       Options can thus only be sent in non data carrying
       segments such as SYN|ACK. */
      memcpy(seg->dataptr, optdata, optlen);
    }
    DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_enqueue: queueing %lu:%lu (0x%x)\n",
      ntohl(seg->tcphdr->seqno),
      ntohl(seg->tcphdr->seqno) + TCP_TCPLEN(seg),
      flags));

    left -= seglen;
    seqno += seglen;
    ptr = (void *)((char *)ptr + seglen);
  }


  /* Now that the data to be enqueued has been broken up into TCP
  segments in the queue variable, we add them to the end of the
  pcb->unsent queue. */
  if(pcb->unsent == NULL) {
    useg = NULL;
  }
  else {
    for(useg = pcb->unsent; useg->next != NULL; useg = useg->next);
  }

  /* If there is room in the last pbuf on the unsent queue,
  chain the first pbuf on the queue together with that. */
  if(useg != NULL &&
    TCP_TCPLEN(useg) != 0 &&
    !(TCPH_FLAGS(useg->tcphdr) & (TCP_SYN | TCP_FIN)) &&
    !(flags & (TCP_SYN | TCP_FIN)) &&
    useg->len + queue->len <= pcb->mss) {
    /* Remove TCP header from first segment. */
    pbuf_header(queue->p, -TCP_HLEN);
    pbuf_chain(useg->p, queue->p);
    useg->len += queue->len;
    useg->next = queue->next;

    DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_enqueue: chaining, new len %u\n", useg->len));
    if(seg == queue) {
      seg = NULL;
    }
    memp_free(MEMP_TCP_SEG, queue);
  }
  else {      
    if(useg == NULL) {
      pcb->unsent = queue;

    }
    else {
      useg->next = queue;
    }
  }
  if((flags & TCP_SYN) || (flags & TCP_FIN)) {
    ++len;
  }
  pcb->snd_lbb += len;
  pcb->snd_buf -= len;
  pcb->snd_queuelen = queuelen;
  DEBUGF(TCP_QLEN_DEBUG, ("tcp_enqueue: %d (after enqueued)\n", pcb->snd_queuelen));
#ifdef LWIP_DEBUG
  if(pcb->snd_queuelen != 0) {
    LWIP_ASSERT("tcp_enqueue: valid queue length", pcb->unacked != NULL ||
      pcb->unsent != NULL);

  }
#endif /* LWIP_DEBUG */

  /* Set the PSH flag in the last segment that we enqueued, but only
  if the segment has data (indicated by seglen > 0). */
  if(seg != NULL && seglen > 0 && seg->tcphdr != NULL) {
    TCPH_FLAGS_SET(seg->tcphdr, TCPH_FLAGS(seg->tcphdr) | TCP_PSH);
  }

  return ERR_OK;
  memerr:
#ifdef TCP_STATS
  ++lwip_stats.tcp.memerr;
#endif /* TCP_STATS */

  if(queue != NULL) {
    tcp_segs_free(queue);
  }
#ifdef LWIP_DEBUG
  if(pcb->snd_queuelen != 0) {
    LWIP_ASSERT("tcp_enqueue: valid queue length", pcb->unacked != NULL ||
      pcb->unsent != NULL);

  }
#endif /* LWIP_DEBUG */
  DEBUGF(TCP_QLEN_DEBUG, ("tcp_enqueue: %d (with mem err)\n", pcb->snd_queuelen));
  return ERR_MEM;
}
/*-----------------------------------------------------------------------------------*/
/* find out what we can send and send it */
err_t
tcp_output(struct tcp_pcb *pcb)
{
  struct pbuf *p;
  struct tcp_hdr *tcphdr;
  struct tcp_seg *seg, *useg;
  u32_t wnd;
#if TCP_CWND_DEBUG
  int i = 0;
#endif /* TCP_CWND_DEBUG */

  /* First, check if we are invoked by the TCP input processing
     code. If so, we do not output anything. Instead, we rely on the
     input processing code to call us when input processing is done
     with. */
  if(tcp_input_pcb == pcb) {
    return ERR_OK;
  }
  
  wnd = MIN(pcb->snd_wnd, pcb->cwnd);

  
  seg = pcb->unsent;

  /* If the TF_ACK_NOW flag is set, we check if there is data that is
     to be sent. If data is to be sent out, we'll just piggyback our
     acknowledgement with the outgoing segment. If no data will be
     sent (either because the ->unsent queue is empty or because the
     window doesn't allow it) we'll have to construct an empty ACK
     segment and send it. */
  if(pcb->flags & TF_ACK_NOW &&
     (seg == NULL ||
      ntohl(seg->tcphdr->seqno) - pcb->lastack + seg->len > wnd)) {
    pcb->flags &= ~(TF_ACK_DELAY | TF_ACK_NOW);
    p = pbuf_alloc(PBUF_IP, TCP_HLEN, PBUF_RAM);
    if(p == NULL) {
      DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_output: (ACK) could not allocate pbuf\n"));
      return ERR_BUF;
    }
    DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_output: sending ACK for %lu\n", pcb->rcv_nxt));    
    
    tcphdr = p->payload;
    tcphdr->src = htons(pcb->local_port);
    tcphdr->dest = htons(pcb->remote_port);
    tcphdr->seqno = htonl(pcb->snd_nxt);
    tcphdr->ackno = htonl(pcb->rcv_nxt);
    TCPH_FLAGS_SET(tcphdr, TCP_ACK);
    tcphdr->wnd = htons(pcb->rcv_wnd);
    tcphdr->urgp = 0;
    TCPH_OFFSET_SET(tcphdr, 5 << 4);
    
    tcphdr->chksum = 0;
    tcphdr->chksum = inet_chksum_pseudo(p, &(pcb->local_ip), &(pcb->remote_ip),
					IP_PROTO_TCP, p->tot_len);

    ip_output(p, &(pcb->local_ip), &(pcb->remote_ip), TCP_TTL,
	      IP_PROTO_TCP);
    pbuf_free(p);

    return ERR_OK;
  } 
  
#if TCP_OUTPUT_DEBUG
  if(seg == NULL) {
    DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_output: nothing to send (%p)\n", pcb->unsent));
  }
#endif /* TCP_OUTPUT_DEBUG */
#if TCP_CWND_DEBUG
  if(seg == NULL) {
    DEBUGF(TCP_CWND_DEBUG, ("tcp_output: snd_wnd %lu, cwnd %lu, wnd %lu, seg == NULL, ack %lu\n",
                            pcb->snd_wnd, pcb->cwnd, wnd,
                            pcb->lastack));
  } else {
    DEBUGF(TCP_CWND_DEBUG, ("tcp_output: snd_wnd %lu, cwnd %lu, wnd %lu, effwnd %lu, seq %lu, ack %lu\n",
                            pcb->snd_wnd, pcb->cwnd, wnd,
                            ntohl(seg->tcphdr->seqno) - pcb->lastack + seg->len,
                            ntohl(seg->tcphdr->seqno), pcb->lastack));
  }
#endif /* TCP_CWND_DEBUG */
  
  while(seg != NULL &&
	ntohl(seg->tcphdr->seqno) - pcb->lastack + seg->len <= wnd) {
#if TCP_CWND_DEBUG
    DEBUGF(TCP_CWND_DEBUG, ("tcp_output: snd_wnd %lu, cwnd %lu, wnd %lu, effwnd %lu, seq %lu, ack %lu, i%d\n",
                            pcb->snd_wnd, pcb->cwnd, wnd,
                            ntohl(seg->tcphdr->seqno) + seg->len -
                            pcb->lastack,
                            ntohl(seg->tcphdr->seqno), pcb->lastack, i));
    ++i;
#endif /* TCP_CWND_DEBUG */

    pcb->unsent = seg->next;
    
    if(pcb->state != SYN_SENT) {
      TCPH_FLAGS_SET(seg->tcphdr, TCPH_FLAGS(seg->tcphdr) | TCP_ACK);
      pcb->flags &= ~(TF_ACK_DELAY | TF_ACK_NOW);
    }
    
    tcp_output_segment(seg, pcb);
    pcb->snd_nxt = ntohl(seg->tcphdr->seqno) + TCP_TCPLEN(seg);
    if(TCP_SEQ_LT(pcb->snd_max, pcb->snd_nxt)) {
      pcb->snd_max = pcb->snd_nxt;
    }
    /* put segment on unacknowledged list if length > 0 */
    if(TCP_TCPLEN(seg) > 0) {
      seg->next = NULL;
      if(pcb->unacked == NULL) {
        pcb->unacked = seg;
	
	
      } else {
        for(useg = pcb->unacked; useg->next != NULL; useg = useg->next);
        useg->next = seg;
      }
    } else {
      tcp_seg_free(seg);
    }
    seg = pcb->unsent;
  }  
  return ERR_OK;
}
/*-----------------------------------------------------------------------------------*/
static void
tcp_output_segment(struct tcp_seg *seg, struct tcp_pcb *pcb)
{
  u16_t len;
  struct netif *netif;

  /* The TCP header has already been constructed, but the ackno and
   wnd fields remain. */
  seg->tcphdr->ackno = htonl(pcb->rcv_nxt);

  /* silly window avoidance */
  if(pcb->rcv_wnd < pcb->mss) {
    seg->tcphdr->wnd = 0;
  } else {
    /* advertise our receive window size in this TCP segment */
    seg->tcphdr->wnd = htons(pcb->rcv_wnd);
  }

  /* If we don't have a local IP address, we get one by
     calling ip_route(). */
  if(ip_addr_isany(&(pcb->local_ip))) {
    netif = ip_route(&(pcb->remote_ip));
    if(netif == NULL) {
      return;
    }
    ip_addr_set(&(pcb->local_ip), &(netif->ip_addr));
  }

  pcb->rtime = 0;
  
  if(pcb->rttest == 0) {
    pcb->rttest = tcp_ticks;
    pcb->rtseq = ntohl(seg->tcphdr->seqno);

    DEBUGF(TCP_RTO_DEBUG, ("tcp_output_segment: rtseq %lu\n", pcb->rtseq));
  }
  DEBUGF(TCP_OUTPUT_DEBUG, ("tcp_output_segment: %lu:%lu\n",
			    htonl(seg->tcphdr->seqno), htonl(seg->tcphdr->seqno) +
			    seg->len));

  len = (u16_t)((u8_t *)seg->tcphdr - (u8_t *)seg->p->payload);
  
  seg->p->len -= len;
  seg->p->tot_len -= len;
  
  seg->p->payload = seg->tcphdr;
    
  seg->tcphdr->chksum = 0;
  seg->tcphdr->chksum = inet_chksum_pseudo(seg->p,
					   &(pcb->local_ip),
					   &(pcb->remote_ip),
					   IP_PROTO_TCP, seg->p->tot_len);
#ifdef TCP_STATS
  ++lwip_stats.tcp.xmit;
#endif /* TCP_STATS */

  ip_output(seg->p, &(pcb->local_ip), &(pcb->remote_ip), TCP_TTL,
	    IP_PROTO_TCP);
}
/*-----------------------------------------------------------------------------------*/
void
tcp_rst(u32_t seqno, u32_t ackno,
	struct ip_addr *local_ip, struct ip_addr *remote_ip,
	u16_t local_port, u16_t remote_port)
{
  struct pbuf *p;
  struct tcp_hdr *tcphdr;
  p = pbuf_alloc(PBUF_IP, TCP_HLEN, PBUF_RAM);
  if(p == NULL) {
      DEBUGF(TCP_DEBUG, ("tcp_rst: could not allocate memory for pbuf\n"));
      return;
  }

  tcphdr = p->payload;
  tcphdr->src = htons(local_port);
  tcphdr->dest = htons(remote_port);
  tcphdr->seqno = htonl(seqno);
  tcphdr->ackno = htonl(ackno);
  TCPH_FLAGS_SET(tcphdr, TCP_RST | TCP_ACK);
  tcphdr->wnd = htons(TCP_WND);
  tcphdr->urgp = 0;
  TCPH_OFFSET_SET(tcphdr, 5 << 4);
  
  tcphdr->chksum = 0;
  tcphdr->chksum = inet_chksum_pseudo(p, local_ip, remote_ip,
				      IP_PROTO_TCP, p->tot_len);

#ifdef TCP_STATS
  ++lwip_stats.tcp.xmit;
#endif /* TCP_STATS */
  ip_output(p, local_ip, remote_ip, TCP_TTL, IP_PROTO_TCP);
  pbuf_free(p);
  DEBUGF(TCP_RST_DEBUG, ("tcp_rst: seqno %lu ackno %lu.\n", seqno, ackno));
}
/*-----------------------------------------------------------------------------------*/
void
tcp_rexmit(struct tcp_pcb *pcb)
{
  struct tcp_seg *seg;

  if(pcb->unacked == NULL) {
    return;
  }
  
  /* Move all unacked segments to the unsent queue. */
  for(seg = pcb->unacked; seg->next != NULL; seg = seg->next);
  
  seg->next = pcb->unsent;
  pcb->unsent = pcb->unacked;
  
  pcb->unacked = NULL;
  
  
  pcb->snd_nxt = ntohl(pcb->unsent->tcphdr->seqno);
  
  ++pcb->nrtx;
  
  /* Don't take any rtt measurements after retransmitting. */    
  pcb->rttest = 0;
  
  /* Do the actual retransmission. */
  tcp_output(pcb);

}










