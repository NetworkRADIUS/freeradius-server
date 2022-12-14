#pragma once
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
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @file io/application.h
 * @brief Application interfaces.
 *
 * @copyright 2017 The FreeRADIUS project
 */
#include <freeradius-devel/io/base.h>
#include <freeradius-devel/server/cf_util.h>
#include <freeradius-devel/server/dl_module.h>
#include <freeradius-devel/server/virtual_servers.h>

/*
 *	src/lib/io/schedule.h
 */
typedef struct fr_schedule_s fr_schedule_t;

/** Open a new socket or other packet source
 *
 * @param[in] instance  of the #fr_app_t.
 * @param[in] sc	we should register sockets with.
 * @param[in] cs	of the listen section that created this #fr_app_t.
 * @return
 *	- 0 on success.
 *	- <0 on failure.
 */
typedef int (*fr_app_open_t)(void *instance, fr_schedule_t *sc, CONF_SECTION *cs);

/** Set the priority of a packet
 *
 * @param[in] instance	of the #fr_app_t.
 * @param[in] buffer	raw packet
 * @param[in] buflen	length of the packet
 * @return
 *	-1 - error, drop the packet
 *	0  - no error, but we still drop the packet
 *	*  - the priority of this packet
 */
typedef int (*fr_app_priority_get_t)(void const *instance, uint8_t const *buffer, size_t buflen);

/** Called by the network thread to pass an event list for the module to use for timer events
 */
typedef void (*fr_app_event_list_set_t)(fr_listen_t *li, fr_event_list_t *el, void *nr);

/** Describes a new application (protocol)
 *
 * This is the main application structure.  It contains different callbacks that
 * are run at different points during the server lifecycle and are called by the IO
 * framework.
 *
 * How the fr_app_t operates is specific to each protocol.
 */
typedef struct {
	module_t			common;		//!< Common fields provided by all modules.

	fr_dict_t const			**dict;		//!< default dictionary for this application.

	fr_app_open_t			open;		//!< Callback to allow the #fr_app_t to build an #fr_listen_t
							///< and register it with the scheduler so we can receive
							///< data.

	fr_io_decode_t			decode;		//!< Translate raw bytes into fr_pair_ts and metadata.
							///< May be NULL.
							///< Here for convenience, so that decode operations common
							///< to all #fr_app_io_t can be performed by the #fr_app_t.

	fr_io_encode_t			encode;		//!< Pack fr_pair_ts back into a byte array.
							///< May be NULL.
							///< Here for convenience, so that encode operations common
							///< to all #fr_app_io_t can be performed by the #fr_app_t.

	fr_app_priority_get_t		priority;	//!< Assign a priority to the packet.
} fr_app_t;

/** Public structure describing an application (protocol) specialisation
 *
 * The fr_app_worker_t provides the state machine that's used to process
 * the packet after its been decoded.
 */
typedef struct {
	module_t			common;		//!< Common fields to all loadable modules.
	module_method_t			entry_point;	//!< Entry point into the protocol subtype's state machine.
	virtual_server_compile_t const	*compile_list;	//!< list of processing sections
} fr_app_worker_t;

#include <freeradius-devel/io/app_io.h>
