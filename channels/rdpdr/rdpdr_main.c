/*
   Copyright (c) 2009-2010 Jay Sorg

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included
   in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
   DEALINGS IN THE SOFTWARE.

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/time.h>

#include "irp.h"
#include "devman.h"
#include "types.h"
#include "types_ui.h"
#include "vchan.h"
#include "chan_stream.h"
#include "constants_rdpdr.h"

#define LOG_LEVEL 1
#define LLOG(_level, _args) \
  do { if (_level < LOG_LEVEL) { printf _args ; } } while (0)
#define LLOGLN(_level, _args) \
  do { if (_level < LOG_LEVEL) { printf _args ; printf("\n"); } } while (0)

struct wait_obj
{
	int sock;
	struct sockaddr_un sa;
};

struct data_in_item
{
	struct data_in_item * next;
	char * data;
	int data_size;
};

CHANNEL_ENTRY_POINTS g_ep;
static void * g_han;
static CHANNEL_DEF g_channel_def[2];
static uint32 g_open_handle[2];
static char * g_data_in[2];
static int g_data_in_size[2];
static int g_data_in_read[2];
static struct wait_obj g_term_event;
static struct wait_obj g_data_in_event;
static struct data_in_item * volatile g_list_head;
static struct data_in_item * volatile g_list_tail;
/* for locking the linked list */
static pthread_mutex_t * g_mutex;
static volatile int g_thread_status;

static uint16 g_versionMinor;
static uint16 g_clientID;
static uint16 g_deviceCount;
RDPDR_DEVICE g_device[RDPDR_MAX_DEVICES];

static int
init_wait_obj(struct wait_obj * obj, const char * name)
{
	int pid;
	int size;

	pid = getpid();
	obj->sock = socket(PF_UNIX, SOCK_DGRAM, 0);
	if (obj->sock < 0)
	{
		LLOGLN(0, ("init_wait_obj: socket failed"));
		return 1;
	}
	obj->sa.sun_family = AF_UNIX;
	size = sizeof(obj->sa.sun_path) - 1;
	snprintf(obj->sa.sun_path, size, "/tmp/%s%8.8x", name, pid);
	obj->sa.sun_path[size] = 0;
	size = sizeof(obj->sa);
	if (bind(obj->sock, (struct sockaddr*)(&(obj->sa)), size) < 0)
	{
		LLOGLN(0, ("init_wait_obj: bind failed"));
		close(obj->sock);
		obj->sock = -1;
		unlink(obj->sa.sun_path);
		return 1;
	}
	return 0;
}

static int
deinit_wait_obj(struct wait_obj * obj)
{
	if (obj->sock != -1)
	{
		close(obj->sock);
		obj->sock = -1;
		unlink(obj->sa.sun_path);
	}
	return 0;
}

static int
is_wait_obj_set(struct wait_obj * obj)
{
	fd_set rfds;
	int num_set;
	struct timeval time;

	FD_ZERO(&rfds);
	FD_SET(obj->sock, &rfds);
	memset(&time, 0, sizeof(time));
	num_set = select(obj->sock + 1, &rfds, 0, 0, &time);
	return (num_set == 1);
}

static int
set_wait_obj(struct wait_obj * obj)
{
	int len;

	if (is_wait_obj_set(obj))
	{
		return 0;
	}
	len = sendto(obj->sock, "sig", 4, 0, (struct sockaddr*)(&(obj->sa)),
		sizeof(obj->sa));
	if (len != 4)
	{
		LLOGLN(0, ("set_wait_obj: error"));
		return 1;
	}
	return 0;
}

static int
clear_wait_obj(struct wait_obj * obj)
{
	int len;

	while (is_wait_obj_set(obj))
	{
		len = recvfrom(obj->sock, &len, 4, 0, 0, 0);
		if (len != 4)
		{
			LLOGLN(0, ("chan_man_clear_ev: error"));
			return 1;
		}
	}
	return 0;
}

