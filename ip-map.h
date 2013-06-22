#ifndef __IP_MAP_H_
#define __IP_MAP_H_

#include "common.h"

struct map_queue {
	int fd;
	uint16_t port;
	ssize_t size_r;
	uint32_t ip;
	ssize_t size_w;
};

void ip_map_init();
void ip_map_close();
uint32_t get_ip(uint16_t port);
void add_ip(uint16_t port, uint32_t ip);
void remove_ip(uint16_t port);
void add_ip_fd(int portfd, int ipfd);
void remove_ip_fd(int portfd);
int handle_connection(struct map_queue *q);
struct map_queue new_map_queue(int fd);
#endif
