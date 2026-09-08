#include "pdu.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int address_end(const unsigned char* pdu)
{
	int submit = 1 + pdu[0];
	return submit + 4 + (pdu[submit + 2] + 1) / 2;
}

int main(void)
{
	unsigned char pdu[SMS_MAX_PDU_LENGTH];
	int encoding, units, length, fields, i;
	char limit[214];
	const char unicode[] = "Xin ch\xC3\xA0o b\xE1\xBA\xA1n";
	const unsigned char unicode_payload[] = {
		0x00,0x58,0x00,0x69,0x00,0x6E,0x00,0x20,
		0x00,0x63,0x00,0x68,0x00,0xE0,0x00,0x6F,
		0x00,0x20,0x00,0x62,0x1E,0xA1,0x00,0x6E
	};
	const char euro[] = "\xE2\x82\xAC";
	const char emoji[] = "\xF0\x9F\x98\x80";
	const char invalid[] = "\xC0\x80";

	length = pdu_encode_ex("", "191", "KTTK", pdu, sizeof(pdu), &encoding, &units);
	assert(length > 0 && encoding == PDU_TEXT_GSM7 && units == 4);
	fields = address_end(pdu);
	assert(pdu[fields + 1] == 0x00);

	length = pdu_encode_ex("", "191", euro, pdu, sizeof(pdu), &encoding, &units);
	assert(length > 0 && encoding == PDU_TEXT_GSM7 && units == 2);

	length = pdu_encode_ex("", "191", unicode, pdu, sizeof(pdu), &encoding, &units);
	assert(length > 0 && encoding == PDU_TEXT_UCS2 && units == 12);
	fields = address_end(pdu);
	assert(pdu[fields + 1] == 0x08);
	assert(pdu[fields + 3] == (int)sizeof(unicode_payload));
	assert(memcmp(pdu + fields + 4, unicode_payload, sizeof(unicode_payload)) == 0);

	for (i = 0; i < 70; i++) {
		limit[i * 3] = (char)0xE7;
		limit[i * 3 + 1] = (char)0x95;
		limit[i * 3 + 2] = (char)0x8C;
	}
	limit[210] = '\0';
	assert(pdu_encode_ex("", "191", limit, pdu, sizeof(pdu), &encoding, &units) > 0);
	assert(encoding == PDU_TEXT_UCS2 && units == 70);
	limit[210] = (char)0xE7; limit[211] = (char)0x95;
	limit[212] = (char)0x8C; limit[213] = '\0';
	assert(pdu_encode("", "191", limit, pdu, sizeof(pdu)) < 0);
	assert(pdu_encode("", "191", emoji, pdu, sizeof(pdu)) < 0);
	assert(pdu_encode("", "191", invalid, pdu, sizeof(pdu)) < 0);

	puts("PASS: GSM-7 and UCS-2 PDU encoding");
	return 0;
}