static int
wait(int timeout, int numr, int * listr)
{
	int max;
	int rv;
	int index;
	int sock;
	struct timeval time;
	struct timeval * ptime;
	fd_set fds;

	ptime = 0;
	if (timeout >= 0)
	{
		time.tv_sec = timeout / 1000;
		time.tv_usec = (timeout * 1000) % 1000000;
		ptime = &time;
	}
	max = 0;
	FD_ZERO(&fds);
	for (index = 0; index < numr; index++)
	{
		sock = listr[index];
		FD_SET(sock, &fds);
		if (sock > max)
		{
			max = sock;
		}
	}
	rv = select(max + 1, &fds, 0, 0, ptime);
	return rv;
}

/* called by main thread
   add item to linked list and inform worker thread that there is data */
static void
signal_data_in(void)
{
	struct data_in_item * item;

	item = (struct data_in_item *) malloc(sizeof(struct data_in_item));
	item->next = 0;
	item->data = g_data_in[0];
	g_data_in[0] = 0;
	item->data_size = g_data_in_size[0];
	g_data_in_size[0] = 0;
	pthread_mutex_lock(g_mutex);
	if (g_list_tail == 0)
	{
		g_list_head = item;
		g_list_tail = item;
	}
	else
	{
		g_list_tail->next = item;
		g_list_tail = item;
	}
	pthread_mutex_unlock(g_mutex);
	set_wait_obj(&g_data_in_event);
}

static void
rdpdr_process_server_announce_request(char* data, int data_size)
{
	/* versionMajor, must be 1 */
	g_versionMinor = GET_UINT16(data, 2); /* versionMinor */
	g_clientID = GET_UINT32(data, 4); /* clientID */

	LLOGLN(0, ("Version Minor: %d\n", g_versionMinor));

	switch(g_versionMinor)
	{
		case 0x000C:
			LLOGLN(0, ("Windows Vista, Windows Vista SP1, Windows Server 2008, Windows 7, and Windows Server 2008 R2"));
			break;

		case 0x000A:
			LLOGLN(0, ("Windows Server 2003 SP2"));
			break;

		case 0x0006:
			LLOGLN(0, ("Windows XP SP3"));
			break;

		case 0x0005:
			LLOGLN(0, ("Windows XP, Windows XP SP1, Windows XP SP2, Windows Server 2003, and Windows Server 2003 SP1"));
			break;

		case 0x0002:
			LLOGLN(0, ("Windows 2000"));
			break;
	}
}

static int
rdpdr_send_client_announce_reply()
{
	uint32 error;
	char* out_data = malloc(12);

	SET_UINT16(out_data, 0, RDPDR_COMPONENT_TYPE_CORE);
	SET_UINT16(out_data, 2, PAKID_CORE_CLIENTID_CONFIRM);

	SET_UINT16(out_data, 4, 1); /* versionMajor, must be set to 1 */
	SET_UINT16(out_data, 6, g_versionMinor); /* versionMinor */
	SET_UINT32(out_data, 8, g_clientID); /* clientID, given by the server in a Server Announce Request */

	error = g_ep.pVirtualChannelWrite(g_open_handle[0], out_data, 12, out_data);

	if (error != CHANNEL_RC_OK)
	{
		LLOGLN(0, ("thread_process_message_formats: "
			"VirtualChannelWrite failed %d", error));
		return 1;
	}

	return 0;
}

