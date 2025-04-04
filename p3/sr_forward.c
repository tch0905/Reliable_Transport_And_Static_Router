#include "sr_forward.h"
#include "./sr.h"
#include "./sr_if.h"
#include "./sr_protocol.h"
#include "./sr_rt.h"
#include "sr_arp.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Packet debugging function
uint16_t debug_print_packet(uint8_t *buf, int len, const char *iface) {
  struct sr_ethernet_hdr *eth_hdr = (struct sr_ethernet_hdr *)buf;

  // Print basic packet info
  printf("\n=== Received %d byte packet @ %s ===\n", len, iface);

  // Parse Ethernet header
  printf("[L2] SRC MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
         eth_hdr->ether_shost[0], eth_hdr->ether_shost[1],
         eth_hdr->ether_shost[2], eth_hdr->ether_shost[3],
         eth_hdr->ether_shost[4], eth_hdr->ether_shost[5]);
  printf("    DST MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
         eth_hdr->ether_dhost[0], eth_hdr->ether_dhost[1],
         eth_hdr->ether_dhost[2], eth_hdr->ether_dhost[3],
         eth_hdr->ether_dhost[4], eth_hdr->ether_dhost[5]);
  printf("    EtherType: 0x%04x\n", ntohs(eth_hdr->ether_type));

  // Protocol-specific parsing
  if (ntohs(eth_hdr->ether_type) == ethertype_ip) {
    struct sr_ip_hdr *ip_hdr =
        (struct sr_ip_hdr *)(buf + sizeof(struct sr_ethernet_hdr));
    printf("[L3] IPv4 Version: %d  Header Len: %d bytes\n", ip_hdr->ip_v,
           ip_hdr->ip_hl * 4);
    printf("     SRC IP: %s\n", inet_ntoa(*(struct in_addr *)&ip_hdr->ip_src));
    printf("     DST IP: %s\n", inet_ntoa(*(struct in_addr *)&ip_hdr->ip_dst));
    printf("     TTL: %d  Protocol: %d\n", ip_hdr->ip_ttl, ip_hdr->ip_p);

    if (ip_hdr->ip_p == IPPROTO_ICMP) {
      struct sr_icmp_hdr *icmp_hdr = (struct sr_icmp_hdr *)(ip_hdr + 1);
      printf("[L4] ICMP Type: %d  Code: %d\n", icmp_hdr->icmp_type,
             icmp_hdr->icmp_code);
    }
  } else if (ntohs(eth_hdr->ether_type) == ethertype_arp) {
    struct sr_arp_hdr *arp_hdr =
        (struct sr_arp_hdr *)(buf + sizeof(struct sr_ethernet_hdr));
    printf("[ARP] Operation: %s\n",
           ntohs(arp_hdr->ar_op) == ARPOP_REQUEST ? "Request" : "Reply");
    printf("      Sender IP: %s\n",
           inet_ntoa(*(struct in_addr *)&arp_hdr->ar_sip));
    printf("      Target IP: %s\n",
           inet_ntoa(*(struct in_addr *)&arp_hdr->ar_tip));
  }

  printf("--------------------------------\n");
  return eth_hdr->ether_type;
}

uint16_t checksum(void *data, size_t len) {
  uint32_t sum = 0;
  uint16_t *ptr = (uint16_t *)data;
  size_t num_words = len / 2;

  for (size_t i = 0; i < num_words; i++) {
    sum += ptr[i];
  }

  if (len % 2 != 0) {
    sum += ((uint8_t *)ptr)[len - 1] << 8;
  }

  while (sum >> 16) {
    sum = (sum & 0xFFFF) + (sum >> 16);
  }

  return (uint16_t)~sum;
}

void get_src_mac_by_iface(struct sr_instance *sr, const char *target_iface,
                          uint8_t *mac) {
  struct sr_interface *target = NULL;

  for (int i = 0; i < SR_NUM_INTERFACES; i++) {
    if (strcmp(sr->ifaces[i].name, target_iface) == 0) {
      target = &sr->ifaces[i];
      break;
    }
  }

  if (target) {
    memcpy(mac, target->mac_addr, ETH_ALEN);
  } else {
    memset(mac, 0, ETH_ALEN);
  }
}

const char *sr_handlepacket(struct sr_instance *sr, uint8_t *packet, int len,
                            const char *recv_iface) {
  if (len < sizeof(sr_ethernet_hdr_t)) {
    return NULL;
  }

  sr_ethernet_hdr_t *eth_hdr = (sr_ethernet_hdr_t *)packet;
  if (ntohs(eth_hdr->ether_type) != ethertype_ip) {
    return NULL;
  }

  if (len < sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t)) {
    return NULL;
  }

  sr_ip_hdr_t *ip_hdr = (sr_ip_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));

  if (ip_hdr->ip_v != 4 || ip_hdr->ip_hl < 5) {
    return NULL;
  }

  uint16_t ip_header_len = ip_hdr->ip_hl * 4;
  uint16_t ip_total_len = ntohs(ip_hdr->ip_len);

  if (ip_total_len < ip_header_len ||
      (len - sizeof(sr_ethernet_hdr_t)) < ip_total_len) {
    return NULL;
  }

  uint16_t original_checksum = ip_hdr->ip_sum;
  ip_hdr->ip_sum = 0;
  uint16_t computed_checksum = checksum(ip_hdr, ip_header_len);
  if (original_checksum != computed_checksum) {
    ip_hdr->ip_sum = original_checksum;
    return NULL;
  }

  // Decrement TTL
  ip_hdr->ip_ttl--;
  if (ip_hdr->ip_ttl == 0) {
    fprintf(stderr, "TTL expired. Dropping packet.\n");
    return NULL;
  }

  ip_hdr->ip_sum = checksum(ip_hdr, ip_header_len);

  struct in_addr dest_ip;
  dest_ip.s_addr = ip_hdr->ip_dst;

  struct sr_rt *best_rt = NULL;
  uint32_t best_mask = 0;

  // Perform Longest Prefix Match (LPM)
  for (struct sr_rt *rt = sr->routing_table; rt != NULL; rt = rt->next) {
    // Match destination IP with prefix using the mask
    if ((dest_ip.s_addr & rt->mask.s_addr) ==
        (rt->dest.s_addr & rt->mask.s_addr)) {
      uint32_t curr_mask = ntohl(rt->mask.s_addr);

      // Update the best match if this route has a longer prefix
      if (curr_mask >= best_mask) {
        best_mask = curr_mask;
        best_rt = rt;
      }
    }
  }

  if (!best_rt) {
    return NULL;
  }

  struct in_addr next_hop = best_rt->gw;
  struct sr_arpcache *arp_entry = NULL;
  for (struct sr_arpcache *entry = sr->arp_cache; entry != NULL;
       entry = entry->next) {
    if (entry->ip.s_addr == next_hop.s_addr) {
      arp_entry = entry;
      break;
    }
  }

  if (!arp_entry) {
    return NULL;
  }

  uint8_t src_mac[ETH_ALEN];
  get_src_mac_by_iface(sr, best_rt->interface, src_mac);

  memcpy(eth_hdr->ether_shost, src_mac, ETH_ALEN);
  memcpy(eth_hdr->ether_dhost, arp_entry->mac_addr, ETH_ALEN);

  return best_rt->interface;
}
