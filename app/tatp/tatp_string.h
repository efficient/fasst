#ifndef TATP_STRING_H
#define TATP_STRING_H

#include <string>
#include <assert.h>
#include "hots.h"

/* A 64-bit encoding for 15-character decimal strings. */
union tatp_sub_nbr_t {
	struct {
		uint32_t dec_0 :4;
		uint32_t dec_1 :4;
		uint32_t dec_2 :4;
		uint32_t dec_3 :4;
		uint32_t dec_4 :4;
		uint32_t dec_5 :4;
		uint32_t dec_6 :4;
		uint32_t dec_7 :4;
		uint32_t dec_8 :4;
		uint32_t dec_9 :4;
		uint32_t dec_10 :4;
		uint32_t dec_11 :4;
		uint32_t dec_12 :4;
		uint32_t dec_13 :4;
		uint32_t dec_14 :4;
		uint32_t dec_15 :4;
	};

	struct {
		uint64_t dec_0_1_2 :12;
		uint64_t dec_3_4_5 :12;
		uint64_t dec_6_7_8 :12;
		uint64_t dec_9_10_11 :12;
		uint64_t unused :16;
	};

	hots_key_t hots_key;
};
static_assert(sizeof(tatp_sub_nbr_t) == sizeof(hots_key_t), "");

/* Debug-only */
static std::string tatp_sub_nbr_to_string(tatp_sub_nbr_t sub_nbr)
{
	std::string ret;
	ret += std::to_string(sub_nbr.dec_14);
	ret += std::to_string(sub_nbr.dec_13);
	ret += std::to_string(sub_nbr.dec_12);
	ret += std::to_string(sub_nbr.dec_11);
	ret += std::to_string(sub_nbr.dec_10);
	ret += std::to_string(sub_nbr.dec_9);
	ret += std::to_string(sub_nbr.dec_8);
	ret += std::to_string(sub_nbr.dec_7);
	ret += std::to_string(sub_nbr.dec_6);
	ret += std::to_string(sub_nbr.dec_5);
	ret += std::to_string(sub_nbr.dec_4);
	ret += std::to_string(sub_nbr.dec_3);
	ret += std::to_string(sub_nbr.dec_2);
	ret += std::to_string(sub_nbr.dec_1);
	ret += std::to_string(sub_nbr.dec_0);

	printf("ret = %s\n", ret.c_str());
	return ret;
}

#endif