static int
rdpdr_send_client_name_request()
{
	char* out_data;
	int out_data_size;
	uint32 error;
	uint32 computerNameLen;

	computerNameLen = 1;
	out_data_size = 16 + computerNameLen * 2;
	out_data = malloc(out_data_size);

	SET_UINT16(out_data, 0, RDPDR_COMPONENT_TYPE_CORE);
	SET_UINT16(out_data, 2, PAKID_CORE_CLIENT_NAME);

	SET_UINT32(out_data, 4, 1); // unicodeFlag, 0 for ASCII and 1 for Unicode
	SET_UINT32(out_data, 8, 0); // codePage, must be set to zero

	/* this part is a hardcoded test, while waiting for a unicode string output function */
	/* we also need to figure out a way of passing settings from the freerdp core */

	SET_UINT32(out_data, 12, computerNameLen); /* computerNameLen */
	SET_UINT16(out_data, 16, 0x0041); /* computerName */

	error = g_ep.pVirtualChannelWrite(g_open_handle[0], out_data, out_data_size, out_data);

	if (error != CHANNEL_RC_OK)
	{
		LLOGLN(0, ("thread_process_message_formats: "
			"VirtualChannelWrite failed %d", error));
		return 1;
	}

	return 0;
}

static int
rdpdr_send_device_list_announce_request()
{
	char* out_data;
	int out_data_size;

	int i;
	uint32 error;
	int offset = 0;
	
	out_data = malloc(64);

	SET_UINT16(out_data, 0, RDPDR_COMPONENT_TYPE_CORE);
	SET_UINT16(out_data, 2, PAKID_CORE_DEVICELIST_ANNOUNCE);
	SET_UINT16(out_data, 4, g_deviceCount); // deviceCount
	offset += 6;

	for (i = 0; i < g_deviceCount; i++)
	{
		SET_UINT16(out_data, offset, g_device[i].deviceType); /* deviceType */
		SET_UINT16(out_data, offset, i); /* deviceID */
		//out_uint8p(s, g_device[i].name, 8); // preferredDosName, Max 8 characters, may not be null terminated
		offset += 12;

		switch (g_device[i].deviceType)
		{
			case DEVICE_TYPE_PRINTER:

				break;

			case DEVICE_TYPE_DISK:

				break;

			case DEVICE_TYPE_SMARTCARD:

				/*
				 * According to [MS-RDPEFS] the deviceDataLength field for
				 * the smart card device type must be set to zero
				 */
				
				SET_UINT32(out_data, offset, 0); // deviceDataLength
				offset += 4;
				break;

			default:
				SET_UINT32(out_data, offset, 0);
				offset += 4;
		}
	}

	out_data_size = offset;
	error = g_ep.pVirtualChannelWrite(g_open_handle[0], out_data, out_data_size, out_data);

	if (error != CHANNEL_RC_OK)
	{
		LLOGLN(0, ("thread_process_message_formats: "
			"VirtualChannelWrite failed %d", error));
		return 1;
	}

	return 0;
}

