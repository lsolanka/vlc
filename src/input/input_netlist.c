/*****************************************************************************
 * dvd_netlist.c: netlist management v2
 *****************************************************************************
 * There is only one major change from input_netlist.c (1) : data is now a
 * pointer to an offset in iovec ; and iovec has a reference counter. It
 * will only be given back to netlist when refcount is zero.
 *****************************************************************************
 * Copyright (C) 1998-2001 VideoLAN
 * $Id: input_netlist.c,v 1.45 2001/11/28 15:08:06 massiot Exp $
 *
 * Authors: Henri Fallon <henri@videolan.org>
 *          St�phane Borel <stef@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include "defs.h"

#include <stdlib.h>
#include <string.h>                                    /* memcpy(), memset() */
#include <sys/types.h>

#ifdef HAVE_UNISTD_H
#   include <unistd.h>
#endif

#if defined( WIN32 )
#   include <io.h>                                                 /* read() */
#else
#   include <sys/uio.h>                                      /* struct iovec */
#endif

#include "config.h"
#include "common.h"
#include "intf_msg.h"                                           /* intf_*Msg */
#include "threads.h"                                                /* mutex */
#include "mtime.h"

#if defined( WIN32 )
#   include "input_iovec.h"
#endif

#include "stream_control.h"
#include "input_ext-intf.h"
#include "input_ext-dec.h"
#include "input_ext-plugins.h"

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/

/*****************************************************************************
 * input_NetlistInit: allocates netlist buffers and init indexes
 * ---
 * Changes from input_NetList: we have to give the length of the buffer which
 * is different from i_nb_data now, since we may have several data pointers
 * in one iovec. Thus we can only delete an iovec when its refcount is 0.
 * We only received a buffer with a GetIovec whereas NewPacket gives a pointer.
 *****************************************************************************/
