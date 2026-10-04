/* tspico.c: TS-Pico interface for the TS2068, via the TS-Pico bridge
   Copyright (c) 2026 TS-Pico project

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

*/

/* The TS-Pico is a Raspberry Pi Pico on the TS2068's bus: SD card, flash
   ROM slots and a printer, reached through ports 0x0e (data) and 0x0f
   (status/control), decoded on the low address byte. Fuse doesn't emulate
   the Pico. Each access to those ports goes, as one frame, to pico_host,
   which runs the real TS-Pico firmware with its SD card in a host folder:

     Fuse -> bridge: [op, value]   op 0 = OUT 0x0e   op 1 = IN 0x0e
                                   op 2 = IN 0x0f    op 3 = OUT 0x0f
                                   op 4 = HELLO (value = bridge version)
     bridge -> Fuse: one byte      the byte read, or 0 for a write

   The full spec, version 1: docs/EMULATOR_BRIDGE.md in
   https://github.com/timex-sinclair-projects/tspico-firmware-build

   The bridge's address is the tspico-bridge setting, else $TSPICO_BRIDGE:
   "tcp:HOST:PORT" or (not on Windows) "unix:PATH"; the default is
   tcp:127.0.0.1:2068. One attempt at the first access, and another after a
   reset. With no bridge, or after losing it, the ports act as if no TS-Pico
   were plugged in (0x0f reads 0xff, 0x0e reads 0x00), so the TS-Pico ROM's
   commands end in a report instead of hanging.

   The TS-Pico ROM is 32K: a 16K HOME ROM (rom-ts2068-0) and a 16K EXROM
   (rom-ts2068-1). */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libspectrum.h"

#include "compat.h"
#include "infrastructure/startup_manager.h"
#include "module.h"
#include "periph.h"
#include "settings.h"
#include "tspico.h"
#include "ui/ui.h"

#ifdef BUILD_TSPICO

#ifdef WIN32
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define TSPICO_BRIDGE_VERSION 1
#define TSPICO_BRIDGE_DEFAULT "tcp:127.0.0.1:2068"

enum { TSPICO_OUT_DATA, TSPICO_IN_DATA, TSPICO_IN_STATUS, TSPICO_OUT_CONTROL,
       TSPICO_HELLO };

static compat_socket_t tspico_socket;
static int tspico_tried = 0;

static void tspico_reset( int hard_reset );
static libspectrum_byte tspico_read( libspectrum_word port,
                                     libspectrum_byte *attached );
static void tspico_write( libspectrum_word port, libspectrum_byte b );

static module_info_t tspico_module_info = {

  /* .reset = */ tspico_reset,
  /* .romcs = */ NULL,
  /* .snapshot_enabled = */ NULL,
  /* .snapshot_from = */ NULL,
  /* .snapshot_to = */ NULL,

};

static const periph_port_t tspico_ports[] = {
  { 0x00ff, 0x000e, tspico_read, tspico_write },
  { 0x00ff, 0x000f, tspico_read, tspico_write },
  { 0, 0, NULL, NULL }
};

static const periph_t tspico_periph = {
  /* .option = */ &settings_current.tspico,
  /* .ports = */ tspico_ports,
  /* .hard_reset = */ 0,
  /* .activate = */ NULL,
};

static void
tspico_disconnect( void )
{
  if( tspico_socket != compat_socket_invalid ) {
    compat_socket_close( tspico_socket );
    tspico_socket = compat_socket_invalid;
  }
}

/* Exactly len bytes, or 0 */
static int
tspico_send( const unsigned char *buf, int len )
{
  while( len > 0 ) {
    int n = (int)send( tspico_socket, (const char *)buf, len, 0 );
    if( n <= 0 ) return 0;
    buf += n; len -= n;
  }
  return 1;
}

static int
tspico_recv( unsigned char *b )
{
  return recv( tspico_socket, (char *)b, 1, 0 ) == 1;
}

/* host_port: "HOST:PORT" */
static compat_socket_t
tspico_connect_tcp( const char *host_port )
{
  char host[256];
  const char *colon = strrchr( host_port, ':' );
  struct addrinfo hints, *res, *ai;
  compat_socket_t fd = compat_socket_invalid;
  size_t length;

  if( !colon ) return compat_socket_invalid;
  length = colon - host_port;
  if( length >= sizeof( host ) ) return compat_socket_invalid;
  memcpy( host, host_port, length );
  host[ length ] = '\0';

  memset( &hints, 0, sizeof( hints ) );
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if( getaddrinfo( host, colon + 1, &hints, &res ) ) return fd;

  for( ai = res; ai; ai = ai->ai_next ) {
    fd = socket( ai->ai_family, ai->ai_socktype, ai->ai_protocol );
    if( fd == compat_socket_invalid ) continue;
    if( !connect( fd, ai->ai_addr, ai->ai_addrlen ) ) break;
    compat_socket_close( fd );
    fd = compat_socket_invalid;
  }
  freeaddrinfo( res );

  if( fd != compat_socket_invalid ) {
    int one = 1;                /* every frame is a round trip */
    setsockopt( fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one,
                sizeof( one ) );
  }

  return fd;
}

