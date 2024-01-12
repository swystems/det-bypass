#pragma once

#include "../common.h"
#include "utils.h"

#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/ip.h>
#include <linux/types.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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
 * @param buf the buffer containing the packet
 * @param id the id to set
 * @return 0 on success, -1 on failure
 */
int setup_packet_payload (char *buf, uint32_t id);

/**
 * Build a sockaddr_ll structure with the given ifindex and destination mac address.
 * This structure is used to send packets to the remote node.
 *
 * @param ifindex the interface index
 * @param dest_mac the destination mac address
 * @return the constructed sockaddr_ll structure
 */
struct sockaddr_ll build_sockaddr (int ifindex, const char *dest_mac);

/**
 * Send `iters` pingpong packets to the remote server.
 *
 * @param ifindex the interface index
 * @param server_ip the ip address of the remote server
 */
void send_packets (int ifindex, const char *server_ip, uint64_t iters, uint64_t interval);