int input_NetlistInit( input_thread_t * p_input,
                       int i_nb_iovec, int i_nb_data, int i_nb_pes,
                       size_t i_buffer_size, int i_read_once )
{
    unsigned int        i_loop;
    netlist_t *         p_netlist;

    /* First we allocate and initialise our netlist struct */
    p_input->p_method_data = malloc(sizeof(netlist_t));
    if ( p_input->p_method_data == NULL )
    {
        intf_ErrMsg("Unable to malloc the netlist struct");
        return (-1);
    }

    p_netlist = (netlist_t *) p_input->p_method_data;
                
    /* Nb of packets read once by input */
    p_netlist->i_read_once = i_read_once;
    
    /* In order to optimize netlist, we are taking i_nb_data a 2^i 
     * so that modulo is an "&".
     * This is not changing i_nb data outside this function except in 
     * the netlist_t struct */ 
    /* As i_loop is unsigned int, and i_ns_data int, this shouldn't be a 
     * problem */
    for( i_loop = 1; i_loop < i_nb_data; i_loop *= 2 )
    {
        ;
    }

    intf_DbgMsg( "Netlist : Required %i byte, got %u",i_nb_data,i_loop );
    i_nb_data = i_loop;

    /* Same thing for i_nb_pes */
    for( i_loop = 1; i_loop < i_nb_pes; i_loop *= 2 )
    {
        ;
    }

    intf_DbgMsg( "Netlist : Required %i byte, got %u",i_nb_pes,i_loop );
    i_nb_pes = i_loop;

     /* Same thing for i_nb_iovec */
    for( i_loop = 1; i_loop < i_nb_iovec; i_loop *= 2 )
    {
        ;
    }

    intf_DbgMsg( "Netlist : Required %i byte, got %u",i_nb_iovec,i_loop );
    i_nb_iovec = i_loop;
   
    /* allocate the buffers */ 
    p_netlist->p_buffers = malloc( i_nb_iovec *i_buffer_size );
    if ( p_netlist->p_buffers == NULL )
    {
        intf_ErrMsg ("Unable to malloc in netlist initialization (1)");
        free( p_netlist );
        return -1;
    }
    
    /* table of pointers to data packets */
    p_netlist->p_data = malloc( i_nb_data *sizeof(data_packet_t) );
    if ( p_netlist->p_data == NULL )
    {
        intf_ErrMsg ("Unable to malloc in netlist initialization (2)");
        free( p_netlist->p_buffers );
        free( p_netlist );
        return -1;
    }
    
    /* table of pointer to PES packets */
    p_netlist->p_pes = malloc( i_nb_pes *sizeof(pes_packet_t) );
    if ( p_netlist->p_pes == NULL )
    {
        intf_ErrMsg ("Unable to malloc in netlist initialization (3)");
        free( p_netlist->p_buffers );
        free( p_netlist->p_data );
        free( p_netlist );
        return -1;
    }
    
    /* allocate the FIFOs : tables of free pointers */
    p_netlist->pp_free_data = 
                        malloc( i_nb_data *sizeof(data_packet_t *) );
    if ( p_netlist->pp_free_data == NULL )
    {
        intf_ErrMsg ("Unable to malloc in netlist initialization (4)");
        free( p_netlist->p_buffers );
        free( p_netlist->p_data );
        free( p_netlist->p_pes );
        free( p_netlist );
        return -1;
    }
    p_netlist->pp_free_pes = 
                        malloc( i_nb_pes *sizeof(pes_packet_t *) );
    if ( p_netlist->pp_free_pes == NULL )
    {
        intf_ErrMsg ("Unable to malloc in netlist initialization (5)");
        free( p_netlist->p_buffers );
        free( p_netlist->p_data );
        free( p_netlist->p_pes );
        free( p_netlist->pp_free_data );
        free( p_netlist );
        return -1;
    }
    
    p_netlist->p_free_iovec =
        malloc( (i_nb_iovec + p_netlist->i_read_once) * sizeof(struct iovec) );
    if ( p_netlist->p_free_iovec == NULL )
    {
        intf_ErrMsg ("Unable to malloc in DVD netlist initialization (6)");
        free( p_netlist->p_buffers );
        free( p_netlist->p_data );
        free( p_netlist->p_pes );
        free( p_netlist->pp_free_data );
        free( p_netlist->pp_free_pes );
        free( p_netlist );
        return -1;
    }

    /* table for reference counter of iovecs */
    p_netlist->pi_refcount = malloc( i_nb_iovec *sizeof(int) );
    if ( p_netlist->pi_refcount == NULL )
    {
        intf_ErrMsg ("Unable to malloc in DVD netlist initialization (7)");
        free( p_netlist->p_buffers );
        free( p_netlist->p_data );
        free( p_netlist->p_pes );
        free( p_netlist->pp_free_data );
        free( p_netlist->pp_free_pes );
        free( p_netlist->p_free_iovec );
        free( p_netlist );
        return -1;
    }

    /* Fill the data FIFO */
    for ( i_loop = 0; i_loop < i_nb_data; i_loop++ )
    {
        p_netlist->pp_free_data[i_loop] = 
            p_netlist->p_data + i_loop;

        /* by default, one data packet for one buffer */
        if( i_nb_data == i_nb_iovec )
        {
            p_netlist->pp_free_data[i_loop]->p_buffer =
                p_netlist->p_buffers + i_loop * i_buffer_size;

            p_netlist->pp_free_data[i_loop]->p_payload_start =
                p_netlist->pp_free_data[i_loop]->p_buffer;

            p_netlist->pp_free_data[i_loop]->p_payload_end =
                p_netlist->pp_free_data[i_loop]->p_buffer + i_buffer_size;
        }
    }

    /* Fill the PES FIFO */
    for ( i_loop = 0; i_loop < i_nb_pes ; i_loop++ )
    {
        p_netlist->pp_free_pes[i_loop] = 
            p_netlist->p_pes + i_loop;
    }
   
    /* Deal with the iovec */
    for ( i_loop = 0; i_loop < i_nb_iovec; i_loop++ )
    {
        p_netlist->p_free_iovec[i_loop].iov_base = 
            p_netlist->p_buffers + i_loop * i_buffer_size;
   
        p_netlist->p_free_iovec[i_loop].iov_len = i_buffer_size;
    }

    /* initialize reference counters */
    memset( p_netlist->pi_refcount, 0, i_nb_iovec *sizeof(int) );
   
    /* vlc_mutex_init */
    vlc_mutex_init (&p_netlist->lock);
    
    /* initialize indexes */
    p_netlist->i_iovec_start = 0;
    p_netlist->i_iovec_end = i_nb_iovec - 1;

    p_netlist->i_data_start = 0;
    p_netlist->i_data_end = i_nb_data - 1;

    p_netlist->i_pes_start = 0;
    p_netlist->i_pes_end = i_nb_pes - 1;

    /* we give (nb - 1) to use & instead of %
     * if you really need nb you have to add 1 */
    p_netlist->i_nb_iovec = i_nb_iovec - 1;
    p_netlist->i_nb_data = i_nb_data - 1;
    p_netlist->i_nb_pes = i_nb_pes - 1;
    p_netlist->i_buffer_size = i_buffer_size;

    return 0; /* Everything went all right */
}

