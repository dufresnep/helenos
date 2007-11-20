/*
 * Copyright (c) 2007 Jakub Jermar
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup fs
 * @{
 */ 

/**
 * @file	vfs_register.c
 * @brief	VFS_REGISTER method.
 */

#include <ipc/ipc.h>
#include <ipc/services.h>
#include <async.h>
#include <fibril.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <bool.h>
#include <futex.h>
#include <as.h>
#include <libadt/list.h>
#include <assert.h>
#include "vfs.h"

atomic_t fs_head_futex = FUTEX_INITIALIZER;
link_t fs_head;

atomic_t fs_handle_next = {
	.count = 1
};

/** Verify the VFS info structure.
 *
 * @param info		Info structure to be verified.
 *
 * @return		Non-zero if the info structure is sane, zero otherwise.
 */
static bool vfs_info_sane(vfs_info_t *info)
{
	int i;

	/*
	 * Check if the name is non-empty and is composed solely of ASCII
	 * characters [a-z]+[a-z0-9_-]*.
	 */
	if (!islower(info->name[0])) {
		dprintf("The name doesn't start with a lowercase character.\n");
		return false;
	}
	for (i = 1; i < FS_NAME_MAXLEN; i++) {
		if (!(islower(info->name[i]) || isdigit(info->name[i])) &&
		    (info->name[i] != '-') && (info->name[i] != '_')) {
			if (info->name[i] == '\0') {
				break;
			} else {
				dprintf("The name contains illegal "
				    "characters.\n");
				return false;
			}
		}
	}
	

	/*
	 * Check if the FS implements mandatory VFS operations.
	 */
	if (info->ops[IPC_METHOD_TO_VFS_OP(VFS_REGISTER)] != VFS_OP_DEFINED) {
		dprintf("Operation VFS_REGISTER not defined by the client.\n");
		return false;
	}
	if (info->ops[IPC_METHOD_TO_VFS_OP(VFS_MOUNT)] != VFS_OP_DEFINED) {
		dprintf("Operation VFS_MOUNT not defined by the client.\n");
		return false;
	}
	if (info->ops[IPC_METHOD_TO_VFS_OP(VFS_UNMOUNT)] != VFS_OP_DEFINED) {
		dprintf("Operation VFS_UNMOUNT not defined by the client.\n");
		return false;
	}
	if (info->ops[IPC_METHOD_TO_VFS_OP(VFS_LOOKUP)] != VFS_OP_DEFINED) {
		dprintf("Operation VFS_LOOKUP not defined by the client.\n");
		return false;
	}
	if (info->ops[IPC_METHOD_TO_VFS_OP(VFS_OPEN)] != VFS_OP_DEFINED) {
		dprintf("Operation VFS_OPEN not defined by the client.\n");
		return false;
	}
	if (info->ops[IPC_METHOD_TO_VFS_OP(VFS_CLOSE)] != VFS_OP_DEFINED) {
		dprintf("Operation VFS_CLOSE not defined by the client.\n");
		return false;
	}
	if (info->ops[IPC_METHOD_TO_VFS_OP(VFS_READ)] != VFS_OP_DEFINED) {
		dprintf("Operation VFS_READ not defined by the client.\n");
		return false;
	}
	
	/*
	 * Check if each operation is either not defined, defined or default.
	 */
	for (i = VFS_FIRST; i < VFS_LAST; i++) {
		if ((info->ops[IPC_METHOD_TO_VFS_OP(i)] != VFS_OP_NULL) && 
		    (info->ops[IPC_METHOD_TO_VFS_OP(i)] != VFS_OP_DEFAULT) && 
		    (info->ops[IPC_METHOD_TO_VFS_OP(i)] != VFS_OP_DEFINED)) {
			dprintf("Operation info not understood.\n");
			return false;
		}
	}
	return true;
}

/** VFS_REGISTER protocol function.
 *
 * @param rid		Hash of the call with the request.
 * @param request	Call structure with the request.
 */
