/*****************************************************************************
 * input_ts.c: TS demux and netlist management
 *****************************************************************************
 * Copyright (C) 1998, 1999, 2000 VideoLAN
 * $Id: input_ts.c,v 1.12 2001/03/19 13:26:59 sam Exp $
 *
 * Authors: Henri Fallon <henri@videolan.org>
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

#define MODULE_NAME ts
#include "modules_inner.h"

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include "defs.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#ifdef SYS_NTO
#include <sys/select.h>
#endif
#include <sys/uio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "config.h"
#include "common.h"
#include "threads.h"
#include "mtime.h"
#include "tests.h"
#include "modules.h"

#include "intf_msg.h"

#include "stream_control.h"
#include "input_ext-intf.h"
#include "input_ext-dec.h"

#include "input.h"
#include "input_ts.h"

#include "mpeg_system.h"
#include "input_netlist.h"

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int  TSProbe     ( probedata_t * );
static int  TSRead      ( struct input_thread_s *,
                          data_packet_t * p_packets[INPUT_READ_ONCE] );
static void TSInit      ( struct input_thread_s * );
static void TSEnd       ( struct input_thread_s * );

/*****************************************************************************
 * Functions exported as capabilities. They are declared as static so that
 * we don't pollute the namespace too much.
 *****************************************************************************/
void _M( input_getfunctions )( function_list_t * p_function_list )
{
#define input p_function_list->functions.input
    p_function_list->pf_probe = TSProbe;
    input.pf_init             = TSInit;
#if !defined( SYS_BEOS ) && !defined( SYS_NTO )
    input.pf_open             = input_NetworkOpen;
    input.pf_close            = input_NetworkClose;
#else
    input.pf_open             = input_FileOpen;
    input.pf_close            = input_FileClose;
#endif
    input.pf_end              = TSEnd;
    input.pf_set_area         = NULL;
    input.pf_read             = TSRead;
    input.pf_demux            = input_DemuxTS;
    input.pf_new_packet       = input_NetlistNewPacket;
    input.pf_new_pes          = input_NetlistNewPES;
    input.pf_delete_packet    = input_NetlistDeletePacket;
    input.pf_delete_pes       = input_NetlistDeletePES;
    input.pf_rewind           = NULL;
    input.pf_seek             = NULL;
#undef input
}

/*****************************************************************************
 * TSProbe: verifies that the stream is a TS stream
 *****************************************************************************/
static int TSProbe( probedata_t * p_data )
{
    input_thread_t * p_input = (input_thread_t *)p_data;

    char * psz_name = p_input->p_source;
    int i_handle;
    int i_score = 1;

    if( TestMethod( INPUT_METHOD_VAR, "ts" ) )
    {
        return( 999 );
    }

    if( ( strlen(psz_name) > 5 ) && !strncasecmp( psz_name, "file:", 5 ) )
    {
        /* If the user specified "file:" then it's probably a file */
        psz_name += 5;
    }

    i_handle = open( psz_name, 0 );
    if( i_handle == -1 )
    {
        return( 0 );
    }
    close( i_handle );

    return( i_score );
}

/*****************************************************************************
 * TSInit: initializes TS structures
 *****************************************************************************/