/*****************************************************************************
 * input_NetlistGetiovec: returns an iovec pointer for a readv() operation
 *****************************************************************************
 * We return an iovec vector, so that readv can read many packets at a time.
 * pp_data will be set to direct to the fifo pointer in DVDMviovec, which
 * will allow us to get the corresponding data_packet.
 *****************************************************************************/
struct iovec * input_NetlistGetiovec( void * p_method_data )
{
    netlist_t *     p_netlist;

    /* cast */
    p_netlist = (netlist_t *)p_method_data;
    
    /* check that we have enough free iovec */
    if( (
     (p_netlist->i_iovec_end - p_netlist->i_iovec_start)
        & p_netlist->i_nb_iovec ) < p_netlist->i_read_once )
    {
        intf_WarnMsg( 12, "input info: waiting for free iovec" );
        msleep( INPUT_IDLE_SLEEP );

        while( (
         (p_netlist->i_iovec_end - p_netlist->i_iovec_start)
            & p_netlist->i_nb_iovec ) < p_netlist->i_read_once )
        {
            msleep( INPUT_IDLE_SLEEP );
        }

        intf_WarnMsg( 12, "input info: found free iovec" );
    }

    if( (
     (p_netlist->i_data_end - p_netlist->i_data_start)
        & p_netlist->i_nb_data ) < p_netlist->i_read_once )
    {
        intf_WarnMsg( 12, "input info: waiting for free data packet" );
        msleep( INPUT_IDLE_SLEEP );

        while( (
         (p_netlist->i_data_end - p_netlist->i_data_start)
            & p_netlist->i_nb_data ) < p_netlist->i_read_once )
        {
            msleep( INPUT_IDLE_SLEEP );
        }

        intf_WarnMsg( 12, "input info: found free data packet" );
    }

    /* readv only takes contiguous buffers 
     * so, as a solution, we chose to have a FIFO a bit longer
     * than i_nb_data, and copy the begining of the FIFO to its end
     * if the readv needs to go after the end */
    if( p_netlist->i_nb_iovec - p_netlist->i_iovec_start + 1 <
                                                    p_netlist->i_read_once )
    {
        memcpy( &p_netlist->p_free_iovec[p_netlist->i_nb_iovec + 1], 
                p_netlist->p_free_iovec, 
                (p_netlist->i_read_once -
                    (p_netlist->i_nb_iovec + 1 - p_netlist->i_iovec_start))
                    * sizeof(struct iovec)
              );

    }

    return p_netlist->p_free_iovec + p_netlist->i_iovec_start;

}

/*****************************************************************************
 * input_NetlistMviovec: move the iovec pointer by one after a readv()
 *  operation and gives a data_packet corresponding to iovec in p_data
 *****************************************************************************/
void input_NetlistMviovec( void * p_method_data, int i_nb_iovec,
                           struct data_packet_s ** pp_data )
{
    netlist_t *         p_netlist;
    unsigned int        i_loop = 0;

    /* cast */
    p_netlist = (netlist_t *)p_method_data;
    
    /* lock */
    vlc_mutex_lock( &p_netlist->lock );

    /* Fills a table of pointers to packets associated with the io_vec's */
    while( i_loop < i_nb_iovec )
    {
        pp_data[i_loop] = p_netlist->pp_free_data[p_netlist->i_data_start];
        
        pp_data[i_loop]->p_buffer =
                    p_netlist->p_free_iovec[p_netlist->i_iovec_start].iov_base;
        
        pp_data[i_loop]->p_payload_start = pp_data[i_loop]->p_buffer;

        pp_data[i_loop]->p_payload_end =
                  pp_data[i_loop]->p_buffer + p_netlist->i_buffer_size;

        pp_data[i_loop]->p_next = NULL;
        pp_data[i_loop]->b_discard_payload = 0;

        pp_data[i_loop]->pi_refcount = p_netlist->pi_refcount +
                                       p_netlist->i_iovec_start;
        (*pp_data[i_loop]->pi_refcount)++;

        p_netlist->i_iovec_start ++;
        p_netlist->i_iovec_start &= p_netlist->i_nb_iovec;

        p_netlist->i_data_start ++;
        p_netlist->i_data_start &= p_netlist->i_nb_data;

        i_loop ++;
    }

    /* unlock */
    vlc_mutex_unlock( &p_netlist->lock );
    
}