void vfs_register(ipc_callid_t rid, ipc_call_t *request)
{
	ipc_callid_t callid;
	ipc_call_t call;
	int rc;
	size_t size;

	dprintf("Processing VFS_REGISTER request received from %p.\n",
	    request->in_phone_hash);

	/*
	 * The first call has to be IPC_M_DATA_SEND in which we receive the
	 * VFS info structure from the client FS.
	 */
	if (!ipc_data_receive(&callid, NULL, &size)) {
		/*
		 * The client doesn't obey the same protocol as we do.
		 */
		dprintf("Receiving of VFS info failed.\n");
		ipc_answer_0(callid, EINVAL);
		ipc_answer_0(rid, EINVAL);
		return;
	}
	
	dprintf("VFS info received, size = %d\n", size);
	
	/*
	 * We know the size of the VFS info structure. See if the client
	 * understands this easy concept too.
	 */
	if (size != sizeof(vfs_info_t)) {
		/*
		 * The client is sending us something, which cannot be
		 * the info structure.
		 */
		dprintf("Received VFS info has bad size.\n");
		ipc_answer_0(callid, EINVAL);
		ipc_answer_0(rid, EINVAL);
		return;
	}

	/*
	 * Allocate and initialize a buffer for the fs_info structure.
	 */
	fs_info_t *fs_info;
	fs_info = (fs_info_t *) malloc(sizeof(fs_info_t));
	if (!fs_info) {
		dprintf("Could not allocate memory for FS info.\n");
		ipc_answer_0(callid, ENOMEM);
		ipc_answer_0(rid, ENOMEM);
		return;
	}
	link_initialize(&fs_info->fs_link);
		
	rc = ipc_data_deliver(callid, &fs_info->vfs_info, size);
	if (rc != EOK) {
		dprintf("Failed to deliver the VFS info into our AS, rc=%d.\n",
		    rc);
		free(fs_info);
		ipc_answer_0(callid, rc);
		ipc_answer_0(rid, rc);
		return;
	}

	dprintf("VFS info delivered.\n");
		
	if (!vfs_info_sane(&fs_info->vfs_info)) {
		free(fs_info);
		ipc_answer_0(callid, EINVAL);
		ipc_answer_0(rid, EINVAL);
		return;
	}
		
	futex_down(&fs_head_futex);

	/*
	 * Check for duplicit registrations.
	 */
	if (fs_name_to_handle(fs_info->vfs_info.name, false)) {
		/*
		 * We already register a fs like this.
		 */
		dprintf("FS is already registered.\n");
		futex_up(&fs_head_futex);
		free(fs_info);
		ipc_answer_0(callid, EEXISTS);
		ipc_answer_0(rid, EEXISTS);
		return;
	}

	/*
	 * Add fs_info to the list of registered FS's.
	 */
	dprintf("Inserting FS into the list of registered file systems.\n");
	list_append(&fs_info->fs_link, &fs_head);

	/*
	 * Now we want the client to send us the IPC_M_CONNECT_TO_ME call so
	 * that a callback connection is created and we have a phone through
	 * which to forward VFS requests to it.
	 */
	callid = async_get_call(&call);
	if (IPC_GET_METHOD(call) != IPC_M_CONNECT_TO_ME) {
		dprintf("Unexpected call, method = %d\n", IPC_GET_METHOD(call));
		list_remove(&fs_info->fs_link);
		futex_up(&fs_head_futex);
		free(fs_info);
		ipc_answer_0(callid, EINVAL);
		ipc_answer_0(rid, EINVAL);
		return;
	}
	fs_info->phone = IPC_GET_ARG3(call);
	ipc_answer_0(callid, EOK);

	dprintf("Callback connection to FS created.\n");

	/*
	 * The client will want us to send him the address space area with PLB.
	 */
	callid = async_get_call(&call);
	if (IPC_GET_METHOD(call) != IPC_M_AS_AREA_RECV) {
		dprintf("Unexpected call, method = %d\n", IPC_GET_METHOD(call));
		list_remove(&fs_info->fs_link);
		futex_up(&fs_head_futex);
		ipc_hangup(fs_info->phone);
		free(fs_info);
		ipc_answer_0(callid, EINVAL);
		ipc_answer_0(rid, EINVAL);
		return;
	}
	
	/*
	 * We can only send the client address space area PLB_SIZE bytes long.
	 */
	size = IPC_GET_ARG2(call);
	if (size != PLB_SIZE) {
		dprintf("Client suggests wrong size of PFB, size = %d\n", size);
		list_remove(&fs_info->fs_link);
		futex_up(&fs_head_futex);
		ipc_hangup(fs_info->phone);
		free(fs_info);
		ipc_answer_0(callid, EINVAL);
		ipc_answer_0(rid, EINVAL);
		return;
	}

	/*
	 * Commit to read-only sharing the PLB with the client.
	 */
	ipc_answer_2(callid, EOK, (ipcarg_t) plb,
	    (ipcarg_t) (AS_AREA_READ | AS_AREA_CACHEABLE));	

	dprintf("Sharing PLB.\n");

	/*
	 * That was it. The FS has been registered.
	 * In reply to the VFS_REGISTER request, we assign the client file
	 * system a global file system handle.
	 */
	fs_info->fs_handle = (int) atomic_postinc(&fs_handle_next);
	ipc_answer_1(rid, EOK, (ipcarg_t) fs_info->fs_handle);
	
	futex_up(&fs_head_futex);
	
	dprintf("\"%.*s\" filesystem successfully registered, handle=%d.\n",
	    FS_NAME_MAXLEN, fs_info->vfs_info.name, fs_info->fs_handle);
}

