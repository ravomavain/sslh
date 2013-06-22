/*
# ip-map.c: Code for port <-> ip association map
#
# Copyright (C) 2013  Romain Labolle
# 
# This program is free software; you can redistribute it
# and/or modify it under the terms of the GNU General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later
# version.
# 
# This program is distributed in the hope that it will be
# useful, but WITHOUT ANY WARRANTY; without even the implied
# warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
# PURPOSE.  See the GNU General Public License for more
# details.
# 
# The full text for the General Public License is here:
# http://www.gnu.org/licenses/gpl.html
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include "ip-map.h"

typedef struct map {
	uint16_t port;
	uint32_t ip;
	int next;
} map_t;

int ip_map_id = -1;
int semid = -1;

int new_shared_map()
{
	int id = shmget(IPC_PRIVATE, sizeof(map_t), IPC_CREAT | IPC_EXCL | 0600);
	CHECK_RES_DIE(id, "shmget");
	map_t *m = shmat(id, NULL, 0);
	m->ip = 0;
	m->port = 0;
	m->next = -1;
	shmdt(m);
	return id;
}

void ip_map_init()
{
	ip_map_id = new_shared_map();
	semid = semget(IPC_PRIVATE, 1, IPC_CREAT | IPC_EXCL | 0666);
	CHECK_RES_DIE(semid, "semget");
	int res = semctl(semid, 0, SETVAL, 1);
	CHECK_RES_DIE(res, "semctl");
	if (verbose) fprintf(stderr, "Port<->IP map initialized.\n");
}

int sem_op(int op)
{
	struct sembuf sb;
	sb.sem_num = 0;
    sb.sem_op = op;
    sb.sem_flg = SEM_UNDO;
    if(semid == -1)
    	return semid;
	return semop(semid, &sb, 1);
}

int sem_lock()
{
	return sem_op(-1);
}

int sem_unlock()
{
	return sem_op(1);
}

void ip_map_close()
{
	int next = ip_map_id;
	int delid;
	map_t *m;
	sem_lock();
	while(next >= 0)
	{
		m = shmat(next, NULL, 0);
		delid = next;
		next = m->next;
		shmdt(m);
		shmctl(delid, IPC_RMID, NULL);
	}
	sem_unlock();
	semctl(semid, 0, IPC_RMID);
	if (verbose) fprintf(stderr, "Port<->IP map closed.\n");
}

uint32_t get_ip(const uint16_t port)
{
	int next = ip_map_id;
	uint32_t ip;
	map_t *m;
	if(sem_lock() == -1)
		return 0;
	while(next >= 0)
	{
		m = shmat(next, NULL, 0);
		if(m->port > port)
		{
			shmdt(m);
			sem_unlock();
			return 0;
		}
		if(m->port == port)
		{
			if (verbose) fprintf(stderr, "got %u->%u from ip map\n", port, m->ip);
			ip = m->ip;
			shmdt(m);
			sem_unlock();
			return ip;
		}
		next = m->next;
		shmdt(m);
	}
	sem_unlock();
	return 0;
}

void add_ip(uint16_t port, uint32_t ip)
{
	int next = ip_map_id;
	int prev = -1;
	int n;
	map_t *m;
	if(ip == 0xFFFFFFFF || sem_lock() == -1)
		return;
	while(next >= 0)
	{
		m = shmat(next, NULL, 0);
		if(m->port > port)
		{
			shmdt(m);
			break;
		}
		if(m->port == port)
		{
			m->ip = ip;
			if (verbose) fprintf(stderr, "updated %u->%u to ip map\n", port, ip);
			shmdt(m);
			sem_unlock();
			return;
		}
		prev = next;
		next = m->next;
		shmdt(m);
	}
	n = new_shared_map();
	m = shmat(n, NULL, 0);
	m->port = port;
	m->ip = ip;
	m->next = next;
	shmdt(m);
	m = shmat(prev, NULL, 0);
	m->next = n;
	shmdt(m);
	sem_unlock();
	if (verbose) fprintf(stderr, "added %u->%u to ip map\n", port, ip);
}

void remove_ip(uint16_t port)
{
	int next = ip_map_id;
	int delid;
	int prev = -1;
	map_t *m;
	if(sem_lock() == -1)
		return;
	while(next >= 0)
	{
		m = shmat(next, NULL, 0);
		if(m->port > port)
		{
			shmdt(m);
			sem_unlock();
			return;
		}
		if(m->port == port)
		{
			if(prev) {
				delid = next;
				next = m->next;
				shmdt(m);
				shmctl(delid, IPC_RMID, NULL);
				m = shmat(prev, NULL, 0);
				m->next = next;
			}
			shmdt(m);
			sem_unlock();
			if (verbose) fprintf(stderr, "removed %u from ip map\n", port);
			return;
		}
		prev = next;
		next = m->next;
		shmdt(m);
	}
	sem_unlock();
}

uint32_t fd2ip(int fd)
{
	socklen_t len;
	struct sockaddr_storage addr;
	len = sizeof addr;

	getpeername(fd, (struct sockaddr*)&addr, &len);

	return ntohl(((struct sockaddr_in *)&addr)->sin_addr.s_addr);
}

uint16_t fd2port(int fd)
{
	socklen_t len;
	struct sockaddr_storage addr;
	len = sizeof addr;

	getsockname(fd, (struct sockaddr*)&addr, &len);

	return ntohs(((struct sockaddr_in *)&addr)->sin_port);
}

void add_ip_fd(int portfd, int ipfd)
{
	add_ip(fd2port(portfd), fd2ip(ipfd));
}

void remove_ip_fd(int portfd)
{
	remove_ip(fd2port(portfd));
}

int handle_connection(struct map_queue *q)
{
	char *buf = (char*)&q->port;
	ssize_t size_r, size_w;
	while(q->size_r < sizeof(q->port))
	{
		size_r = read(q->fd, buf + q->size_r, sizeof(q->port) - q->size_r);
		if (size_r == -1) {
			switch (errno) {
				case EAGAIN:
					return FD_NODATA;

				case ECONNRESET:
				case EPIPE:
					return FD_CNXCLOSED;
			}
		}
		CHECK_RES_RETURN(size_r, "read");
		if (size_r == 0)
			return FD_CNXCLOSED;
		q->size_r += size_r;
	}

	if (verbose) fprintf(stderr, "request fd %d: %d\n", q->fd, ntohs(q->port));

	if(q->ip == 0xFFFFFFFF)
		q->ip = htonl(get_ip(ntohs(q->port)));

	if (verbose) fprintf(stderr, "got ip %u associated with port %u\n", ntohl(q->ip), ntohs(q->port));

	buf = (char*)&q->ip;
	while(q->size_w < sizeof(q->ip))
	{
		size_w = write(q->fd, buf + q->size_w, sizeof(q->ip) - q->size_w);
		if (size_w == -1) {
			switch (errno) {
				case EAGAIN:
					return FD_STALLED;

				case ECONNRESET:
				case EPIPE:
					return FD_CNXCLOSED;
			}
		}
		CHECK_RES_RETURN(size_w, "write");
		q->size_w += size_w;
	}
	q->size_r = 0;
	q->size_w = 0;
	q->ip = 0xFFFFFFFF;
	return 1;
}

struct map_queue new_map_queue(int fd)
{
	struct map_queue q;
	memset(&q, 0, sizeof(struct map_queue));
	q.ip = 0xFFFFFFFF;
	q.fd = fd;
	return q;
}