#ifndef WIN32
static compat_socket_t
tspico_connect_unix( const char *path )
{
  struct sockaddr_un addr;
  compat_socket_t fd = socket( AF_UNIX, SOCK_STREAM, 0 );

  if( fd == compat_socket_invalid ) return fd;
  memset( &addr, 0, sizeof( addr ) );
  addr.sun_family = AF_UNIX;
  strncpy( addr.sun_path, path, sizeof( addr.sun_path ) - 1 );
  if( connect( fd, (struct sockaddr *)&addr, sizeof( addr ) ) ) {
    compat_socket_close( fd );
    return compat_socket_invalid;
  }
  return fd;
}
#endif

static void
tspico_connect( void )
{
  const char *where = settings_current.tspico_bridge;
  unsigned char hello[2] = { TSPICO_HELLO, TSPICO_BRIDGE_VERSION };
  unsigned char version;

  if( tspico_tried ) return;
  tspico_tried = 1;

  if( !where || !*where ) where = getenv( "TSPICO_BRIDGE" );
  if( !where || !*where ) where = TSPICO_BRIDGE_DEFAULT;

  if( !strncmp( where, "tcp:", 4 ) ) {
    tspico_socket = tspico_connect_tcp( where + 4 );
#ifndef WIN32
  } else if( !strncmp( where, "unix:", 5 ) ) {
    tspico_socket = tspico_connect_unix( where + 5 );
#endif
  } else {
    ui_error( UI_ERROR_ERROR,
              "TS-Pico bridge must be tcp:HOST:PORT or unix:PATH, not '%s'",
              where );
    return;
  }

  if( tspico_socket == compat_socket_invalid ) {
    ui_error( UI_ERROR_INFO,
              "TS-Pico: nothing at %s; running as if no TS-Pico", where );
    return;
  }

  if( !tspico_send( hello, 2 ) || !tspico_recv( &version ) ) {
    ui_error( UI_ERROR_WARNING, "TS-Pico bridge at %s didn't answer HELLO",
              where );
    tspico_disconnect();
    return;
  }

  /* Connected: nothing to say. (Every ui_error() is a dialog on some UIs,
     the Win32 one a modal one.) The bridge's version is for later ones. */
  (void)version;
}

/* One frame; the bridge's reply, or what an absent TS-Pico reads as */
static libspectrum_byte
tspico_frame( libspectrum_byte op, libspectrum_byte value )
{
  unsigned char frame[2], reply;

  tspico_connect();
  if( tspico_socket != compat_socket_invalid ) {
    frame[0] = op; frame[1] = value;
    if( tspico_send( frame, 2 ) && tspico_recv( &reply ) ) return reply;
    ui_error( UI_ERROR_WARNING, "TS-Pico: lost the bridge" );
    tspico_disconnect();
  }
  return op == TSPICO_IN_STATUS ? 0xff : 0x00;
}

static libspectrum_byte
tspico_read( libspectrum_word port, libspectrum_byte *attached )
{
  *attached = 0xff;
  return tspico_frame( ( port & 0xff ) == 0x0f ? TSPICO_IN_STATUS
                                               : TSPICO_IN_DATA, 0 );
}

static void
tspico_write( libspectrum_word port, libspectrum_byte b )
{
  tspico_frame( ( port & 0xff ) == 0x0f ? TSPICO_OUT_CONTROL
                                        : TSPICO_OUT_DATA, b );
}

/* After a reset, try the bridge again if we haven't got it */
static void
tspico_reset( int hard_reset GCC_UNUSED )
{
  if( tspico_socket == compat_socket_invalid ) tspico_tried = 0;
}

static int
tspico_init( void *context )
{
  tspico_socket = compat_socket_invalid;

  module_register( &tspico_module_info );
  periph_register( PERIPH_TYPE_TSPICO, &tspico_periph );

  compat_socket_networking_init();

  return 0;
}

static void
tspico_end( void )
{
  tspico_disconnect();
  compat_socket_networking_end();
}

void
tspico_register_startup( void )
{
  startup_manager_module dependencies[] = { STARTUP_MANAGER_MODULE_SETUID };
  startup_manager_register( STARTUP_MANAGER_MODULE_TSPICO, dependencies,
                            ARRAY_SIZE( dependencies ), tspico_init, NULL,
                            tspico_end );
}

#else /* #ifdef BUILD_TSPICO */

/* No TS-Pico support */

void
tspico_register_startup( void )
{
}

#endif /* #ifdef BUILD_TSPICO */