static void
rdpdr_process_irp(char* data, int data_size)
{
	IRP irp;
	memset((void*)&irp, '\0', sizeof(IRP));

	irp.ioStatus = RD_STATUS_SUCCESS;

	/* Device I/O Request Header */
	irp.deviceID = GET_UINT32(data, 0); /* deviceID */
	irp.fileID = GET_UINT32(data, 4); /* fileID */
	irp.completionID = GET_UINT32(data, 8); /* completionID */
	irp.majorFunction = GET_UINT32(data, 12); /* majorFunction */
	irp.minorFunction = GET_UINT32(data, 16); /* minorFunction */

	/* In the end, devices will be registered by each sub-module, and this
	 * step won't be necessary anymore. Ideally, there would be a linked list
	 * of currently registered devices, with information such as device type
	 * and callbacks to different functions abstracting the device. The linked
	 * list is necessary because it will make dynamic registering and unregistering
	 * of devices easier, such as when a user connects a usb drive to his computer
	 * while the RDP session is already initiated.
	 */

	switch(g_device[irp.deviceID].deviceType)
	{
		case DEVICE_TYPE_SERIAL:
			//irp.fns = &serial_fns;
			//irp.rwBlocking = False;
			break;

		case DEVICE_TYPE_PARALLEL:
			//irp.fns = &parallel_fns;
			//irp.rwBlocking = False;
			break;

		case DEVICE_TYPE_PRINTER:
			//irp.fns = &printer_fns;
			//irp.rwBlocking = False;
			break;

		case DEVICE_TYPE_DISK:
			//irp.fns = &disk_fns;
			//irp.rwBlocking = False;
			break;

		case DEVICE_TYPE_SMARTCARD:

		default:
			//ui_error(NULL, "IRP bad deviceID %ld\n", irp.deviceID);
			return;
	}

	LLOGLN(0, ("IRP MAJOR: %d MINOR: %d\n", irp.majorFunction, irp.minorFunction));

	switch(irp.majorFunction)
	{
		case IRP_MJ_CREATE:
			LLOGLN(0, ("IRP_MJ_CREATE\n"));
			irp_process_create_request(&data[20], data_size - 20, &irp);
			irp_send_create_response(&irp);
			break;

		case IRP_MJ_CLOSE:
			LLOGLN(0, ("IRP_MJ_CLOSE\n"));
			irp_process_close_request(&data[20], data_size - 20, &irp);
			irp_send_close_response(&irp);
			break;

		case IRP_MJ_READ:
			LLOGLN(0, ("IRP_MJ_READ\n"));
			irp_process_read_request(&data[20], data_size - 20, &irp);
			break;

		case IRP_MJ_WRITE:
			LLOGLN(0, ("IRP_MJ_WRITE\n"));
			irp_process_write_request(&data[20], data_size - 20, &irp);
			break;

		case IRP_MJ_QUERY_INFORMATION:
			LLOGLN(0, ("IRP_MJ_QUERY_INFORMATION\n"));
			irp_process_query_information_request(&data[20], data_size - 20, &irp);
			irp_send_query_information_response(&irp);
			break;

		case IRP_MJ_SET_INFORMATION:
			LLOGLN(0, ("IRP_MJ_SET_INFORMATION\n"));
			irp_process_set_volume_information_request(&data[20], data_size - 20, &irp);
			break;

		case IRP_MJ_QUERY_VOLUME_INFORMATION:
			LLOGLN(0, ("IRP_MJ_QUERY_VOLUME_INFORMATION\n"));
			irp_process_query_volume_information_request(&data[20], data_size - 20, &irp);
			break;

		case IRP_MJ_DIRECTORY_CONTROL:
			LLOGLN(0, ("IRP_MJ_DIRECTORY_CONTROL\n"));
			irp_process_directory_control_request(&data[20], data_size - 20, &irp);
			break;

		case IRP_MJ_DEVICE_CONTROL:
			LLOGLN(0, ("IRP_MJ_DEVICE_CONTROL\n"));
			irp_process_device_control_request(&data[20], data_size - 20, &irp);
			break;

		case IRP_MJ_LOCK_CONTROL:
			LLOGLN(0, ("IRP_MJ_LOCK_CONTROL\n"));
			irp_process_file_lock_control_request(&data[20], data_size - 20, &irp);
			break;

		default:
			//ui_unimpl(NULL, "IRP majorFunction=0x%x minorFunction=0x%x\n", irp.majorFunction, irp.minorFunction);
			return;
	}

	if (irp.buffer)
		free(irp.buffer);
}