/*****************************************************************************
 * input_NetlistNewPtr: returns a free data_packet_t
 * Gives a pointer ; its fields need to be initialized
 *****************************************************************************/
struct data_packet_s * input_NetlistNewPtr( void * p_method_data )
{    
    netlist_t *             p_netlist; 
    struct data_packet_s *  p_return;
    
    /* cast */
    p_netlist = (netlist_t *)p_method_data; 

    /* lock */
    vlc_mutex_lock ( &p_netlist->lock );
        
    /* check */
    if ( p_netlist->i_data_start == p_netlist->i_data_end )
    {
        intf_ErrMsg("Empty Data FIFO in netlist. Unable to allocate memory");
        return ( NULL );
    }
    
    p_return = (p_netlist->pp_free_data[p_netlist->i_data_start]);

    p_netlist->i_data_start++;
    p_netlist->i_data_start &= p_netlist->i_nb_data;

    p_return->p_payload_start = p_return->p_buffer;

    p_return->p_payload_end =
              p_return->p_buffer + p_netlist->i_buffer_size;

    p_return->p_next = NULL;
    p_return->b_discard_payload = 0;

    /* unlock */
    vlc_mutex_unlock (&p_netlist->lock);

    return ( p_return );
}

/*****************************************************************************
 * input_NetlistNewPacket: returns a free data_packet_t, and takes
 * a corresponding storage iovec.
 *****************************************************************************/
struct data_packet_s * input_NetlistNewPacket( void * p_method_data,
                                               size_t i_buffer_size )
{
    netlist_t *             p_netlist;
    struct data_packet_s *  p_packet;

    /* cast */
    p_netlist = (netlist_t *)p_method_data;

#ifdef DEBUG
    if( i_buffer_size > p_netlist->i_buffer_size )
    {
        /* This should not happen */
        intf_ErrMsg( "Netlist packet too small !" );
        return NULL;
    }
#endif
    
    /* lock */
    vlc_mutex_lock( &p_netlist->lock );

     /* check */
    if ( p_netlist->i_iovec_start == p_netlist->i_iovec_end )
    {
        intf_ErrMsg("Empty io_vec FIFO in netlist. Unable to allocate memory");
        return ( NULL );
    }

    if ( p_netlist->i_data_start == p_netlist->i_data_end )
    {
        intf_ErrMsg("Empty Data FIFO in netlist. Unable to allocate memory");
        return ( NULL );
    }


    /* Gives an io_vec and associated data */
    p_packet = p_netlist->pp_free_data[p_netlist->i_data_start];
        
    p_packet->p_buffer =
              p_netlist->p_free_iovec[p_netlist->i_iovec_start].iov_base;
        
    p_packet->p_payload_start = p_packet->p_buffer;
        
    p_packet->p_payload_end =
              p_packet->p_buffer + i_buffer_size;

    p_packet->p_next = NULL;
    p_packet->b_discard_payload = 0;

    p_packet->pi_refcount = p_netlist->pi_refcount + p_netlist->i_iovec_start;
    (*p_packet->pi_refcount)++;

    p_netlist->i_iovec_start ++;
    p_netlist->i_iovec_start &= p_netlist->i_nb_iovec;

    p_netlist->i_data_start ++;
    p_netlist->i_data_start &= p_netlist->i_nb_data;

    /* unlock */
    vlc_mutex_unlock( &p_netlist->lock );

    return p_packet;
}

/*****************************************************************************
 * input_NetlistNewPES: returns a free pes_packet_t
 *****************************************************************************/
struct pes_packet_s * input_NetlistNewPES( void * p_method_data )
{
    netlist_t *         p_netlist;
    pes_packet_t *      p_return;
    
    /* cast */ 
    p_netlist = (netlist_t *)p_method_data;
    
    /* lock */
    vlc_mutex_lock ( &p_netlist->lock );
    
    /* check */
    if ( p_netlist->i_pes_start == p_netlist->i_pes_end )
    {
        intf_ErrMsg("Empty PES FIFO in netlist - Unable to allocate memory");
        return ( NULL );
    }

    /* allocate */
    p_return = p_netlist->pp_free_pes[p_netlist->i_pes_start];
    p_netlist->i_pes_start++;
    p_netlist->i_pes_start &= p_netlist->i_nb_pes; 
   
    /* unlock */
    vlc_mutex_unlock (&p_netlist->lock);
    
