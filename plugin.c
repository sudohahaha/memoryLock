#include <nfp/mem_atomic.h>

#include <pif_plugin.h>

//#include <pkt_ops.h>

#include <pif_headers.h>

#include <nfp_override.h>

#include <pif_common.h>

#include <std/hash.h>

#include <nfp/me.h>


#define BUCKET_SIZE 16

#define STATE_TABLE_SIZE 0xF /* 16777200 state table entries available */

volatile __emem __export uint32_t global_semaphores[STATE_TABLE_SIZE + 1] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

void semaphore_down(volatile __declspec(mem addr40) void * addr) {
	unsigned int addr_hi, addr_lo;
	__declspec(read_write_reg) int xfer;
	SIGNAL_PAIR my_signal_pair;
	addr_hi = ((unsigned long long int)addr >> 8) & 0xff000000;
	addr_lo = (unsigned long long int)addr & 0xffffffff;
	do {
		xfer = 1;
		__asm {
            mem[test_subsat, xfer, addr_hi, <<8, addr_lo, 1],\
                sig_done[my_signal_pair];
            ctx_arb[my_signal_pair]
        }
	    sleep(500);
	} while (xfer == 0);
}
void semaphore_up(volatile __declspec(mem addr40) void * addr) {
	unsigned int addr_hi, addr_lo;
	__declspec(read_write_reg) int xfer;
	addr_hi = ((unsigned long long int)addr >> 8) & 0xff000000;
	addr_lo = (unsigned long long int)addr & 0xffffffff;
    __asm {
        mem[incr, --, addr_hi, <<8, addr_lo, 1];
    }
}

typedef struct bucket_entry_info {
    uint32_t hit_count; /* for timeouts */
} bucket_entry_info;

typedef struct bucket_entry {
    uint32_t key[3]; /* ip1, ip2, ports */
    bucket_entry_info bucket_entry_info_value;
}bucket_entry;

typedef struct bucket_list {
    struct bucket_entry entry[BUCKET_SIZE];
}bucket_list;

__shared __export __addr40 __emem bucket_list state_hashtable[STATE_TABLE_SIZE + 1];

int pif_plugin_state_update(EXTRACTED_HEADERS_T *headers,
                        MATCH_DATA_T *match_data)
{
    PIF_PLUGIN_ipv4_T *ipv4;
    PIF_PLUGIN_udp_T *udp;
    volatile uint32_t update_hash_value;
    uint32_t update_hash_key[3];
    __xread uint32_t hash_key_r[3];
    __addr40 __emem bucket_entry_info *b_info;
    __xwrite bucket_entry_info tmp_b_info;
    __addr40 uint32_t *key_addr;
    __xrw uint32_t key_val_rw[3];

    uint32_t i = 0;
    ipv4 = pif_plugin_hdr_get_ipv4(headers);
    udp = pif_plugin_hdr_get_udp(headers);
    
    update_hash_key[0] = ipv4->srcAddr;
    update_hash_key[1] = ipv4->dstAddr;
    update_hash_key[2] = (udp->srcPort << 16) | udp->dstPort;

    key_val_rw[0] = ipv4->srcAddr;
    key_val_rw[1] = ipv4->dstAddr;
    key_val_rw[2] = (udp->srcPort << 16) | udp->dstPort;

    update_hash_value = hash_me_crc32((void *)update_hash_key,sizeof(update_hash_key), 1);
    update_hash_value &= (STATE_TABLE_SIZE);

    semaphore_down(&global_semaphores[update_hash_value]);
    //
    for (i = 0; i < BUCKET_SIZE; i++) {
        mem_read_atomic(hash_key_r, state_hashtable[update_hash_value].entry[i].key, sizeof(hash_key_r)); /* TODO: Read whole bunch at a time */
        //if (hash_key_r[0] == 0) {
        //    b_info = &state_hashtable[update_hash_value].entry[i].bucket_entry_info_value;
        //    key_addr =(__addr40 uint32_t *) state_hashtable[update_hash_value].entry[i].key;
	    
        //    tmp_b_info.hit_count = 1;
        //    mem_write_atomic(&tmp_b_info, b_info, sizeof(tmp_b_info));
        //    mem_write_atomic(key_val_rw,(__addr40 void *)key_addr, sizeof(key_val_rw));
	//    break;
        //}
        if (hash_key_r[0] == update_hash_key[0] &&
            hash_key_r[1] == update_hash_key[1] &&
            hash_key_r[2] == update_hash_key[2] ) { /* Hit */
            __xrw uint32_t count;
            b_info = &state_hashtable[update_hash_value].entry[i].bucket_entry_info_value;
            count = 1;
            mem_test_add(&count,(__addr40 void *)&b_info->hit_count, 1 << 2);
            if (count == 0xFFFFFFFF-1) { /* Never incr to 0 or 2^32 */
                count = 2;
                mem_add32(&count,(__addr40 void *)&b_info->hit_count, 1 << 2);
            } else if (count == 0xFFFFFFFF) {
                mem_incr32((__addr40 void *)&b_info->hit_count);
            }
	    break;
        }
	else if (hash_key_r[0] == 0) {
            b_info = &state_hashtable[update_hash_value].entry[i].bucket_entry_info_value;
            key_addr =(__addr40 uint32_t *) state_hashtable[update_hash_value].entry[i].key;

            tmp_b_info.hit_count = 1;
            mem_write_atomic(&tmp_b_info, b_info, sizeof(tmp_b_info));
            mem_write_atomic(key_val_rw,(__addr40 void *)key_addr, sizeof(key_val_rw));
            break;
        }
    }
    semaphore_up(&global_semaphores[update_hash_value]);

    if (i == BUCKET_SIZE)
	return PIF_PLUGIN_RETURN_FORWARD;

    //tmp_b_info.hit_count = 1;
    //mem_write_atomic(&tmp_b_info, b_info, sizeof(tmp_b_info));
    //mem_write_atomic(key_val_rw,(__addr40 void *)key_addr, sizeof(key_val_rw));
    return PIF_PLUGIN_RETURN_FORWARD;

}