static int
thread_process_message(char * data, int data_size)
{
	uint16 component;
	uint16 packetID;
	uint32 deviceID;
	uint32 status;

	component = GET_UINT16(data, 0);
	packetID = GET_UINT16(data, 2);

	if (component == RDPDR_COMPONENT_TYPE_CORE)
	{
		LLOGLN(0, ("RDPDR_COMPONENT_TYPE_CORE"));
		switch (packetID)
		{
			case PAKID_CORE_SERVER_ANNOUNCE:
				LLOGLN(0, ("PAKID_CORE_SERVER_ANNOUNCE"));
				rdpdr_process_server_announce_request(&data[4], data_size - 4);
				rdpdr_send_client_announce_reply();
				rdpdr_send_client_name_request();
				break;

			case PAKID_CORE_CLIENTID_CONFIRM:
				LLOGLN(0, ("PAKID_CORE_CLIENTID_CONFIRM"));
				//rdpdr_send_device_list();
				break;

			case PAKID_CORE_DEVICE_REPLY:
				/* connect to a specific resource */
				LLOGLN(0, ("PAKID_CORE_DEVICE_REPLY"));
				deviceID = GET_UINT32(data, 4);
				status = GET_UINT32(data, 8);
				break;

			case PAKID_CORE_DEVICE_IOREQUEST:
				LLOGLN(0, ("PAKID_CORE_DEVICE_IOREQUEST"));
				//rdpdr_process_irp(s);
				break;

			case PAKID_CORE_SERVER_CAPABILITY:
				/* server capabilities */
				LLOGLN(0, ("PAKID_CORE_SERVER_CAPABILITY"));
				//rdpdr_process_capabilities(s);
				//rdpdr_send_capabilities();
				break;

			default:
				//ui_unimpl(NULL, "RDPDR core component, packetID: 0x%02X\n", packetID);
				break;

		}
	}
	else if (component == RDPDR_COMPONENT_TYPE_PRINTING)
	{
		LLOGLN(0, ("RDPDR_COMPONENT_TYPE_PRINTING"));

		switch (packetID)
		{
			case PAKID_PRN_CACHE_DATA:
				LLOGLN(0, ("PAKID_PRN_CACHE_DATA"));
				//printercache_process(s);
				break;

			default:
				//ui_unimpl(NULL, "RDPDR printer component, packetID: 0x%02X\n", packetID);
				break;
		}
	}
	//else
		//ui_unimpl(NULL, "RDPDR component: 0x%02X packetID: 0x%02X\n", component, packetID);

	return 0;
}

/* process the linked list of data that has come in */
static int
thread_process_data(void)
{
	char * data;
	int data_size;
	struct data_in_item * item;

	while (1)
	{
		pthread_mutex_lock(g_mutex);

		if (g_list_head == 0)
		{
			pthread_mutex_unlock(g_mutex);
			break;
		}

		data = g_list_head->data;
		data_size = g_list_head->data_size;
		item = g_list_head;
		g_list_head = g_list_head->next;

		if (g_list_head == 0)
		{
			g_list_tail = 0;
		}

		pthread_mutex_unlock(g_mutex);
		if (data != 0)
		{
			thread_process_message(data, data_size);
			free(data);
		}
		if (item != 0)
		{
			free(item);
		}
	}

	return 0;
}

static void *
thread_func(void * arg)
{
	int listr[2];
	int numr;

	g_thread_status = 1;
	LLOGLN(10, ("thread_func: in"));

	while (1)
	{
		listr[0] = g_term_event.sock;
		listr[1] = g_data_in_event.sock;
		numr = 2;
		wait(-1, numr, listr);

		if (is_wait_obj_set(&g_term_event))
		{
			break;
		}
		if (is_wait_obj_set(&g_data_in_event))
		{
			clear_wait_obj(&g_data_in_event);
			/* process data in */
			thread_process_data();
		}
	}

	LLOGLN(10, ("thread_func: out"));
	g_thread_status = -1;
	return 0;
}

static void
OpenEventProcessReceived(uint32 openHandle, void * pData, uint32 dataLength,
	uint32 totalLength, uint32 dataFlags)
{
	int index;
	index = (openHandle == g_open_handle[0]) ? 0 : 1;

	LLOGLN(10, ("OpenEventProcessReceived: receive openHandle %d dataLength %d "
		"totalLength %d dataFlags %d",
		openHandle, dataLength, totalLength, dataFlags));

	if (dataFlags & CHANNEL_FLAG_FIRST)
	{
		g_data_in_read[index] = 0;
		if (g_data_in[index] != 0)
		{
			free(g_data_in[index]);
		}
		g_data_in[index] = (char *) malloc(totalLength);
		g_data_in_size[index] = totalLength;
	}

	memcpy(g_data_in[index] + g_data_in_read[index], pData, dataLength);
	g_data_in_read[index] += dataLength;

	if (dataFlags & CHANNEL_FLAG_LAST)
	{
		if (g_data_in_read[index] != g_data_in_size[index])
		{
			LLOGLN(0, ("OpenEventProcessReceived: read error"));
		}
		if (index == 0)
		{
			signal_data_in();
		}
	}
}

