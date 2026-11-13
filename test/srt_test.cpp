/* The .srt reader (runtime/srt.cpp) on its own: cues as GOG's files have them (CRLF, empty cues,
 * code page 850), UTF-8, a BOM, cues that are out of order, and which cue shows when. Built and run by
 * test/srt.test.ts. */
#include <cstdio>

#include "subtitle.h"

static int failed;
#define CHECK(x) \
	do { \
		if (!(x)) { \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
			failed++; \
		} \
	} while (0)

int main(void)
{
	/* GOG's German: CRLF, an empty cue before the first, code page 850 (0x81: u umlaut) */
	auto de = srt_parse("1\r\n00:01:36,866 --> 00:01:37,199\r\n\r\n\r\n2\r\n00:01:41,733 --> 00:01:44,065\r\n"
		"He, vielen Dank f\x81r die Begleitung.\r\n\r\n3\r\n00:02:08,333 --> 00:02:11,133\r\nZwei\r\nZeilen\r\n");
	CHECK(de.size() == 3);
	CHECK(de[0].text.empty() && de[0].start == 96.866 && de[0].end == 97.199);
	CHECK(de[1].text == U"He, vielen Dank für die Begleitung.");
	CHECK(de[2].text == U"Zwei\nZeilen" && de[2].end == 131.133);
	CHECK(srt_parse("00:00:01,000 --> 00:00:02,000\n\x84\x94\xe1\x82\x87\x9a\n")[0].text == U"\u00e4\u00f6\u00df\u00e9\u00e7\u00dc");
	CHECK(srt_at(de, 97.0) == nullptr);       /* the empty cue: nothing */
	CHECK(srt_at(de, 100.0) == nullptr);      /* between cues */
	CHECK(srt_at(de, 101.733) == &de[1]);     /* from its start */
	CHECK(srt_at(de, 104.065) == nullptr);    /* to its end */
	CHECK(srt_at(de, 0) == nullptr && srt_at(de, 1000) == nullptr);

	/* UTF-8 with a BOM, LF only, no empty line between the cues, a '.' before the milliseconds,
	 * out of order */
	auto u = srt_parse("\xef\xbb\xbf" "2\n00:00:05.000 --> 00:00:06.000\nZ\xc3\xa9ro\n1\n00:00:01,000 --> 00:00:02,500\nOne\n");
	CHECK(u.size() == 2);
	CHECK(u[0].text == U"One" && u[0].end == 2.5);
	CHECK(u[1].text == U"Zéro" && u[1].start == 5);

	/* nothing that's a cue */
	CHECK(srt_parse("").empty() && srt_parse("hello\nworld\n").empty());
	CHECK(srt_read("/nonexistent/x.srt").empty());

	if (failed) fprintf(stderr, "%d checks failed\n", failed);
	return failed != 0;
}