/** For a given file system handle, implement policy for allocating a phone.
 *
 * @param handle	File system handle.
 *
 * @return		Phone over which a multi-call request can be safely
 *			sent. Return 0 if no phone was found.
 */
int vfs_grab_phone(int handle)
{
	/*
	 * For now, we don't try to be very clever and very fast.
	 * We simply lookup the phone in the fs_head list. We currently don't
	 * open any additional phones (even though that itself would be pretty
	 * straightforward; housekeeping multiple open phones to a FS task would
	 * be more demanding). Instead, we simply take the respective
	 * phone_futex and keep it until vfs_release_phone().
	 */
	futex_down(&fs_head_futex);
	link_t *cur;
	fs_info_t *fs;
	for (cur = fs_head.next; cur != &fs_head; cur = cur->next) {
		fs = list_get_instance(cur, fs_info_t, fs_link);
		if (fs->fs_handle == handle) {
			futex_up(&fs_head_futex);
			/*
			 * For now, take the futex unconditionally.
			 * Oh yeah, serialization rocks.
			 * It will be up'ed in vfs_release_phone().
			 */
			futex_down(&fs->phone_futex);
			/*
			 * Avoid deadlock with other fibrils in the same thread
			 * by disabling fibril preemption.
			 */
			fibril_inc_sercount();
			return fs->phone; 
		}
	}
	futex_up(&fs_head_futex);
	return 0;
}

/** Tell VFS that the phone is in use for any request.
 *
 * @param phone		Phone to FS task.
 */
void vfs_release_phone(int phone)
{
	bool found = false;

	/*
	 * Undo the fibril_inc_sercount() done in vfs_grab_phone().
	 */
	fibril_dec_sercount();
	
	futex_down(&fs_head_futex);
	link_t *cur;
	for (cur = fs_head.next; cur != &fs_head; cur = cur->next) {
		fs_info_t *fs = list_get_instance(cur, fs_info_t, fs_link);
		if (fs->phone == phone) {
			found = true;
			futex_up(&fs_head_futex);
			futex_up(&fs->phone_futex);
			return;
		}
	}
	futex_up(&fs_head_futex);

	/*
	 * Not good to get here.
	 */
	assert(found == true);
}

/** Convert file system name to its handle.
 *
 * @param name		File system name.
 * @param lock		If true, the function will down and up the
 * 			fs_head_futex.
 *
 * @return		File system handle or zero if file system not found.
 */
int fs_name_to_handle(char *name, bool lock)
{
	int handle = 0;
	
	if (lock)
		futex_down(&fs_head_futex);
	link_t *cur;
	for (cur = fs_head.next; cur != &fs_head; cur = cur->next) {
		fs_info_t *fs = list_get_instance(cur, fs_info_t, fs_link);
		if (strcmp(fs->vfs_info.name, name) == 0) { /* XXX: strncmp() */
			handle = fs->fs_handle;
			break;
		}
	}
	if (lock)
		futex_up(&fs_head_futex);
	return handle;
}

/**
 * @}
 */ 