static void
OpenEvent(uint32 openHandle, uint32 event, void * pData, uint32 dataLength,
	uint32 totalLength, uint32 dataFlags)
{
	LLOGLN(10, ("OpenEvent: event %d", event));
	switch (event)
	{
		case CHANNEL_EVENT_DATA_RECEIVED:
			OpenEventProcessReceived(openHandle, pData, dataLength,
				totalLength, dataFlags);
			break;
		case CHANNEL_EVENT_WRITE_COMPLETE:
			free(pData);
			break;
	}
}

static void
InitEventProcessConnected(void * pInitHandle, void * pData, uint32 dataLength)
{
	uint32 error;
	pthread_t thread;

	if (pInitHandle != g_han)
	{
		LLOGLN(0, ("InitEventProcessConnected: error no match"));
	}
	error = g_ep.pVirtualChannelOpen(g_han, &(g_open_handle[0]),
		g_channel_def[0].name, OpenEvent);
	if (error != CHANNEL_RC_OK)
	{
		LLOGLN(0, ("InitEventProcessConnected: Open failed"));
	}
	error = g_ep.pVirtualChannelOpen(g_han, &(g_open_handle[1]),
		g_channel_def[1].name, OpenEvent);
	if (error != CHANNEL_RC_OK)
	{
		LLOGLN(0, ("InitEventProcessConnected: Open failed"));
	}
	pthread_create(&thread, 0, thread_func, 0);
	pthread_detach(thread);
}

static void
InitEventProcessTerminated(void)
{
	int index;

	set_wait_obj(&g_term_event);
	index = 0;
	while ((g_thread_status > 0) && (index < 100))
	{
		index++;
		usleep(250 * 1000);
	}
	deinit_wait_obj(&g_term_event);
	deinit_wait_obj(&g_data_in_event);
}

static void
InitEvent(void * pInitHandle, uint32 event, void * pData, uint32 dataLength)
{
	LLOGLN(10, ("InitEvent: event %d", event));
	switch (event)
	{
		case CHANNEL_EVENT_CONNECTED:
			InitEventProcessConnected(pInitHandle, pData, dataLength);
			break;
		case CHANNEL_EVENT_DISCONNECTED:
			break;
		case CHANNEL_EVENT_TERMINATED:
			InitEventProcessTerminated();
			break;
	}
}

int
VirtualChannelEntry(PCHANNEL_ENTRY_POINTS pEntryPoints)
{
	LLOGLN(10, ("VirtualChannelEntry:"));
	g_data_in_size[0] = 0;
	g_data_in_size[1] = 0;
	g_data_in[0] = 0;
	g_data_in[1] = 0;
	g_ep = *pEntryPoints;

	memset(&(g_channel_def[0]), 0, sizeof(g_channel_def));
	g_channel_def[0].options = CHANNEL_OPTION_INITIALIZED | CHANNEL_OPTION_ENCRYPT_RDP;
	strcpy(g_channel_def[0].name, "rdpdr");

	g_mutex = (pthread_mutex_t *) malloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(g_mutex, 0);
	g_list_head = 0;
	g_list_tail = 0;

	init_wait_obj(&g_term_event, "freerdprdpdrterm");
	init_wait_obj(&g_data_in_event, "freerdprdpdrdatain");

	g_thread_status = 0;

	g_ep.pVirtualChannelInit(&g_han, g_channel_def, 2,
		VIRTUAL_CHANNEL_VERSION_WIN2000, InitEvent);

	return 1;
}

