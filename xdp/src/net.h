#pragma once

#include "../common.h"
#include "utils.h"

#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/types.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int setup_socket (void);

/**
 * Construct the base structure of the packet inside buf by writing the ethernet and ip headers
 * to the given buffer.
 *
 * @param buf the buffer to write the headers to
 * @param src_mac the source mac address
 * @param dest_mac the destination mac address
 * @param src_ip the source ip address
 * @param dest_ip the destination ip address
 * @return 0 on success, -1 on failure
 */
int build_base_packet (char *buf, const uint8_t *src_mac, const uint8_t *dest_mac,
                       const char *src_ip, const char *dest_ip);

/**
 * Set the id of the payload inside buf.
 * This function both modifies the pingpong payload id and the ip header id.
 *
 * @return 0 on success, -1 on failure
 */
int set_packet_payload (char *buf, struct pingpong_payload *payload);

/**
 * Send a pingpong packet to the remote node.
 *
 * @param sock the socket to use to send the packet
 * @param buf the buffer to be sent
 * @param ifindex the interface index to send the packet from
 * @param dest_mac the destination mac address
 * @return 0 on success, -1 on failure
 */
int send_pingpong_packet (int sock, const char *buf, int ifindex, const uint8_t *dest_mac);