int pif_plugin_lookup_state(EXTRACTED_HEADERS_T *headers, MATCH_DATA_T *match_data) {
    if(__ctx() > 0){
        PIF_PLUGIN_ipv4_T *ipv4;
        PIF_PLUGIN_udp_T *udp;
        volatile uint32_t hash_value;
        uint32_t  hash_key[3];
        __xread uint32_t hash_key_r[3];
        __addr40 bucket_entry_info *b_info;
        uint32_t i;

        ipv4 = pif_plugin_hdr_get_ipv4(headers);
        udp = pif_plugin_hdr_get_udp(headers);

        hash_key[0] = ipv4->srcAddr;
        hash_key[1] = ipv4->dstAddr;
        hash_key[2] = (udp->srcPort << 16) | udp->dstPort;
        hash_value = hash_me_crc32((void *) hash_key,sizeof(hash_key), 1);
        hash_value &= (STATE_TABLE_SIZE);
        for (i = 0; i < BUCKET_SIZE; i++) {
            mem_read_atomic(hash_key_r, state_hashtable[hash_value].entry[i].key, sizeof(hash_key_r)); /* TODO: Read whole bunch at a time */
            if (hash_key_r[0] == 0) {
                continue;
            }
            if (hash_key_r[0] == hash_key[0] &&
                hash_key_r[1] == hash_key[1] &&
                hash_key_r[2] == hash_key[2] ) { /* Hit */
                __xrw uint32_t count;
                b_info = (__addr40 bucket_entry_info *)&state_hashtable[hash_value].entry[i].bucket_entry_info_value;
                count = 1;
                mem_test_add(&count,(__addr40 void *)&b_info->hit_count, 1 << 2);
                if (count == 0xFFFFFFFF-1) { /* Never incr to 0 or 2^32 */
                    count = 2;
                    mem_add32(&count,(__addr40 void *)&b_info->hit_count, 1 << 2);
                } else if (count == 0xFFFFFFFF) {
                    mem_incr32((__addr40 void *)&b_info->hit_count);
                }
                return PIF_PLUGIN_RETURN_FORWARD;
            }
        }
      if (pif_plugin_state_update(headers, match_data) == PIF_PLUGIN_RETURN_DROP) {
            return PIF_PLUGIN_RETURN_DROP;
        }
        return PIF_PLUGIN_RETURN_FORWARD;
    }

}

