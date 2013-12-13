/*
 *
 * Copyright © 2013 Serge Hallyn
 * Author: Serge Hallyn <serge.hallyn@ubuntu.com>
 *
 * based on cgroup.c from
 * lxc: linux Container library
 * (C) Copyright IBM Corp. 2007, 2008
 * Authors:
 * Daniel Lezcano <daniel.lezcano at free.fr>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sched.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <sys/param.h>
#include <stdbool.h>
#include <dirent.h>

#include <nih/macros.h>
#include <nih/alloc.h>
#include <nih/string.h>
#include <nih/io.h>
#include <nih/option.h>
#include <nih/main.h>
#include <nih/logging.h>
#include <nih/error.h>

#include <nih-dbus/dbus_connection.h>
#include <nih-dbus/dbus_proxy.h>

#include "fs.h"

extern bool setns_pid_supported;
extern unsigned long mypidns;

/*
 * Get a pid passed in a SCM_CREDENTIAL over a unix socket
 * @sock: the socket fd.
 * Credentials are invalid of *p == 1.
 */
void get_scm_creds(int sock, uid_t *u, gid_t *g, pid_t *p)
{
        struct msghdr msg = { 0 };
        struct iovec iov;
        struct cmsghdr *cmsg;
	struct ucred cred;
        char cmsgbuf[CMSG_SPACE(sizeof(cred))];
        char buf[1];
	int ret, tries=0;

	cred.pid = -1;
	cred.uid = -1;
	cred.gid = -1;
        msg.msg_name = NULL;
        msg.msg_namelen = 0;
        msg.msg_control = cmsgbuf;
        msg.msg_controllen = sizeof(cmsgbuf);

        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

	// retry logic is not ideal, especially as we are not
	// threaded.  Sleep at most 1 second waiting for the client
	// to send us the scm_cred
again:
	ret = recvmsg(sock, &msg, 0);
	if (ret < 0) {
		if (tries++ == 0 && errno == EAGAIN) {
			nih_info("got EAGAIN for scm_cred");
			sleep(1);
			goto again;
		}
		nih_error("Failed to receive scm_cred: %s",
			  strerror(errno));
		goto out;
	}

        cmsg = CMSG_FIRSTHDR(&msg);

        if (cmsg && cmsg->cmsg_len == CMSG_LEN(sizeof(struct ucred)) &&
            cmsg->cmsg_level == SOL_SOCKET &&
            cmsg->cmsg_type == SCM_CREDENTIALS) {
		memcpy(&cred, CMSG_DATA(cmsg), sizeof(cred));
        }
out:
	*u = cred.uid;
	*g = cred.gid;
	*p = cred.pid;
        return;
}

pid_t get_scm_pid(int sock)
{
	pid_t p;
	uid_t u;
	gid_t g;

	get_scm_creds(sock, &u, &g, &p);
	return p;
}

int send_pid(int sock, int pid)
{
	struct msghdr msg = { 0 };
	struct iovec iov;
	struct cmsghdr *cmsg;
	struct ucred cred = {
		.pid = pid,
		.uid = geteuid(),
		.gid = getegid(),
	};
	char cmsgbuf[CMSG_SPACE(sizeof(cred))];
	char buf[1];
	buf[0] = 'p';

	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_len = CMSG_LEN(sizeof(struct ucred));
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_CREDENTIALS;
	memcpy(CMSG_DATA(cmsg), &cred, sizeof(cred));

	msg.msg_name = NULL;
	msg.msg_namelen = 0;

	iov.iov_base = buf;
	iov.iov_len = sizeof(buf);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	if (sendmsg(sock, &msg, 0) < 0) {
		perror("sendmsg");
		return -1;
	}
	return 0;
}

/*
 * Return true if pid is in my pidns
 * Figure this out by comparing the /proc/pid/ns/pid link names.
 */
bool is_same_pidns(int pid)
{
	if (!setns_pid_supported)
		return false;
	if (read_pid_ns_link(pid) != mypidns)
		return false;
	return true;
}

/*
 * May the requestor @r move victim @v to a new cgroup?
 * This is allowed if
 *   . they are the same task
 *   . they are ownedy by the same uid
 *   . @r is root on the host, or
 *   . @v's uid is mapped into @r's where @r is root.
 *
 * XXX do we want to add a restriction that @v must already
 * be under @r's cgroup?
 */
bool may_move_pid(pid_t r, uid_t r_uid, pid_t v)
{
	uid_t v_uid;
	gid_t v_gid;

	if (r == v)
		return true;
	if (r_uid == 0)
		return true;
	get_pid_creds(v, &v_uid, &v_gid);
	if (r_uid == v_uid)
		return true;
	if (hostuid_to_ns(r_uid, r) == 0 && hostuid_to_ns(v_uid, r) != -1)
		return true;
	return false;
}