    /* initialize PES */
    p_return->b_data_alignment = 0;
    p_return->b_discontinuity = 0; 
    p_return->i_pts = 0;
    p_return->i_dts = 0;
    p_return->i_pes_size = 0;
    p_return->p_first = NULL;

    return ( p_return );
}

/*****************************************************************************
 * input_NetlistDeletePacket: puts a data_packet_t back into the netlist
 *****************************************************************************/
void input_NetlistDeletePacket( void * p_method_data, data_packet_t * p_data )
{
    netlist_t * p_netlist;
    
    /* cast */
    p_netlist = (netlist_t *) p_method_data;

    /* lock */
    vlc_mutex_lock ( &p_netlist->lock );

   /* Delete data_packet */
    p_netlist->i_data_end ++;
    p_netlist->i_data_end &= p_netlist->i_nb_data;
    
    p_data->p_payload_start = p_data->p_buffer;
    p_data->p_payload_end = p_data->p_buffer + p_netlist->i_buffer_size;
        
    p_netlist->pp_free_data[p_netlist->i_data_end] = p_data;

    p_data->p_next = NULL;
    p_data->b_discard_payload = 0;

    /* Update reference counter */
    (*p_data->pi_refcount)--;

    if( (*p_data->pi_refcount) <= 0 )
    {

        p_netlist->i_iovec_end++;
        p_netlist->i_iovec_end &= p_netlist->i_nb_iovec;
        p_netlist->p_free_iovec[p_netlist->i_iovec_end].iov_base =
                                                            p_data->p_buffer;
    }
 
    /* unlock */
    vlc_mutex_unlock (&p_netlist->lock);
}

/*****************************************************************************
 * input_NetlistDeletePES: puts a pes_packet_t back into the netlist
 *****************************************************************************/
void input_NetlistDeletePES( void * p_method_data, pes_packet_t * p_pes )
{
    netlist_t *         p_netlist; 
    data_packet_t *     p_current_packet;
    data_packet_t *     p_next_packet;
    
    /* cast */
    p_netlist = (netlist_t *)p_method_data;

    /* lock */
    vlc_mutex_lock ( &p_netlist->lock );

    /* delete free  p_pes->p_first, p_next ... */
    p_current_packet = p_pes->p_first;
    while ( p_current_packet != NULL )
    {
        /* copy of NetListDeletePacket, duplicate code avoid many locks */

        p_netlist->i_data_end ++;
        p_netlist->i_data_end &= p_netlist->i_nb_data;

        /* re initialize */
        p_current_packet->p_payload_start = p_current_packet->p_buffer;
        p_current_packet->p_payload_end = p_current_packet->p_buffer
            + p_netlist->i_buffer_size;
        
        p_netlist->pp_free_data[p_netlist->i_data_end] = p_current_packet;

        /* Update reference counter */
        (*p_current_packet->pi_refcount)--;

        if( (*p_current_packet->pi_refcount) <= 0 )
        {
            (*p_current_packet->pi_refcount) = 0;
            p_netlist->i_iovec_end++;
            p_netlist->i_iovec_end &= p_netlist->i_nb_iovec;
            p_netlist->p_free_iovec[p_netlist->i_iovec_end].iov_base =
                    p_current_packet->p_buffer;
        }
    
        p_next_packet = p_current_packet->p_next;
        p_current_packet->p_next = NULL;
        p_current_packet->b_discard_payload = 0;
        p_current_packet = p_next_packet;
    }
 
    /* delete our current PES packet */
    p_netlist->i_pes_end ++;
    p_netlist->i_pes_end &= p_netlist->i_nb_pes;
    p_netlist->pp_free_pes[p_netlist->i_pes_end] = p_pes;
    
    /* unlock */
    vlc_mutex_unlock (&p_netlist->lock);

}

/*****************************************************************************
 * input_NetlistEnd: frees all allocated structures
 *****************************************************************************/
void input_NetlistEnd( input_thread_t * p_input )
{
    netlist_t * p_netlist;

    /* cast */
    p_netlist = ( netlist_t * ) p_input->p_method_data;

    /* destroy the mutex lock */
    vlc_mutex_destroy( &p_netlist->lock );
    
    /* free the FIFO, the buffer, and the netlist structure */
    free( p_netlist->pi_refcount );
    free( p_netlist->p_free_iovec );
    free( p_netlist->pp_free_pes );
    free( p_netlist->pp_free_data );
    free( p_netlist->p_pes );
    free( p_netlist->p_data );
    free( p_netlist->p_buffers );

    /* free the netlist */
    free( p_netlist );
}