static void TSInit( input_thread_t * p_input )
{
    /* Initialize netlist and TS structures */
    thread_ts_data_t    * p_method;
    es_descriptor_t     * p_pat_es;
    es_ts_data_t        * p_demux_data;
    stream_ts_data_t    * p_stream_data;

    /* Initialise structure */
    p_method = malloc( sizeof( thread_ts_data_t ) );
    if( p_method == NULL )
    {
        intf_ErrMsg( "TS input : Out of memory" );
        p_input->b_error = 1;
        return;
    }

    p_input->p_plugin_data = (void *)p_method;
    p_input->p_method_data = NULL;
 
    
    /* Initialize netlist */
    if( input_NetlistInit( p_input, NB_DATA, NB_PES, TS_PACKET_SIZE, 
                INPUT_READ_ONCE ) )
    {
        intf_ErrMsg( "TS input : Could not initialize netlist" );
        return;
    }
   
    /* Initialize the stream */
    input_InitStream( p_input, sizeof( stream_ts_data_t ) );

    /* Init */
    p_stream_data = (stream_ts_data_t *)p_input->stream.p_demux_data;
    p_stream_data->i_pat_version = PAT_UNINITIALIZED ;

    /* We'll have to catch the PAT in order to continue 
     * Then the input will catch the PMT and then the others ES
     * The PAT es is indepedent of any program. */
    p_pat_es = input_AddES( p_input, NULL,
                           0x00, sizeof( es_ts_data_t ) );
    p_demux_data=(es_ts_data_t *)p_pat_es->p_demux_data;
    p_demux_data->b_psi = 1;
    p_demux_data->i_psi_type = PSI_IS_PAT;
    p_demux_data->p_psi_section = malloc(sizeof(psi_section_t));
    p_demux_data->p_psi_section->b_is_complete = 1;
    
}

/*****************************************************************************
 * TSEnd: frees unused data
 *****************************************************************************/
static void TSEnd( input_thread_t * p_input )
{
    es_descriptor_t     * p_pat_es;
    
    p_pat_es = input_FindES( p_input, 0x00 );

    if( p_pat_es != NULL )
        input_DelES( p_input, p_pat_es );
    free(p_input->p_plugin_data);
}

/*****************************************************************************
 * TSRead: reads data packets
 *****************************************************************************
 * Returns -1 in case of error, 0 if everything went well, and 1 in case of
 * EOF.
 *****************************************************************************/
static int TSRead( input_thread_t * p_input,
                   data_packet_t * pp_packets[INPUT_READ_ONCE] )
{
    thread_ts_data_t    * p_method;
    unsigned int    i_read, i_loop;
    int             i_data;
    struct iovec  * p_iovec;
    struct timeval  s_wait;
    

    /* Init */
    p_method = ( thread_ts_data_t * )p_input->p_plugin_data;
   
    /* Initialize file descriptor set */
    FD_ZERO( &(p_method->s_fdset) );
    FD_SET( p_input->i_handle, &(p_method->s_fdset) );

    
    /* We'll wait 0.5 second if nothing happens */
    s_wait.tv_sec = 0.5;
    s_wait.tv_usec = 0;
    
    /* Reset pointer table */
    memset( pp_packets, 0, INPUT_READ_ONCE*sizeof(data_packet_t *) );
    
    /* Get iovecs */
    p_iovec = input_NetlistGetiovec( p_input->p_method_data );
    
    if ( p_iovec == NULL )
    {
        return( -1 ); /* empty netlist */
    } 

    /* Fill if some data is available */
    i_data = select(p_input->i_handle + 1, &(p_method->s_fdset), NULL, NULL, 
                    &s_wait);
    
    if( i_data == -1 )
    {
        intf_ErrMsg( "TS input : Error in select : %s", strerror(errno) );
        return( -1 );
    }
    
    if( i_data )
    {
        i_read = readv( p_input->i_handle, p_iovec, INPUT_READ_ONCE );
        
        if( i_read == -1 )
        {
            intf_ErrMsg( "TS input : Could not readv" );
            return( -1 );
        }
    
        input_NetlistMviovec( p_input->p_method_data, 
                (int)(i_read/TS_PACKET_SIZE) , pp_packets );
    
        /* check correct TS header */
        for( i_loop=0; i_loop * TS_PACKET_SIZE < i_read; i_loop++ )
        {
            if( pp_packets[i_loop]->p_buffer[0] != 0x47 )
                intf_ErrMsg( "TS input : Bad TS Packet (starcode != 0x47)." );
        }
    }
    return 0;
}
