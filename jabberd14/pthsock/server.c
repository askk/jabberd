/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 *  Jabber
 *  Copyright (C) 1998-2000 The Jabber Team http://jabber.org/
 */

/*
    <!-- For use without an external DNS component -->
  <service id="127.0.0.1 s2s">
    <host/>
    <load main="pthsock_server">
      <pthsock_server>../load/pthsock_server.so</pthsock_server>
    </load>
  </service>

  <!-- for use with an external DNS component -->
  <service id="127.0.0.1 s2s">
    <host>pthsock-s2s.127.0.0.1</host> <!-- add this host to DNS config section -->
    <load main="pthsock_server">
      <pthsock_server>../load/pthsock_server.so</pthsock_server>
    </load>
  </service>
*/

#include "io.h"

typedef enum { conn_CONNECTING, conn_IN, conn_OUT, conn_CLOSED } conn_type;
struct sdata_st;
iosi io__instance=NULL;

/* server 2 server instance */
typedef struct ssi_st
{
    instance i;
    HASHTABLE out_tab;
} *ssi, _ssi;

typedef struct sdata_st
{
    ssi i;
    pool p;
    conn_type type;
    pth_msgport_t queue;
    char *to;
    void *arg;
} *sdata, _sdata;

/* recieve data from io_select */
void pthsock_server_in(int type, xmlnode x, void *arg)
{
    sock c=(sock)arg;
    sdata sd = (sdata)c->arg;
    xmlnode h;
    jid j;
    char *block, *to;

    switch(type)
    {
    case XSTREAM_ROOT:
        if(sd->type==conn_IN)
        {
            if(xmlnode_get_attrib(x,"xmlns:etherx")==NULL&&
               xmlnode_get_attrib(x,"etherx:secret")==NULL)
            {
                to=xmlnode_get_attrib(x,"to");
                if(sd->to==NULL)
                    sd->to=pstrdup(c->p,to);
                if(to==NULL)
                {
                    io_write_str(c,"<stream::error>You didn't send your to='host' attribute.</stream:error>");
                    io_close(c);
                    sd->type = conn_CLOSED;
                }
                else
                {
                    h=xstream_header("jabber:server",NULL,to);
                    block = xstream_header_char(h);
                    io_write_str(c,block);
                    xmlnode_free(h);
                }
            }
            else
            {
                io_write_str(c,"<stream::error>Transport Access is Denied</stream:error>");
                io_close((sock)sd->arg);
                sd->type = conn_CLOSED;   /* it wants to be a transport, to bad */
            }
        }
        else
        { /* we finnally connected, dump the queue */
            wbq q;
            while((q=(wbq)pth_msgport_get(sd->queue))!=NULL)
                io_write(c,xmlnode_get_firstchild(q->x));
        }

        xmlnode_free(x);
        break;
    case XSTREAM_NODE:
        if(sd->type==conn_OUT)
        {
            log_debug(ZONE,"Outgoing connection tried to receive data!");
            io_write_str(c,"<stream:error>This connection does not accept incoming data</stream:error>");
            xmlnode_free(x);
            break;
        }
        log_debug(ZONE,"node received for %d",c->fd);

        xmlnode_hide_attrib(x,"etherx:from");
        xmlnode_hide_attrib(x,"etherx:to");
        xmlnode_hide_attrib(x,"sto");
        xmlnode_hide_attrib(x,"sfrom");
        xmlnode_hide_attrib(x,"ip");

        /* make sure we don't get packets on an incoming connection */
        /* that are destined for a connection we have established */
        /* as outgoing.. this is to fix a looping issue */
        j=jid_new(xmlnode_pool(x),xmlnode_get_attrib(x,"to"));
        if(j!=NULL)c=ghash_get(sd->i->out_tab,j->server);
        if(c==NULL)
        {
            xmlnode r=xmlnode_wrap(x,"route");
            xmlnode_put_attrib(r,"to",xmlnode_get_attrib(x,"to"));
            xmlnode_put_attrib(r,"from",xmlnode_get_attrib(x,"from"));
            deliver(dpacket_new(r),sd->i->i);
        }
        else
        {
            xmlnode r=xmlnode_wrap(x,"route");
            jutil_error(x,TERROR_EXTERNAL);
            xmlnode_put_attrib(r,"to",xmlnode_get_attrib(x,"to"));
            xmlnode_put_attrib(r,"from",xmlnode_get_attrib(x,"from"));
            deliver(dpacket_new(r),sd->i->i);
        }
        break;
    case XSTREAM_ERR:
        log_debug(ZONE,"failed to parse XML for %d",c->fd);
        io_write_str(c,"<stream::error>You sent malformed XML</stream:error>");
    case XSTREAM_CLOSE:
        /* they closed there connections to us */
        log_debug(ZONE,"closing XML stream for %d",c->fd);
        io_close(c);
        xmlnode_free(x);
    }
}


void pthsock_server_read(sock c,char *buffer,int bufsz,int flags,void *arg)
{
    ssi si=(ssi)arg;
    sdata sd;
    wbq q;
    int ret;

    switch(flags)
    {
    case IO_INIT:
        log_debug(ZONE,"io_select INIT event");
        break;
    case IO_NEW:
        log_debug(ZONE,"io_select NEW socket connected at %d",c->fd);
        sd=(sdata)c->arg;
        if(sd==NULL)
        { /* if this is an incoming connection, there is no sdata */
            sd = pmalloco(c->p, sizeof(_sdata));
            sd->type=conn_IN;
            sd->arg=(void*)c;
            c->arg=(void*)sd;
            sd->i = si;
        }
        else 
        { /* we already have an sdata for outgoing conns */
            /* once we made the connection, send the header */
            xmlnode x=xstream_header("jabber:server",sd->to,NULL);
            register_instance(si->i,sd->to);
            log_debug(ZONE,"\n\n\n%s\n\n\n",xstream_header_char(x));
            sd->arg=(void*)c;
            io_write_str(c,xstream_header_char(x));
            xmlnode_free(x);
            sd->type=conn_OUT;
        }
        c->xs = xstream_new(c->p,(void*)pthsock_server_in,(void*)c);
        break;
    case IO_NORMAL:
        log_debug(ZONE,"io_select NORMAL data");
        ret=xstream_eat(c->xs,buffer,bufsz);
        break;
    case IO_CLOSED:
        sd=(sdata)c->arg;
        if (sd->type == conn_OUT)
        {
            ghash_remove(si->out_tab,sd->to);
            unregister_instance(si->i,sd->to);
        }
        /* if this is outgoing connection, we will have a pool to free */
        if(sd->p!=NULL)pool_free(sd->p);
        break;
    case IO_ERROR:
        /* bounce the write queue */
        /* check the sock queue */
        sd=(sdata)c->arg;
        log_debug(ZONE,"Socket Error to host %s, bouncing queue",sd->to);
        if(c->xbuffer!=NULL)
        {
            if(((int)c->xbuffer)!=-1)
            {
                xmlnode r=xmlnode_wrap(c->xbuffer,"route");
                jutil_error(c->xbuffer,TERROR_EXTERNAL);
                xmlnode_put_attrib(r,"to",xmlnode_get_attrib(c->xbuffer,"to"));
                xmlnode_put_attrib(r,"from",xmlnode_get_attrib(c->xbuffer,"from"));
                deliver(dpacket_new(c->xbuffer),si->i);
            }
            else pool_free(c->pbuffer);
            c->xbuffer=NULL;
            c->wbuffer=c->cbuffer=NULL;
            c->pbuffer=NULL;
            while((q=(wbq)pth_msgport_get(c->queue))!=NULL)
            {
                if(q->type==queue_XMLNODE)
                {
                    xmlnode r=xmlnode_wrap(q->x,"route");
                    jutil_error(q->x,TERROR_EXTERNAL);
                    xmlnode_put_attrib(r,"to",xmlnode_get_attrib(q->x,"to"));
                    xmlnode_put_attrib(r,"from",xmlnode_get_attrib(q->x,"from"));
                    deliver(dpacket_new(q->x),si->i);
                } else pool_free(q->p);
            }
        }
        /* as well as our queue */
        while((q=(wbq)pth_msgport_get(sd->queue))!=NULL)
        {
            xmlnode r=xmlnode_wrap(q->x,"route");
            jutil_error(q->x,TERROR_EXTERNAL);
            xmlnode_put_attrib(r,"to",xmlnode_get_attrib(q->x,"to"));
            xmlnode_put_attrib(r,"from",xmlnode_get_attrib(q->x,"from"));
            deliver(dpacket_new(q->x),si->i);
        }
    }
}

result pthsock_server_packets(instance id, dpacket dp, void *arg)
{
    ssi si = (ssi) arg;
    pool p;
    sdata sd;
    char *ip;
    int port;
    wbq q;
    jid from,to;

    to=jid_new(xmlnode_pool(dp->x),xmlnode_get_attrib(dp->x,"to"));
    from=jid_new(xmlnode_pool(dp->x),xmlnode_get_attrib(dp->x,"from"));

    ip=xmlnode_get_attrib(dp->x,"ip"); /* look for ip="12.34.56.78:5269" header */ 
    if(ip!=NULL)
    {
        char *colon=strchr(ip,':');
        if(colon==NULL) 
            port=5269;
        else
        {
            colon[0]='\0';
            colon++;
            port=atoi(colon);
        }
    }
    else
    {
        ip=to->server;
        port=5269;
    }


    log_debug(ZONE,"pthsock_server looking up %s",ip);

    if (from)
        xmlnode_put_attrib(dp->x,"etherx:from",from->server);
    xmlnode_put_attrib(dp->x,"etherx:to",to->server);

    sd = ghash_get(si->out_tab,to->server);

    if (sd != NULL)
        if (sd->type == conn_CLOSED)
            sd = NULL;

    q=pmalloco(dp->p,sizeof(_wbq));
    q->x=dp->x;

    if (sd == NULL)
    {
        log_debug(ZONE,"Creating new connection to %s",to->server);

        p = pool_new();
        sd = pmalloco(p,sizeof(_sdata));
        sd->p=p;
        sd->type = conn_CONNECTING;
        sd->to = pstrdup(p,to->server);
        sd->i = si;
        sd->queue = pth_msgport_create("queue");
        ghash_put(si->out_tab,sd->to,sd);

        io_select_connect(ip,port,(void*)sd,pthsock_server_read,(void*)si);
    }

    xmlnode_hide_attrib(q->x,"sto");
    xmlnode_hide_attrib(q->x,"sfrom");
    xmlnode_hide_attrib(q->x,"ip");

    if(sd->type==conn_CONNECTING)
        pth_msgport_put(sd->queue,(void*)q);
    else
        io_write((sock)sd->arg,xmlnode_get_firstchild(dp->x));

    return r_DONE;
}


/* everything starts here */
void pthsock_server(instance i, xmlnode x)
{
    ssi si;

    log_debug(ZONE,"pthsock_server loading");


    si = pmalloco(i->p,sizeof(_ssi));
    si->out_tab = ghash_create(20,(KEYHASHFUNC)str_hash_code,(KEYCOMPAREFUNC)j_strcmp);
    si->i=i;

    io_select_listen(5269,NULL,pthsock_server_read,(void*)si);
    register_phandler(i,o_DELIVER,pthsock_server_packets,(void*)si);
}
